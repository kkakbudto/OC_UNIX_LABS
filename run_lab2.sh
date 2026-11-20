#!/bin/sh
set -u

IMAGE_NAME="lab2-concurrent"
VOLUME_NAME="lab2_shared_vol"
PREFIX="lab2_worker"

stop_containers() {
    ids=$(docker ps -aq --filter "name=^/${PREFIX}_")
    if [ -n "$ids" ]; then
        echo "Остановка и удаление контейнеров..."
        docker stop -t 3 $ids >/dev/null
        docker rm $ids >/dev/null
        echo "Контейнеры удалены."
    else
        echo "Запущенных контейнеров $PREFIX не найдено."
    fi

    if docker volume inspect "$VOLUME_NAME" >/dev/null 2>&1 \
        && docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
        docker run --rm --entrypoint /bin/rm \
            -v "${VOLUME_NAME}:/data" \
            "$IMAGE_NAME" -f /data/.lock >/dev/null 2>&1 || true
    fi
}

case "${1:-}" in
    stop)
        stop_containers
        exit 0
        ;;
    clean)
        stop_containers
        docker volume rm -f "$VOLUME_NAME" >/dev/null 2>&1 || true
        echo "Том $VOLUME_NAME удалён."
        exit 0
        ;;
    ""|*[!0-9]*)
        echo "Использование:"
        echo "  $0 <количество>   — собрать образ, создать том и запустить N контейнеров (например, 1, 10 или 50)"
        echo "  $0 stop           — остановить и удалить все контейнеры (и очистить .lock в томе)"
        echo "  $0 clean          — остановить контейнеры и удалить разделяемый том"
        exit 1
        ;;
esac

COUNT="$1"
if [ "$COUNT" -lt 1 ]; then
    echo "ОШИБКА: Количество контейнеров должно быть не меньше 1" >&2
    exit 1
fi

stop_containers

echo "Создание разделяемого тома Docker: $VOLUME_NAME"
docker volume create "$VOLUME_NAME" >/dev/null

echo "Сборка образа: $IMAGE_NAME"
docker build -t "$IMAGE_NAME" .

echo "Запуск $COUNT экземпляр(ов) контейнера..."
i=1
while [ "$i" -le "$COUNT" ]; do
    docker run -d \
        --name "${PREFIX}_${i}" \
        -v "${VOLUME_NAME}:/data" \
        "$IMAGE_NAME" >/dev/null
    i=$((i + 1))
done

echo "Запущено контейнеров: $COUNT"
echo "Проверить работающие контейнеры: docker ps --filter name=${PREFIX}_"
echo "Посмотреть файлы в томе: docker exec ${PREFIX}_1 ls -la /data"
echo "Остановить все контейнеры: $0 stop"