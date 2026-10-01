# Duo S: ROI thresholding on C906L FreeRTOS

## Purpose and scope

The follow-up [bare-metal diagnosis](../apps/tinytag_detect/bare_metal/README.md)
keeps the same kernel, compiler optimization and shared-buffer handover while
removing task dispatch, the scheduler and interrupts. Its separate FIP and
benchmark firmware identity allow a controlled comparison with these results.
The first board comparison is complete: at 700 MHz, bare-metal computation
was 15.05961 ms versus FreeRTOS's 15.11100 ms for 640x360, and 67.83551 ms
versus 67.78074 ms for 1280x800. Removing scheduling and interrupts did not
materially improve this kernel. The large gap to A53 is not explained by
FreeRTOS overhead in these measurements. See the linked diagnosis for the
complete comparison and Linux timing interference, and
[the recorded output](benchmarks/duo-s-threshold-bare-metal.tsv).

This experiment measures how much work it takes to move a real decoder stage
to the small core, its computation speed, and the cost of Linux/FreeRTOS
communication. It precedes the proposed NOMMU Linux experiment.

Only ArUco Nano's **initial per-ROI thresholding** moves:

```text
A53 Linux                        C906L FreeRTOS
capture / TPU / tracking
select grayscale ROI
copy ROI into ION buffer -------> normalized box mean
                                 saturated mean-minus-pixel
                                 binary threshold
read threshold result <--------- publish completion
contours / quads / tag decoding
corner refinement / results
preview / OSD / VENC / RTSP
```

Candidate bit-grid thresholding and Otsu retries remain on Linux. Neural-network
mask thresholding also remains on Linux. Jobs are synchronous initially: Linux
waits before contour processing. This measures the cost of this partition; it
does not yet overlap thresholding another ROI/frame with Linux decoding.

**Default remains Linux OpenCV.** The firmware adds an idle worker task, but
no jobs are dispatched unless the standalone benchmark or opt-in live backend
is used. No Linux kernel/module ABI or board memory-map change is required.

## Implementation

- `apps/common/roi_threshold.h`: allocation-free plain C rolling-sum box mean,
  with replicated, isolated ROI borders. Odd window sizes 1–63; crops up to
  1280×800; arbitrary input/output row strides. Scratch is 1280 32-bit column
  sums (5120 bytes maximum).
- Mean follows OpenCV 3.2's 8-bit box-filter arithmetic: window areas up to 256
  use its approximate fixed-point reciprocal, rather than exact integer
  rounding. Isolated one-row/one-column inputs collapse the corresponding
  window dimension. A pixel becomes
  255 only when the saturated difference is strictly greater than the threshold.
  The normal decoder uses a 15×15 window and threshold 3.
- The optimized worker inverts that comparison into a 256-entry cutoff table:
  for nonnegative thresholds below 255, a pixel is white exactly when its box
  sum is at least `cutoff[input_byte]`. The cutoff includes OpenCV's fixed-point
  rounding. Interior horizontal scans have no border clamps, normalization or
  saturation arithmetic. Other threshold values and a 1×1 kernel have constant
  outputs. The table uses another 1024 bytes of temporary stack storage;
  column scratch remains 5120 bytes. The same C code runs on A53 and C906L.
- `roi_threshold_protocol.h`: versioned fixed-width request/completion ABI,
  each on its own 64-byte cache line. Input and output buffers are separate.
- `freertos/.../roi_threshold_worker.c`: a low-priority task receives physical
  buffer addresses from the existing command task. It validates the descriptor
  and buffer ranges against the **generated board ION map**, rather than older
  hardcoded BSP memory constants. Image work never runs in the mailbox ISR or
  high-priority command task. Vendor startup, audio and region tasks remain.
- `roi_threshold_offload.cc`: cached ION allocation, packed ROI input copy,
  explicit cache handoff, stock one-way mailbox send, completion polling, and
  output copy. `IP_SYSTEM=6`, experimental command `0x40`; no second IRQ owner.
- Firmware publishes timing and completion with ordering/cache maintenance.
  Linux validates the matching sequence, ABI and status before reading output.
  Requests are serialized; the Linux client uses a process lock and the live
  hook also uses a mutex. Shared image storage is about 1.95 MiB in existing ION,
  not the 2 MiB firmware region.

This intentionally avoids the stock `RTOS_CMDQU_SEND_WAIT` ioctl: the inspected
driver has an unchecked send/wait result and an uninitialized `wait_list` free
in that ioctl path. The experiment does not modify that driver; it uses the
one-way send ioctl and its own shared completion protocol.

