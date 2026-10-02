#!/usr/bin/env python3
"""Pack and verify a matched experimental boot pair, keeping stock outputs intact."""
from pathlib import Path
import hashlib, importlib.util, os, re, shutil, struct, subprocess, tarfile
task = Path(__file__).resolve().parent
sdk = task.parents[1]
out = task / 'build'
userspace = os.environ.get('NOMMU_USERSPACE') == '1'
shell = os.environ.get('NOMMU_SHELL') == '1'
bundle = out / ('bundle-shell' if shell else 'bundle-userspace' if userspace else 'bundle')
archive_path = out / ('duos-nommu-shell.tar.gz' if shell else 'duos-nommu-userspace.tar.gz' if userspace else 'duos-nommu.tar.gz')
cxx = os.environ.get('NOMMU_CXX_PROBE', '')
if cxx:
    assert cxx in ('runtime_probe', 'opencv_probe', 'fp_probe', 'runtime_hardfloat_probe', 'opencv_hardfloat_probe') and userspace and not shell
    bundle = out / ('bundle-' + cxx)
    archive_path = out / ('duos-nommu-' + cxx + '.tar.gz')

# These vendor macros come from SDK make variables, not U-Boot's .config.
# Without them the board boots to soph# with an empty boot command.
uboot = (out / 'uboot/u-boot.bin').read_bytes()
assert b'bootcmd=run sdboot\0' in uboot, 'missing vendor SD autoboot environment'
assert b'sdboot=' in uboot and b'boot.sd' in uboot
assert b'root=root=/dev/mmcblk0p3 rootwait rw\0' in uboot, 'unexpected SD root arguments'
stock = sdk / 'install/soc_sg2000_milkv_duos_glibc_arm64_sd'
rv = sdk / 'host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-'
def run(args, **kwargs):
    return subprocess.check_output(list(map(str, args)), **kwargs)
def entry(elf):
    text = run([str(rv)+'readelf', '-h', elf], text=True)
    return int(re.search(r'Entry point address:\s*(0x[0-9a-fA-F]+)', text).group(1), 16)
assert entry(out / 'shim.elf') == 0x9e000000
assert entry(out / 'kernel/vmlinux') == 0x9e200000
shim = (out / 'shim.bin').read_bytes()
dtb = (out / 'little.dtb').read_bytes()
image = (out / 'kernel/arch/riscv/boot/Image').read_bytes()
assert len(shim) < 0x10000 and len(dtb) < 0x10000
offset, image_size = struct.unpack_from('<QQ', image, 8)
assert offset == 0 and len(image) <= image_size < 0x01de0000
payload = bytearray(0x200000 + len(image))
payload[:len(shim)] = shim
payload[0x20000:0x20000+len(dtb)] = dtb
payload[0x200000:] = image
(out / 'little-payload.bin').write_bytes(payload)
fiptool = sdk / 'fsbl/plat/cv181x/fiptool.py'
# Snapshot first: vendor FIP parser uses mutable class attributes.
spec = importlib.util.spec_from_file_location('fiptool', fiptool)
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
old = module.FIP(); old.read_fip(str(stock / 'fip.bin'))
before = {(section, name): bytes(value.content)
          for section in ('body1', 'body2') for name, value in getattr(old, section).items()}
# With OLD_FIP the vendor CLI ignores an explicit run address if the old
# parameter is nonzero. Set it through the parser API, then use the vendor
# packer to regenerate offsets/checksums; never patch final FIP bytes.
old.body2['BLCP_2ND'].content = bytes(payload)
old.param2['BLCP_2ND_RUNADDR'].content = 0x9e000000
old.body2['LOADER_2ND'].content = (out / 'uboot/u-boot.bin').read_bytes()
assert old.body2['LOADER_2ND'].content[4:8] == b'BL33'
old.compress_algo = 'lzma'
from types import SimpleNamespace
(bundle / 'fip-nommu.bin').write_bytes(old.make(SimpleNamespace(
    BLCP_2ND_RUNADDR=0x9e000000, MONITOR_RUNADDR=old.param2['MONITOR_RUNADDR'].toint())))
new = module.FIP(); new.read_fip(str(bundle / 'fip-nommu.bin'))
for (section, name), data in before.items():
    if name not in ('BLCP_2ND', 'LOADER_2ND'):
        assert bytes(getattr(new, section)[name].content) == data, (section, name)
