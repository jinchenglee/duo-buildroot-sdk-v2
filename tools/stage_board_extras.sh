#!/bin/bash
# Stage root's authorized_keys into device/<board>/overlay/ from
# device/<board>/ssh_keys/*.pub, so the next ./build.sh bakes them into the
# image. Application payloads are staged by their own build scripts -- see
# apps/tinytag_detect/build.sh.
#
#   tools/stage_board_extras.sh [board]
#
# With no argument the board currently selected in device/target is used.
#
# Adding another computer: drop its public key into device/<board>/ssh_keys/,
# re-run this, and rebuild the image. To add one to a *running* board without
# reflashing, just append the key to /root/.ssh/authorized_keys over ssh.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
TOP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

info() { printf "\e[1;32m%s\e[0m\n" "$1"; }
warn() { printf "\e[1;33m%s\e[0m\n" "$1"; }
die()  { printf "\e[1;31mError: %s\e[0m\n" "$1" >&2; exit 1; }

BOARD="${1:-}"
if [ -z "${BOARD}" ]; then
    [ -L "${TOP_DIR}/device/target" ] || die "no board given and device/target is not a symlink"
    BOARD="$(basename "$(readlink "${TOP_DIR}/device/target")")"
fi
[ -d "${TOP_DIR}/device/${BOARD}" ] || die "unknown board: ${BOARD}"

OVERLAY_DIR="${TOP_DIR}/device/${BOARD}/overlay"
KEYS_DIR="${TOP_DIR}/device/${BOARD}/ssh_keys"

info "Board: ${BOARD}"

# --- authorized_keys ------------------------------------------------------
# Modes matter: dropbear's checkpubkeyperms() rejects ~/.ssh or authorized_keys
# that are group- or other-writable. This repo's umask creates files 0660, so
# the modes below must be set explicitly rather than inherited.
shopt -s nullglob
keys=( "${KEYS_DIR}"/*.pub )
shopt -u nullglob

if [ ${#keys[@]} -gt 0 ]; then
    install -d -m 0700 "${OVERLAY_DIR}/root/.ssh"
    : > "${OVERLAY_DIR}/root/.ssh/authorized_keys"
    for key in "${keys[@]}"; do
        cat "${key}" >> "${OVERLAY_DIR}/root/.ssh/authorized_keys"
        info "  key: $(basename "${key}") ($(awk '{print $1, $NF}' "${key}"))"
    done
    chmod 0600 "${OVERLAY_DIR}/root/.ssh/authorized_keys"
    info "Staged ${#keys[@]} key(s) -> /root/.ssh/authorized_keys"
else
    warn "No *.pub in ${KEYS_DIR} -- with an empty root password, dropbear will"
    warn "reject every ssh login. Add a public key there before reflashing, or"
    warn "you will only be able to reach the board over the serial console."
fi

# Same umask concern as in apps/tinytag_detect/build.sh: normalize directory
# modes, then restore the two that must be tighter than 0755.
find "${OVERLAY_DIR}" -type d -exec chmod 0755 {} +
if [ -d "${OVERLAY_DIR}/root/.ssh" ]; then
    chmod 0700 "${OVERLAY_DIR}/root/.ssh"
    chmod 0600 "${OVERLAY_DIR}/root/.ssh/authorized_keys"
fi

info ""
info "Rebuild to apply:  ./build.sh ${BOARD}"