A request has a two-second completion deadline. After timeout/send uncertainty,
the session is disabled and its outstanding ION allocation is retained to avoid
reuse while firmware might still access it. Reboot before repeating a failed
session. Live decoding falls back to Linux OpenCV and logs `DISABLED`; standalone
benchmark failure returns nonzero. A failed/fallback run is not an offload result.

## Build a test bundle

Use the existing selected `milkv-duos-glibc-arm64-sd` SDK outputs and Docker:

```sh
docker start duodocker
docker exec duodocker bash -lc \
  'cd /home/work && bash apps/tinytag_detect/build_threshold_experiment.sh'
```

The helper builds FreeRTOS, both Linux executables and the portable reference
test. It creates `apps/tinytag_detect/build_threshold_experiment/bundle/`:

- `fip-threshold.bin`: existing FIP repacked with the new C906L firmware.
- `fip-original.bin`: the selected base FIP for recovery.
- `cvirtos.bin`: the embedded firmware, for inspection (not a Linux executable).
- `tinytag_threshold_bench`, `tinytag_detect_live`, `run_threshold_bench.sh`.
- `SHA256SUMS`, and a copy of this document.

The same files are packed as
`build_threshold_experiment/tinytag-threshold-experiment.tar.gz` for transfer.

The script verifies that all other parsed FIP firmware payloads and the
small-core entry address match the base FIP, and that the embedded BLCP2ND
payload matches the new firmware. It preserves first-build firmware snapshots
under `build_threshold_experiment/original/`. The normal SDK FIP output and SD
image are not replaced. The FreeRTOS build/install outputs are rebuilt normally.

`TINYTAG_THRESHOLD_BASE_FIP` can select a matching base FIP explicitly. The
default is the SDK's current SG2000 A53 SD FIP. Confirm that this matches the
board's boot configuration before installing; the supplied recovery copy is
the SDK base, not a backup read from the running SD card.

## Install without rebuilding/burning the whole SD image

Copy the Linux files to `/app/tinytag_detect/` and make the launcher executable.
Existing `libsys.so` and OpenCV libraries are reused; no new shared library is
required.

The small core boots firmware from **FIP**: copying `cvirtos.bin` into the Linux
filesystem does not activate it. On the SD boot partition, preserve the actual
existing `fip.bin`, then install `fip-threshold.bin` as `fip.bin`, sync and reboot.
Confirm the FAT boot partition/mount from the board's actual layout first; do
not overwrite a guessed block device. This can be done with the mounted boot
partition or by mounting that partition on a host. Recovery restores the saved
original `fip.bin` and reboots. App-only baseline/local tests need no firmware
change; full offload tests require the matching FIP.

Board local-only equality, A53 timings and standalone firmware offload timings
are verified below. Live camera/offload stability has not been tested. Automatic approval
review blocked the attempted SSH connectivity check as a raw network utility.

## Run and verify

First, on the board, test the portable algorithm against its installed OpenCV:

```sh
export LD_LIBRARY_PATH=/mnt/system/usr/lib:/mnt/system/lib
/app/tinytag_detect/tinytag_threshold_bench --local-only --iterations 20
```

It runs 1890 byte-comparison cases over multiple sizes, windows, threshold values
and patterns, including non-contiguous ROIs whose parent has different pixels.
Validation is outside timed regions. A discrepancy stops the benchmark.

After booting the experiment FIP:

```sh
/app/tinytag_detect/run_threshold_bench.sh
# Optional actual image as the source of centered crops:
/app/tinytag_detect/run_threshold_bench.sh --input /mnt/data/frame.png
```

The script runs sleep-polling (`--poll-us 50`) then busy-polling (`--poll-us 0`),
saving separate TSVs and logs under `/tmp/tinytag-threshold-*`. Each pass uses
finite iteration counts (100 measured + 5 warmups per case by default), a no-op
transport case, and crops 32², 64², 128², 200², 320×240, 640×360 and 1280×800.
Image-input cases larger than the supplied image are skipped. **Every remote
output is compared with OpenCV outside the timing measurement.** Expect progress
by ROI size after startup validation; an individual firmware wait cannot hang
indefinitely. Overall elapsed time depends on measured performance.

To start with fewer samples:

```sh
TINYTAG_THRESHOLD_ITERATIONS=20 /app/tinytag_detect/run_threshold_bench.sh
```

### TSV fields and interpretation

