# Duo S bare-metal threshold diagnosis

This firmware runs the same portable threshold kernel and request processing
code as the FreeRTOS experiment, with the same `-O2`, RV64IMAFDC, LP64D compiler
settings. It keeps the vendor startup cache/prefetch settings and the same
ION input/output handover. It replaces task dispatch with a polling mailbox
loop, with interrupts disabled and no scheduler. It reports cache-control
registers and a firmware identity through unused completion fields.

This is a standalone benchmark firmware. Vendor small-core multimedia/audio
services are absent. Stop the camera/application and disable its automatic
startup for this experiment. Do not use it for live detection or streaming.
Linux still handles the normal command mailbox. A minimal FSBL transfer and
Linux initialization handshake is retained; other service commands are ignored.
Unexpected exceptions park the core and produce a Linux benchmark timeout.

## Build on the host

Run inside the existing Docker container, with Duo S ARM64 SD selected and
normal SDK/FIP outputs already built:

```sh
docker exec duodocker bash -lc 'cd /home/work && bash apps/tinytag_detect/build_bare_metal_threshold.sh'
```

Output: `apps/tinytag_detect/build_bare_metal_threshold/bundle` and
`tinytag-bare-metal-threshold.tar.gz` beside it. The normal SDK firmware and
FIP outputs are not replaced. The builder verifies that only BLCP_2ND changes
and its entry matches the generated firmware reservation. FSBL, clocks, DDR,
Linux and all other FIP payloads come from the selected base FIP. To retain the
measured OD profile, use an OD base FIP; this helper does not change clocks.

## Install on the board

Copy/extract the archive into `/mnt/data/bare-metal-threshold`. Use your own
current `/boot/fip.bin` as the recovery backup; `fip-restore.bin` in the bundle
is the build host's base FIP, which may differ from the board's current FIP.
Confirm `/boot` is mounted and there is enough room for both files. Keep the
backup also on the host or another medium so it is available if boot fails.

```sh
cd /mnt/data/bare-metal-threshold/bundle
sha256sum -c SHA256SUMS
cp /boot/fip.bin /mnt/data/fip-before-bare-metal.bin
cp fip-bare-metal.bin /boot/fip.bin
sync
reboot
```

## Measure after reboot

Ensure `tinytag_detect` and other camera/streaming applications are stopped.
First collect clocks, then run the same 100-iteration benchmark:

```sh
cat /sys/kernel/debug/clk/clk_summary > /mnt/data/bare-metal-clocks.txt
TINYTAG_THRESHOLD_ITERATIONS=100 \
TINYTAG_THRESHOLD_OUT_DIR=/mnt/data/tinytag-threshold-bare-metal \
  /mnt/data/bare-metal-threshold/bundle/run_threshold_bench.sh --bare-metal
```

The launcher tests sleeping polling (50 us) and busy polling (0 us). The
benchmark validates local OpenCV equivalence before timing, checks every remote
output, and rejects a firmware identity mismatch. TSV mode names are
`bare-metal-nop` and `bare-metal`. The raw `.log` reports cache and prefetch
registers; these controls do not prove that the entire ION mapping is cacheable.
Do not use timings from a failed validation. After a pending-job timeout,
reboot before retesting, as with the FreeRTOS experiment.

Compare **remote_compute_us** and **remote_cycles** at matching clocks first.
Compare **remote_service_us** to assess cache maintenance, and total/NOP times
to assess dispatch and Linux polling. If compute remains near the FreeRTOS
value (~15.1 ms for 640x360 at 700 MHz), scheduler removal has not solved the
kernel bottleneck. If compute drops substantially, investigate FreeRTOS
preemption/cache configuration next. Removing interrupts also removes interrupt
work, so a speedup alone cannot identify scheduler bookkeeping as the cause.
Linux CPU and elapsed times can still diverge due to Linux scheduling.

This first comparison deliberately keeps copies and the scalar kernel. It does
not introduce OpenCV, RVV, a different compiler optimization level, or an input
layout change. Those would confound the OS comparison and can be tested later.
## First board result

The user ran both polling modes with 100 measured iterations. The launcher
completed without a reported validation or firmware-identity failure. All 44
printed aggregate rows are preserved in
[the bare-metal TSV](../../../docs/benchmarks/duo-s-threshold-bare-metal.tsv).
This is a transcription of the supplied terminal output, not a download of the
board logs. Raw logs remain at `/mnt/data/tinytag-threshold-bare-metal` and the
clock report at `/mnt/data/bare-metal-clocks.txt`; neither was supplied yet.
Compute cycles divided by elapsed compute microseconds independently indicate
approximately 700 MHz on C906L in both polling modes.

Comparison with the earlier OD FreeRTOS run, using sleeping polling (50 us):

| ROI | FreeRTOS compute ms | Bare-metal compute ms | Change |
| --- | ---: | ---: | ---: |
| 32x32 | 0.08138 | 0.07864 | -3.37% |
| 64x64 | 0.28197 | 0.28042 | -0.55% |
| 128x128 | 1.05947 | 1.05445 | -0.47% |
| 200x200 | 2.55161 | 2.54551 | -0.24% |
| 320x240 | 4.94927 | 4.91455 | -0.70% |
| 640x360 | 15.11100 | 15.05961 | -0.34% |
| 1280x800 | 67.78074 | 67.83551 | +0.08% |

Removing FreeRTOS scheduling and interrupts did not materially improve this
kernel's computation time. These runs do not support attributing the large
A53/C906L gap to FreeRTOS overhead. Bare-metal is useful for diagnosis, but
does not make synchronous threshold offload competitive in this implementation.
This conclusion applies to this kernel and firmware, not every possible task.

Busy-poll NOP mean latency decreased from 11.21 us (FreeRTOS) to 6.21 us
(bare-metal). That is a useful dispatch improvement for very short jobs, but
about 5 us is negligible beside a 15 ms threshold computation. At 640x360,
bare-metal compute time is 15.05961 ms sleeping versus 15.06098 ms busy polling;
busy polling consumes much more A53 CPU without improving the remote kernel.

Linux elapsed times in this run show substantial interference. For example,
the 640x360 sleeping scalar baseline took 6.71657 ms wall time but 3.26088 ms
thread CPU time; its earlier OD baseline took 3.27933 ms wall and 3.24871 ms
CPU. The similar CPU times with different wall times are consistent with
Linux descheduling; the responsible activity is unknown. This affects local
and offload elapsed comparisons but not the stable bare-metal kernel counter
measurement. Do not interpret the inflated local wall times as an A53 kernel
regression or use them to claim a reduced offload penalty.

Next diagnosis should inspect generated instructions (including possible A53
auto-vectorization), retired-instruction counts and cycles per instruction,
plus C906L cache/memory attributes and load stalls. The reported cache control
fields are in the raw `.log`, not the TSV. Establish cache behavior before
changing algorithms or importing a different OpenCV implementation. There is
no evidence here that replacing FreeRTOS alone would improve detector speed.

## Restore

```sh
cp /mnt/data/fip-before-bare-metal.bin /boot/fip.bin
sync
reboot
```

If Linux fails to boot, restore the saved `fip.bin` on the SD boot partition
using another computer. Restore the application startup setting after returning
to the normal firmware.
