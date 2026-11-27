#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kTempPrefix = ".hardlink_dedup_tmp_";
constexpr const char* kLegacyTempPrefix = ".hardlink_tmp_";

struct FileInfo {
    fs::path path;
    std::uintmax_t size = 0;
    std::string sha1;
    std::uintmax_t link_count = 1;
};

class Sha1 {
public:
    Sha1() {
        reset();
    }

    void reset() {
        h_[0] = 0x67452301u;
        h_[1] = 0xEFCDAB89u;
        h_[2] = 0x98BADCFEu;
        h_[3] = 0x10325476u;
        h_[4] = 0xC3D2E1F0u;
        total_bytes_ = 0;
        buf_len_ = 0;
    }

    void update(const unsigned char* data, std::size_t len) {
        total_bytes_ += static_cast<std::uint64_t>(len);
        std::size_t offset = 0;

        if (buf_len_ > 0) {
            std::size_t to_copy = std::min(len, block_.size() - buf_len_);
            std::memcpy(block_.data() + buf_len_, data, to_copy);
            buf_len_ += to_copy;
            offset += to_copy;
            if (buf_len_ == block_.size()) {
                process_block(block_.data());
                buf_len_ = 0;
            }
        }

        while (offset + block_.size() <= len) {
            process_block(data + offset);
            offset += block_.size();
        }

        if (offset < len) {
            buf_len_ = len - offset;
            std::memcpy(block_.data(), data + offset, buf_len_);
        }
    }

    std::string final_hex() {
        const std::uint64_t total_bits = total_bytes_ * 8u;

        block_[buf_len_++] = 0x80u;
        if (buf_len_ > 56) {
            std::memset(block_.data() + buf_len_, 0, block_.size() - buf_len_);
            process_block(block_.data());
            buf_len_ = 0;
        }
        std::memset(block_.data() + buf_len_, 0, 56 - buf_len_);

        for (int i = 0; i < 8; ++i) {
            block_[56 + i] = static_cast<unsigned char>((total_bits >> ((7 - i) * 8)) & 0xFFu);
        }
        process_block(block_.data());

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (std::uint32_t word : h_) {
            oss << std::setw(8) << word;
        }
        reset();
        return oss.str();
    }

private:
    static std::uint32_t rotl(std::uint32_t value, unsigned int bits) {
        return (value << bits) | (value >> (32u - bits));
    }

    void process_block(const unsigned char* block) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h_[0];
        std::uint32_t b = h_[1];
        std::uint32_t c = h_[2];
        std::uint32_t d = h_[3];
        std::uint32_t e = h_[4];

        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }

            std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    std::array<std::uint32_t, 5> h_{};
    std::array<unsigned char, 64> block_{};
    std::uint64_t total_bytes_ = 0;
    std::size_t buf_len_ = 0;
};

bool starts_with(const std::string& str, const char* prefix) {
    const std::size_t prefix_len = std::strlen(prefix);
    return str.size() >= prefix_len && str.compare(0, prefix_len, prefix) == 0;
}

bool is_stale_temp_name(const fs::path& p) {
    const std::string name = p.filename().string();
    return starts_with(name, kTempPrefix) || starts_with(name, kLegacyTempPrefix);
}