| Field | Meaning |
| --- | --- |
| `total_mean_us`, `total_p50_us`, `total_p95_us` | Linux-observed stage time; FreeRTOS rows include input preparation, notification/wait, result retrieval. |
| `linux_cpu_us` | Calling Linux thread CPU consumption; excludes sleeping and the separately run correctness check. |
| `remote_compute_us` | C906L box mean/subtraction/threshold region, including any preemption during that interval. |
| `remote_service_us` | C906L service interval through output cache handoff, before final completion publication; excludes queue waiting. |
| `prepare_us` | Output allocation/reuse, descriptor preparation, input packing and cache flush. |
| `wait_us` | Send ioctl, firmware queue/service, completion publication and Linux polling/wakeup. |
| `finish_us` | Response checks, output invalidation and copy into the Linux output matrix. |
| `remote_cycles` | Cycle counter delta around computation, also including preemption. |

`freertos-nop` estimates notification/scheduling/completion overhead with no
image data. It is not sufficient to estimate large-crop transfer/cache cost.
Sleep-polling trades Linux CPU for completion delay; busy-polling can reduce
that delay while consuming the main core. Cache invalidation occurs per poll,
so busy-polling can also increase bus contention. Compare both results.

`scalar-a53` uses the same C arithmetic as the worker. Compare it with C906L
compute time to characterize this algorithm on the two CPUs. Compare **OpenCV**
with **full FreeRTOS total** to judge the application partition. A slow scalar
implementation can still be easy to port; those are separate findings. Warm
fixed-input microbenchmarks do not reproduce the camera pipeline's DDR load.

## Live A/B test

Use the newly built live binary and otherwise identical scene/settings:

```sh
TINYTAG_THRESHOLD_BACKEND=opencv /app/tinytag_detect/run_live.sh --no-rtsp
TINYTAG_THRESHOLD_BACKEND=scalar /app/tinytag_detect/run_live.sh --no-rtsp
TINYTAG_THRESHOLD_BACKEND=freertos /app/tinytag_detect/run_live.sh --no-rtsp
```

FreeRTOS live verification defaults **on**: each threshold output is compared
with OpenCV before contours run. Its extra OpenCV pass is included in the live
threshold/profile time, although the `[threshold-offload]` transfer/compute
timing excludes verification. After standalone and live correctness checks pass,
disable verification for a performance comparison:

```sh
TINYTAG_THRESHOLD_BACKEND=freertos TINYTAG_THRESHOLD_VERIFY=0 \
  TINYTAG_THRESHOLD_POLL_US=50 /app/tinytag_detect/run_live.sh --no-rtsp
```

Repeat baseline/offload/offload/baseline on the same replay or stable scene.
Record crop threshold time, whole-loop time, FPS, result age, detected tags and
any fallback/validation failures. The backend only applies to the live/replay
executable; offline `tinytag_detect` and `point_ldc_check` remain unchanged.

For this synchronous partition, approximate the expected loop-time change as
the sum, over ROIs, of `offload total - baseline OpenCV threshold time`. No
speedup is assumed. If the small worker is useful but transfer dominates,
later experiments can test registered source buffers, batched jobs and bounded
overlap. Those require a separate ownership/scheduling design.

Do not extrapolate one universal small/big CPU ratio from box filtering:
contours have different branch/memory behavior, dictionaries different work,
and OpenCV on A53 may use optimized instructions absent from the scalar worker.
This experiment establishes one real data point plus transport overhead.

### Account for different CPU clocks

The C906L runs at a lower frequency than the A53 in this setup. Record the
actual clock settings for each run, together with any scaling policy and load;
do not infer current clocks from advertised maximum frequencies. Compare the
same portable kernel on both cores to characterize the port, and compare
complete offload time against the OpenCV A53 path to evaluate application use.

Report both measured elapsed time and cycles per pixel when possible. For the
remote worker, `remote_cycles / (width * height)` is the recorded cycle count
per pixel; it includes FreeRTOS preemption. With a stable cycle-counter clock,
`remote_cycles / remote_compute_us` estimates effective MHz over that interval.
It is a diagnostic estimate, not independent confirmation of the clock setting.
The completion's `timer_hz` is the timebase frequency (25 MHz here), **not**
the C906L CPU clock. Frequency normalization does not remove differences in
instruction scheduling, SIMD, caches, memory bandwidth or preemption.

For the current synchronous worker, evaluate measured `prepare + wait + finish`
against the original A53 threshold time: freeing A53 CPU time does not by
itself reduce frame latency. A later asynchronous partition can improve
throughput even if the worker takes longer, provided useful A53 processing
overlaps it and the worker sustains the required job rate. Include queueing,
data ownership, communication and result deadlines in that assessment.

## Decision after the first successful board offload benchmark

