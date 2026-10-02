# Duo S C906L Linux NOMMU boot probe

This opt-in experiment runs a separate Linux v6.18 kernel in M-mode on the
small C906L core alongside the existing A53 Linux. It replaces the small-core
FreeRTOS firmware. Production SDK sources, board configuration and image
defaults are not changed.

**Status:** first board test reached U-Boot with 480 MiB RAM but stopped at
`soph#`: the isolated build omitted the vendor default-environment and SD
storage flags. Packaging now supplies those flags and checks the binary's
SD autoboot/root environment. The corrected bundle passed its first cold-boot
kernel-only test on 2026-10-02 alongside responsive A53 Linux.
The initial U-Boot also aborted on `md.l 9fff0000 10`: the log was outside
its MMU map. The corrected build maps only that 64 KiB log without caching,
while keeping allocator/relocation RAM at 480 MiB. Do not read this address
from the original experimental U-Boot.

The next warm-reboot attempt stopped after `Requesting system reboot`, with
no new FSBL output. Ping still worked but SSH refused connections after
shutdown had stopped Dropbear; this does not demonstrate a new Linux boot.
Use a full power cycle for the next probe and retain serial output. The cause
was initially unresolved; the fast-image DT fix below subsequently passed
two warm reboots. The main CVI restart driver accesses
RTC registers directly (including unbounded polling); it does not send a
FreeRTOS mailbox reset request.

### First successful cold boot (2026-10-02)

Board log: `stage=3 hart=0 heartbeat=20 ticks=576637284 trap=0 pc=0 value=0`.
Linux 6.18 initialized the C906L local interrupt controller and the CLINT
timer at 25 MHz, reported 28,320 KiB available memory, and kept PID 1 as a
kernel task. All 20 recorded page allocation/fill/check/free cycles reported
`OK`; heartbeats progressed from 0.063 to 19.841 seconds. The main-core boot
mailbox reply was sent at 8.961 seconds. Main Linux reached its serial shell.

This verifies kernel entry, timer-driven scheduling, the page-allocation
probe and boot handover for one cold boot. It does not establish long-term
stability, userspace, network/device support or working warm reset. The
missing initial console and `/init` warnings are expected for this kernel-only
configuration. The delay-loop value of 50 BogoMIPS is derived from the timer;
it is not a measurement of the CPU clock or application throughput.

The default build is a kernel-only test. PID 1 remains a kernel task.
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
| Small kernel allocator RAM, new builds | `0x9e200000` | 30 MiB minus 128 KiB |
| Shared shell console, new builds | `0x9ffe0000` | 64 KiB |
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

## Optional minimal userspace probe

Build separately after the kernel-only bundle exists:

```sh
docker exec duodocker bash -c 'cd /home/work && NOMMU_USERSPACE=1 bash apps/duos_nommu/package.sh'
```

Output: `build/duos-nommu-userspace.tar.gz`, containing `bundle/` from
`build/bundle-userspace/`. The known kernel-only archive is retained. This
userspace profile passed its first board cold boot on 2026-10-02.

Linux v6.18's upstream `BINFMT_ELF_FDPIC` loader supports RISC-V NOMMU
non-FDPIC ET_DYN binaries. `/init` is a small static PIE, soft-float RV64IMAC,
with no interpreter, dynamic relocations or libc. Its 16 KiB stack size is
declared in `PT_GNU_STACK`. An embedded uncompressed initramfs provides `/init`
and `/dev/kmsg`; messages written to `/dev/kmsg` reach the shared console log.
The build verifies the ELF format, absence of interpreter/relocations and
selected kernel configuration.

The probe tests user-mode entry, `getpid`, `openat`/`write`, anonymous `mmap`,
all-byte page fill/verification, `munmap`, monotonic `clock_gettime` and
`nanosleep`. It loops indefinitely as PID 1 while the kernel heartbeat and
mailbox service continue. Success requires `duos-user: entered user mode;
pid=1`, increasing user iteration and monotonic time, allocation cycles `OK`,
no `duos-user: FAIL` messages and continued kernel heartbeat. This does not
test libc, threads, subprocesses, a shell or networking.

### Board results (2026-10-02)

Before installing userspace, the kernel-only stability script recorded seven
snapshots over one minute: heartbeat 845 to 903, stage 3 throughout, and zero
recorded trap fields. No logged allocation failure was reported by the script.

