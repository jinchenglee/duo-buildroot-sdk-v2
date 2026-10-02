#!/bin/sh
# Observe a running kernel; never reboot or modify boot files.
set -eu
reader=${1:-/mnt/data/duos-nommu-fixed/bundle/read_nommu_log}
out=${2:-/mnt/data/nommu-stability}
mkdir -p "$out"
previous=0
for n in 0 1 2 3 4 5 6; do
    "$reader" > "$out/snapshot-$n.txt" 2>&1
    head -n 1 "$out/snapshot-$n.txt"
    header=$(head -n 1 "$out/snapshot-$n.txt")
    if ! printf '%s\n' "$header" | grep -Eq '^stage=3 .* trap=0 pc=0 value=0$'; then
        echo "FAIL: unexpected stage or trap state" >&2; exit 1
    fi
    heartbeat=$(printf '%s\n' "$header" | sed 's/.*heartbeat=\([0-9]*\).*/\1/')
    if [ "$n" -gt 0 ] && [ "$heartbeat" -le "$previous" ]; then
        echo "FAIL: heartbeat stopped advancing" >&2; exit 1
    fi
    previous=$heartbeat
    if grep -q 'duos-user: FAIL\|allocation verification failed\|allocation=FAILED' "$out/snapshot-$n.txt"; then
        echo "FAIL: allocation or userspace syscall error in log" >&2; exit 1
    fi
    [ "$n" -eq 6 ] || sleep 10
done
echo "Snapshots saved to $out; check increasing heartbeat and zero traps."
