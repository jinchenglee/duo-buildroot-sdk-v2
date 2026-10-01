#!/bin/bash
# Run inside duodocker after selecting/building Duo S ARM64 SD. This creates
# a separate diagnostic FIP; it does not rebuild or replace vendor firmware.
set -euo pipefail
APP_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TOP_DIR="$(cd "${APP_DIR}/../.." && pwd)"
TASK_OUT="${APP_DIR}/build_bare_metal_threshold"
PROJECT_FULLNAME=sg2000_milkv_duos_glibc_arm64_sd
BASE_FIP="${TINYTAG_THRESHOLD_BASE_FIP:-${TOP_DIR}/install/soc_${PROJECT_FULLNAME}/fip.bin}"
MAP_DIR="${TOP_DIR}/build/output/${PROJECT_FULLNAME}"
grep -q '^CONFIG_BOARD="milkv_duos_glibc_arm64_sd"$' "${TOP_DIR}/build/.config"
grep -q '^CONFIG_ARCH="arm64"$' "${TOP_DIR}/build/.config"
[ -f "${BASE_FIP}" ]
mkdir -p "${TASK_OUT}/bundle"
export PATH="${TOP_DIR}/host-tools/gcc/riscv64-elf-x86_64/bin:${PATH}"
# Retain vendor cache/prefetch startup and hart selection. Disable interrupts
# before any initialization; park on an unexpected synchronous exception.
python3 - "${TOP_DIR}" "${TASK_OUT}" <<'PY_START'
from pathlib import Path
import sys
source=Path(sys.argv[1]+'/freertos/cvitek/arch/riscv64/src/start.S').read_text()
assert source.count('li x3, 0x880') == 1
source=source.replace('li x3, 0x880','li x3, 0')
source=source.replace('// Continue primary hart','csrc mstatus, 8\n\tcsrw mie, zero\n\tla t0, bare_trap\n\tcsrw mtvec, t0\n\t// Continue primary hart')
source+='\n.balign 4\nbare_trap:\n\tcsrw mie, zero\n\tcsrc mstatus, 8\n1:\n\twfi\n\tj 1b\n'
Path(sys.argv[2]+'/start.S').write_text(source)
PY_START
ARCH_DIR="${TOP_DIR}/freertos/cvitek/arch/riscv64"
FLAGS=(-O2 -std=gnu11 -march=rv64imafdc -mabi=lp64d -mcmodel=medany
       -fno-builtin -ffunction-sections -fdata-sections -nostdlib -g -Wall -Wextra
       -I"${ARCH_DIR}/include" -I"${MAP_DIR}" -I"${TOP_DIR}/apps/common")
riscv64-unknown-elf-gcc "${FLAGS[@]}" -c "${TASK_OUT}/start.S" -o "${TASK_OUT}/start.o"
riscv64-unknown-elf-gcc "${FLAGS[@]}" -c "${APP_DIR}/bare_metal/main.c" -o "${TASK_OUT}/main.o"
riscv64-unknown-elf-gcc "${FLAGS[@]}" -c "${ARCH_DIR}/src/cache.c" -o "${TASK_OUT}/cache.o"
riscv64-unknown-elf-gcc "${FLAGS[@]}" -L"${MAP_DIR}" -T"${APP_DIR}/bare_metal/link.ld" \
    -Wl,--gc-sections,-Map,"${TASK_OUT}/bare-metal.map" \
    "${TASK_OUT}/start.o" "${TASK_OUT}/main.o" "${TASK_OUT}/cache.o" -lgcc -o "${TASK_OUT}/bare-metal.elf"
