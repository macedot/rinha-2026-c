FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile Makefile
COPY src/ src/

RUN make

FROM debian:trixie-slim
COPY --from=build /src/rinha-server /app/server
COPY --from=build /src/lb /app/lb
COPY data/ /app/data/

WORKDIR /app
ENV INDEX_PATH=/app/data

ENTRYPOINT ["/app/server"]