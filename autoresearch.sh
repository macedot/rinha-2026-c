#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Cleanup function
cleanup() {
    cd "$SCRIPT_DIR"
    docker compose down -v --remove-orphans 2>/dev/null || true
}
trap cleanup EXIT

# Build and start
echo "Starting docker compose..." >&2

docker compose up -d --build 2>&1 | tail -5

# Wait for ready
echo "Waiting for /ready..." >&2
for i in {1..60}; do
    if curl -sf http://localhost:9999/ready >/dev/null 2>&1; then
        echo "Ready." >&2
        break
    fi
    sleep 1
done

# Run k6 test
echo "Running k6 load test..." >&2
export K6_NO_USAGE_REPORT=true
k6 run ../test/test.js > /dev/null 2>&1

# Parse results
if [[ -f ../test/results.json ]]; then
    FINAL_SCORE=$(jq -r '.scoring.final_score // 0' ../test/results.json)
    P99=$(jq -r '.p99 // "0ms"' ../test/results.json)
    P99_NUM=$(echo "$P99" | sed 's/ms//')
    FP=$(jq -r '.scoring.breakdown.false_positive_detections // 0' ../test/results.json)
    FN=$(jq -r '.scoring.breakdown.false_negative_detections // 0' ../test/results.json)
    ERRS=$(jq -r '.scoring.breakdown.http_errors // 0' ../test/results.json)

    echo "METRIC final_score=$FINAL_SCORE"
    echo "METRIC p99_ms=$P99_NUM"
    echo "METRIC fp=$FP"
    echo "METRIC fn=$FN"
    echo "METRIC http_errors=$ERRS"

    cat ../test/results.json | jq . >&2
else
    echo "METRIC final_score=0"
    echo "METRIC p99_ms=9999"
    echo "METRIC fp=0"
    echo "METRIC fn=0"
    echo "METRIC http_errors=1"
    echo "ERROR: results.json not found" >&2
    exit 1
fi
