#!/bin/bash
# Explicit static PIE link: the SDK's -static-pie specs do not do this correctly.
set -euo pipefail
TASK=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK=$(cd "$TASK/../.." && pwd)
OUT="$TASK/build"
CROSS="$SDK/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-"
NAME=$1
SOURCE=$2
ABI=lp64
CV_OUT="$OUT/opencv"
R="$OUT/runtime/lib"
EXTRA=()
FLAGS=(-march=rv64imac -mabi=lp64)
case "$NAME" in
    runtime_hardfloat_probe|opencv_hardfloat_probe)
        ABI=lp64d; FLAGS=(-march=rv64imafdc -mabi=lp64d)
        CV_OUT="$OUT/opencv-hardfloat"; R="$OUT/runtime-hardfloat/lib"
        EXTRA=(-DDUOS_HARDFLOAT)
        ;;
esac
if [ "$NAME" = fp_probe ]; then FLAGS=(-march=rv64imafdc -mabi=lp64); fi
"${CROSS}g++" "${FLAGS[@]}" "${EXTRA[@]}" -O2 -fPIE -Wall -Wextra -Werror \
    -I"$OUT/opencv-source/modules/core/include" \
    -I"$OUT/opencv-source/modules/imgproc/include" -I"$CV_OUT" \
    -c "$SOURCE" -o "$OUT/$NAME.o"
LIBS=()
if [[ "$NAME" = opencv*probe ]]; then
    LIBS=("$CV_OUT/lib/libopencv_imgproc.a" "$CV_OUT/lib/libopencv_core.a")
fi
"${CROSS}g++" "${FLAGS[@]}" -nostdlib \
    -Wl,-static,-pie,-Bsymbolic,--exclude-libs,ALL,--no-dynamic-linker,-z,stack-size=524288,--gc-sections \
    "$R/rcrt1.o" "$R/crti.o" \
    "$("${CROSS}gcc" "${FLAGS[@]}" -print-file-name=crtbeginS.o)" \
    "$OUT/$NAME.o" -Wl,--start-group "${LIBS[@]}" \
    "$("${CROSS}gcc" "${FLAGS[@]}" -print-file-name=libstdc++.a)" \
    "$("${CROSS}gcc" "${FLAGS[@]}" -print-file-name=libatomic.a)" \
    "$R/libc.a" -lgcc -lgcc_eh -Wl,--end-group \
    "$("${CROSS}gcc" "${FLAGS[@]}" -print-file-name=crtendS.o)" "$R/crtn.o" \
    -o "$OUT/$NAME"
python3 "$TASK/runtime/normalize_static_pie.py" "$OUT/$NAME"
"${CROSS}readelf" -h -l -d -r "$OUT/$NAME" > "$OUT/$NAME-elf.txt"
python3 "$TASK/runtime/verify_elf.py" "$OUT/$NAME" "$ABI"
