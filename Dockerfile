FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev gzip libgomp1 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile Makefile
COPY src/ src/
COPY indexer/ indexer/
COPY resources/ resources/
# Copy pre-generated i16 index if present in the build context (the normal fast path).
# This makes `docker compose build` complete in seconds instead of 20+ minutes.
# The good zero-error index (data/part*_data_i16.bin + labels + i16 bbox) is committed in git.
COPY data/ data/

ARG INPUT_FILE=resources/references.json.gz
# Only run the indexer if the required i16 output files are missing.
# This supports both "fast docker build with committed index" and "full re-index from source".
RUN if [ ! -f data/part0_data_i16.bin ] || [ ! -f data/part0_labels.bin ]; then \
        make indexer/indexer && \
        indexer/indexer $INPUT_FILE data; \
    fi && \
    make

FROM debian:trixie-slim AS server
COPY --from=build /src/rinha-server /app/server
COPY --from=build /src/data /app/data
COPY --from=build /src/resources/references.json.gz /app/data/references.json.gz

WORKDIR /app
ENV INDEX_PATH=/app/data

ENTRYPOINT ["/app/server"]