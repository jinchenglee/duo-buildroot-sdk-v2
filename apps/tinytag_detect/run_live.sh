#!/bin/sh
# TinyTag live-camera launcher. Installed as /app/tinytag_detect/run_live.sh.
# Extra arguments are passed to tinytag_detect_live after the defaults, so a
# caller can override an option by supplying it later on the command line.
set -eu

MODEL="${TINYTAG_LIVE_MODEL:-/app/tinytag_detect/cvimodel/tinytag-v40c.int8.cvimodel}"
BIN="${TINYTAG_LIVE_BIN:-/app/tinytag_detect/tinytag_detect_live}"
THRES="${TINYTAG_LIVE_THRES:-0.20}"
MAX="${TINYTAG_LIVE_MAX:-8}"
EXPAND="${TINYTAG_LIVE_EXPAND:-1.5}"
IOU="${TINYTAG_LIVE_IOU:-0.5}"
DECODE="${TINYTAG_LIVE_DECODE:-strict}"
DEBUG="${TINYTAG_LIVE_DEBUG:-1}"

[ -x "${BIN}" ] || { echo "Error: ${BIN} not found or not executable" >&2; exit 1; }
[ -f "${MODEL}" ] || { echo "Error: model ${MODEL} not found" >&2; exit 1; }

# Non-interactive SSH and init shells do not source /etc/profile.
case ":${LD_LIBRARY_PATH:-}:" in
    *:/mnt/system/lib:*) ;;
    *) LD_LIBRARY_PATH="/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:${LD_LIBRARY_PATH:-}"
       export LD_LIBRARY_PATH ;;
esac

exec "${BIN}" "${MODEL}" \
    --thres "${THRES}" \
    --max "${MAX}" \
    --expand "${EXPAND}" \
    --iou "${IOU}" \
    --decode "${DECODE}" \
    --debug "${DEBUG}" \
    --rtsp \
    "$@"