**Keep ROI thresholding on the Linux A53. Rule out the tested synchronous
FreeRTOS threshold offload for the production pipeline.** It increases elapsed
threshold time at every measured ROI size. At 640×360, total time is 24.66 ms
versus 4.47 ms for the original OpenCV path; at 1280×800 it is 109.93 ms versus
17.99 ms. Busy polling provides essentially no improvement for those sizes and
occupies the A53 while it waits. Sleep polling consumes less A53 CPU, but its
CPU saving at large sizes is modest relative to the added frame latency.
This is a decision about this implementation and workload; it does not
establish that all FreeRTOS work or all small-core offloads are unsuitable.

The complete 44 timing rows from the user-provided console output are saved in
[the benchmark TSV](benchmarks/duo-s-threshold-freertos.tsv), with `poll_us`
added to distinguish the two runs. All original numeric timing fields are
retained. Source board result directory:
`/tmp/tinytag-threshold-19700101-000111`. That timestamp is the board's unset
wall clock, not the real experiment date. The underlying validation logs and
individual iteration timings have not been retrieved; this record preserves
the reported aggregates. Both runs used 100 measured iterations, 5 warmups,
synthetic texture, kernel 15 and threshold 3. The successful launcher checks
every output against OpenCV and exits on any mismatch.

### Are we using FreeRTOS correctly?

The source audit supports functional correctness of the tested mechanism:

- Mailbox ISR and command task dispatch to a dedicated worker queue. The
  image kernel runs in task context and makes no per-pixel FreeRTOS calls.
- No allocation occurs inside the image kernel. Column scratch is static;
  the cutoff table is on the worker stack. Firmware assembly reserves
  1312 bytes for the kernel frame, within the configured 8192-byte task stack;
  actual stack high-water usage has not been measured.
- Linux and firmware explicitly flush/invalidate shared buffers during
  ownership changes. Completion is published after output handoff. Successful
  output comparisons support this protocol for the tested runs.
- The vendor startup enables instruction/data caches, branch prediction and
  data prefetch. There is no evidence here that caches were globally omitted.
  Runtime cache-control state and attributes for the ION address range have
  not been independently read back.
- Worker computation timing encloses the C kernel and excludes its input/output
  cache-maintenance calls. Queueing and notification do not explain the
  approximately 24 ms compute interval at 640×360.

However, the experiment is not a controlled measurement of OS overhead:

- C906L counters imply approximately 425 MHz. A subsequent board clock report
  independently shows C906L at 425 MHz and A53 at 800 MHz (see below);
  frequencies were not traced continuously during the benchmark.
  Milk-V's [Duo S specifications](https://milkv.io/duo-s)
  list the small C906 at 700 MHz; the counter estimate is not a confirmed
  configuration or a hardware limit. At 700 MHz, ideal clock-proportional
  scaling of 24.09 ms from 425 MHz gives about 14.62 ms at 640×360, still
  roughly 3.3 times the measured OpenCV A53 time. This is an extrapolation,
  not a 700 MHz measurement, and memory-bound costs may scale differently.
  Clock differences must be separated from cycles
  per pixel before attributing a gap to software.
- A53 uses `-O3`; the vendor firmware uses `-O2`, `-fno-builtin` and
  `-fno-strict-aliasing`. The generated RV64 kernel is scalar and has stack
  spills and 32-bit arithmetic/address conversions. Their costs are not
  isolated. Matching source does not mean matching machine-code efficiency.
- The worker has priority `tskIDLE_PRIORITY + 1`, permitting vendor tasks and
  interrupts to preempt it. Computation counters include that time; no
  preemption accounting was collected. Tight large-ROI timing distributions
  support repeatability but do not exclude regular interruption overhead.
- Sleep polling repeatedly invalidates a completion cache line and re-enters
  Linux cache-maintenance APIs. Its CPU cost is measurable; this is not an
  interrupt-driven asynchronous application partition.

Thus the measured slowdown is real, but its cause remains incompletely
isolated. A useful follow-up would match optimization settings, read back
clocks/cache state, account for worker interruptions, and compare the same
kernel under bare-metal with matching buffers and cache configuration. A
separate OpenCV port can assess algorithm/code-generation changes. Changing
both the OS and algorithm simultaneously cannot identify which helped.

Clock-source audit: the selected SDK target is `CONFIG_ARCH="arm64"`.
`fsbl/plat/cv181x/platform.c` programs C9061 explicitly in its RISC-V clock
initialization branches (including a 594 MHz setting in one mode), whereas
the corresponding ARM branch programs A53 and does not make that same
C9061 write. The Linux clock driver exposes `clk_c906_1`, its source and
divider. The board's subsequent `/sys/kernel/debug/clk/clk_summary` reports:

