#!/bin/sh
if [ -z "$1" ]; then
    echo "ОШИБКА: Не указан исходный файл"
    exit 2
fi
INPUT_FILE=$1
ORIG_DIR=$(pwd)
if [ ! -e "$INPUT_FILE" ]; then
    echo "ОШИБКА: Исходный файл не найден"
    exit 3
fi

case "$INPUT_FILE" in
    /*) ABS_INPUT_FILE="$INPUT_FILE" ;;
    *) ABS_INPUT_FILE="$ORIG_DIR/$INPUT_FILE" ;;
esac
FILENAME=$(basename "$ABS_INPUT_FILE")
TARGET_DIR=$(dirname "$ABS_INPUT_FILE")

OUTPUT_FILE=$(sed -n 's/.*Output:[ \t]*\([^ \t]*\).*/\1/p' "$ABS_INPUT_FILE" | head -n 1)
if [ -z "$OUTPUT_FILE" ]; then
    OUTPUT_FILE="default_out"
fi

TEMP_DIR=$(mktemp -d)
if [ ! -d "$TEMP_DIR" ]; then
    echo "ОШИБКА: Не удалось создать временный каталог"
    exit 4
fi

trap 'rc=$?; rm -rf "$TEMP_DIR"; exit $rc' EXIT HUP INT QUIT PIPE TERM

cd "$TEMP_DIR" || exit 1
case "$FILENAME" in
    *.c)
        echo "Выполняется сборка C файла: $FILENAME (результат: $OUTPUT_FILE)"
        cc -Wall -Wextra -O2 "$ABS_INPUT_FILE" -o "$OUTPUT_FILE"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            cp "$OUTPUT_FILE" "$TARGET_DIR/" || rc=$?
        fi
        ;;
    *.cpp|*.cc|*.cxx)
        echo "Выполняется сборка C++ файла: $FILENAME (результат: $OUTPUT_FILE)"
        c++ -Wall -Wextra -O2 "$ABS_INPUT_FILE" -o "$OUTPUT_FILE"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            cp "$OUTPUT_FILE" "$TARGET_DIR/" || rc=$?
        fi
        ;;
    *.tex)
        echo "Выполняется сборка TeX документа: $FILENAME (результат: $OUTPUT_FILE.pdf)"
        pdflatex -interaction=nonstopmode -jobname="$OUTPUT_FILE" "$ABS_INPUT_FILE"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            cp "$OUTPUT_FILE.pdf" "$TARGET_DIR/" || rc=$?
        fi
        ;;
    *)
        echo "ОШИБКА: Неподдерживаемый формат входного файла"
        exit 5
        ;;
esac

cd "$ORIG_DIR" || exit 6
if [ "$rc" -eq 0 ]; then
    echo "Сборка успешно завершена"
else
    echo "В процессе сборки произошла ошибка (Код: $rc)"
fi
exit $rc