# Duo S C906L Linux NOMMU boot probe

This opt-in experiment runs a separate Linux v6.18 kernel in M-mode on the
small C906L core alongside the existing A53 Linux. It replaces the small-core
FreeRTOS firmware. Production SDK sources, board configuration and image
defaults are not changed.

**Status:** cross-built and packaged; board boot is not yet verified. This is
a kernel-only test, not a userspace Linux system. PID 1 remains a kernel task.
There is no shell, storage, Ethernet, decoder, VENC or RTSP on the small core.
Replacing FreeRTOS also removes its vendor multimedia services; camera
continuity is not an acceptance criterion for this initial probe.

## What it tests

- Vendor C906L cache startup and the existing FSBL BLCP2ND launch path.
- RISC-V Linux entry with hart ID and a private device tree.
- NOMMU high-DDR RAM base, traps, allocator and scheduler.
- C906L timer: 25 MHz time CSR, safe 32-bit compare register writes at
  `0x74004000`/`0x74004004`. CPU clock remains the existing 700 MHz OD setting.
- A worker allocates, fills, checks and frees a page, then sleeps for one second.
- A separate kernel thread supplies only the A53 boot-transfer mailbox reply.
- A bounded DDR boot log readable by A53, avoiding shared UART ownership.

Kernel patches are generated exclusively in the downloaded, ignored source
tree. Linux v6.18 already supplies the configurable NOMMU RAM base; its
generic CLINT driver still needs the explicit C906L changes in `patch_kernel.py`.
The kernel archive is pinned by SHA256. No MMU, SMP, FPU, vector, PLIC or
network driver is enabled in this first test.

## Memory and boot pair

| Region | Address | Size |
| --- | --- | --- |
| Main Linux / U-Boot visible RAM | `0x80000000` | 480 MiB |
| ION, within main visible RAM | `0x95400000` | 140 MiB |
| Shim and loading space | `0x9e000000` | 2 MiB |
| Preserved FSBL boot transfer | `0x9e010000` | 40 bytes |
| Small kernel DTB | `0x9e020000` | below 64 KiB |
| Small kernel allocator RAM | `0x9e200000` | 30 MiB minus 64 KiB |
| Shared log, outside both allocators | `0x9fff0000` | 64 KiB |

The small-core reservation grows from 2 to 32 MiB. Its extra 30 MiB comes
from the **upper end of ION** (170 to 140 MiB), preserving the existing ION,
ISP, bitstream and framebuffer base addresses. FSBL/DDR/ATF payloads are
retained. FIP supplies the new C906L entry address.
The SDK CLI ignores its address override with a nonzero old FIP address;
`package.py` explicitly sets the parser parameter and uses the vendor packer
to regenerate offsets and checksums, then verifies the resulting address.

U-Boot is rebuilt separately with the 480 MiB RAM limit, so its relocation
cannot overwrite the small kernel. The main device tree has the same RAM
limit and a fixed 140 MiB ION region. The compressed main kernel is verified
byte-for-byte against the current production FIT. The root filesystem is
retained. Install **both** experimental boot files together: a FIP-only change
would leave the old U-Boot/main memory map capable of overwriting the probe.

## Build

Requires an already completed production `milkv-duos-glibc-arm64-sd` SDK build.
The preparation script intentionally rejects a different FIT image layout.
Build inside the repository's Docker container:

```sh
docker exec duodocker bash -c 'cd /home/work && bash apps/duos_nommu/package.sh'
```

Outputs are isolated under `apps/duos_nommu/build/`:

- `duos-nommu.tar.gz`: deployable bundle.
- `bundle/fip-nommu.bin`, `bundle/boot-nommu.sd`: matched experiment boot pair.
- `bundle/fip-restore.bin`, `bundle/boot-restore.sd`: matched current SDK pair.
- `bundle/read_nommu_log`: A53 Linux read-only `/dev/mem` log reader.
- `bundle/SHA256SUMS`, `BUILD.txt`, kernel configuration and size report.
- Kernel/shim ELF files, disassembly, generated DTs and build logs for diagnosis.

The experimental `boot-nommu.sd` is a raw FIT, as needed on the boot FAT
partition. The SDK's CIMG-wrapped top-level `boot.sd` is not interchangeable.

## Board test and recovery

Stage the archive under `/mnt/data/duos-nommu.tar.gz`. Use USB networking for
this experiment: wired Ethernet is disabled in the main DT, while Wi-Fi and
USB Ethernet remain on A53. Verify the archive and back up the board's actual
boot pair before installing. Keep that backup accessible from a host SD reader
if the experimental boot fails.

```sh
mkdir -p /mnt/data/duos-nommu
tar -xzf /mnt/data/duos-nommu.tar.gz -C /mnt/data/duos-nommu
cd /mnt/data/duos-nommu/bundle
sha256sum -c SHA256SUMS
mkdir -p /mnt/data/duos-nommu/board-backup
[ -e /mnt/data/duos-nommu/board-backup/fip.bin ] || cp -p /boot/fip.bin /mnt/data/duos-nommu/board-backup/fip.bin
[ -e /mnt/data/duos-nommu/board-backup/boot.sd ] || cp -p /boot/boot.sd /mnt/data/duos-nommu/board-backup/boot.sd
cp fip-nommu.bin /boot/fip.bin
cp boot-nommu.sd /boot/boot.sd
sync
reboot
```

After reconnecting over USB:

```sh
/mnt/data/duos-nommu/bundle/read_nommu_log > /mnt/data/nommu-boot-1.txt 2>&1
sleep 10
/mnt/data/duos-nommu/bundle/read_nommu_log > /mnt/data/nommu-boot-2.txt 2>&1
cat /mnt/data/nommu-boot-2.txt
```

Acceptance: A53 stays responsive; log shows the Linux banner and timer
initialization at 25 MHz; `stage=3`; heartbeat increases between snapshots;
allocation reports `OK`; trap fields stay zero. Repeat cold boots before
claiming the platform works. Stage 1 is shim startup, 2 is kernel handoff,
99 captures an early exception (cause, PC and value). Later kernel exceptions
should appear in the console log. A banner without heartbeat progression
does not establish timer/scheduler success.

Restore both actual board backups together:

```sh
cp /mnt/data/duos-nommu/board-backup/fip.bin /boot/fip.bin
cp /mnt/data/duos-nommu/board-backup/boot.sd /boot/boot.sd
sync
reboot
```

If Linux is unreachable, mount the SD boot partition on the host and replace
both files from the backup. Do not overwrite the only backup on repeated tests.

## Later Ethernet ownership

The intended split is **wired Ethernet on C906L; Wi-Fi and USB Ethernet on
A53**. Disabling the main Ethernet node reserves ownership; it does not yet
provide a functioning small-core interface. This first small DT contains no
Ethernet or PLIC node.

After scheduler stability, add PLIC routing/context support, SG2000 MAC/DMA
and PHY glue, dedicated DMA descriptors/buffers, cache maintenance and explicit
clock/reset/pin ownership. Audit the existing PHY sleepable-wait fix when
porting the driver. Ensure main Linux cannot independently reset or access
the same MAC/PHY. Then add a NOMMU-compatible network userspace or initially
test packets with a kernel diagnostic. Routing between the two OSes, if
needed, requires a separate shared-memory link. An upstream stmmac driver
alone does not establish SG2000 board support.

Tiny userspace is a separate next milestone (static compatible `/init`,
syscalls and allocation/sleep tests). Audit libc, binary loader, TLS/pthreads
and FPU before attempting decoder or RTSP ports. NOMMU Linux changes the
software environment; it does not remove the measured C906L compute limit.