| Clock | Parent | Reported rate |
| --- | --- | ---: |
| `clk_a53` | `clk_mpll` (800 MHz) | 800 MHz |
| `clk_c906_1` (worker) | `clk_tpll` (850 MHz) | 425 MHz |
| `clk_c906_0` | `clk_fpll` (1500 MHz) | 750 MHz |

The relevant excerpt is saved in
[the clock record](benchmarks/duo-s-threshold-clock-summary.txt). C9060's
750 MHz is not the frequency of the running Linux A53 or the FreeRTOS worker.
The 425 MHz worker rate is consistent with TPLL divided by two and with the
independent cycle/time counter estimate. No clock setting was changed.

For 640×360 in the sleep-poll run, C906L compute time divided by portable A53
elapsed time is `24085.34 / 4129.63 ≈ 5.83`. The reported frequency ratio is
`800 / 425 ≈ 1.88`. Multiplying each elapsed time by its reported frequency
gives a ratio of approximately 3.10 in clock-normalized compute intervals.
Using A53 thread CPU time instead gives approximately 3.17. These estimates
include different machine code and, for the remote interval, possible
preemption; they are not a direct measurement of retired instructions or
OS-only overhead. At this size the worker recorded about 44.4 cycles/pixel,
versus approximately 14.0 A53 cycles/pixel estimated from thread CPU time.
The lower clock explains part, but not all, of the measured gap. Merely
switching operating systems or increasing the clock cannot be assumed to
remove the remaining difference.

### SDK performance clock profile

The initial benchmark used a board configuration with `CONFIG_OD_CLK_SEL`
disabled. Future `milkv-duos-glibc-arm64-sd` image builds now enable it by
default in the board defconfig. See the repository README's power and
performance profiles for selection and rollback instructions. Enabling it
selects the existing FSBL `sys_pll_od()` profile:

| Component | Current clock report/config | OD profile |
| --- | ---: | ---: |
| A53 | 800 MHz | 1000 MHz |
| TPLL | 850 MHz | 1400 MHz |
| C906L | TPLL/2 = 425 MHz | Expected TPLL/2 = 700 MHz |
| TPU | FPLL/3 = 500 MHz | TPLL/2 = 700 MHz |
| Core supply | Not measured | SDK requests PWM setting documented as 1.00 V |
| DDR3 | Configured 1866 MT/s | Unchanged by this profile |

The A53 and TPLL/TPU settings are explicit in
`fsbl/plat/cv181x/platform.c:sys_pll_od`. In ARM builds it does not explicitly
rewrite the C906L source/divider, so the expected 700 MHz follows from the
board's current TPLL/2 configuration. Verify the rate after boot. The SDK
profile also changes audio and video clocks; this is a coupled boot-time
profile, not two isolated CPU-frequency changes.

The current Linux kernel has `CONFIG_CPU_FREQ` disabled. There is no cpufreq
governor setting that activates this profile. Build a fresh FIP/FSBL, with
the intended worker firmware, then install that FIP on the mounted boot
partition and reboot. A whole SD image/rootfs rebuild is unnecessary for
this clock-only experiment. Avoid using the existing BLCP-only repacking
helper to activate OD: that retains the base FIP's old FSBL clock setup.

SDK commands for an OD FIP build (inside Docker; not executed during this
clock audit):

```sh
cd /home/work
source build/envsetup_milkv.sh milkv-duos-glibc-arm64-sd
setconfig OD_CLK_SEL=y
clean_fsbl
build_fsbl
```

Preserve the original FIP before rebuilding and use the usual board FIP
backup/restore procedure. Output is
`install/soc_sg2000_milkv_duos_glibc_arm64_sd/fip.bin`. For subsequent full
builds, `CONFIG_OD_CLK_SEL=y` is now retained in the selected board defconfig;
reselecting that defconfig restores the performance default even if a temporary
`setconfig OD_CLK_SEL=n` was used for a normal-profile measurement.
Clean FSBL first because changing compile-time clock defines must rebuild
its objects, not reuse a previous normal-clock binary.

DDR is initialized and trained during boot through a separate DDR PHY and
timing configuration; it is not reported in the provided Linux clock tree.
The selected config is `ddr3_1866_x16`, nominally about 933 MHz clock and
1866 MT/s data transfer. Confirm the actual `Data rate=...` boot message.
A generic `ddr3_2133_x16` profile exists, but its presence does not establish
qualification of the Duo S's SiP memory for 2133 MT/s. Do not treat it as a
supported maximum for this board or change the DDR PLL live.