After a power cycle with the userspace pair, `/init` executed as PID 1 at
0.065641 seconds. Visible user iterations advanced from 1 to 19, successful
allocation cycles were reported through count 18, and monotonic time advanced
from 0 to 17 seconds in the visible messages. Kernel heartbeat advanced to
20 at 19.841280 seconds with all logged kernel allocations `OK`; header:
`stage=3 hart=0 heartbeat=20 ticks=577126528 trap=0 pc=0 value=0`.
The main-core boot reply completed at 8.961256 seconds; A53 Linux was responsive.
No userspace `FAIL` message appeared in the supplied snapshot.

Some user iterations/messages are absent from this snapshot. The probe writes
three messages per second through `/dev/kmsg`, whose default per-open rate
limit is 10 messages per five seconds. The gaps are consistent with that
logging limit and do not by themselves establish scheduling gaps. It also
means absence of a printed failure is not an exhaustive per-iteration check.
Use fewer messages or explicit persistent result counters before longer
automated acceptance tests. The initial-console warning is expected because
the shared early console is not a userspace TTY; `/dev/kmsg` logging still works.

This establishes one minimal user-mode/syscall boot, alongside the main OS.
Longer userspace stability, repeated cold boots, libc, threading
and network/device drivers remain unverified.

On the board, extract to a separate directory, verify `SHA256SUMS`, and install
both `fip-nommu.bin` and `boot-nommu.sd` as in the kernel-only instructions.
Use `sync`, then a full power cycle when upgrading an earlier image whose
fast-image probe has already oopsed. Corrected builds support warm reboot. Preserve
the original board backup. The userspace bundle also includes the previous
kernel-only pair as `fip-kernel-only.bin` / `boot-kernel-only.sd` for rollback.

Before replacing the working kernel-only pair, record a minute of progress:

```sh
sh /mnt/data/run_nommu_stability.sh \
  /mnt/data/duos-nommu-fixed/bundle/read_nommu_log \
  /mnt/data/nommu-stability-kernel
```

The same script is bundled as `run_stability.sh`. It saves seven snapshots,
checks stage/traps and increasing kernel heartbeat, and rejects logged probe
failures. Userspace progress still needs inspection of `duos-user` messages.
Repeated cold boots remain a manual hardware test; neither the script nor
the host build claims to have performed them.

## Interactive diagnostic shell and warm-reset diagnosis

```sh
docker exec duodocker bash -c 'cd /home/work && NOMMU_SHELL=1 bash apps/duos_nommu/package.sh'
```

This builds `build/duos-nommu-shell.tar.gz` from `build/bundle-shell/`,
preserving the kernel-only and initial userspace archives. It requires both
earlier bundles for rollback. The shell passed its first board test on
2026-10-02: attachment through the A53 terminal, `help`, `uname` reporting
`Linux 6.18.0 riscv64`, `pid` reporting 1, successful 4096-byte `memtest`,
`uptime`, `sleep 2`, and clean terminal detachment. This is still a diagnostic
shell, not a BusyBox/POSIX environment.
The kernel allocator ends at `0x9ffe0000`: a new 64 KiB console reservation
sits immediately below the existing boot log. Main Linux/ION ownership and
the 480 MiB U-Boot relocation limit are unchanged. U-Boot maps both reserved
regions without caching for diagnostics.

`/init` is an interactive libc-free diagnostic shell. Builtins: `help`,
`echo TEXT`, `uname`, `uptime`, `pid`, `memtest`, `sleep SECONDS` (0–3600).
It does not yet support external programs, pipes, redirection, job control
or the POSIX shell language. BusyBox/libc remains a separate porting step.
It writes no periodic `/dev/kmsg` messages, avoiding the initial probe's
rate-limit issue. The kernel heartbeat continues independently.

A small-core misc character device `/dev/duos-console` connects the shell
to two single-producer/single-consumer shared-memory rings. The A53
`nommu_terminal` program maps the reservation through `/dev/mem` with
`O_SYNC`, uses memory barriers, and permits only one attachment using a file
lock. Each index has a separate 64-byte cache line; C906 invalidates remote
indices/input and cleans output/local indices before publication. Full rings
apply backpressure, without overwriting unread bytes. Idle small-core reads
sleep in 5 ms intervals. This console does not acquire UART0.

After installing the matched shell boot pair and power-cycling, run on A53:

```sh
/mnt/data/duos-nommu-shell/bundle/nommu_terminal
```

