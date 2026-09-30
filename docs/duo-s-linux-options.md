# Duo S Linux root filesystem options for TinyTag

Status: compatibility assessment, 2026-09-30. Alternative distributions and a
minimal image have not been built or tested by this assessment.

## What the app needs

The application is not tied to a distribution name, but the current working
stack is tied to the board's hardware support and userspace ABI. Preserve:

- AArch64 execution on the Cortex-A53. The RISC-V board profile is a different
  architecture and does not make ARM binaries compatible with musl.
- The working boot firmware, device tree, kernel configuration, reserved camera
  and multimedia memory, and matching vendor kernel modules.
- Compatible CVI camera/ISP/VPSS/TPU/codec userspace libraries and their driver
  interfaces, sensor libraries, sensor configuration, and tuning assets.
- The C/C++ runtime, OpenCV and other shared libraries, device nodes, mounts,
  module loading, and network/init behavior used by the app.

Changing the root filesystem while retaining the working kernel and firmware
is the first compatibility experiment. A generic distro kernel cannot be
assumed to provide the CVI hardware interfaces.

Local evidence:

- `buildroot/configs/milkv-duos-glibc-arm64-sd_defconfig` selects AArch64 and the
  external Linaro glibc toolchain.
- `readelf -l` on the built `tinytag_detect_live` requests
  `/lib/ld-linux-aarch64.so.1`.
- Its `DT_NEEDED` entries include `libc.so.6`, `libstdc++.so.6`, OpenCV 3.2,
  CVI runtime/kernel libraries, and camera/ISP/VPSS/GDC/codec/RTSP libraries.
- The inspected `libcviruntime.so` depends on glibc libraries and contains
  `GLIBC_2.17` version requirements. This is not a complete dependency audit or
  a proven minimum libc version for the whole application.
- `device/generic/rootfs_overlay/duos/mnt/system/ko/loadsystemko.sh` loads the
  vendor modules; `device/generic/br_overlay/common/etc/init.d/S99user` runs
  board initialization and startup hooks.

## Alpine

Alpine provides AArch64 userspace and uses musl, BusyBox, and OpenRC [1]. Its
userspace is a candidate for a custom root filesystem over the vendor kernel.
The current glibc application and vendor libraries cannot simply be substituted
into a pure musl environment.

Possible routes are a glibc application environment within Alpine, with the
required devices/mounts available, or rebuilding the entire application and
dependency chain for musl where source and ABI support permit. Recompiling only
TinyTag does not convert its prebuilt vendor dependencies. Alpine documents
gcompat and glibc chroots [2]; gcompat compatibility with this camera/TPU stack
is unverified and should not be presumed. Retaining a glibc environment reduces
the simplicity and size advantage of replacing the existing root filesystem.

## Tiny Core

Tiny Core's piCore64 releases provide AArch64 images for Raspberry Pi boards
[3]. Those boot images are not Duo S images. Reusing suitable userspace would
still require Duo-specific boot/kernel integration, compatible libc/libraries,
vendor initialization, and suitable device access. The app running successfully
on such a port has not been demonstrated here. Ordinary x86 Tiny Core images
do not match the current target architecture.

## A minimal Buildroot image

Buildroot can generate a custom root filesystem, kernel and bootloader, or use
only the needed parts of that process. It is intended for complete embedded
images rather than producing an Alpine/Tiny Core distribution with its package
repository and upgrade model [4]. It can provide a similarly small appliance
environment while retaining the known glibc and board stack.

This repository already uses Buildroot. Its current board defconfig enables
Python/pip, GDB, fio, stress-ng, archive tools, Bluetooth utilities, and many
other utilities beyond the application's runtime requirements. Its image layout
allocates 128 MiB for boot and 768 MiB for rootfs, plus a 2 MiB logo partition.
The root size also appears as `BR2_TARGET_ROOTFS_EXT2_SIZE="786432K"` in the
defconfig. These are partition capacities, not measured installed payload size.

For a separate minimal profile:

1. Keep the working AArch64/glibc toolchain, kernel, board initialization, sensor
   selection, and required vendor stack.
2. Select a small BusyBox/init/network environment with SSH for app updates.
   Remove development, stress-test, sample, and optional service packages after
   checking runtime and startup dependencies. Use a fresh build output when
   removing packages so stale installed files do not defeat the reduction.
3. Audit the transitive shared-library dependencies and vendor overlay payload.
   Keep libraries that the binary still links, even when their runtime feature
   is disabled. Removing recording/RTSP libraries may require a corresponding
   application build option, not just deleting files.
4. Measure the resulting filesystem payload and reduce rootfs capacity in both
   the Buildroot and genimage settings. Review boot capacity and the board's
   first-boot resize policy separately.
5. Optionally investigate compressed read-only root storage with writable
   configuration/log/data storage. The current SD boot and packaging expect
   ext4; switching filesystem format needs matching kernel, mount, and packaging
   changes. It is not achieved by only selecting a Buildroot output format.
6. Validate boot, module loading, both camera profiles, inference, decoding,
   optional preview/replay, app deployment, and clean shutdown.

The recommended first effort for a smaller image is a separate minimal
Buildroot/glibc profile. It keeps the established hardware and binary interfaces
while reducing packages and oversized partitions. There is no measured minimum
image size yet. A generic tiny base filesystem's advertised size excludes this
application's OpenCV/vendor stack and camera buffers. Smaller storage use does
not imply proportionally lower RAM use or faster crop decoding.

## Sources

1. [Alpine overview](https://wiki.alpinelinux.org/wiki/Alpine_Linux%3AOverview).
2. [Alpine: running glibc programs](https://wiki.alpinelinux.org/wiki/Software_management#Running_glibc_programs).
3. [Tiny Core project's piCore 15 release](https://forum.tinycorelinux.net/index.php?topic=27338.0).
4. [Buildroot manual](https://buildroot.org/downloads/manual/manual.html),
   especially the introduction and section 11.7 on distribution/package scope.