Under ideal frequency scaling, A53 CPU-bound processing at 1 GHz takes
80% of its 800 MHz time; C906L CPU-bound processing at 700 MHz takes about
61% of its 425 MHz time. TPU compute may also benefit from 500→700 MHz.
Memory traffic and scheduling limit those estimates. Recheck byte equality,
camera capture, actual TinyTag loop/result age and preview/RTSP performance,
plus steady-state temperature and tail latency, before making OD the
production default. Keep the prior benchmark record labeled 800/425 MHz.

The user subsequently rebuilt/installed the OD FIP and rebooted. The new
board clock report verifies A53 at **1000 MHz**, C906L at **700 MHz** from
TPLL/2, and TPU at **700 MHz**. TPLL is 1400 MHz and MPLL is 1000 MHz.
Video-codec AXI is now 450 MHz (previously 360 MHz); VIP AXI remains
300 MHz. Camera clock `clk_cam1` reports 24 MHz. The relevant excerpt is
preserved in [the OD clock record](benchmarks/duo-s-threshold-clock-summary-od.txt).
DDR and actual core voltage are not established by this Linux report.
The original 44-row benchmark TSV describes the earlier 800/425 MHz runs;
the separate OD result record below describes 1000/700 MHz. Stop the live app before the
standalone OD threshold benchmark to keep competing Linux/firmware load
comparable with the earlier run, then measure the complete live pipeline
separately. Do not merge results from the two clock profiles.

## Detailed verification history

- Optimized portable C reference test: 1176 cases passed with undefined-behavior sanitizer
  in Docker, including replicated borders, saturated subtraction and stride
  padding. AddressSanitizer could not run reliably in this container environment.
- Linux A53 benchmark/live and C906L firmware cross-builds: completed.
- FIP payload comparison passed: only the BLCP2ND firmware payload changed;
  the original small-core entry address was retained. Firmware ELF text grew
  from 75648 to 77862 bytes (+2214); BSS from 707424 to 712632 (+5208).
  The worker also allocates a 1024-word RV64 task stack (8192 bytes) and a
  small queue from the existing FreeRTOS heap. The firmware linked within
  the existing 2 MiB reservation.
- First board local-only validation failed the scalar/OpenCV comparison. No
  timing conclusion is valid from that run. The original port used exact mean
  rounding; inspection of OpenCV 3.2 identified a different small-window
  fixed-point normalizer, including the normal 15×15 window. The port now
  follows that arithmetic and reports the first mismatching pixel, input,
  OpenCV mean, dimensions, window, threshold, pattern and header version.
  The subsequent board rerun passed all 1080 scalar/OpenCV equality cases.
- Read-only inspection of `/mnt/sda_500gb/git_repo/opencv-mobile-metalv`
  (OpenCV 4.12) found the ushort box-filter reciprocal calculation in
  `modules/imgproc/src/box_filter.simd.hpp`, but with a 23-bit shift rather than
  OpenCV 3.2's 16-bit shift. It is not a byte-equivalent replacement for the
  baseline. Its bare-metal build guides are
  useful references for future C++/OpenCV decoder work; this threshold
  experiment retains the small plain-C worker and the SDK OpenCV 3.2 baseline.
- Board local-only rerun: `aarch64`, 20 measured iterations and 5 warmups,
  synthetic texture, 15×15 window; OpenCV reported 8 configured threads.
  Mean elapsed times (microseconds):

  | ROI | OpenCV A53 | Scalar A53 |
  | --- | ---: | ---: |
  | 32×32 | 65.23 | 58.57 |
  | 64×64 | 163.74 | 148.14 |
  | 128×128 | 382.76 | 585.03 |
  | 200×200 | 839.88 | 1348.08 |
  | 320×240 | 1421.43 | 2555.20 |
  | 640×360 | 4407.59 | 8082.78 |
  | 1280×800 | 17992.83 | 34313.34 |

  These are A53 measurements only: `--local-only` sends no firmware jobs,
  so zero-valued remote columns do not measure C906L performance. With only
  20 samples, small-ROI differences are provisional; at 200×200 and above
  the scalar mean is approximately 1.6–1.9 times the OpenCV mean. Eight
  configured OpenCV threads does not establish that box filtering used eight
  workers (Linux has one big core in this setup).
- On-board transport, camera stability and small-core performance: pending.
  No numerical C906L speed or end-to-end gain is claimed yet.
- The first offload attempt timed out with no completion. The user confirmed
  that the board had not rebooted after FIP installation; reboot is required
  to activate the worker. This run provides no remote timing evidence.
