#!/bin/bash
# Inside duodocker; source must be copied into the ignored build directory first.
set -euo pipefail
TASK=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK=$(cd "${TASK}/../.." && pwd)
OUT="${TASK}/build"
CROSS="${SDK}/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-"
MUSL_CROSS="${SDK}/host-tools/gcc/riscv64-linux-x86_64/bin/riscv64-unknown-linux-gnu-"
ABI=${NOMMU_FLOAT_ABI:-lp64}
case "$ABI" in
    lp64) ARCH=rv64imac; MUSL_SRC="$OUT/musl-1.2.5"; LIB_ROOT="$OUT/runtime"; CV_OUT="$OUT/opencv"; SUFFIX=probe ;;
    lp64d) ARCH=rv64imafdc; MUSL_SRC="$OUT/musl-hardfloat"; LIB_ROOT="$OUT/runtime-hardfloat"; CV_OUT="$OUT/opencv-hardfloat"; SUFFIX=hardfloat_probe ;;
    *) echo 'NOMMU_FLOAT_ABI must be lp64 or lp64d' >&2; exit 1 ;;
esac
mkdir -p "$OUT"
if [ ! -f "$OUT/musl-1.2.5.tar.gz" ]; then
    curl -fL https://musl.libc.org/releases/musl-1.2.5.tar.gz -o "$OUT/musl-1.2.5.tar.gz"
fi
echo "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4  $OUT/musl-1.2.5.tar.gz" | sha256sum -c -
if [ ! -d "$MUSL_SRC" ]; then
    mkdir -p "$MUSL_SRC"
    tar -C "$MUSL_SRC" --strip-components=1 -xf "$OUT/musl-1.2.5.tar.gz"
fi
(cd "$MUSL_SRC"
CC="${MUSL_CROSS}gcc" AR="${MUSL_CROSS}ar" RANLIB="${MUSL_CROSS}ranlib" \
    CFLAGS="-O2 -march=$ARCH -mabi=$ABI -fPIC" \
    ./configure --target=riscv64-linux-musl --prefix="$LIB_ROOT" --disable-shared
make -j"${NOMMU_JOBS:-4}" AR="${MUSL_CROSS}ar" RANLIB="${MUSL_CROSS}ranlib"
make install) > "$OUT/musl-$ABI-build.log" 2>&1
cmake -S "$OUT/opencv-source" -B "$CV_OUT" \
    -DCMAKE_TOOLCHAIN_FILE="$TASK/runtime/opencv-toolchain.cmake" \
    -DNOMMU_OPENCV_ABI="$ABI" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_LIST=core,imgproc \
    -DOPENCV_DISABLE_THREAD_SUPPORT=ON -DOPENCV_DISABLE_FILESYSTEM_SUPPORT=ON \
    -DOPENCV_DISABLE_ENV_SUPPORT=ON -DWITH_PTHREADS_PF=OFF -DWITH_OPENMP=OFF \
    -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF \
    -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF \
    -DENABLE_PRECOMPILED_HEADERS=OFF -DCV_ENABLE_INTRINSICS=OFF \
    -DCPU_BASELINE='' -DCPU_DISPATCH='' -DWITH_OPENCL=OFF \
    > "$OUT/opencv-$ABI-config.log" 2>&1
cmake --build "$CV_OUT" -j"${NOMMU_JOBS:-4}" > "$OUT/opencv-$ABI-build.log" 2>&1
bash "$TASK/runtime/link.sh" "runtime_$SUFFIX" "$TASK/runtime/runtime_probe.cc"
bash "$TASK/runtime/link.sh" "opencv_$SUFFIX" "$TASK/runtime/opencv_probe.cc"

if [ "$ABI" = lp64 ]; then
    bash "$TASK/runtime/link.sh" fp_probe "$TASK/runtime/fp_probe.cc"
fi
