#!/bin/bash
# Run in duodocker. Does not replace any SDK source, board configuration or boot file.
set -euo pipefail
TASK_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(cd "${TASK_DIR}/../.." && pwd)"
OUT="${TASK_DIR}/build"
KERNEL="${OUT}/linux-6.18"
CROSS="${SDK_DIR}/host-tools/gcc/riscv64-linux-x86_64/bin/riscv64-unknown-linux-gnu-"
mkdir -p "${OUT}"
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
make -C "${KERNEL}" O="${OUT}/kernel" ARCH=riscv CROSS_COMPILE="${CROSS}" \
    KCONFIG_ALLCONFIG="${TASK_DIR}/config.fragment" allnoconfig
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
assert offset == 0 and 0 < size < 0x01df0000, (offset, size)
config = (out / 'kernel/.config').read_text()
for line in ['# CONFIG_MMU is not set', '# CONFIG_SMP is not set', 'CONFIG_RISCV_M_MODE=y',
             'CONFIG_DUOS_NOMMU_PROBE=y', 'CONFIG_PHYS_RAM_BASE=0x9e200000']:
    assert line in config, line
print(f'Kernel-only probe built: file {len(image)} bytes, including BSS {size} bytes')
PY
