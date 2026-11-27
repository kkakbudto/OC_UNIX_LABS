#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace {

struct FileInfo {
    fs::path path;
    std::uintmax_t size = 0;
    std::string sha1;
    std::uintmax_t link_count = 1;
};

struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept {
        EVP_MD_CTX_free(ctx);
    }
};

using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

std::string to_utf8(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& ws) {
    if (ws.empty()) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), len, nullptr, nullptr);
    return out;
}
#endif

bool compute_sha1(const fs::path& file_path, std::string& out_hex, std::string& out_error) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        out_error = "cannot open file for reading";
        return false;
    }

    EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) {
        out_error = "EVP_MD_CTX_new failed";
        return false;
    }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) != 1) {
        out_error = "EVP_DigestInit_ex failed";
        return false;
    }

    std::array<char, 65536> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(bytes_read)) != 1) {
                out_error = "EVP_DigestUpdate failed";
                return false;
            }
        }
    }

    if (file.bad()) {
        out_error = "stream read error";
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1) {
        out_error = "EVP_DigestFinal_ex failed";
        return false;
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    out_hex = oss.str();
    return true;
}

bool files_have_identical_bytes(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa.is_open() || !fb.is_open()) {
        return false;
    }

    std::array<char, 65536> ba{};
    std::array<char, 65536> bb{};
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        std::streamsize ca = fa.gcount();
        std::streamsize cb = fb.gcount();
        if (ca != cb) {
            return false;
        }
        if (!std::equal(ba.begin(), ba.begin() + ca, bb.begin())) {
            return false;
        }
    }
    return fa.eof() && fb.eof() && !fa.bad() && !fb.bad();
}

fs::path make_unique_temp_path(const fs::path& target_path, std::uint64_t& counter) {
    const fs::path dir = target_path.parent_path();
    std::error_code ec;
    while (true) {
        fs::path candidate = dir / (".hardlink_tmp_" + std::to_string(++counter));
        if (!fs::exists(fs::symlink_status(candidate, ec))) {
            return candidate;
        }
    }
}

bool replace_with_hard_link(const fs::path& keeper, const fs::path& duplicate, std::uint64_t& tmp_counter, std::string& out_error) {
    std::error_code ec;
    fs::path temp_link = make_unique_temp_path(duplicate, tmp_counter);

    fs::create_hard_link(keeper, temp_link, ec);
    if (ec) {
        out_error = "create_hard_link failed: " + ec.message();
        return false;
    }

    fs::rename(temp_link, duplicate, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(temp_link, rm_ec);
        out_error = "rename failed: " + ec.message();
        return false;
    }

    return true;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " <directory> [options]\n"
              << "\n"
              << "Traverses <directory>, computes SHA-1 for every regular file,\n"
              << "keeps one instance per identical content, and replaces duplicates\n"
              << "with hard links pointing to the kept instance.\n"
              << "\n"
              << "Options:\n"
              << "  -n, --dry-run   Show planned actions without modifying files\n"
              << "  -q, --quiet     Suppress per-file hash listing\n"
              << "  -h, --help      Show this help message\n";
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    (void)argc;
    SetConsoleOutputCP(CP_UTF8);
#endif

    bool dry_run = false;
    bool quiet = false;
    fs::path root;
    bool has_root = false;

#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        std::cerr << "Error: failed to parse command line arguments.\n";
        return 1;
    }
    struct LocalFreeGuard {
        LPWSTR* ptr;
        ~LocalFreeGuard() { LocalFree(ptr); }
    } wargv_guard{wargv};

    std::string prog_utf8 = (wargc > 0) ? wide_to_utf8(wargv[0]) : argv[0];
    for (int i = 1; i < wargc; ++i) {
        std::wstring warg = wargv[i];
        if (warg == L"-h" || warg == L"--help") {
            print_usage(prog_utf8.c_str());
            return 0;
        } else if (warg == L"-n" || warg == L"--dry-run") {
            dry_run = true;
        } else if (warg == L"-q" || warg == L"--quiet") {
            quiet = true;
        } else if (!warg.empty() && warg[0] == L'-') {
            std::cerr << "Unknown option: " << wide_to_utf8(warg) << "\n";
            print_usage(prog_utf8.c_str());
            return 1;
        } else if (!has_root) {
            root = fs::path(warg);
            has_root = true;
        } else {
            std::cerr << "Unexpected extra argument: " << wide_to_utf8(warg) << "\n";
            print_usage(prog_utf8.c_str());
            return 1;
        }
    }
    if (!has_root) {
        print_usage(prog_utf8.c_str());
        return 1;
    }
