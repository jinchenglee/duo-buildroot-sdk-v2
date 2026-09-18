# TinyTag on Milk-V Duo S -- handover

Orientation for picking this work up cold. The detailed experiment log and
remaining roadmap are in `docs/live-camera-workplan.md`; sections 1-9 are
complete as a hardware-verified checkpoint. Section 10, hardware lens
distortion correction, and section 11, native OV5647 1280x720@60, are
explicitly not started.

## Preview scheduling checkpoint

The final order-balanced benchmark of no RTSP, colour `--rtsp`, and exact-luma
`--rtsp-luma` is hardware verified on the accessible Duo-S. These are the
order-balanced means from the posted 2026-09-18 runs. Time values are ms;
`otherCPU` is process CPU minus detector CPU.

| Preview nice | Mode | fps | loop | detCPU | procCPU | otherCPU | core% | acqAge | resultAge | release | crop |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | no-rtsp | 30.00 | 9.80 | 7.75 | 9.73 | 1.99 | 29.2 | 3.70 | 13.49 | 0.04 | 7.46 |
| 0 | colour | 30.01 | 11.00 | 7.99 | 17.05 | 9.05 | 51.1 | 8.88 | 19.88 | 0.04 | 8.50 |
| 0 | luma | 29.99 | 15.07 | 7.87 | 16.35 | 8.48 | 49.0 | 3.75 | 18.84 | 5.13 | 7.64 |
| 10 | no-rtsp | 30.01 | 9.40 | 7.38 | 9.38 | 2.00 | 28.1 | 3.70 | 13.10 | 0.03 | 7.08 |
| 10 | colour | 29.98 | 12.25 | 8.40 | 17.71 | 9.31 | 53.1 | 3.88 | 16.14 | 0.05 | 9.76 |
| 10 | luma | 29.95 | 10.03 | 7.88 | 16.42 | 8.55 | 49.2 | 3.76 | 13.79 | 0.07 | 7.65 |

All modes remained camera-bound at about 30 fps. Nice 10 lowered exact-luma
loop time by 5.04 ms and result age by 5.05 ms; it lowered colour result age
by 3.74 ms. Preview other-thread CPU stayed essentially unchanged, so this is
a scheduling-latency improvement rather than less work.

Benchmark column guide: `fps` is detector frames per second. All remaining
time columns are milliseconds per detector frame: `loop` is detector wall time;
`detCPU` is detector-thread CPU time; `procCPU` is all application-thread CPU
time; and `otherCPU` is `procCPU - detCPU`. `core%` is whole-process CPU use as
a percentage of the one Linux core. `acqAge` is capture-to-detector-start
latency, while `resultAge` is the software approximation `acqAge + loop`.
`release` is detector-thread frame handoff/release wall time and is not an
independent preview-cost measure. `crop` is full-resolution AprilTag crop
decode time and varies with scene content.
Code review shows normal luma handoff does not call `CVI_VPSS_ReleaseChnFrame` on the
detector thread: it wakes the preview worker, which can preempt the detector on
the single Linux core before the next wall timestamp. Colour instead creates a
third VPSS output, apparently shifting cost into frame readiness. Do not infer
an optimization from `busy` alone.

The binary now reports `proc-cpu` beside detector-thread `cpu`, supports a true
`--no-rtsp` override, and exposes preview-worker niceness as an A/B control.
`run_preview_bench.sh` retains raw logs, rejects startup windows below 25 fps,
balances run order over two rounds, and reports fps, loop wall time, detector
CPU, process CPU, other-thread CPU, one-core utilization, acquisition age,
approximate result age, release wall time, and crop time.

Preview niceness now defaults to 10. Use the default benchmark for a production
measurement, or explicitly set zero for the historical normal-priority A/B
baseline:

    TINYTAG_BENCH_PREVIEW_NICE=0 \
      /app/tinytag_detect/run_preview_bench.sh

Do not collapse colour `--rtsp` into luma based only on the old `release` wall
bucket; exact-luma remains the production preview.

## Build and deploy

Always build in the `duodocker` container. The app links against the TPU SDK
created by a successful full SDK build:

    docker exec duodocker /bin/bash -c \
      'cd /home/work && ./apps/tinytag_detect/build.sh milkv-duos-glibc-arm64-sd'

The generated overlay is under
`device/milkv-duos-glibc-arm64-sd/overlay/app/tinytag_detect/`. Iterating on
the app does not require reflashing. The Duo-S is `root@192.168.42.1`; in the
current development environment SSH execution is unavailable, but SCP works:

    scp -O device/milkv-duos-glibc-arm64-sd/overlay/app/tinytag_detect/tinytag_detect_live \
      root@192.168.42.1:/app/tinytag_detect/

Use SCP readback plus `sha256sum` when deployment identity matters.

## Benchmark reproduction checklist

