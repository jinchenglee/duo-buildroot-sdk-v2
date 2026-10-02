#!/bin/bash
# Run inside duodocker after a complete production Duo S ARM64 SD build.
set -euo pipefail
TASK_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(cd "${TASK_DIR}/../.." && pwd)"
OUT="${TASK_DIR}/build"
mkdir -p "${OUT}"
ARM="${SDK_DIR}/host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-"
RV="${SDK_DIR}/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-"
grep -q '^CONFIG_BOARD="milkv_duos_glibc_arm64_sd"$' "${SDK_DIR}/build/.config"
grep -q '^CONFIG_ARCH="arm64"$' "${SDK_DIR}/build/.config"
bash "${TASK_DIR}/build.sh" > "${OUT}/kernel-build.log" 2>&1
python3 "${TASK_DIR}/prepare_boot.py" > "${OUT}/prepare-boot.log" 2>&1
make -C "${OUT}/uboot-source" O="${OUT}/uboot" ARCH=arm \
    CHIP=sg2000 CVIBOARD=milkv_duos_glibc_arm64_sd CROSS_COMPILE="${ARM}" \
    olddefconfig all -j"${NOMMU_JOBS:-4}" > "${OUT}/uboot-build.log" 2>&1
python3 "${TASK_DIR}/prepare_shim.py" "${SDK_DIR}" "${OUT}"
ARCH_DIR="${SDK_DIR}/freertos/cvitek/arch/riscv64"
FLAGS=(-O2 -std=gnu11 -march=rv64imafdc -mabi=lp64d -mcmodel=medany
       -fno-builtin -ffunction-sections -fdata-sections -nostdlib -g -Wall -Wextra
       -I"${ARCH_DIR}/include")
"${RV}gcc" "${FLAGS[@]}" -c "${OUT}/start.S" -o "${OUT}/start.o"
"${RV}gcc" "${FLAGS[@]}" -c "${TASK_DIR}/shim/main.c" -o "${OUT}/shim.o"
"${RV}gcc" "${FLAGS[@]}" -T"${TASK_DIR}/shim/link.ld" \
    -Wl,--gc-sections,-Map,"${OUT}/shim.map" "${OUT}/start.o" "${OUT}/shim.o" \
    -lgcc -o "${OUT}/shim.elf"
"${RV}objcopy" -O binary "${OUT}/shim.elf" "${OUT}/shim.bin"
"${RV}objdump" -d "${OUT}/shim.elf" > "${OUT}/shim.disasm"
mkdir -p "${OUT}/bundle"
"${ARM}gcc" -O2 -std=gnu11 -Wall -Wextra "${TASK_DIR}/read_log.c" -o "${OUT}/bundle/read_nommu_log"
python3 "${TASK_DIR}/package.py"
