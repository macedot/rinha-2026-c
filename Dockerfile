FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev gzip libgomp1 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile Makefile
COPY src/ src/
COPY indexer/ indexer/
COPY resources/ resources/
COPY test/ test/

ARG INPUT_FILE=resources/references.json.gz
RUN mkdir -p data && \
    make indexer/indexer && \
    indexer/indexer $INPUT_FILE data && \
    make && \
    mkdir -p /app && \
    cp rinha-server lb /app/

FROM debian:trixie-slim
COPY --from=build /app/rinha-server /app/server
COPY --from=build /app/lb /app/lb
COPY --from=build /src/data /app/data

WORKDIR /app
ENV INDEX_PATH=/app/data

# Default to server; docker-compose will override command for lb service
ENTRYPOINT ["/app/server"]
