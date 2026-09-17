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
    -DMIDDLEWARE_SDK_ROOT="${TOP_DIR}/cvi_mpi" \
    -DTDL_SDK_INCLUDE_PATH="${TOP_DIR}/tdl_sdk/include" \
    -DCVI_RTSP_ROOT="${TOP_DIR}/cvi_rtsp" \
    -DCMAKE_INSTALL_PREFIX="${OVERLAY_DIR}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"
cmake --install "${BUILD_DIR}"

# --- stage the launcher and the model -------------------------------------
# Everything lives under one directory on the board, /app/tinytag_detect/, so
# nothing else scatters across /usr/local, /mnt/cvimodel and /mnt/data. A
# symlink keeps `run_tinytag.sh` on PATH for the documented one-liner usage.
install -Dm755 "${APP_DIR}/run.sh" "${OVERLAY_DIR}/app/tinytag_detect/run_tinytag.sh"
install -Dm755 "${APP_DIR}/run_live.sh" "${OVERLAY_DIR}/app/tinytag_detect/run_live.sh"
install -d "${OVERLAY_DIR}/usr/local/bin"
ln -sfn /app/tinytag_detect/run_tinytag.sh "${OVERLAY_DIR}/usr/local/bin/run_tinytag.sh"

# Sample frames ship with the app, so a freshly flashed board can run the
# detector immediately. See samples/README.md for what each one exercises.
if [ -d "${APP_DIR}/samples" ]; then
    SAMPLES_DST="${OVERLAY_DIR}/app/tinytag_detect/samples"
    install -d "${SAMPLES_DST}"
    rm -f "${SAMPLES_DST}"/*
    sample_count=0
    for sample in "${APP_DIR}"/samples/*; do
        case "${sample}" in *.md) continue ;; esac
        [ -f "${sample}" ] || continue
        install -m 0644 "${sample}" "${SAMPLES_DST}/"
        sample_count=$((sample_count + 1))
    done
    info "Staged ${sample_count} sample image(s) -> /app/tinytag_detect/samples/"
fi

# cmake --install and install(1) create parent directories using the caller's
# umask, which in this repo is 0007 -- that would put mode 0770 directories into
# the image. Force the conventional 0755 so the overlay does not depend on the
# destination happening to exist already in the rootfs skeleton.
find "${OVERLAY_DIR}" -type d -exec chmod 0755 {} +
chmod 0700 "${OVERLAY_DIR}/root/.ssh" 2>/dev/null || true

# A hardware-validated cvimodel is committed to the overlay, so a fresh clone
# already has one. Only stage over it when a freshly built model is present in
# the toolchain work directory (or TINYTAG_CVIMODEL points somewhere).
CVIMODEL_DST="${OVERLAY_DIR}/app/tinytag_detect/cvimodel"
CVIMODEL_SRC="${TINYTAG_CVIMODEL:-${TOP_DIR}/tools/tinytag_cvimodel/work/tinytag-v40c.int8.cvimodel}"
if [ -f "${CVIMODEL_SRC}" ]; then
    install -Dm644 "${CVIMODEL_SRC}" "${CVIMODEL_DST}/$(basename "${CVIMODEL_SRC}")"
    info "Staged model: $(basename "${CVIMODEL_SRC}")"
elif ls "${CVIMODEL_DST}"/*.cvimodel >/dev/null 2>&1; then
    for staged in "${CVIMODEL_DST}"/*.cvimodel; do
        info "Model already in overlay (tracked in git): $(basename "${staged}")"
    done
else
    printf "\e[1;33mWarning: no cvimodel found, and none staged in the overlay.\e[0m\n"
    printf "\e[1;33m  Build one with tools/tinytag_cvimodel, or set TINYTAG_CVIMODEL.\e[0m\n"
    printf "\e[1;33m  run_tinytag.sh will fail without it.\e[0m\n"
fi

# The golden bundle is optional and deliberately not tracked (3.2 MB); it only
# enables --selftest. Detection works without it.
GOLDEN_SRC="${TINYTAG_GOLDEN:-${TOP_DIR}/tools/tinytag_cvimodel/work/tinytag-v40c.ttgold}"
if [ -f "${GOLDEN_SRC}" ]; then
    install -Dm644 "${GOLDEN_SRC}" "${CVIMODEL_DST}/$(basename "${GOLDEN_SRC}")"
    info "Staged golden bundle: $(basename "${GOLDEN_SRC}") ($(du -h "${GOLDEN_SRC}" | cut -f1))"
elif ls "${CVIMODEL_DST}"/*.ttgold >/dev/null 2>&1; then
    info "Golden bundle already in overlay"
else
    info "No golden bundle (optional) -- --selftest unavailable; detection works."
    info "  Regenerate with tools/tinytag_cvimodel/make_golden.py if wanted."
fi

info ""
info "Installed into ${OVERLAY_DIR}:"
find "${OVERLAY_DIR}" -type f -printf '  %P\n' | sort
info ""
info "Now rebuild the image to pick it up:"
info "  ./build.sh ${BOARD}"
