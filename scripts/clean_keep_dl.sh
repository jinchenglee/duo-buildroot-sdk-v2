#!/bin/bash
# Wipe every build output so the next ./build.sh starts from scratch, while
# keeping buildroot/dl (the untarred download cache) and host-tools (the
# toolchains).
#
# It removes exactly the git-ignored files, so tracked sources and uncommitted
# edits are never touched. Build outputs are root-owned (see "Always build in
# Docker" in README.md), so on the host this runs inside the build container.
#
# Usage: scripts/clean_keep_dl.sh       preview what would be removed (dry run)
#        scripts/clean_keep_dl.sh -f    actually remove it
#
# DUO_CONTAINER overrides the container name (default: duodocker).

set -e

case "$1" in
    "")  mode=-n ;;
    -f)  mode=-f ;;
    *)   echo "usage: $0 [-f]" >&2; exit 2 ;;
esac

CONTAINER="${DUO_CONTAINER:-duodocker}"
CLEAN="git -c safe.directory='*' clean ${mode}dX \
    -e '!buildroot/dl' -e '!buildroot/dl/**' \
    -e '!host-tools' -e '!host-tools/**'"

if [ "$(id -u)" = 0 ]; then
    # Already root (inside the container, or sudo on the host).
    cd "$(dirname "$0")/.."
    eval "$CLEAN"
else
    docker exec "$CONTAINER" /bin/bash -c "cd /home/work && $CLEAN"
fi

if [ "$mode" = -n ]; then
    echo "Dry run only. Re-run with -f to delete (copy any out/*.img you want to keep first)."
fi
