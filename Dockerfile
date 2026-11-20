FROM alpine:latest

WORKDIR /app

COPY lab2.sh /app/lab2.sh
RUN chmod +x /app/lab2.sh && mkdir -p /data

VOLUME ["/data"]

ENTRYPOINT ["/bin/sh", "/app/lab2.sh"]
