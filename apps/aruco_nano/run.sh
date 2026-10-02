#!/bin/sh
# Pure ArUco Nano live-detector launcher. Installed as
# /app/aruco_nano/run_aruco_nano.sh. Extra arguments are passed after the
# defaults, so a caller can override an option by supplying it later.
set -eu

BIN="${ARUCO_NANO_BIN:-/app/aruco_nano/aruco_nano}"
MODE="${ARUCO_NANO_MODE:-strict}"
# OV5647 is horizontally mirrored on this board; OV9281 is not. Let an
# explicit environment override win, otherwise follow the selected sensor.
DEFAULT_MIRROR=1
if [ -r /mnt/data/sensor_cfg.ini ] &&
   grep -Eq '^name[[:space:]]*=[[:space:]]*OV_OV9281_' /mnt/data/sensor_cfg.ini; then
    DEFAULT_MIRROR=0
fi
MIRROR="${ARUCO_NANO_MIRROR:-${DEFAULT_MIRROR}}"
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

# Mirror follows the selected sensor by default: OV5647 is horizontally
# mirrored; OV9281 is not. Set ARUCO_NANO_MIRROR=0|1 to override.
exec "${BIN}" \
    --mode "${MODE}" \
    --mirror "${MIRROR}" \
    --flip "${FLIP}" \
    --debug "${DEBUG}" \
    --tag-output "${TAG_OUTPUT}" \
    "$@"
