FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev gzip && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile Makefile
COPY src/ src/
COPY resources/ resources/

RUN mkdir -p data && gzip -dkc resources/index.bin.gz > data/index.bin && make && mkdir -p /app && cp rinha-server lb /app/

FROM debian:trixie-slim
COPY --from=build /app/rinha-server /app/server
COPY --from=build /app/lb /app/lb
COPY --from=build /src/data/index.bin /app/resources/index.bin

WORKDIR /app
ENV INDEX_PATH=/app/resources/index.bin
ENV IVF_NPROBE=5
ENV IVF_FULL_NPROBE=24
ENV CANDIDATES=0

# Default to server; docker-compose will override command for lb service
ENTRYPOINT ["/app/server"]
