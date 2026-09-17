#!/bin/sh
# TinyTag detector launcher -- installed to /app/tinytag_detect/run_tinytag.sh
# on the board by apps/tinytag_detect/build.sh, with a /usr/local/bin symlink
# so it stays on PATH.
#
# Wraps tinytag_detect with the K230 production operating point so the common
# case is just:
#
#   run_tinytag.sh /path/to/image.jpg
#
# Any of the defaults can be overridden from the environment, and any extra
# arguments are passed straight through to the binary:
#
#   TINYTAG_THRES=0.20 run_tinytag.sh frame.jpg
#   TINYTAG_DECODE=strict run_tinytag.sh frame.jpg    # full two-stage pipeline
#   run_tinytag.sh frame.jpg --repeat 20
set -eu

# --- defaults -------------------------------------------------------------
# These match the K230 production launcher (utils/run.sh there): threshold 0.35,
# an 8-proposal cap, and ROI expansion 1.5. Threshold 0.20 is the training
# repo's frozen value: higher recall, roughly twice the proposals.
TINYTAG_MODEL="${TINYTAG_MODEL:-/app/tinytag_detect/cvimodel/tinytag-v40c.int8.cvimodel}"
TINYTAG_THRES="${TINYTAG_THRES:-0.35}"
TINYTAG_MAX="${TINYTAG_MAX:-8}"
TINYTAG_EXPAND="${TINYTAG_EXPAND:-1.5}"
TINYTAG_IOU="${TINYTAG_IOU:-0.5}"
TINYTAG_OUT="${TINYTAG_OUT:-/tmp/tinytag_det.jpg}"
TINYTAG_DEBUG="${TINYTAG_DEBUG:-1}"
TINYTAG_BIN="${TINYTAG_BIN:-/app/tinytag_detect/tinytag_detect}"
TINYTAG_GOLDEN="${TINYTAG_GOLDEN:-/app/tinytag_detect/cvimodel/tinytag-v40c.ttgold}"
TINYTAG_REPEAT="${TINYTAG_REPEAT:-20}"
TINYTAG_WARMUP="${TINYTAG_WARMUP:-2}"
TINYTAG_MAX_MAE="${TINYTAG_MAX_MAE:-0.05}"
# Empty disables stage two. Set to "strict" or "tolerant" to run the ArUco Nano
# AprilTag 36h11 decoder over every proposal -- i.e. the full two-stage
# pipeline, minus camera capture.
TINYTAG_DECODE="${TINYTAG_DECODE:-}"
# With no <image> argument, run against this bundled sample -- the real
# deployment path (see samples/README.md) -- so `run_tinytag.sh` alone works.
TINYTAG_IMAGE="${TINYTAG_IMAGE:-/app/tinytag_detect/samples/arena-1280x800.jpg}"
# --------------------------------------------------------------------------

usage() {
    cat <<USAGE
Usage: $(basename "$0") [image] [extra tinytag_detect args...]
       $(basename "$0") --selftest [extra args...]

With no [image], runs against the bundled sample (TINYTAG_IMAGE below) so
every other option already has a sane default -- just run it.

--selftest replays golden frames through the TPU and reports both the
quantization error against FP32 and any divergence from the host simulator,
plus inference timing. Exits non-zero if any gate fails.

Environment overrides (current values shown):
  TINYTAG_IMAGE   ${TINYTAG_IMAGE}
  TINYTAG_MODEL   ${TINYTAG_MODEL}
  TINYTAG_THRES   ${TINYTAG_THRES}
  TINYTAG_MAX     ${TINYTAG_MAX}
  TINYTAG_EXPAND  ${TINYTAG_EXPAND}
  TINYTAG_IOU     ${TINYTAG_IOU}
  TINYTAG_OUT     ${TINYTAG_OUT}
  TINYTAG_DEBUG   ${TINYTAG_DEBUG}
  TINYTAG_BIN     ${TINYTAG_BIN}
  TINYTAG_GOLDEN  ${TINYTAG_GOLDEN}
  TINYTAG_REPEAT  ${TINYTAG_REPEAT}
  TINYTAG_WARMUP  ${TINYTAG_WARMUP}
  TINYTAG_MAX_MAE ${TINYTAG_MAX_MAE}
  TINYTAG_DECODE  ${TINYTAG_DECODE:-(off)}
USAGE
}

# /mnt/system/lib is already on LD_LIBRARY_PATH via /etc/profile, but a
# non-login shell (ssh "cmd", init script, cron) does not source that.
set_library_path() {
    case ":${LD_LIBRARY_PATH:-}:" in
        *:/mnt/system/lib:*) ;;
        *) LD_LIBRARY_PATH="/mnt/system/lib:/mnt/system/usr/lib:${LD_LIBRARY_PATH:-}"
           export LD_LIBRARY_PATH ;;
    esac
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi

if [ "${1:-}" = "--selftest" ]; then
    shift
    [ -x "${TINYTAG_BIN}" ] || { echo "Error: ${TINYTAG_BIN} not found or not executable" >&2; exit 1; }
    [ -f "${TINYTAG_MODEL}" ] || { echo "Error: model ${TINYTAG_MODEL} not found" >&2; exit 1; }
    [ -f "${TINYTAG_GOLDEN}" ] || { echo "Error: golden bundle ${TINYTAG_GOLDEN} not found" >&2; exit 1; }
    set_library_path
    exec "${TINYTAG_BIN}" \
        "${TINYTAG_MODEL}" \
        --selftest "${TINYTAG_GOLDEN}" \
        --repeat "${TINYTAG_REPEAT}" \
        --warmup "${TINYTAG_WARMUP}" \
        --max-mae "${TINYTAG_MAX_MAE}" \
        --debug "${TINYTAG_DEBUG}" \
        "$@"
fi

image="${1:-${TINYTAG_IMAGE}}"
[ $# -ge 1 ] && shift

[ -x "${TINYTAG_BIN}" ] || { echo "Error: ${TINYTAG_BIN} not found or not executable" >&2; exit 1; }
[ -f "${TINYTAG_MODEL}" ] || { echo "Error: model ${TINYTAG_MODEL} not found" >&2; exit 1; }
[ -f "${image}" ] || { echo "Error: image ${image} not found" >&2; exit 1; }

set_library_path

decode_args=""
if [ -n "${TINYTAG_DECODE}" ]; then
    decode_args="--decode ${TINYTAG_DECODE}"
fi

# shellcheck disable=SC2086 -- decode_args is intentionally word-split
exec "${TINYTAG_BIN}" \
    "${TINYTAG_MODEL}" \
    "${image}" \
    --thres "${TINYTAG_THRES}" \
    --max "${TINYTAG_MAX}" \
    --expand "${TINYTAG_EXPAND}" \
    --iou "${TINYTAG_IOU}" \
    --out "${TINYTAG_OUT}" \
    --repeat "${TINYTAG_REPEAT}" \
    --warmup "${TINYTAG_WARMUP}" \
    --debug "${TINYTAG_DEBUG}" \
    ${decode_args} \
    "$@"
