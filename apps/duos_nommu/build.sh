#!/bin/bash
# Run in duodocker. Does not replace any SDK source, board configuration or boot file.
set -euo pipefail
TASK_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(cd "${TASK_DIR}/../.." && pwd)"
OUT="${TASK_DIR}/build"
KERNEL="${OUT}/linux-6.18"
CROSS="${SDK_DIR}/host-tools/gcc/riscv64-linux-x86_64/bin/riscv64-unknown-linux-gnu-"
mkdir -p "${OUT}"
if [ "${NOMMU_SHELL:-0}" = 1 ]; then export NOMMU_USERSPACE=1; fi
if [ ! -f "${OUT}/linux-6.18.tar.xz" ]; then
    curl --fail --location https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.tar.xz -o "${OUT}/linux-6.18.tar.xz"
fi
if [ -f "${KERNEL}/.duos-nommu-patched" ]; then
    # Only the disposable, downloaded experiment source may be removed.
    rm -rf "${KERNEL}"
fi
if [ ! -d "${KERNEL}" ]; then tar -C "${OUT}" -xf "${OUT}/linux-6.18.tar.xz"; fi
echo "9106a4605da9e31ff17659d958782b815f9591ab308d03b0ee21aad6c7dced4b  ${OUT}/linux-6.18.tar.xz" | sha256sum -c -
python3 "${TASK_DIR}/patch_kernel.py" "${KERNEL}"
FRAGMENT="${TASK_DIR}/config.fragment"
if [ "${NOMMU_USERSPACE:-0}" = 1 ]; then
    mkdir -p "${OUT}/userspace"
    EXTRA_FLAGS=()
    if [ "${NOMMU_SHELL:-0}" = 1 ]; then EXTRA_FLAGS=(-DDUOS_SHELL); fi
    "${CROSS}gcc" -Os -Wall -Wextra -Werror -march=rv64imac -mabi=lp64 \
        -fPIE -fno-builtin -fno-stack-protector -msmall-data-limit=0 -nostdlib -pie \
        -Wl,--no-relax,--no-dynamic-linker,-e,_start,-z,stack-size=16384,--build-id=none \
        "${EXTRA_FLAGS[@]}" "${TASK_DIR}/userspace/init.c" -o "${OUT}/userspace/init"
    "${CROSS}readelf" -h -l -r "${OUT}/userspace/init" > "${OUT}/userspace/init-elf.txt"
    python3 - "${OUT}/userspace/init" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
assert data[:5] == b'\x7fELF\x02'
assert struct.unpack_from('<HH', data, 16) == (3, 243), 'must be RV64 ET_DYN'
phoff = struct.unpack_from('<Q', data, 32)[0]
phsize, phnum = struct.unpack_from('<HH', data, 54)
types = [struct.unpack_from('<I', data, phoff+i*phsize)[0] for i in range(phnum)]
assert 3 not in types, 'must not require PT_INTERP'
PY
    grep -q 'There are no relocations' "${OUT}/userspace/init-elf.txt"
    cat > "${OUT}/userspace/initramfs.list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/kmsg 0600 0 0 c 1 11
file /init ${OUT}/userspace/init 0755 0 0
EOF
    if [ "${NOMMU_SHELL:-0}" = 1 ]; then
        echo 'nod /dev/duos-console 0600 0 0 c 10 250' >> "${OUT}/userspace/initramfs.list"
    fi
    # Replace explicit disabled settings rather than append conflicting ones.
    sed '/CONFIG_BLK_DEV_INITRD/d; /CONFIG_BINFMT_ELF_FDPIC/d' "$FRAGMENT" > "${OUT}/userspace/config.fragment"
    cat >> "${OUT}/userspace/config.fragment" <<EOF
CONFIG_DUOS_NOMMU_USER_PROBE=y
CONFIG_BLK_DEV_INITRD=y
CONFIG_BINFMT_ELF_FDPIC=y
CONFIG_INITRAMFS_SOURCE="${OUT}/userspace/initramfs.list"
CONFIG_INITRAMFS_COMPRESSION_NONE=y
EOF
    if [ "${NOMMU_SHELL:-0}" = 1 ]; then
        echo 'CONFIG_DUOS_NOMMU_SHELL=y' >> "${OUT}/userspace/config.fragment"
    fi
    FRAGMENT="${OUT}/userspace/config.fragment"
fi
make -C "${KERNEL}" O="${OUT}/kernel" ARCH=riscv CROSS_COMPILE="${CROSS}" \
    KCONFIG_ALLCONFIG="${FRAGMENT}" allnoconfig
make -C "${KERNEL}" O="${OUT}/kernel" ARCH=riscv CROSS_COMPILE="${CROSS}" \
    -j"${NOMMU_JOBS:-4}" Image
"${OUT}/kernel/scripts/dtc/dtc" -I dts -O dtb -o "${OUT}/little.dtb" "${TASK_DIR}/little.dts"
"${CROSS}size" "${OUT}/kernel/vmlinux" > "${OUT}/kernel-size.txt"
"${CROSS}readelf" -h "${OUT}/kernel/vmlinux" > "${OUT}/kernel-elf-header.txt"
# Image size includes BSS; compare against the allocator RAM and verify actual config.
python3 - "${OUT}" <<'PY'
from pathlib import Path
import struct, sys
out = Path(sys.argv[1]); image = (out / 'kernel/arch/riscv/boot/Image').read_bytes()
offset, size = struct.unpack_from('<QQ', image, 8)
assert offset == 0 and 0 < size < 0x01de0000, (offset, size)
config = (out / 'kernel/.config').read_text()
for line in ['# CONFIG_MMU is not set', '# CONFIG_SMP is not set', 'CONFIG_RISCV_M_MODE=y',
             'CONFIG_DUOS_NOMMU_PROBE=y', 'CONFIG_PHYS_RAM_BASE=0x9e200000']:
    assert line in config, line
import os
mode = 'userspace' if os.environ.get('NOMMU_USERSPACE') == '1' else 'kernel-only'
if mode == 'userspace':
    for line in ['CONFIG_DUOS_NOMMU_USER_PROBE=y', 'CONFIG_BINFMT_ELF_FDPIC=y',
                 'CONFIG_BLK_DEV_INITRD=y']:
        assert line in config, line
print(f'{mode} probe built: file {len(image)} bytes, including BSS {size} bytes')
PY
