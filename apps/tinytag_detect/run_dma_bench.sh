#!/bin/sh
set -eu

APP_DIR=/app/tinytag_detect
BENCH=${APP_DIR}/tinytag_ive_dma_bench
ITERATIONS=${TINYTAG_DMA_ITERATIONS:-100}
WARMUP=${TINYTAG_DMA_WARMUP:-5}

export LD_LIBRARY_PATH="/mnt/system/usr/lib:/mnt/system/lib:${LD_LIBRARY_PATH:-}"

if [ ! -x "${BENCH}" ]; then
    echo "missing benchmark: ${BENCH}" >&2
    exit 1
fi

echo "IVE DMA benchmark: iterations=${ITERATIONS} warmup=${WARMUP}"
echo "instant=0: kernel completion/IRQ wait; instant=1: kernel register polling"

if [ -w /proc/ive/hw_profiling ]; then
    echo 1 > /proc/ive/hw_profiling
fi

for cached in 0 1; do
    for instant in 0 1; do
        "${BENCH}" --sweep --iterations "${ITERATIONS}" --warmup "${WARMUP}" \
            --cached "${cached}" --instant "${instant}"
    done
done

# Representative padded stride. It is aligned but differs from the payload
# width, exercising row stepping without asking the hardware to accept an
# unsupported byte alignment.
for instant in 0 1; do
    "${BENCH}" --width 1272 --height 720 --stride 1280 \
        --iterations "${ITERATIONS}" --warmup "${WARMUP}" \
        --cached 0 --instant "${instant}"
done

if [ -r /proc/ive/hw_profiling ]; then
    echo "Last kernel IVE profiling record:"
    cat /proc/ive/hw_profiling
fi
