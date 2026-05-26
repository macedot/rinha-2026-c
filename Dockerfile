FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev gzip libgomp1 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile Makefile
COPY src/ src/
COPY indexer/ indexer/
COPY resources/ resources/

ARG INPUT_FILE=resources/references.json.gz
RUN make indexer/indexer && \
    indexer/indexer $INPUT_FILE data && \
    make

FROM debian:trixie-slim AS server
COPY --from=build /src/rinha-server /app/server
COPY --from=build /src/data /app/data
COPY --from=build /src/resources/references.json.gz /app/data/references.json.gz

WORKDIR /app
ENV INDEX_PATH=/app/data

ENTRYPOINT ["/app/server"]

FROM debian:trixie-slim AS lb
COPY --from=build /src/lb /app/lb

WORKDIR /app
ENTRYPOINT ["/app/lb"]