#!/bin/bash
# Run inside duodocker. Build a firmware/app test bundle without rebuilding an
# SD image or modifying the normal FIP output. Existing SDK outputs required.
set -euo pipefail
APP_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TOP_DIR="$(cd "${APP_DIR}/../.." && pwd)"
TASK_OUT="${APP_DIR}/build_threshold_experiment"
PROJECT_FULLNAME=sg2000_milkv_duos_glibc_arm64_sd
export PROJECT_FULLNAME
BASE_FIP="${TINYTAG_THRESHOLD_BASE_FIP:-${TOP_DIR}/install/soc_${PROJECT_FULLNAME}/fip.bin}"
grep -q '^CONFIG_BOARD="milkv_duos_glibc_arm64_sd"$' "${TOP_DIR}/build/.config" || {
    echo "Select milkv-duos-glibc-arm64-sd first; this experiment uses its memory map." >&2; exit 1;
}
[ -f "${BASE_FIP}" ] || { echo "Missing base FIP: ${BASE_FIP}" >&2; exit 1; }
mkdir -p "${TASK_OUT}/original" "${TASK_OUT}/bundle"
if [ ! -f "${TASK_OUT}/original/fip.bin" ]; then
    cp "${BASE_FIP}" "${TASK_OUT}/original/fip.bin"
    cp "${TOP_DIR}/freertos/cvitek/install/bin/cvirtos.bin" "${TASK_OUT}/original/cvirtos.bin"
    cp "${TOP_DIR}/freertos/cvitek/install/bin/cvirtos.elf" "${TASK_OUT}/original/cvirtos.elf"
fi

export BUILD_PATH="${TOP_DIR}/build" DDR_64MB_SIZE=n
export PATH="${TOP_DIR}/host-tools/gcc/riscv64-elf-x86_64/bin:${PATH}"
(
    cd "${TOP_DIR}/freertos/cvitek"
    ./build_cv181x.sh
) >"${TASK_OUT}/freertos-build.log" 2>&1 || {
    tail -40 "${TASK_OUT}/freertos-build.log"; exit 1;
}
cmake -S "${APP_DIR}" -B "${TASK_OUT}/app" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${APP_DIR}/cmake/toolchain-aarch64-linux.cmake" \
    -DTPU_SDK_PATH="${TOP_DIR}/install/soc_${PROJECT_FULLNAME}/tpu_64bit/cvitek_tpu_sdk" \
    -DMIDDLEWARE_SDK_ROOT="${TOP_DIR}/cvi_mpi" \
    -DTDL_SDK_INCLUDE_PATH="${TOP_DIR}/tdl_sdk/include" \
    -DCVI_RTSP_ROOT="${TOP_DIR}/cvi_rtsp" >"${TASK_OUT}/configure.log" 2>&1
cmake --build "${TASK_OUT}/app" --target tinytag_threshold_bench tinytag_detect_live -j4 \
    >"${TASK_OUT}/app-build.log" 2>&1 || { tail -40 "${TASK_OUT}/app-build.log"; exit 1; }
cc -O2 -Wall -Wextra -Werror -fsanitize=undefined \
    "${APP_DIR}/tests/roi_threshold_test.c" -o "${TASK_OUT}/roi_threshold_test"
"${TASK_OUT}/roi_threshold_test" | tee "${TASK_OUT}/portable-test.log"

python3 "${TOP_DIR}/fsbl/plat/cv181x/fiptool.py" genfip \
    --OLD_FIP "${BASE_FIP}" \
    --BLCP_2ND "${TOP_DIR}/freertos/cvitek/install/bin/cvirtos.bin" \
    "${TASK_OUT}/bundle/fip-threshold.bin" >"${TASK_OUT}/fip-build.log" 2>&1
# Verify all other firmware payloads and little-core entry address are retained.
python3 - "${TOP_DIR}" "${BASE_FIP}" "${TASK_OUT}/bundle/fip-threshold.bin" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('fiptool', sys.argv[1] + '/fsbl/plat/cv181x/fiptool.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
old = module.FIP()
old.read_fip(sys.argv[2])
# fiptool uses mutable class attributes: snapshot before reading the second FIP.
before = {(section, name): bytes(entry.content)
          for section in ('body1', 'body2')
          for name, entry in getattr(old, section).items()}
entry_address = bytes(old.param2['BLCP_2ND_RUNADDR'].content)
new = module.FIP()
new.read_fip(sys.argv[3])
for (section, name), content in before.items():
    if name != 'BLCP_2ND':
        assert content == getattr(new, section)[name].content, (section, name)
assert entry_address == new.param2['BLCP_2ND_RUNADDR'].content
with open(sys.argv[1] + '/freertos/cvitek/install/bin/cvirtos.bin', 'rb') as f:
    firmware = f.read()
payload = new.body2['BLCP_2ND'].content
assert payload[:len(firmware)] == firmware and not any(payload[len(firmware):])
print('PASS: FIP payload comparison; only BLCP_2ND replaced, entry retained')
PY
cp "${TASK_OUT}/app/tinytag_threshold_bench" "${TASK_OUT}/app/tinytag_detect_live" "${TASK_OUT}/bundle/"
cp "${APP_DIR}/run_threshold_bench.sh" "${TASK_OUT}/bundle/"
cp "${TOP_DIR}/freertos/cvitek/install/bin/cvirtos.bin" "${TASK_OUT}/bundle/"
cp "${BASE_FIP}" "${TASK_OUT}/bundle/fip-original.bin"
cp "${TOP_DIR}/docs/duo-s-freertos-threshold-experiment.md" "${TASK_OUT}/bundle/README.md"
(
    cd "${TASK_OUT}/bundle"
    sha256sum fip-threshold.bin fip-original.bin cvirtos.bin tinytag_threshold_bench \
        tinytag_detect_live run_threshold_bench.sh > SHA256SUMS
)
riscv64-unknown-elf-size "${TASK_OUT}/original/cvirtos.elf" \
    "${TOP_DIR}/freertos/cvitek/install/bin/cvirtos.elf" | tee "${TASK_OUT}/firmware-size.txt"
tar -C "${TASK_OUT}" -czf "${TASK_OUT}/tinytag-threshold-experiment.tar.gz" bundle
echo "Ready: ${TASK_OUT}/bundle (not deployed). Firmware requires reboot."