At `c906l#`, try `help`, `uname`, `pid`, `uptime`, `memtest`, `sleep 2`,
then `uptime` again. Ctrl-] detaches without stopping either OS.
Keep `read_nommu_log` available to confirm the kernel heartbeat continues.
Rollback pairs: `fip-user-probe.bin`/`boot-user-probe.sd` and
`fip-kernel-only.bin`/`boot-kernel-only.sd`. Always replace both boot files.

### Warm-reset trace module (A53 only)

`nommu_reset_diag.ko` is built against the existing production A53 kernel.
It is an opt-in diagnostic and does not initiate reset, bypass shutdown,
modify RTC registers, or claim to fix warm reboot. No kprobe support is
required. A highest-priority reboot notifier arms a timer and retains the
shutdown task and enables normal console logging for stack dumps. The first
trace reached `reboot notifiers completed; device shutdown next` but never
reached the syscore marker; ten timer markers appeared without visible stacks.
This places the block after reboot notifiers and before syscore shutdown,
most likely in device shutdown; the task stack/device trace must confirm it.
The updated diagnostic enables console verbosity, and Linux's existing
`initcall_debug` parameter can reveal shutdown callback device names.

A highest-priority reboot notifier retains the
shutdown task; a lowest-priority notifier marks completion of the notifier
chain; a syscore callback marks completed device shutdown; a restart notifier
marks entry to the reset-handler chain. Its stage markers use bounded UART0
polling, so they can still print after the normal console stops. If timers
remain active during a hang, it requests up to ten task-stack dumps at two
second intervals through the kernel console. Disabled interrupts or a halted
clock can prevent those later dumps; missing dumps alone do not locate a bug.

Run with serial capture active, preferably on the already verified userspace
profile before changing to the shell image:

```sh
insmod /mnt/data/nommu_reset_diag.ko
echo Y > /sys/module/kernel/parameters/initcall_debug
sync
reboot
```

Preserve the last stage and any task stack. If reset stalls, power-cycle;
the module is not installed in startup scripts. To cancel before reboot:
`rmmod nommu_reset_diag` (restores the console log level). Restore the previous
`initcall_debug` setting separately if canceling. A targeted reset fix is
pending this hardware trace.

The second reset trace located the shutdown task in
`wait_for_device_probe+0xa8/0xd0`, called by `device_shutdown`. Disassembly
of the matching A53 kernel maps this offset to the wait for `probe_count`
to reach zero, after the deferred-timeout and deferred-work waits. This
confirms an unfinished driver probe, before per-device shutdown callbacks,
syscore shutdown or RTC reset. Do not bypass the wait or change the RTC
handler as a substitute for locating and resolving the stuck probe.
The production kernel does not expose `/proc/sysrq-trigger`; the initial
SysRq collection command could not run. Diagnostic revision 3 instead
collects up to 64 blocked-thread stacks when loaded, briefly enabling serial
console logging and holding task references while printing outside RCU.
After cold-boot recovery, collect stacks without rebooting:

```sh
insmod /mnt/data/nommu_reset_diag_v3.ko
dmesg > /mnt/data/nommu-blocked-tasks.txt
```

The retrieved boot log then identified a kernel oops at 5.255688 seconds,
PID 155 (`insmod`), in the `cv181x_fast_image` platform probe. Its main DT
node still described the old FreeRTOS memory region at `0x9fe00000`.
The driver combined that mapping with the NOMMU shim's handover pointer
`0x9e010000`, producing an unmapped virtual address (`ffffffc00f210000`)
and aborting during the probe. The oops bypassed normal probe completion,
leaving `probe_count` nonzero; no still-blocked probe thread remained to dump.
Idle (`state:I`) tasks in the diagnostic snapshot are not failed probes.

`prepare_boot.py` now disables the experimental main DT `fast_image` node
(`cvitek,rtos_image`). The NOMMU kernel provides no vendor FreeRTOS fast-image
service; moving this node to the new reservation would not supply that
service. The driver can remain loaded for exported zero-buffer interfaces,
but its disabled node cannot bind or dereference the stale handover mapping.
Production DT/module source and regular image defaults are unchanged.
The rebuilt FIT preserves the exact A53 kernel and existing shell FIP.
After installing this FIT and cold-booting, the user completed two warm
reboots on 2026-10-02, both returning to the main Linux prompt. The captured
trace shows `reboot: Restarting system`, then ROM/FSBL output, warm-reset
reason `0x40f0003`, and DDR initialization. This verifies the targeted warm
reset fix for the shell profile in two trials; longer stress remains untested.
No RTC reset-driver or generic probe-wait bypass was required.
