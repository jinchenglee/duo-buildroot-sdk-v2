#!/bin/bash
# Source tpu-mlir's environment, then hand off to the requested command.
# envsetup.sh must be sourced (not executed) -- it only exports variables.
set -e

# envsetup.sh writes into $HOME; when the container runs as the host UID (which
# tpu_docker.sh does, so output files are not root-owned) that may not exist.
export HOME="${HOME:-/tmp}"
[ -d "$HOME" ] || export HOME=/tmp

# shellcheck disable=SC1091
source /opt/tpu-mlir/envsetup.sh

exec "$@"
