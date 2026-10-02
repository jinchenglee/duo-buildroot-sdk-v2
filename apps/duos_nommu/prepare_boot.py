#!/usr/bin/env python3
"""Stage a separate U-Boot tree and main FIT; never edit SDK build artifacts."""
from pathlib import Path
import re, shutil, subprocess
root = Path(__file__).resolve().parents[2]
task = root / 'apps/duos_nommu'
out = task / 'build'
source = out / 'uboot-source'
normal = root / 'u-boot-2021.10'
if not source.exists():
    shutil.copytree(normal, source, symlinks=True,
                    ignore=lambda directory, names: [n for n in names if n in ('build', '.git')])
header = (root / 'build/output/sg2000_milkv_duos_glibc_arm64_sd/cvi_board_memmap.h').read_text()
# Keep the stock ISP/bitstream/framebuffer addresses intact: take the extra
# 30 MiB from the upper end of ION, not from ordinary A53 RAM. FSBL transfer
# fields therefore remain consistent with its unmodified BL2/DDR sections.
changes = {'FREERTOS_ADDR': 0x9e000000, 'FREERTOS_SIZE': 0x02000000,
           'FSBL_C906L_START_ADDR': 0x9e000000, 'KERNEL_MEMORY_SIZE': 0x1e000000,
           'ION_SIZE': 0x08c00000}
for name, value in changes.items():
    header, count = re.subn(r'(^#define CVIMMAP_' + name + r')\s+0x[0-9a-fA-F]+[^\n]*',
                           rf'\1 0x{value:x} /* isolated NOMMU probe */', header, flags=re.M)
    assert count == 1, name
memory = source / 'include/cvi_board_memmap.h'
if memory.is_symlink(): memory.unlink()
memory.write_text(header)
(out / 'cvi_board_memmap.h').write_text(header)
ubout = out / 'uboot'
ubout.mkdir(exist_ok=True)
shutil.copyfile(normal / 'build/sg2000_milkv_duos_glibc_arm64_sd/.config', ubout / '.config')
# Extract the exact current FIT's kernel/ramdisk/DT, preserving its kernel and rootfs.
fit = root / 'install/soc_sg2000_milkv_duos_glibc_arm64_sd/rawimages/boot.sd'
fitout = out / 'main-fit'
fitout.mkdir(exist_ok=True)
dump = normal / 'build/sg2000_milkv_duos_glibc_arm64_sd/tools/dumpimage'
listing = subprocess.check_output([str(dump), '-l', str(fit)], text=True)
(fitout / 'original-fit-list.txt').write_text(listing)
assert 'Image 0 (kernel-1)' in listing and 'Compression:  lzma compressed' in listing, listing
assert 'Image 1 (fdt-sg2000_milkv_duos_glibc_arm64_sd)' in listing and 'ramdisk-1' not in listing, listing
for i, name in enumerate(['Image.lzma', 'main-original.dtb']):
    subprocess.run([str(dump), '-T', 'flat_dt', '-p', str(i), '-o', str(fitout / name), str(fit)], check=True)
# Work from the extracted DT (not an assumed source-tree DT).
subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', '-o', str(fitout / 'main.dts'),
                str(fitout / 'main-original.dtb')], check=True, stderr=subprocess.DEVNULL)
dts = (fitout / 'main.dts').read_text()
dts, count = re.subn(r'(memory@80000000\s*\{.*?reg = )<[^>]+>',
                    r'\1<0 0x80000000 0 0x1e000000>', dts, count=1, flags=re.S)
assert count == 1
# Replace dynamically allocated ION with a fixed, smaller region below the small kernel.
dts, count = re.subn(r'(\n\s*ion\s*\{.*?compatible = "ion-region";).*?size = <[^>]+>;',
                    r'\1\n\t\t\treg = <0 0x95400000 0 0x08c00000>;', dts, count=1, flags=re.S)
assert count == 1
# Reserve wired Ethernet for the later small-core port; USB/Wi-Fi are unchanged.
dts, count = re.subn(r'(ethernet@4070000\s*\{)', r'\1\n\t\tstatus = "disabled";', dts, count=1)
assert count == 1
(fitout / 'main.dts').write_text(dts)
subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(fitout / 'sg2000_milkv_duos_glibc_arm64_sd.dtb'),
                str(fitout / 'main.dts')], check=True, stderr=subprocess.DEVNULL)
its = '''/dts-v1/;
/ {
 description = "Duo S isolated small-core NOMMU experiment";
 #address-cells = <2>;
 images {
  kernel-1 {
   description = "cvitek kernel";
   data = /incbin/("Image.lzma");
   type = "kernel"; arch = "arm64"; os = "linux"; compression = "lzma";
   load = <0 0x80108000>; entry = <0 0x80108000>;
   hash-2 { algo = "crc32"; };
  };
  fdt-sg2000_milkv_duos_glibc_arm64_sd {
   description = "cvitek device tree - sg2000_milkv_duos_glibc_arm64_sd";
   data = /incbin/("sg2000_milkv_duos_glibc_arm64_sd.dtb");
   type = "flat_dt"; arch = "arm64"; compression = "none";
   hash-1 { algo = "sha256"; };
  };
 };
 configurations {
  config-sg2000_milkv_duos_glibc_arm64_sd {
   kernel = "kernel-1"; fdt = "fdt-sg2000_milkv_duos_glibc_arm64_sd";
  };
 };
};
'''
(fitout / 'multi.its').write_text(its)
assert 0x95400000 + 0x08c00000 == 0x9e000000
print('Prepared isolated U-Boot map: A53/U-Boot 480 MiB, ION 140 MiB at unchanged base; C906L 32 MiB')
