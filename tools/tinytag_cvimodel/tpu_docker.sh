#!/bin/bash
# Build (once) and run the TinyTag cvimodel toolchain container.
#
#   ./tpu_docker.sh build                 # build the image
#   ./tpu_docker.sh run <cmd> [args...]   # run a command inside it
#   ./tpu_docker.sh shell                 # interactive shell
#
# The host directories that matter are bind-mounted:
#   this repo            -> /workspace   (scripts, output)
#   $TINYTAG_ASSETS      -> /assets      (external ONNX / images / video, read-only)
# TINYTAG_ASSETS defaults to ~/Downloads because the trained model and the raw
# footage deliberately live outside this repository.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
TOP_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

IMAGE="${TINYTAG_TPU_IMAGE:-tinytag-tpu-mlir:v1.3.228}"
ASSETS_DIR="${TINYTAG_ASSETS:-$HOME/Downloads}"

usage() {
  sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
  exit "${1:-0}"
}

cmd_build() {
  echo "Building ${IMAGE} ..."
  docker build -t "${IMAGE}" "${SCRIPT_DIR}"
  echo "Built ${IMAGE}"
}

have_image() {
  docker image inspect "${IMAGE}" >/dev/null 2>&1
}

docker_run() {
  have_image || { echo "Image ${IMAGE} not found; run '$0 build' first." >&2; exit 1; }
  [ -d "${ASSETS_DIR}" ] || { echo "Assets dir ${ASSETS_DIR} not found; set TINYTAG_ASSETS." >&2; exit 1; }

  local tty_flags=()
  [ -t 0 ] && tty_flags=(-it)

  # --user keeps generated files owned by the invoking user rather than root.
  docker run --rm "${tty_flags[@]}" \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "${TOP_DIR}:/workspace" \
    -v "${ASSETS_DIR}:/assets:ro" \
    -w /workspace \
    "${IMAGE}" "$@"
}

case "${1:-}" in
  build) cmd_build ;;
  run)   shift; [ $# -gt 0 ] || usage 1; docker_run "$@" ;;
  shell) docker_run bash ;;
  -h|--help|"") usage ;;
  *) echo "Unknown subcommand: $1" >&2; usage 1 ;;
esac
