#!/bin/sh
# Pure ArUco Nano live-detector launcher. Installed as
# /app/aruco_nano/run_aruco_nano.sh. Extra arguments are passed after the
# defaults, so a caller can override an option by supplying it later.
set -eu

BIN="${ARUCO_NANO_BIN:-/app/aruco_nano/aruco_nano}"
MODE="${ARUCO_NANO_MODE:-strict}"
MIRROR="${ARUCO_NANO_MIRROR:-1}"
FLIP="${ARUCO_NANO_FLIP:-0}"
DEBUG="${ARUCO_NANO_DEBUG:-1}"
TAG_OUTPUT="${ARUCO_NANO_TAG_OUTPUT:-0}"

[ -x "${BIN}" ] || { echo "Error: ${BIN} not found or not executable" >&2; exit 1; }

# Non-interactive SSH and init shells do not source /etc/profile.
case ":${LD_LIBRARY_PATH:-}:" in
    *:/mnt/system/lib:*) ;;
    *) LD_LIBRARY_PATH="/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:${LD_LIBRARY_PATH:-}"
       export LD_LIBRARY_PATH ;;
esac

# Mirror defaults to 1: the OV5647 module on this board delivers a horizontally
# mirrored frame, and AprilTag markers are chiral so a mirrored frame decodes
# zero tags. Set ARUCO_NANO_MIRROR=0 to see the uncorrected image.
exec "${BIN}" \
    --mode "${MODE}" \
    --mirror "${MIRROR}" \
    --flip "${FLIP}" \
    --debug "${DEBUG}" \
    --tag-output "${TAG_OUTPUT}" \
    "$@"
