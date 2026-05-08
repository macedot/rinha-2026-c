FROM --platform=linux/amd64 debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev gzip && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile Makefile
COPY src/ src/
COPY resources/ resources/
COPY data/index.bin.gz ./data/

RUN gunzip -k data/index.bin.gz && make && mkdir -p /app && cp rinha-server /app/server

FROM scratch
COPY --from=build /app/server /app/server
COPY --from=build /src/data/index.bin /app/resources/index.bin

WORKDIR /app
ENV INDEX_PATH=/app/resources/index.bin
ENV LISTEN_TCP=0
ENV IVF_NPROBE=8
ENV IVF_FULL_NPROBE=24
ENV CANDIDATES=0

ENTRYPOINT ["/app/server"]
