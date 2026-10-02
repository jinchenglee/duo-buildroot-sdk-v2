#!/bin/bash
# Run inside duodocker after a complete production Duo S ARM64 SD build.
set -euo pipefail
TASK_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(cd "${TASK_DIR}/../.." && pwd)"
OUT="${TASK_DIR}/build"
mkdir -p "${OUT}"
if [ "${NOMMU_SHELL:-0}" = 1 ]; then export NOMMU_USERSPACE=1; fi
if [ -n "${NOMMU_CXX_PROBE:-}" ]; then export NOMMU_USERSPACE=1; fi
ARM="${SDK_DIR}/host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-"
ARM_KERNEL="${SDK_DIR}/host-tools/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-"
RV="${SDK_DIR}/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-"
grep -q '^CONFIG_BOARD="milkv_duos_glibc_arm64_sd"$' "${SDK_DIR}/build/.config"
grep -q '^CONFIG_ARCH="arm64"$' "${SDK_DIR}/build/.config"
bash "${TASK_DIR}/build.sh" > "${OUT}/kernel-build.log" 2>&1
python3 "${TASK_DIR}/prepare_boot.py" > "${OUT}/prepare-boot.log" 2>&1
make -C "${OUT}/uboot-source" O="${OUT}/uboot" ARCH=arm \
    CHIP=sg2000 CVIBOARD=milkv_duos_glibc_arm64_sd CROSS_COMPILE="${ARM}" \
    CONFIG_USE_DEFAULT_ENV=y CONFIG_SKIP_RAMDISK=y STORAGE_TYPE=sd MULTI_FIP=0 \
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
BUNDLE="${OUT}/bundle"
if [ "${NOMMU_USERSPACE:-0}" = 1 ]; then BUNDLE="${OUT}/bundle-userspace"; fi
if [ "${NOMMU_SHELL:-0}" = 1 ]; then BUNDLE="${OUT}/bundle-shell"; fi
if [ -n "${NOMMU_CXX_PROBE:-}" ]; then BUNDLE="$OUT/bundle-$NOMMU_CXX_PROBE"; fi
mkdir -p "${BUNDLE}"
"${ARM}gcc" -O2 -std=gnu11 -Wall -Wextra "${TASK_DIR}/read_log.c" -o "${BUNDLE}/read_nommu_log"
if [ "${NOMMU_SHELL:-0}" = 1 ]; then
    "${ARM}gcc" -O2 -std=gnu11 -Wall -Wextra -Werror "${TASK_DIR}/terminal.c" -o "${BUNDLE}/nommu_terminal"
    mkdir -p "${OUT}/reset-diag"
    cp "${TASK_DIR}/reset_diag/Makefile" "${TASK_DIR}/reset_diag/nommu_reset_diag.c" "${OUT}/reset-diag/"
    make -C "${SDK_DIR}/linux_5.10" O="${SDK_DIR}/linux_5.10/build/sg2000_milkv_duos_glibc_arm64_sd" \
        ARCH=arm64 CROSS_COMPILE="${ARM_KERNEL}" M="${OUT}/reset-diag" modules > "${OUT}/reset-diag-build.log" 2>&1
    cp "${OUT}/reset-diag/nommu_reset_diag.ko" "${BUNDLE}/"
fi
python3 "${TASK_DIR}/package.py"
