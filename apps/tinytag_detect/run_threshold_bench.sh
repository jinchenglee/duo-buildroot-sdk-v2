#!/bin/sh
# Run beside the benchmark on the board. Finite job counts; firmware waits
# have their own 2-second deadline. Preserve partial results on any failure.
set -eu
APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT_DIR=${TINYTAG_THRESHOLD_OUT_DIR:-/tmp/tinytag-threshold-$(date +%Y%m%d-%H%M%S)}
ITERATIONS=${TINYTAG_THRESHOLD_ITERATIONS:-100}
WARMUP=${TINYTAG_THRESHOLD_WARMUP:-5}
export LD_LIBRARY_PATH="/mnt/system/usr/lib:/mnt/system/lib:${LD_LIBRARY_PATH:-}"
mkdir -p "${OUT_DIR}"
echo "Threshold benchmark: ${OUT_DIR}"
for poll in 50 0; do
    echo "Running poll=${poll} us: OpenCV, scalar A53, FreeRTOS; every output checked"
    if ! "${APP_DIR}/tinytag_threshold_bench" --iterations "${ITERATIONS}" --warmup "${WARMUP}" \
        --poll-us "${poll}" "$@" >"${OUT_DIR}/poll-${poll}.tsv" 2>"${OUT_DIR}/poll-${poll}.log"; then
        cat "${OUT_DIR}/poll-${poll}.log" >&2
        echo "FAILED; partial results retained. Do not use timings after a validation failure." >&2
        exit 1
    fi
    cat "${OUT_DIR}/poll-${poll}.tsv"
done
echo "Results and validation logs: ${OUT_DIR}"
