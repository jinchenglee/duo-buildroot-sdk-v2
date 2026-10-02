#!/bin/bash
# Fast portable regressions; OpenCV/libc/FP integration still requires the board.
set -euo pipefail
TASK=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK=$(cd "$TASK/../.." && pwd)
TEST_OUT=$(mktemp -d "${TMPDIR:-/tmp}/duos-threshold-tests.XXXXXX")
trap 'rm -rf -- "$TEST_OUT"' EXIT
for SHIFT in 16 23; do
    "${CC:-cc}" -O2 -Wall -Wextra -Werror -DTT_THRESHOLD_FIXED_SHIFT="$SHIFT" \
        "$TASK/runtime/rounding_test.c" -o "$TEST_OUT/rounding-$SHIFT"
    "$TEST_OUT/rounding-$SHIFT"
done
"${CC:-cc}" -O2 -Wall -Wextra -Werror \
    "$SDK/apps/tinytag_detect/tests/roi_threshold_test.c" -o "$TEST_OUT/portable"
"$TEST_OUT/portable"