- Optimization after the initial A53 baseline: cutoff-table comparison and
  separate border/interior loops, with compiler alias information for column
  scratch. A Docker native x86 comparison against the previous rolling-sum
  kernel checked equal output for all seven benchmark sizes. Alternating
  before/after measurements for 200×200 through 1280×800 took approximately
  0.49–0.51 times the prior kernel time. These are host measurements only,
  with allocations outside timing; A53 and C906L speedups remain unmeasured.
  Expanded portable and board checks include thresholds 1, 127 and 254 to
  exercise the inverted comparison near its limits.
- Board cutoff-table rerun passed all 1890 scalar/OpenCV checks, with 100
  measured iterations and 5 warmups. Mean and median elapsed times in
  microseconds:

  | ROI | OpenCV mean | Portable mean | OpenCV median | Portable median |
  | --- | ---: | ---: | ---: | ---: |
  | 32×32 | 66.84 | 31.99 | 56.60 | 30.64 |
  | 64×64 | 132.34 | 110.91 | 121.04 | 102.68 |
  | 128×128 | 362.19 | 389.91 | 345.56 | 380.48 |
  | 200×200 | 844.29 | 1047.26 | 832.72 | 941.60 |
  | 320×240 | 1422.91 | 1768.76 | 1409.20 | 1755.04 |
  | 640×360 | 4494.25 | 5354.90 | 4402.64 | 5269.36 |
  | 1280×800 | 17846.31 | 23795.61 | 17553.44 | 23375.40 |

  For 128×128 and larger, portable median times fell about 29–31% from the
  initial board run. These runs had different sample counts and occurred
  separately. The remaining gap against OpenCV in this run is about 10–33%
  by median; the mean at 200×200 is more affected by slow outliers.
- Further optimization after that board result processes four interior pixels
  per iteration, loading independent window deltas ahead of the running sum;
  explicit non-overlap information covers source, destination and column
  scratch. Portable UBSAN checks (1176 cases), cross-builds and FIP payload
  checks passed again. Docker native x86 timings for larger crops took about
  0.40 times the original pre-cutoff kernel time, with equal output at all
  seven benchmark sizes. Board timings for this further revision are pending.
  Current firmware text is 78372 bytes (+2724 over the original), with BSS
  still 712632 bytes. Earlier text-size figures describe the cutoff-only
  revision.
- Board four-pixel-loop rerun passed all 1890 equality checks, again with
  100 measured iterations, 5 warmups, synthetic texture and a 15×15 window:

  | ROI | OpenCV mean µs | Portable mean µs | OpenCV median µs | Portable median µs |
  | --- | ---: | ---: | ---: | ---: |
  | 32×32 | 64.97 | 27.91 | 56.64 | 27.60 |
  | 64×64 | 125.07 | 94.77 | 120.92 | 86.28 |
  | 128×128 | 354.99 | 311.45 | 348.92 | 301.56 |
  | 200×200 | 842.28 | 750.07 | 830.52 | 739.00 |
  | 320×240 | 1518.08 | 1360.47 | 1411.72 | 1346.00 |
  | 640×360 | 4471.39 | 4069.58 | 4399.12 | 4014.24 |
  | 1280×800 | 17991.77 | 18088.84 | 17559.52 | 17741.08 |

  Portable medians are lower at every size through 640×360, and about 1%
  higher at 1280×800. Compared with the initial portable board baseline,
  medians at 200×200 and larger fell approximately 44–48%; these were
  separate runs, not an interleaved before/after comparison. The A53
  efficiency gate is satisfied for this synthetic workload. No A53 NEON
  intrinsics were added; compiler-generated SIMD remains possible under
  the existing A53 build flags.
  OpenCV 3.2 has NEON code in its box-filter column operations, and the SDK
  imgproc binary contains NEON instructions. The measurements do not isolate
  the fraction of OpenCV's time saved by NEON. The portable algorithm retains
  RISC-V compatibility and the existing `rv64imafdc` firmware target.
  Install/reboot the newly rebuilt FIP before comparing C906L and A53 so
  both execute this optimized algorithm revision. An older FIP can pass
  output checks but would give an obsolete worker speed comparison.
