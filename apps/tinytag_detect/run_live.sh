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

# Sensor orientation, applied once in VI hardware (no per-frame cost).
# MIRROR defaults to 1: the OV5647 module on this board delivers a
# horizontally mirrored frame, and because AprilTag markers are chiral a
# mirrored frame decodes ZERO tags while proposals still look correct.
# Set TINYTAG_LIVE_MIRROR=0 to see the uncorrected image, e.g. on a module
# that does not need it.
MIRROR="${TINYTAG_LIVE_MIRROR:-1}"
FLIP="${TINYTAG_LIVE_FLIP:-0}"

# Widen each decode crop horizontally to a multiple of this many pixels, so
# every crop row starts 4-byte aligned and spans whole 32-bit words. Set 0 or 1
# to disable and compare. Aligned regions show pink on the preview.
CROP_ALIGN="${TINYTAG_LIVE_CROP_ALIGN:-4}"

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
    --debug "${DEBUG}" \
    --rtsp \
    "$@"