This follow-up is intended directly on top of checkpoint `c9a6f29c4`. The
current source was cross-built successfully in `duodocker`; shell syntax,
`git diff --check`, staged-artifact identity, and a synthetic parser run also
passed. The Duo-S measurement above completed the original gate; use this
checklist to reproduce it after material preview or scheduling changes.

1. Build from the new commit using the command above. Do not reuse a binary
   from `c9a6f29c4`, because it lacks `proc-cpu`, `--no-rtsp`, and the preview
   priority control.
2. Stop any existing `tinytag_detect_live` process on the board. From the SDK
   host, deploy both new artifacts:

       OVERLAY=device/milkv-duos-glibc-arm64-sd/overlay/app/tinytag_detect
       sha256sum "$OVERLAY/tinytag_detect_live" \
         "$OVERLAY/run_preview_bench.sh"
       scp -O "$OVERLAY/tinytag_detect_live" \
         "$OVERLAY/run_preview_bench.sh" \
         root@192.168.42.1:/app/tinytag_detect/

   On the board, make the runner executable and use `sha256sum` again to
   confirm that the deployed files match the host:

       chmod 0755 /app/tinytag_detect/run_preview_bench.sh
       sha256sum /app/tinytag_detect/tinytag_detect_live \
         /app/tinytag_detect/run_preview_bench.sh

3. Keep the camera, scene, lighting, and RTSP-client state unchanged for both
   runs. Prefer no connected RTSP client; if one is used, it must reconnect
   consistently for every colour and luma case. Run the production-priority
   test on the board:

       TINYTAG_BENCH_OUT_DIR=/tmp/tinytag-preview-nice10 \
         /app/tinytag_detect/run_preview_bench.sh

4. Run the same order-balanced test at normal preview priority for comparison:

       TINYTAG_BENCH_PREVIEW_NICE=0 \
       TINYTAG_BENCH_OUT_DIR=/tmp/tinytag-preview-nice0 \
         /app/tinytag_detect/run_preview_bench.sh

   Each command runs six 12-second cases: two rounds with reversed
   no-RTSP/colour/luma order. The runner requires clean shutdown and at least
   one steady camera window at 25 fps or higher per case.
5. Preserve both stdout summaries. Pull the raw logs and TSV results back to
   the host if needed:

       scp -O -r root@192.168.42.1:/tmp/tinytag-preview-nice0 .
       scp -O -r root@192.168.42.1:/tmp/tinytag-preview-nice10 .

6. Before changing defaults, verify the raw logs show zero detector stale
   frames, zero model-input pair mismatches, zero preview ownership errors,
   and bounded borrowed surfaces. Compare `procCPU` and `otherCPU` against
   no-RTSP to measure actual preview CPU. Compare acquisition plus result age
   and `[tails]` across modes; do not optimize the `release` bucket alone.
   Nice 10 is the production default based on the 2026-09-18 run. Retain it
   only while these checks continue to show no ownership or sustained-backlog
   problems. Decide whether colour `--rtsp` should become a luma alias only
   after separate measurements and an explicit decision.

## Production command and architecture

    /app/tinytag_detect/run_live.sh --rtsp-luma

The production model is `tinytag-v40c.int8.cvimodel`. The OV5647 captures
1920x1080 RAW at 30 fps, then VI/ISP feeds one VPSS group:

- channel 0: 1280x720 YUV400 for full-resolution CPU crop decoding and borrowed
  exact-luma RTSP preview;
- channel 1: 640x360 YUV400 bound directly to the ordinary compact TPU input.

Independent latest-value capture slots drain both channels. Detection starts
from the newest channel-1 frame immediately. After synchronous TPU inference,
the code requires the matching channel-0 `u32TimeRef` before crop/decode. Thus
channel-0 readiness is hidden behind inference rather than placed in front of
it. A newer channel-0 sequence discards the result instead of pairing unlike
frames.

Direct binding of the ordinary compact model is allowed only for this guarded
layout: uint8 grayscale, exact dimensions, stride 640 == width, sufficient
plane length, and tensor-size equality. The same-frame validation compared all
75,600 output floats against copied input and reported `exact=1, max-abs=0`.
Set `TINYTAG_LIVE_DIRECT_COMPACT_INPUT=0` for the copied-input baseline and
`TINYTAG_LIVE_VALIDATE_COMPACT_INPUT=1` to rerun the expensive proof.

The preview worker borrows channel-0 Y; it does not copy the 0.9 MB luma plane.
It is latest-value and deliberately subordinate to detection. Slow clients may
drop or delay preview frames without reducing detector freshness or throughput.

## Hardware-verified state