- First completed remote benchmark: both polling modes completed 100
  iterations per size with every output checked. Representative means in
  microseconds (the sleep-poll run):

  | ROI | OpenCV A53 | Portable A53 | C906L compute | Offload total | A53 thread CPU during offload |
  | --- | ---: | ---: | ---: | ---: | ---: |
  | 32×32 | 56.53 | 27.98 | 129.35 | 252.91 | 46.36 |
  | 64×64 | 121.77 | 88.00 | 451.90 | 589.97 | 91.06 |
  | 128×128 | 355.15 | 303.82 | 1709.34 | 1810.31 | 271.09 |
  | 200×200 | 839.80 | 746.08 | 4120.47 | 4302.39 | 623.19 |
  | 320×240 | 1426.77 | 1423.09 | 7953.64 | 8207.42 | 1154.44 |
  | 640×360 | 4474.47 | 4129.63 | 24085.34 | 24655.66 | 3333.52 |
  | 1280×800 | 17991.61 | 18112.56 | 107633.03 | 109925.98 | 14618.89 |

  At 640×360 the compute interval alone accounts for about 98% of total
  offload time. Busy polling changed total mean to 24611.41 µs, with
  computation still 24069.88 µs; A53 thread CPU grew to 24279.50 µs.
  The no-op means were 121.64 µs with sleep polling and 14.35 µs with busy
  polling. Polling changes notification latency, not the large-ROI compute
  bottleneck. Repeated cache-invalidation polling also consumes appreciable
  A53 CPU in the sleep mode.
  Recorded cycles divided by compute time consistently estimate approximately
  425 MHz for C906L. At 640×360 and 1280×800 the worker takes about 44–45
  cycles/pixel. A53 frequency has not been measured in this experiment, so
  the roughly sixfold portable elapsed-time gap cannot yet be partitioned
  quantitatively into clock and per-cycle efficiency differences.
  Remote timing includes preemption; these results do not isolate FreeRTOS
  scheduler cost. For this workload the synchronous offload is unsuitable
  for reducing frame latency. Other kernels need separate measurement.

### Performance-profile benchmark: A53 1000 MHz / C906L 700 MHz

The user's OD run completed both polling modes with 100 measured iterations
per size and every remote result checked against OpenCV. All 44 aggregate
rows are saved in [the OD benchmark TSV](benchmarks/duo-s-threshold-freertos-od.tsv).
The board's original result directory is `/mnt/data/tinytag-threshold-od`.
Kernel, input, warmup and sample counts match the previous remote run.
Linux/firmware competing-task activity was not captured with the pasted
aggregates. The new default flag and board profile documentation are in the
repository README; reselecting the ARM64 SD board was checked to retain
`CONFIG_OD_CLK_SEL=y` and DDR1866.

Sleep-poll means, in microseconds:

| ROI | OpenCV A53 | Portable A53 | C906L compute | Offload total | A53 thread CPU during offload |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32×32 | 45.52 | 22.16 | 81.38 | 125.66 | 25.71 |
| 64×64 | 97.68 | 69.55 | 281.97 | 355.35 | 52.58 |
| 128×128 | 369.40 | 242.79 | 1059.47 | 1183.91 | 166.31 |
| 200×200 | 683.55 | 597.90 | 2551.61 | 2733.33 | 378.09 |
| 320×240 | 1158.24 | 1096.89 | 4949.27 | 5161.94 | 682.67 |
| 640×360 | 3668.21 | 3279.33 | 15111.00 | 15670.56 | 1942.34 |
| 1280×800 | 14973.67 | 14465.66 | 67780.74 | 70159.48 | 7256.60 |

At 640×360, portable A53 mean improved from 4129.63 to 3279.33 µs (about
21% shorter), and remote compute from 24085.34 to 15111.00 µs (about 37%
shorter). At 1280×800 the corresponding reductions are about 20% and 37%.
These are separate runs; improvements broadly follow the changed clocks,
but are not isolated CPU-frequency experiments because OD changes several
shared clocks and voltage together. Counter/time ratios now estimate about
700 MHz independently of the clock-tree report. At 640×360 recorded worker
cycles per pixel rose from about 44.4 to 45.9, rather than staying exactly
constant; fixed DDR speed, memory stalls or changed interference could
contribute, but have not been isolated.

The large-ROI conclusion remains: total offload at 640×360 is about 4.3
times the OpenCV A53 time, and at 1280×800 about 4.7 times. Sleep polling
reduces calling-thread CPU consumption (about 1.94 ms versus the portable
A53's 3.25 ms thread CPU at 640×360) while increasing elapsed time. It is
still unsuitable as a synchronous replacement for a latency-sensitive
threshold stage; asynchronous throughput benefits are not demonstrated.

The busy-poll OD run contains additional Linux-side delays for 200×200,
320×240 and 640×360. For example OpenCV at 640×360 reports 7653.62 µs
elapsed but 3291.11 µs calling-thread CPU, versus 3668.21/3277.76 in the
sleep-poll run. Portable A53 shows the same pattern. Remote compute is
essentially unchanged between polling modes (15111.00 versus 15106.88 µs
at that size), so the increased wall times do not show a slower C906L
kernel. They are consistent with Linux descheduling or other interference;
the exact competing task/event is unknown. Treat that portion's means and
p95 as the observed loaded run, not a clean algorithm-speed comparison.