riscv64-unknown-elf-objcopy -O binary "${TASK_OUT}/bare-metal.elf" "${TASK_OUT}/bare-metal.bin"
riscv64-unknown-elf-objdump -d "${TASK_OUT}/bare-metal.elf" >"${TASK_OUT}/bare-metal.disasm"
riscv64-unknown-elf-size "${TASK_OUT}/bare-metal.elf" | tee "${TASK_OUT}/firmware-size.txt"
# Check generated reservation and ELF entry before repacking.
python3 - "${MAP_DIR}/cvi_board_memmap.h" "${TASK_OUT}/bare-metal.elf" <<'PY_ENTRY'
import re, subprocess, sys
m=re.search(r'^#define CVIMMAP_FREERTOS_ADDR\s+(0x[0-9a-fA-F]+)',open(sys.argv[1]).read(),re.M)
assert m
header=subprocess.check_output(['riscv64-unknown-elf-readelf','-h',sys.argv[2]],text=True)
entry=int(re.search(r'Entry point address:\s*(0x[0-9a-fA-F]+)',header).group(1),16)
assert entry==int(m.group(1),16), (entry,m.group(1))
print('PASS: ELF entry equals generated firmware reservation')
PY_ENTRY
cmake -S "${APP_DIR}" -B "${TASK_OUT}/app" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${APP_DIR}/cmake/toolchain-aarch64-linux.cmake" \
    -DTPU_SDK_PATH="${TOP_DIR}/install/soc_${PROJECT_FULLNAME}/tpu_64bit/cvitek_tpu_sdk" \
    -DMIDDLEWARE_SDK_ROOT="${TOP_DIR}/cvi_mpi" \
    -DTDL_SDK_INCLUDE_PATH="${TOP_DIR}/tdl_sdk/include" \
    -DCVI_RTSP_ROOT="${TOP_DIR}/cvi_rtsp" >"${TASK_OUT}/configure.log" 2>&1
cmake --build "${TASK_OUT}/app" --target tinytag_threshold_bench -j4 >"${TASK_OUT}/app-build.log" 2>&1
python3 "${TOP_DIR}/fsbl/plat/cv181x/fiptool.py" genfip \
    --OLD_FIP "${BASE_FIP}" --BLCP_2ND "${TASK_OUT}/bare-metal.bin" \
    "${TASK_OUT}/bundle/fip-bare-metal.bin" >"${TASK_OUT}/fip-build.log" 2>&1
python3 - "${TOP_DIR}" "${BASE_FIP}" "${TASK_OUT}/bundle/fip-bare-metal.bin" <<'PY'
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
import re, subprocess
elf = subprocess.check_output(['riscv64-unknown-elf-readelf', '-h',
    sys.argv[1] + '/apps/tinytag_detect/build_bare_metal_threshold/bare-metal.elf'], text=True)
elf_entry = int(re.search(r'Entry point address:\s*(0x[0-9a-fA-F]+)', elf).group(1), 16)
assert int.from_bytes(entry_address, 'little') == elf_entry, 'base FIP entry differs from bare-metal ELF'
new = module.FIP()
new.read_fip(sys.argv[3])
for (section, name), content in before.items():
    if name != 'BLCP_2ND':
        assert content == getattr(new, section)[name].content, (section, name)
assert entry_address == new.param2['BLCP_2ND_RUNADDR'].content
with open(sys.argv[1] + '/apps/tinytag_detect/build_bare_metal_threshold/bare-metal.bin', 'rb') as f:
    firmware = f.read()
payload = new.body2['BLCP_2ND'].content
assert payload[:len(firmware)] == firmware and not any(payload[len(firmware):])
print('PASS: FIP payload comparison; only BLCP_2ND replaced, entry retained')
PY

cp "${BASE_FIP}" "${TASK_OUT}/bundle/fip-restore.bin"
cp "${TASK_OUT}/app/tinytag_threshold_bench" "${TASK_OUT}/bundle/"
cp "${APP_DIR}/run_threshold_bench.sh" "${TASK_OUT}/bundle/"
cp "${TASK_OUT}/bare-metal.bin" "${TASK_OUT}/bundle/"
cp "${APP_DIR}/bare_metal/README.md" "${TASK_OUT}/bundle/README.md"
(cd "${TASK_OUT}/bundle" && sha256sum fip-bare-metal.bin fip-restore.bin bare-metal.bin tinytag_threshold_bench run_threshold_bench.sh > SHA256SUMS)
tar -C "${TASK_OUT}" -czf "${TASK_OUT}/tinytag-bare-metal-threshold.tar.gz" bundle
echo "Ready: ${TASK_OUT}/bundle (not deployed). Install FIP and reboot before testing."
