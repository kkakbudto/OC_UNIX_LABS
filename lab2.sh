#!/bin/sh
set -u

SHARED_DIR="${1:-/data}"
LOCK_FILE="$SHARED_DIR/.lock"

if [ ! -d "$SHARED_DIR" ]; then
    mkdir -p "$SHARED_DIR" || {
        echo "ОШИБКА: Не удалось создать каталог общего тома: $SHARED_DIR" >&2
        exit 1
    }
fi

if [ -r /proc/sys/kernel/random/uuid ]; then
    CONTAINER_ID=$(cat /proc/sys/kernel/random/uuid)
else
    CONTAINER_ID=$(dd if=/dev/urandom bs=8 count=1 2>/dev/null | od -A n -t x1 | tr -d ' \n\t')
fi

if [ -z "$CONTAINER_ID" ]; then
    echo "ОШИБКА: Не удалось сгенерировать идентификатор контейнера" >&2
    exit 2
fi

CURRENT_FILE=""

cleanup() {
    if [ -n "$CURRENT_FILE" ]; then
        rm -f "$CURRENT_FILE"
    fi
    exit 0
}

trap cleanup INT TERM HUP QUIT

echo "Контейнер запущен. ID: $CONTAINER_ID, каталог тома: $SHARED_DIR"

SEQ_NUM=0

while :; do
    {
        if ! flock -x 9; then
            echo "[$CONTAINER_ID] ОШИБКА: Не удалось захватить блокировку $LOCK_FILE" >&2
            sleep 1
            continue
        fi

        i=1
        while :; do
            if [ "$i" -lt 10 ]; then
                FILE_NAME="00$i"
            elif [ "$i" -lt 100 ]; then
                FILE_NAME="0$i"
            else
                FILE_NAME="$i"
            fi

            FILE_PATH="$SHARED_DIR/$FILE_NAME"
            if [ ! -e "$FILE_PATH" ]; then
                SEQ_NUM=$((SEQ_NUM + 1))
                printf '%s %d\n' "$CONTAINER_ID" "$SEQ_NUM" > "$FILE_PATH"
                CURRENT_FILE="$FILE_PATH"
                break
            fi
            i=$((i + 1))
        done
    } 9>>"$LOCK_FILE"

    echo "[$CONTAINER_ID] Создан файл $FILE_NAME (порядковый номер: $SEQ_NUM)"

    sleep 1

    TARGET_FILE="$CURRENT_FILE"
    CURRENT_FILE=""
    rm -f "$TARGET_FILE"
    echo "[$CONTAINER_ID] Удалён файл $FILE_NAME (порядковый номер: $SEQ_NUM)"

    sleep 1
done