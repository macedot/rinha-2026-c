FROM --platform=linux/amd64 debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc ca-certificates gzip && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile Makefile
COPY src/ src/
COPY resources/ resources/
COPY data/index.bin.gz ./data/

RUN gunzip -k data/index.bin.gz && make && mkdir -p /app && cp rinha-server /app/server

FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/server /app/server
COPY --from=build /src/data/index.bin /app/resources/index.bin

ENV INDEX_PATH=/app/resources/index.bin
ENV LISTEN_TCP=0
ENV IVF_NPROBE=8
ENV IVF_FULL_NPROBE=24
ENV CANDIDATES=0

ENTRYPOINT ["/app/server"]