#else
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-n" || arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        } else if (!has_root) {
            root = fs::path(arg);
            has_root = true;
        } else {
            std::cerr << "Unexpected extra argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    if (!has_root) {
        print_usage(argv[0]);
        return 1;
    }
#endif

    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        std::cerr << "Error: '" << to_utf8(root) << "' is not an existing directory.\n";
        return 1;
    }

    std::vector<fs::path> file_paths;
    fs::recursive_directory_iterator it(
        root,
        fs::directory_options::skip_permission_denied,
        ec
    );
    if (ec) {
        std::cerr << "Error opening directory '" << to_utf8(root) << "': " << ec.message() << "\n";
        return 1;
    }

    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            std::cerr << "Warning: directory traversal error: " << ec.message() << "\n";
            ec.clear();
            continue;
        }
        const fs::directory_entry& entry = *it;
        if (entry.is_symlink(ec)) {
            continue;
        }
        if (ec) {
            ec.clear();
            continue;
        }
        if (entry.is_regular_file(ec)) {
            file_paths.push_back(entry.path());
        } else if (ec) {
            ec.clear();
        }
    }

    std::sort(file_paths.begin(), file_paths.end(), [](const fs::path& a, const fs::path& b) {
        return to_utf8(a.lexically_normal()) < to_utf8(b.lexically_normal());
    });

    std::vector<FileInfo> files;
    files.reserve(file_paths.size());
    std::size_t hash_errors = 0;

    if (!quiet) {
        std::cout << "=== File list and SHA-1 hashes (" << file_paths.size() << " files) ===\n";
    }

    for (const fs::path& p : file_paths) {
        FileInfo info;
        info.path = p;
        info.size = fs::file_size(p, ec);
        if (ec) {
            std::cerr << "Warning: cannot get size of '" << to_utf8(p) << "': " << ec.message() << "\n";
            ++hash_errors;
            ec.clear();
            continue;
        }

        info.link_count = fs::hard_link_count(p, ec);
        if (ec) {
            info.link_count = 1;
            ec.clear();
        }

        std::string err;
        if (!compute_sha1(p, info.sha1, err)) {
            std::cerr << "Warning: cannot hash '" << to_utf8(p) << "': " << err << "\n";
            ++hash_errors;
            continue;
        }

        if (!quiet) {
            std::cout << info.sha1 << "  (" << info.size << " B, links=" << info.link_count << ")  "
                      << to_utf8(info.path) << "\n";
        }
        files.push_back(std::move(info));
    }

    std::map<std::string, std::vector<FileInfo>> groups;
    for (const auto& f : files) {
        groups[f.sha1].push_back(f);
    }

    std::size_t duplicate_groups = 0;
    std::size_t replaced_count = 0;
    std::size_t already_linked_count = 0;
    std::size_t link_errors = 0;
    std::uintmax_t bytes_saved = 0;
    std::uint64_t tmp_counter = 0;

    std::cout << "\n=== Deduplication " << (dry_run ? "(dry-run) " : "") << "===\n";

    for (auto& [sha1, group] : groups) {
        if (group.size() < 2) {
            continue;
        }

        // Prefer keeping a file that already has more hard links, tie-break alphabetically.
        std::sort(group.begin(), group.end(), [](const FileInfo& a, const FileInfo& b) {
            if (a.link_count != b.link_count) {
                return a.link_count > b.link_count;
            }
            return to_utf8(a.path.lexically_normal()) < to_utf8(b.path.lexically_normal());
        });

        const FileInfo& keeper = group.front();
        bool group_reported = false;
        // Track remaining hard-link counts per distinct inode during dry-run.
        std::vector<std::pair<fs::path, std::uintmax_t>> dry_run_inodes;

        auto report_group_header = [&]() {
            if (!group_reported) {
                ++duplicate_groups;
                std::cout << "\nSHA-1: " << sha1 << " (" << keeper.size << " bytes)\n"
                          << "  [KEEP] " << to_utf8(keeper.path) << "\n";
                group_reported = true;
            }
        };

        for (std::size_t i = 1; i < group.size(); ++i) {
            const FileInfo& candidate = group[i];

            // Check inode equivalence before reading file bytes from disk.
            bool equiv = fs::equivalent(keeper.path, candidate.path, ec);
            if (ec) {
                report_group_header();
                std::cerr << "  [ERR ] equivalence check failed for '" << to_utf8(candidate.path)
                          << "': " << ec.message() << "\n";
                ++link_errors;
                ec.clear();
                continue;
            }

            if (equiv) {
                report_group_header();
                std::cout << "  [SAME] already hard-linked: " << to_utf8(candidate.path) << "\n";
                ++already_linked_count;
                continue;
            }

            if (candidate.size != keeper.size || !files_have_identical_bytes(keeper.path, candidate.path)) {
                std::cerr << "Warning: SHA-1 match without identical content, skipping: '"
                          << to_utf8(candidate.path) << "'\n";
                continue;
            }

            report_group_header();

            if (dry_run) {
                std::cout << "  [PLAN] would replace with hard link: " << to_utf8(candidate.path) << "\n";
                ++replaced_count;

                // Disk space is freed only when the last hard link to an inode is removed.
                bool found_inode = false;
                for (auto& [rep_path, remaining] : dry_run_inodes) {
                    std::error_code eq_ec;
                    if (fs::equivalent(rep_path, candidate.path, eq_ec) && !eq_ec) {
                        found_inode = true;
                        if (remaining > 0) {
                            --remaining;
                            if (remaining == 0) {
                                bytes_saved += candidate.size;
                            }
                        }
                        break;
                    }
                }
                if (!found_inode) {
                    std::error_code lc_ec;
                    std::uintmax_t links = fs::hard_link_count(candidate.path, lc_ec);
                    if (lc_ec || links <= 1) {
                        bytes_saved += candidate.size;
                    } else {
                        dry_run_inodes.emplace_back(candidate.path, links - 1);
                    }
                }
                continue;
            }

            std::error_code lc_ec;
            std::uintmax_t links_before = fs::hard_link_count(candidate.path, lc_ec);
            bool frees_inode = (!lc_ec && links_before <= 1);

            std::string link_err;
            if (replace_with_hard_link(keeper.path, candidate.path, tmp_counter, link_err)) {
                std::cout << "  [LINK] replaced with hard link: " << to_utf8(candidate.path) << "\n";
                ++replaced_count;
                if (frees_inode) {
                    bytes_saved += candidate.size;
                }
            } else {
                std::cerr << "  [ERR ] failed to link '" << to_utf8(candidate.path)
                          << "': " << link_err << "\n";
                ++link_errors;
            }
        }
    }

    std::cout << "\n=== Summary ===\n"
              << "Files scanned:         " << file_paths.size() << "\n"
              << "Files hashed:          " << files.size() << "\n"
              << "Duplicate groups:      " << duplicate_groups << "\n"
              << (dry_run ? "Would replace:         " : "Replaced with links:   ") << replaced_count << "\n"
              << "Already hard-linked:   " << already_linked_count << "\n"
              << "Errors:                " << (hash_errors + link_errors) << "\n"
              << (dry_run ? "Estimated bytes saved: " : "Bytes saved:           ") << bytes_saved << "\n";

    return (hash_errors + link_errors) > 0 ? 1 : 0;
}