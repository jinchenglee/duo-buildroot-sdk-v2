#!/bin/bash
# Cross-compile tinytag_detect and install it into the board overlay, so the
# next ./build.sh <board> bakes it into the SD image.
#
#   apps/tinytag_detect/build.sh [board]
#
# With no argument the board currently selected by the SDK (device/target) is
# used. The overlay directory this installs into -- device/<board>/overlay -- is
# already honoured by build/Makefile's br-rootfs-prepare target, so no SDK file
# needs modifying.
set -euo pipefail

APP_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
TOP_DIR="$(cd "${APP_DIR}/../.." && pwd)"

info() { printf "\e[1;32m%s\e[0m\n" "$1"; }
die()  { printf "\e[1;31mError: %s\e[0m\n" "$1" >&2; exit 1; }

# --- resolve the board ----------------------------------------------------
BOARD="${1:-}"
if [ -z "${BOARD}" ]; then
    [ -L "${TOP_DIR}/device/target" ] || die "no board given and device/target is not a symlink; run ./build.sh lunch first, or pass a board name"
    BOARD="$(basename "$(readlink "${TOP_DIR}/device/target")")"
fi
[ -d "${TOP_DIR}/device/${BOARD}" ] || die "unknown board: ${BOARD}"

# shellcheck source=/dev/null
source "${TOP_DIR}/device/${BOARD}/boardconfig.sh"
PROJECT="${MV_BOARD_LINK}"
info "Board   : ${BOARD}"
info "Project : ${PROJECT}"

# --- locate the TPU SDK that ./build.sh produced --------------------------
TPU_SDK_PATH="${TOP_DIR}/install/soc_${PROJECT}/tpu_64bit/cvitek_tpu_sdk"
[ -d "${TPU_SDK_PATH}" ] || die "TPU SDK not found at ${TPU_SDK_PATH}. Run a full ./build.sh ${BOARD} first -- the TPU SDK is a build product, not a checked-in artifact."

# --- pick the matching toolchain file -------------------------------------
# These live in this directory rather than being taken from the TPU SDK: the
# SDK's own cmake/*.cmake files are extracted mode 0640 root:root and are
# therefore unreadable to the user who normally runs this script.
case "${BOARD}" in
    *-glibc-arm64-*)  TOOLCHAIN="toolchain-aarch64-linux.cmake" ;;
    *-musl-riscv64-*) TOOLCHAIN="toolchain-riscv64-linux-musl.cmake" ;;
    *) die "cannot infer a toolchain for board ${BOARD}; add one under ${APP_DIR}/cmake and extend the case in $0" ;;
esac
TOOLCHAIN_FILE="${APP_DIR}/cmake/${TOOLCHAIN}"
[ -r "${TOOLCHAIN_FILE}" ] || die "toolchain file not readable: ${TOOLCHAIN_FILE}"
info "Toolchain: ${TOOLCHAIN}"

HOST_TOOLS_PATH="${TOP_DIR}/host-tools"
[ -d "${HOST_TOOLS_PATH}/gcc" ] || die "host-tools not found at ${HOST_TOOLS_PATH}; run ./build.sh once to fetch it"

# --- build ----------------------------------------------------------------
BUILD_DIR="${APP_DIR}/build_${BOARD}"
OVERLAY_DIR="${TOP_DIR}/device/${BOARD}/overlay"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cmake -S "${APP_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DHOST_TOOLS_PATH="${HOST_TOOLS_PATH}" \
    -DTPU_SDK_PATH="${TPU_SDK_PATH}" \
    -DOPENCV_PATH="${TPU_SDK_PATH}/opencv" \
    -DCMAKE_INSTALL_PREFIX="${OVERLAY_DIR}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"
cmake --install "${BUILD_DIR}"

# --- stage the launcher and the model -------------------------------------
install -Dm755 "${APP_DIR}/run.sh" "${OVERLAY_DIR}/usr/local/bin/run_tinytag.sh"

CVIMODEL_SRC="${TINYTAG_CVIMODEL:-${TOP_DIR}/tools/tinytag_cvimodel/work/tinytag-v40c.int8.cvimodel}"
if [ -f "${CVIMODEL_SRC}" ]; then
    install -Dm644 "${CVIMODEL_SRC}" "${OVERLAY_DIR}/mnt/cvimodel/$(basename "${CVIMODEL_SRC}")"
    info "Staged model: $(basename "${CVIMODEL_SRC}")"
else
    printf "\e[1;33mWarning: no cvimodel at %s -- build one with tools/tinytag_cvimodel, or set TINYTAG_CVIMODEL.\e[0m\n" "${CVIMODEL_SRC}"
fi

GOLDEN_SRC="${TINYTAG_GOLDEN:-${TOP_DIR}/tools/tinytag_cvimodel/work/tinytag-v40c.ttgold}"
if [ -f "${GOLDEN_SRC}" ]; then
    install -Dm644 "${GOLDEN_SRC}" "${OVERLAY_DIR}/mnt/cvimodel/$(basename "${GOLDEN_SRC}")"
    info "Staged golden bundle: $(basename "${GOLDEN_SRC}") ($(du -h "${GOLDEN_SRC}" | cut -f1))"
else
    printf "\e[1;33mWarning: no golden bundle at %s -- build one with tools/tinytag_cvimodel/make_golden.py to enable --selftest on the board.\e[0m\n" "${GOLDEN_SRC}"
fi

info ""
info "Installed into ${OVERLAY_DIR}:"
find "${OVERLAY_DIR}" -type f -printf '  %P\n' | sort
info ""
info "Now rebuild the image to pick it up:"
info "  ./build.sh ${BOARD}"