bool compute_sha1(const fs::path& file_path, std::string& out_hex, std::string& out_error) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        out_error = "cannot open file for reading";
        return false;
    }

    Sha1 sha1;
    std::array<char, 65536> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            sha1.update(reinterpret_cast<const unsigned char*>(buffer.data()),
                        static_cast<std::size_t>(bytes_read));
        }
    }

    if (file.bad()) {
        out_error = "stream read error";
        return false;
    }

    out_hex = sha1.final_hex();
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
    const auto pid = static_cast<std::uint64_t>(::getpid());
    std::error_code ec;
    while (true) {
        fs::path candidate = dir / (std::string(kTempPrefix) + std::to_string(pid) + "_" + std::to_string(++counter));
        fs::file_status st = fs::symlink_status(candidate, ec);
        if (st.type() == fs::file_type::not_found || ec) {
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
    std::cout << "Usage: " << prog_name << " [options] [--] <directory>\n"
              << "\n"
              << "Traverses <directory>, computes SHA-1 for every regular file,\n"
              << "keeps one instance per identical content, and replaces duplicates\n"
              << "with hard links pointing to the kept instance.\n"
              << "\n"
              << "Options:\n"
              << "  -n, --dry-run   Show planned actions without modifying files\n"
              << "  -q, --quiet     Suppress per-file hash listing\n"
              << "  -h, --help      Show this help message\n"
              << "  --              End of options (allows directory names starting with '-')\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    bool dry_run = false;
    bool quiet = false;
    bool end_of_options = false;
    fs::path root;
    bool has_root = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!end_of_options && arg == "--") {
            end_of_options = true;
            continue;
        }
        if (!end_of_options && (arg == "-h" || arg == "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!end_of_options && (arg == "-n" || arg == "--dry-run")) {
            dry_run = true;
        } else if (!end_of_options && (arg == "-q" || arg == "--quiet")) {
            quiet = true;
        } else if (!end_of_options && !arg.empty() && arg[0] == '-') {
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

    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        std::cerr << "Error: '" << root.string() << "' is not an existing directory.\n";
        return 1;
    }

    std::vector<fs::path> file_paths;
    std::vector<fs::path> stale_temps;
    fs::recursive_directory_iterator it(
        root,
        fs::directory_options::skip_permission_denied,
        ec
    );
    if (ec) {
        std::cerr << "Error opening directory '" << root.string() << "': " << ec.message() << "\n";
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
            if (is_stale_temp_name(entry.path())) {
                stale_temps.push_back(entry.path());
            } else {
                file_paths.push_back(entry.path());
            }
        } else if (ec) {
            ec.clear();
        }
    }

    std::size_t cleanup_count = 0;
    std::size_t cleanup_errors = 0;
    if (!stale_temps.empty()) {
        std::sort(stale_temps.begin(), stale_temps.end(), [](const fs::path& a, const fs::path& b) {
            return a.lexically_normal().string() < b.lexically_normal().string();
        });
        for (const fs::path& tmp : stale_temps) {
            if (dry_run) {
                std::cout << "[CLEAN] would remove stale temp file: " << tmp.string() << "\n";
                ++cleanup_count;
            } else {
                std::error_code rm_ec;
                if (fs::remove(tmp, rm_ec) && !rm_ec) {
                    std::cout << "[CLEAN] removed stale temp file: " << tmp.string() << "\n";
                    ++cleanup_count;
                } else {
                    std::cerr << "Warning: cannot remove stale temp file '" << tmp.string()
                              << "': " << rm_ec.message() << "\n";
                    ++cleanup_errors;
                }
            }
        }
    }

    std::sort(file_paths.begin(), file_paths.end(), [](const fs::path& a, const fs::path& b) {
        return a.lexically_normal().string() < b.lexically_normal().string();
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
            std::cerr << "Warning: cannot get size of '" << p.string() << "': " << ec.message() << "\n";
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
            std::cerr << "Warning: cannot hash '" << p.string() << "': " << err << "\n";
            ++hash_errors;
            continue;
        }

        if (!quiet) {
            std::cout << info.sha1 << "  (" << info.size << " B, links=" << info.link_count << ")  "
                      << info.path.string() << "\n";
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
            return a.path.lexically_normal().string() < b.path.lexically_normal().string();
        });

        const FileInfo& keeper = group.front();
        bool group_reported = false;
        // Track remaining hard-link counts per distinct inode during dry-run.
        std::vector<std::pair<fs::path, std::uintmax_t>> dry_run_inodes;

        auto report_group_header = [&]() {
            if (!group_reported) {
                ++duplicate_groups;
                std::cout << "\nSHA-1: " << sha1 << " (" << keeper.size << " bytes)\n"
                          << "  [KEEP] " << keeper.path.string() << "\n";
                group_reported = true;
            }
        };

        for (std::size_t i = 1; i < group.size(); ++i) {
            const FileInfo& candidate = group[i];

            // Check inode equivalence before reading file bytes from disk.
            bool equiv = fs::equivalent(keeper.path, candidate.path, ec);
            if (ec) {
                report_group_header();
                std::cerr << "  [ERR ] equivalence check failed for '" << candidate.path.string()
                          << "': " << ec.message() << "\n";
                ++link_errors;
                ec.clear();
                continue;
            }

            if (equiv) {
                report_group_header();
                std::cout << "  [SAME] already hard-linked: " << candidate.path.string() << "\n";
                ++already_linked_count;
                continue;
            }

            if (candidate.size != keeper.size || !files_have_identical_bytes(keeper.path, candidate.path)) {
                std::cerr << "Warning: SHA-1 match without identical content, skipping: '"
                          << candidate.path.string() << "'\n";
                continue;
            }

            report_group_header();

            if (dry_run) {
                std::cout << "  [PLAN] would replace with hard link: " << candidate.path.string() << "\n";
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
                std::cout << "  [LINK] replaced with hard link: " << candidate.path.string() << "\n";
                ++replaced_count;
                if (frees_inode) {
                    bytes_saved += candidate.size;
                }
            } else {
                std::cerr << "  [ERR ] failed to link '" << candidate.path.string()
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
              << (dry_run ? "Would clean temp:      " : "Cleaned stale temp:    ") << cleanup_count << "\n"
              << "Errors:                " << (hash_errors + link_errors + cleanup_errors) << "\n"
              << (dry_run ? "Estimated bytes saved: " : "Bytes saved:           ") << bytes_saved << "\n";

    return (hash_errors + link_errors + cleanup_errors) > 0 ? 1 : 0;
}