- 31.2-31.5 detector fps, camera-bound, with zero stale frames in normal runs.
- Acquisition age is typically about 3.7 ms.
- Persistent VPSS mapping costs about 0.11 ms after five initial misses.
- Direct model input reduces preprocessing from about 1.6 ms to 0.01 ms.
- TPU inference is about 2.1 ms.
- Direct-logit proposal decoding reduces that stage from about 0.91 ms to
  0.07-0.08 ms with identical saved-frame proposals.
- Crop decode is the dominant scene-dependent CPU stage. The profile separates
  threshold, contour, quad, marker decode, refine, pixels, candidates, attempts,
  and markers.
- A 100 ms injected preview delay preserved detector fps/freshness, bounded
  borrowed ownership to two frames, and replaced pending preview frames.
- Bad-model startup exits before camera acquisition. Direct and copied-input
  modes shut down cleanly. Two automated back-to-back SIGINT runs both reached
  31.2-31.4 fps, emitted `[camera] stopping`, and had zero stale frames.
- The OV5647 image arrives horizontally mirrored on this module. VI hardware
  mirror correction defaults on; without it neural proposals still look valid
  but AprilTag decoding fails because markers are chiral.

`run_live.sh` defaults per-tag stdout off because terminal or pipe backpressure
must not stall acquisition. Set `TINYTAG_LIVE_TAG_OUTPUT=1` only when needed.

## Rejected or deferred work

- IVE DMA is not used for preview. At 1280x720 it takes about 3.165 ms at
  291 MB/s versus borrowing VPSS Y at zero copy. IRQ wait consumes only about
  86 us caller CPU, so the measurements remain useful for future AMP work.
  The SDK wrapper discards the ioctl return, and the 1080p IRQ path exposes a
  driver timeout bug; use `run_dma_bench.sh`, which validates pixels.
- A persistent 1280x720 ArUco threshold workspace plus smaller initial contour
  reserves produced identical fixed-image output after its ordering bug was
  corrected, but regressed 500-run median crop decode from 4.188 to 4.281 ms
  and throughput from 124.2 to 122.7 fps. It was removed.
- VI-online mode did not improve the measured detector path; VI offline/VPSS
  online remains the default.
- VPSS wrap is a VPSS-to-VENC binding contract, not a completed-frame contract
  for CPU/TPU consumers, so it was rejected for detector channels.
- Colour-preview latest-value work and RGN/OSD are low return. Production is
  exact-luma preview, and preview latency is secondary.
- Absolute photon-to-result/display latency needs an external LED/GPIO test.
  Software acquisition and approximate result-age distributions are available.
- Hardware VPSS lens-distortion correction is planned in section 10 and must
  pass geometry, recall, and performance gates before enablement.
- Native OV5647 720p60 and sensor-derived rate plumbing belong to workplan
  section 11. Do not begin them unless explicitly requested.

## Runtime diagnostics

`[camera]` reports fps and per-frame wait, pair, map, pre, infer, proposal
decode, crop, release, output, wall service, detector-thread `cpu`, whole-process
`proc-cpu`, proposal count, stale/sequence counters, acquisition age, and
mapping-cache state.

`[preview]` reports its independent published/dequeued/encoded counts, drops,
sequence gaps, queue/VENC/RTSP time, pending/borrowed surfaces, and ownership
errors. Preview drops under stress are acceptable; detector stale frames are
not.

Every ten seconds `[tails]` reports p50/p95/p99/max for service wall time,
detector CPU, crop decode, acquisition age, and approximate result age.
`[crop-profile]` explains why crop cost changes with scene content. ROI area and
decode attempts matter more than proposal count alone.

Useful tools:

- `tools/tinytag_cvimodel/diagnose_frame.py`: investigate a captured miss;
- `tools/tinytag_cvimodel/reference_proposals.py`: FP32 proposal reference;
- `tools/tinytag_cvimodel/validate_cvimodel.py`: gated model validation;
- `apps/tinytag_detect/run_dma_bench.sh`: validated IVE DMA characterization.
- `apps/tinytag_detect/run_preview_bench.sh`: order-balanced no-RTSP/colour/luma
  process-CPU and latency comparison.

## Constraints and next steps

Do not change `roi_expand=1.5` or the operating point to hide performance or
recall problems. They are experimentally tuned, and crop cost scales with ROI
area.

Linux currently has one Cortex-A53, so CPU-bound threads compete for one core.
SG2000 also has a C906 running FreeRTOS. AMP offload is a later investigation;
include mailbox, cache-coherency, buffer-ownership, and handoff latency and keep
a Linux-only fallback.

The aligned-input compiler option and aligned-only runtime API remain for VPSS
layouts whose stride differs from compact width. The deployed fixed 640x360
layout does not require a separately converted aligned model.

Generated overlay binaries, launchers, samples, and DMA tools are ignored;
their sources live under `apps/tinytag_detect/`. The hardware-validated
cvimodel is the intentional tracked overlay exception.
