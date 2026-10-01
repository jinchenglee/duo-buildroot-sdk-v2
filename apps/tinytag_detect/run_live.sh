#!/bin/sh
# TinyTag live-camera launcher. Installed as /app/tinytag_detect/run_live.sh.
# Extra arguments are passed to tinytag_detect_live after the defaults, so a
# caller can override an option by supplying it later on the command line.
set -eu

MODEL="${TINYTAG_LIVE_MODEL:-/app/tinytag_detect/cvimodel/tinytag_v7_synthetic_area_cost.int8.cvimodel}"
BIN="${TINYTAG_LIVE_BIN:-/app/tinytag_detect/tinytag_detect_live}"
THRES="${TINYTAG_LIVE_THRES:-0.30}"
MAX="${TINYTAG_LIVE_MAX:-20}"
EXPAND="${TINYTAG_LIVE_EXPAND:-1.0}"
IOU="${TINYTAG_LIVE_IOU:-0.5}"
DECODE="${TINYTAG_LIVE_DECODE:-strict}"
DEBUG="${TINYTAG_LIVE_DEBUG:-1}"
# Per-tag stdout is a debug/result-export policy, not part of the real-time
# detector path. Keep it off by default: a slow terminal or pipe otherwise
# stalls acquisition. Set to 1 when the textual stream is explicitly needed.
TAG_OUTPUT="${TINYTAG_LIVE_TAG_OUTPUT:-0}"

# Direct binding is the verified default for the ordinary compact v40c model.
# It is accepted only when the VPSS luma plane exactly matches the dense tensor.
# Set 0 only for a copied-input A/B baseline.
DIRECT_COMPACT_INPUT="${TINYTAG_LIVE_DIRECT_COMPACT_INPUT:-1}"
# Diagnostic-only same-frame copied/direct bit comparison. The production
# default is off now that the board has passed exact validation.
VALIDATE_COMPACT_INPUT="${TINYTAG_LIVE_VALIDATE_COMPACT_INPUT:-0}"

# Sensor orientation, applied once in VI hardware (no per-frame cost).
# The OV5647 module needs mirroring. OV9281 starts unmirrored; the environment
# variable can override either profile for a different module orientation.
DEFAULT_MIRROR=1
if [ -r /mnt/data/sensor_cfg.ini ] &&
   grep -Eq '^name[[:space:]]*=[[:space:]]*OV_OV9281_' /mnt/data/sensor_cfg.ini; then
    DEFAULT_MIRROR=0
fi
MIRROR="${TINYTAG_LIVE_MIRROR:-${DEFAULT_MIRROR}}"
FLIP="${TINYTAG_LIVE_FLIP:-0}"

# Widen each decode crop horizontally to a multiple of this many pixels, so
# every crop row starts 4-byte aligned and spans whole 32-bit words. Set 0 or 1
# to disable and compare. Aligned regions show pink on the preview.
CROP_ALIGN="${TINYTAG_LIVE_CROP_ALIGN:-4}"
RETIRE_FRAMES="${TINYTAG_LIVE_RETIRE_FRAMES:-5}"

# For ownership/backlog testing only: TINYTAG_LIVE_PREVIEW_DELAY_MS adds a
# worker-side delay while it owns a preview surface. The binary reads this
# environment variable directly; normal operation leaves it unset or zero.

# TINYTAG_LIVE_PREVIEW_NICE (0..19) changes only the preview worker priority.
# It defaults to 10 so preview yields to detection on the single Linux core.
# Set it to 0 only for an A/B baseline. The binary reads this environment
# variable directly.

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
    --mirror "${MIRROR}" \
    --flip "${FLIP}" \
    --crop-align "${CROP_ALIGN}" \
    --retire-frames "${RETIRE_FRAMES}" \
    --debug "${DEBUG}" \
    --tag-output "${TAG_OUTPUT}" \
    --direct-compact-input "${DIRECT_COMPACT_INPUT}" \
    --validate-compact-input "${VALIDATE_COMPACT_INPUT}" \
    --no-rtsp \
    "$@"
