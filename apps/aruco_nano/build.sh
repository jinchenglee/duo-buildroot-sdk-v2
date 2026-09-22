#!/bin/bash
# Cross-compile the aruco_nano live detector and install it into the board
# overlay, so the next ./build.sh <board> bakes it into the SD image.
#
#   apps/aruco_nano/build.sh [board]
#
# With no argument the board currently selected by the SDK (device/target) is
# used. The overlay directory this installs into -- device/<board>/overlay -- is
# already honoured by build/Makefile's br-rootfs-prepare target, so no SDK file
# needs modifying.
#
# Unlike apps/tinytag_detect, this app has NO cvitek TPU dependency (no
# cviruntime/cvikernel/cvimodel). It still needs a full ./build.sh to have run
# once, but only because the TPU SDK it drops into
# install/soc_<project>/tpu_64bit/ supplies the OpenCV headers/libs that
# aruco_dictionary.cpp and the decoder are compiled/linked against. Point
# OPENCV_PATH elsewhere to build without it.
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

# --- locate the OpenCV headers/libs (from the TPU SDK build product) -------
TPU_SDK_PATH="${TOP_DIR}/install/soc_${PROJECT}/tpu_64bit/cvitek_tpu_sdk"
[ -d "${TPU_SDK_PATH}" ] || die "TPU SDK not found at ${TPU_SDK_PATH}. Run a full ./build.sh ${BOARD} first -- it supplies the OpenCV headers this links against."

# --- pick the matching toolchain file -------------------------------------
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
    -DMIDDLEWARE_SDK_ROOT="${TOP_DIR}/cvi_mpi" \
    -DCVI_RTSP_ROOT="${TOP_DIR}/cvi_rtsp" \
    -DTDL_SDK_INCLUDE_PATH="${TOP_DIR}/tdl_sdk/include" \
    -DCMAKE_INSTALL_PREFIX="${OVERLAY_DIR}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"
cmake --install "${BUILD_DIR}"

# --- stage the launcher ----------------------------------------------------
install -Dm755 "${APP_DIR}/run.sh" "${OVERLAY_DIR}/app/aruco_nano/run_aruco_nano.sh"
install -d "${OVERLAY_DIR}/usr/local/bin"
ln -sfn /app/aruco_nano/run_aruco_nano.sh "${OVERLAY_DIR}/usr/local/bin/run_aruco_nano.sh"

# cmake --install and install(1) create parent directories using the caller's
# umask, which in this repo is 0007 -- that would put mode 0770 directories into
# the image. Force the conventional 0755 so the overlay does not depend on the
# destination happening to exist already in the rootfs skeleton.
find "${OVERLAY_DIR}" -type d -exec chmod 0755 {} +
chmod 0700 "${OVERLAY_DIR}/root/.ssh" 2>/dev/null || true

info ""
info "Installed into ${OVERLAY_DIR}:"
find "${OVERLAY_DIR}" -type f -printf '  %P\n' | sort
info ""
info "Now rebuild the image to pick it up:"
info "  ./build.sh ${BOARD}"