assert new.param2['BLCP_2ND_RUNADDR'].toint() == 0x9e000000
packed = new.body2['BLCP_2ND'].content
assert packed[:len(payload)] == payload and not any(packed[len(payload):])
assert 0x9e000000 + len(packed) < 0x9fff0000
# Also prove the compressed BL33 contains the isolated U-Boot, not a stale binary.
loader = new.body2['LOADER_2ND'].content
assert loader[4:8] == b'B3MA'
import lzma
decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
assert decoder.decompress(loader[32:]) == (out / 'uboot/u-boot.bin').read_bytes()[32:]
assert decoder.eof and not any(decoder.unused_data)
fitout = out / 'main-fit'
mkimage = out / 'uboot/tools/mkimage'
run([mkimage, '-f', 'multi.its', bundle / 'boot-nommu.sd'], cwd=fitout)
dump = out / 'uboot/tools/dumpimage'
run([dump, '-T', 'flat_dt', '-p', '0', '-o', out / 'verify-kernel.lzma', bundle / 'boot-nommu.sd'])
assert (out / 'verify-kernel.lzma').read_bytes() == (fitout / 'Image.lzma').read_bytes()
run([dump, '-T', 'flat_dt', '-p', '1', '-o', out / 'verify-main.dtb', bundle / 'boot-nommu.sd'])
assert (out / 'verify-main.dtb').read_bytes() == (fitout / 'sg2000_milkv_duos_glibc_arm64_sd.dtb').read_bytes()
shutil.copyfile(stock / 'fip.bin', bundle / 'fip-restore.bin')
shutil.copyfile(stock / 'rawimages/boot.sd', bundle / 'boot-restore.sd')
shutil.copyfile(task / 'README.md', bundle / 'README.md')
if cxx:
    shutil.copyfile(task / 'runtime/README.md', bundle / 'PROBE-README.md')
    shutil.copyfile(out / (cxx + '-elf.txt'), bundle / 'probe-elf.txt')
    shutil.copyfile(out / 'bundle-shell/fip-nommu.bin', bundle / 'fip-shell.bin')
    # The updated main FIT disables the stale fast-image service for all profiles.
    shutil.copyfile(bundle / 'boot-nommu.sd', bundle / 'boot-shell.sd')

shutil.copyfile(task / 'run_stability.sh', bundle / 'run_stability.sh')
if userspace:
    shutil.copyfile(out / 'userspace/init-elf.txt', bundle / 'init-elf.txt')
    baseline = out / 'bundle'
    assert 'CONFIG_DUOS_NOMMU_USER_PROBE=y' not in (baseline / 'small-kernel.config').read_text()
    shutil.copyfile(baseline / 'fip-nommu.bin', bundle / 'fip-kernel-only.bin')
    # Retain the earlier small-core FIP, but use the fixed main FIT. Earlier
    # rollback FITs still advertise the invalid FreeRTOS fast-image node.
    run([dump, '-T', 'flat_dt', '-p', '0', '-o', out / 'verify-rollback-kernel.lzma', baseline / 'boot-nommu.sd'])
    assert (out / 'verify-rollback-kernel.lzma').read_bytes() == (fitout / 'Image.lzma').read_bytes()
    shutil.copyfile(bundle / 'boot-nommu.sd', bundle / 'boot-kernel-only.sd')
if shell:
    baseline = out / 'bundle-userspace'
    shutil.copyfile(baseline / 'fip-nommu.bin', bundle / 'fip-user-probe.bin')
    run([dump, '-T', 'flat_dt', '-p', '0', '-o', out / 'verify-rollback-user.lzma', baseline / 'boot-nommu.sd'])
    assert (out / 'verify-rollback-user.lzma').read_bytes() == (fitout / 'Image.lzma').read_bytes()
    shutil.copyfile(bundle / 'boot-nommu.sd', bundle / 'boot-user-probe.sd')
shutil.copyfile(out / 'kernel/.config', bundle / 'small-kernel.config')
shutil.copyfile(out / 'kernel-size.txt', bundle / 'kernel-size.txt')
manifest = ['Verified: only BLCP_2ND and LOADER_2ND FIP payloads changed; DDR/BL2/ATF retained.',
            'Verified: A53 compressed kernel byte-identical; no production rootfs changed.',
            f'Small Image file={len(image)} bytes; including BSS={image_size} bytes.',
            'A53 RAM 480 MiB; ION 140 MiB at 0x95400000; small reservation 32 MiB.',
            ('Diagnostic shell tested; fast-image DT fix passed two warm reboots; longer stability unverified.' if shell else
             'Libc-free userspace init included; first userspace cold boot verified.'
             if userspace else 'Kernel-only probe; first cold boot verified, repeated boots and warm reset outstanding.')]
if cxx:
    manifest[-1] = f'{cxx}: C++ static PIE; this bundle hardware runtime/timings NOT YET VERIFIED.'
(bundle / 'BUILD.txt').write_text('\n'.join(manifest)+'\n')
files = sorted(p for p in bundle.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
(bundle / 'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n' for p in files))
with tarfile.open(archive_path, 'w:gz') as archive:
    archive.add(bundle, arcname='bundle')
print('\n'.join(manifest))
print(f'Ready: {archive_path}')
