# tinytag_detect

Still-image TinyTag proposal detector for the Milk-V Duo S, running on the
SG2000 TPU via `cviruntime`.

A port of the K230 two-stage AprilTag detector (see
`buildroot-overlay/package/ai_demo/tinytag_detect` in the K230 SDK): a small
network proposes tag ROIs on a downscaled frame, then a traditional-CV decoder
reads the AprilTag 36h11 id out of each proposal at full resolution.

The default checkpoint is `tinytag_v7_synthetic_area_cost`, combining a coverage
mask with TinyTag ROI predictions. Its INT8 cvimodel is
`cvimodel/tinytag_v7_synthetic_area_cost.int8.cvimodel`; cviruntime exposes its
six-channel output as FP32. Stage one decodes the mask into separate tag
regions, using ROI peaks to split connected regions; stage two optionally runs
ArUco Nano on full-resolution crops with `--decode`.

Both `run_live.sh` and `run_tinytag.sh` select this model automatically. The
model is checked into the Duo S overlay and has been tested live on the board.
Use `TINYTAG_LIVE_MODEL` (live) or `TINYTAG_MODEL` (still images) to override it.
The older coverage checkpoint remains available. The v7 golden self-test bundle
is not provided; generate a matching bundle before using `--selftest`, or select
both the older model and its matching golden bundle explicitly.

The [progress and clean-image record](../../docs/duo-s-progress-seal.md)
summarizes the bare-metal comparison, Ethernet PHY diagnosis/fix and image
verification. The [worker diagnosis](../../docs/duo-s-ethernet-phy-diagnosis.md) preserves the
before/after evidence and kernel installation/recovery steps.

Threshold controls are separate:

- `--thres_heat f`: center-heat probability gate (default 0.30).
- `--thres_mask f`: optional final proposal mask-score gate (default 0,
  disabled), supported by six-channel coverage+ROI models. It filters the
  maximum assigned mask score before sorting and applying `--max`, including
  mask-only fallback proposals. It does not alter seed/grow thresholds (0.4/0.3).
- `--max N` caps current proposals and never fills unused slots. Historical
  blue boxes and recovery scans can exceed that count; the new mask gate does
  not remove maintained tracks. Their recoveries can use full resolution.

`--thres` has been removed; use the explicit names above. Live environment
variables are `TINYTAG_LIVE_THRES_HEAT` and `TINYTAG_LIVE_THRES_MASK`; still-image
variables are `TINYTAG_THRES_HEAT` and `TINYTAG_THRES_MASK`. The old environment
names are not used. Extra command-line arguments override launcher defaults.
A higher mask gate may suppress real tags or cause more history recovery;
compare recall, first-detection delay and total decode time when tuning it.

```sh
./run_live.sh --rtsp-luma --thres_heat 0.5 --thres_mask 0.5 --max 6
```

Overlapping decode crops now share a scan by default (`--merge-crops 1`).
This applies to both ordinary and adaptive decoding, independently of legacy
proposal IoU suppression (which the A+C model bypasses). Blue maintained blob
boxes keep their individual tracking state; pink boxes show the actual decoder
windows, including any recovery scans.

The execution planner merges same-resolution crops when at least half of the
smaller crop overlaps and the bounding union saves at least 10% of their summed
pixel area. It retains all original crop coverage, decodes multiple tags in
one invocation, and assigns results back to original member ROIs. Full scans
and half-size scans remain separate so audits are preserved. If a merged full
scan misses a recently tracked tag, its original crop is retried. Merging can
change thresholding at crop boundaries; new-tag recall still needs board tests.

`[crop-merge]` reports per-frame input/output scan counts and native pixel areas
before half-size scaling, plus additional recovery scans/pixels. Use actual
`[crop-profile]` pixels and crop/service timing to judge the net benefit:

```sh
./run_live.sh --rtsp-luma --merge-crops 1
./run_live.sh --rtsp-luma --merge-crops 0  # independent-crop comparison
```

Host coverage/policy tests and both ARM64 binaries are built successfully;
hardware latency, throughput and recall measurements are pending. The host
test can be run without the TPU/OpenCV SDK:

```sh
g++ -std=c++11 -O2 apps/tinytag_detect/tests/crop_merge_test.cc -o /tmp/crop_merge_test
/tmp/crop_merge_test
```

The adaptive ROI decode path and its A/B acceptance rules are in
[`docs/tinytag-adaptive-roi-rules.md`](../../docs/tinytag-adaptive-roi-rules.md).
It is enabled by default (`--adaptive-decode 1`); use `--adaptive-decode 0`
for full-resolution decoding. For large ROIs, adaptive mode tries a half-size
crop first and can defer some full-resolution scans. Its 10,000-pixel ROI-area
gate (the area of a 100×100 box) is provisional; recall and timing tests are
needed to choose a threshold. Set `--adaptive-min-roi-area N` to try a different
positive integer area in **full-resolution pixels**, for example
`--adaptive-decode 1 --adaptive-min-roi-area 22500` for the area of a 150×150
box. The gate is area, not minimum width and height; a 300×75 ROI also meets
22500. The known-small-tag guard, fallbacks and audit rules still apply.

The proposed training effort to reduce expensive false ROIs, including synthetic
data, recoverable tag-size limits, and cost-weighted losses, is recorded in
[`docs/tinytag-proposal-training.md`](../../docs/tinytag-proposal-training.md).

## Build and install

Run this **in the Docker container**, like every other build in this tree --
see "Always build in Docker" in the top-level README. Mixing host and
container builds leaves root-owned files that later break the SDK build with
what look like toolchain errors.

```sh
docker exec -it duodocker /bin/bash -c \
    "cd /home/work && ./apps/tinytag_detect/build.sh milkv-duos-glibc-arm64-sd"
```

The board argument defaults to whatever `device/target` points at, but a fresh
clone has no `device/target` (it is gitignored), so pass it explicitly until a
full build has been run once.

This is step 2 of 3. It must be preceded by a full `./build.sh` (which produces
the TPU SDK this links against) and followed by another (which bakes the staged
overlay into the image). Skipping it is silent: the image builds fine and
simply contains no detector.

A **failed** SDK build runs `clean_all` first and therefore wipes
`install/soc_<project>/`, TPU SDK included. If this script reports the SDK
missing, run a full successful build before retrying.

Cross-compiles against the cvitek TPU SDK that a full `./build.sh <board>` drops
in `install/soc_<project>/tpu_64bit/`, then installs into
`device/<board>/overlay/`:

```
app/tinytag_detect/tinytag_detect             static-image binary
app/tinytag_detect/tinytag_detect_live        live-camera binary
app/tinytag_detect/run_tinytag.sh             static launcher with defaults
app/tinytag_detect/run_live.sh                live launcher with defaults
app/tinytag_detect/cvimodel/tinytag_v7_synthetic_area_cost.int8.cvimodel default model (~42 KB)
app/tinytag_detect/cvimodel/coverage_roi_context_aug03.int8.cvimodel older coverage model (~42 KB)
app/tinytag_detect/cvimodel/coverage_roi_context_aug03.bf16.cvimodel  alternate BF16 model (143 KB)
app/tinytag_detect/cvimodel/tinytag-v40c.int8.cvimodel                legacy model (43 KB)
app/tinytag_detect/samples/                   sample frames from samples/
usr/local/bin/run_tinytag.sh -> /app/tinytag_detect/run_tinytag.sh  (symlink, keeps it on PATH)
```

`TINYTAG_CVIMODEL` and `TINYTAG_GOLDEN` select different artifacts to stage.
The golden bundle is optional: without it the app still detects, it just cannot
self-check.

Then rebuild the image; the overlay is picked up automatically:

```sh
./build.sh <board>
```

## Deploy app updates without reflashing

Always build in the `duodocker` container. The app links against the TPU SDK
created by a successful full SDK build:

    docker exec duodocker /bin/bash -c \
      'cd /home/work && ./apps/tinytag_detect/build.sh milkv-duos-glibc-arm64-sd'

The generated overlay is under
`device/milkv-duos-glibc-arm64-sd/overlay/app/tinytag_detect/`. Iterating on
the app does not require reflashing. The reference Duo-S uses USB Ethernet
at `root@192.168.42.1`; use SSH/SCP when reachable (password `milkv`):

    scp -O device/milkv-duos-glibc-arm64-sd/overlay/app/tinytag_detect/tinytag_detect_live \
      root@192.168.42.1:/app/tinytag_detect/

Use binary hashes when deployment identity matters. The example build above
is the full app build; [the LDC findings](../../docs/ldc-performance-findings.md)
retain historical incremental CMake commands and dated deployment hashes.

## How the overlay reaches the image

`build/Makefile`'s `br-rootfs-prepare` target already copies
`device/$(MV_BOARD)/overlay/` into the rootfs staging tree, alongside the
SDK's own `device/generic/{rootfs_overlay,br_overlay}` layers:

```make
ifneq ($(wildcard $(TOP_DIR)/device/$(MV_BOARD)/overlay/*),)
        ${Q}cp -arf $(TOP_DIR)/device/$(MV_BOARD)/overlay/* $(BR_ROOTFS_DIR)/
endif
```

That staging tree is then stripped and handed to buildroot as
`BR2_ROOTFS_OVERLAY`. **No SDK file has to be modified** to add an application —
the hook already exists, it was simply unused.

Note that `br-rootfs-prepare` strips every executable it finds, so the binary
lands on the board stripped even though the build tree keeps its symbols.

### When to graduate to a buildroot package instead

The overlay is right while the application is changing: rebuild the app, rerun
`./build.sh`, done. Once it stabilizes, the more conventional home is a real
buildroot package — `buildroot/package/tinytag-detect/` with a `Config.in` and
a `.mk`, modeled on the existing `buildroot/package/duo-wiringx/`, enabled with
`BR2_PACKAGE_TINYTAG_DETECT=y` in
`buildroot/configs/<board>_defconfig`. That buys dependency tracking and
`make menuconfig` visibility, at the cost of teaching buildroot where the
cvitek TPU SDK lives, which the overlay route sidesteps entirely.

## Runtime dependencies

None beyond what the image already ships. The binary needs
`libcviruntime.so`, `libcvikernel.so` and OpenCV 3.2
(`core`/`imgproc`/`imgcodecs`), all present in `/mnt/system/lib`, which
`/etc/profile` puts on `LD_LIBRARY_PATH`. `run_tinytag.sh` sets that path
itself as well, so it also works from a non-login shell (`ssh board 'cmd'`,
init scripts, cron).

The live app also needs the CVI camera/ISP/VPSS/codec stack and matching vendor
kernel modules. The current ARM64 build uses glibc. See
[Duo S Linux options](../../docs/duo-s-linux-options.md) for alternative root
filesystems and the proposed minimal Buildroot profile.

## Preview cadence and resolution

`--rtsp` and `--rtsp-luma` default to a **15 fps cap at 640×360**.
The detector still uses its full-resolution frame and 640×360 model input.

```sh
./run_live.sh --rtsp-luma                         # 640×360, up to 15 fps
./run_live.sh --rtsp-luma --preview-fps 10
./run_live.sh --rtsp-luma --preview-size 1280x720 # optional 720p
./run_live.sh --rtsp-luma --preview-fps 0         # uncapped comparison
./run_live.sh --rtsp-luma --record --record-fps 30
```

Monotonic time gates select frames before overlay drawing and VENC submission.
They enforce minimum spacing, never sleep the detector and never catch up with
bursts after a delay. Actual rates can be lower than the cap. The luma worker
retains a single pending latest-frame slot; old pending frames can be replaced.
Both publication and worker gates prevent delayed jobs from accumulating bursts.

Colour preview uses a 640×360 VPSS output unless recording also needs full
resolution. With direct VPSS model input, **640×360 luma preview borrows the original
model Y buffer after inference: zero Y copies and no CPU resize**. Only one model
buffer may be held across producer, pending slot and worker. Preview opportunities
are dropped while that borrow is busy; the detector never waits for preview.
The frame is mapped for overlays, paired with private neutral chroma and submitted
to VENC. Successful `GetStream` marks encoding completion: the model mapping and
VPSS frame are released **before RTSP bitstream transmission**. Matching sequence
and PTS checks still ensure that the overlay results belong to this image.
Early exits, replacement, failed publication and worker shutdown also return the
borrowed model buffer. `[preview-model]` reports current borrowing (limit 1) and
cumulative dropped preview opportunities. There is no zero-copy A/B option.

Adaptive decoding still makes its existing small-image copy when needed; that
happens before the preview worker can draw on model Y. It is detector work, not a
preview copy. At 720p, preview continues to borrow the detector Y frame.

Software LDC and copied-input operation retain selected full-frame resizing,
so software LDC still appears in the preview. A failed model mapping drops the
preview instead of introducing a hidden copy. For 640×360, the full-resolution
detector frame is returned after optional clean recording, before preview drawing
and encoding; the model frame supplies preview Y. Overlays are scaled from
detector coordinates. Zero-copy removes copy/resize CPU work but holds one pool
buffer longer; board measurements must establish its effect on throughput and
capture age.

Recording has its own `--record-fps` cap (default 30, range 1–120) and always
uses 1280×720 clean frames. Its encoder and cadence are independent of the
preview cap; shared workers and latest-frame replacement still make recording
best effort rather than a guaranteed rate. `--preview-fps` accepts 0–120.
Lowering preview cadence reduces CPU drawing/submission/network work and encoder
traffic; it does not eliminate Linux media driver work or all VPSS traffic.

Validation: the ARM64 cross-build and deterministic cadence tests passed.
Mocked MPI checks of the production model-buffer lease covered ownership
transfer, release after DMA completion, cancellation and failed mapping. The
zero-copy 640×360 binary was deployed to Duo S and the user confirmed it works
well. No numerical performance improvement is claimed from that confirmation.

### Small-core offload considerations

The Duo S can use a RISC-V main core. Current ARM64 libraries cannot execute on
RISC-V, but that alone does **not** rule out a RISC-V media stack or Linux NOMMU.
A proposed stack needs checks for matching ABI/libc, MMU assumptions, DMA buffer
allocation, encoder interrupts and device access. Existing small-core firmware
provides region drawing/compression, not a ready H.264 encoder plus RTSP network
service. Full offload requires those drivers and explicit ownership/cache
handover for frames, results and the chosen network interface.

DDR is shared in both designs: leaving preview on the main core also consumes
DDR bandwidth. Offloading can free main-core CPU time without reducing that
traffic; copies introduced by handover can increase it. Evaluate measured CPU
cost, frame lifetime and end-to-end latency, rather than using shared DDR as a
reason to reject offload. Preview cadence/resolution reduction is useful
independently of which core hosts it.

## Live preview benchmark

On a Duo S with the live camera application installed, run
`/app/tinytag_detect/run_preview_bench.sh`. It executes order-balanced
no-RTSP, colour-RTSP, and exact-luma-RTSP cases and retains raw logs plus a TSV
file under `/tmp`. Preview workers default to niceness 10; set
`TINYTAG_BENCH_PREVIEW_NICE=0` to reproduce the normal-priority baseline.

The summary reports per-mode means. `fps` is detector frames per second;
`loop`, `detCPU`, `procCPU`, `acqAge`, `resultAge`, `release`, and `crop` are
milliseconds per detector frame. `loop` is detector wall time, while `detCPU`
is CPU time only for that thread and `procCPU` is CPU time for all application
threads. `otherCPU` is `procCPU - detCPU`, and `core%` is whole-process CPU
use as a percentage of the one Linux core. `acqAge` is capture-to-detector
start latency; `resultAge` is the software approximation `acqAge + loop`.
`release` measures detector-thread frame handoff/release wall time and should
not be optimized in isolation. `crop` is full-resolution AprilTag crop-decode
time, which varies with scene content.

## Experimental FreeRTOS ROI thresholding

The initial ArUco Nano ROI box-filter/subtraction/binary-threshold stage can
run on C906L; contours, quads, decoding and results stay on Linux. It is
**off by default**, requires matching FreeRTOS firmware in FIP, and includes
a byte-checked benchmark comparing OpenCV/A53, plain C/A53 and plain C/C906L.
See [the experiment guide](../../docs/duo-s-freertos-threshold-experiment.md)
for the recoverable test bundle, installation and timing interpretation.

Select with `TINYTAG_THRESHOLD_BACKEND=opencv|scalar|freertos`. FreeRTOS live
verification defaults on; use `TINYTAG_THRESHOLD_VERIFY=0` for timing only
after correctness checks pass. Failures log a Linux fallback and must not be
counted as successful offload measurements.

## Live camera usage

`run_live.sh` defaults to detector-only operation with no RTSP preview. Pass
`--rtsp` or `--rtsp-luma` to enable a preview; later command-line arguments
override the launcher defaults. To record the scene or run the detector on a
recording instead of the camera, see "Recording and replay" below.

The low-latency 720p60 profile is:

```sh
TINYTAG_LIVE_OV5647_720P60=1 \
  /app/tinytag_detect/run_live.sh \
  --max-exposure-us 10000 \
  --quiet
```

The 24 MHz-compensated OV5647 PLL is the default for 720p60 and reaches about
60 fps on the Duo-S. The older ~57.6 fps timing table remains available with
`TINYTAG_LIVE_OV5647_720P60_PLL24=0`. The 10 ms exposure ceiling keeps AE
enabled while preventing low-light slow shutter; the trade-off is more gain
and noise. Omit it to allow normal AE to choose longer exposures.

The launcher defaults to ROI expansion `1.3`, strict decoding, direct compact
model input, and per-tag text output disabled. `--quiet` emits one `[stats]`
line per second plus batched `[tag]` lines, while suppressing diagnostics.
Use `TINYTAG_LIVE_EXPAND=1.5` to compare the larger decode ROI.

```sh
# Default 1080p30 detector path
/app/tinytag_detect/run_live.sh --quiet

# 720p60 with luma RTSP preview
TINYTAG_LIVE_OV5647_720P60=1 \
  /app/tinytag_detect/run_live.sh --rtsp-luma --max-exposure-us 10000

# Camera delivery only, without inference or decoding
TINYTAG_LIVE_OV5647_720P60=1 \
  /app/tinytag_detect/run_live.sh --capture-only --max-exposure-us 10000
```

## Recording and replay

The live binary can record the camera image while you watch the annotated
preview, then run the detector on that recording through the same hardware
pipeline. Together they let you find tags that the live detector missed, and
re-check them after a change.

### Recording (`--record`)

```sh
# Colour recording, overlay-free; live view in colour with overlay
/app/tinytag_detect/run_live.sh --rtsp --record          # writes rec.mp4

# Monochrome recording of the detector's own input; live view in luma
/app/tinytag_detect/run_live.sh --rtsp-luma --record     # writes rec_mono.mp4

# Explicit path
/app/tinytag_detect/run_live.sh --rtsp-luma --record /root/scene1.mp4
```

On the host, watch with `ffplay rtsp://192.168.42.1/h264`.

- The file is 1280x720 H.264 at 4 Mbps (about 30 MB per minute), encoded by
  a second VENC channel independent of the RTSP encoder. No overlay is ever
  drawn into it.
- `--rtsp-luma` records exactly the Y plane the detector saw, with neutral
  chroma. `--rtsp`, or `--record` with no preview, records colour.
- VPSS device 1 has only three output channels, so recording shares the
  colour preview channel, or the detector Y frame for luma preview. Clean
  recording happens before overlay drawing, at its own rate cap. Recording
  adds encoder work to the shared worker even when preview is capped lower.
- Stop with Ctrl-C or SIGTERM. The MP4 index is written on a clean stop; a
  `kill -9`, crash or power cut leaves an unplayable file.
- Default file names are relative to the directory you run from.

### Replay (`--input`)

```sh
# Recorded timing: behaves like the camera, skipping frames it can't keep up with
/app/tinytag_detect/run_live.sh --input /root/rec_mono.mp4

# Every frame, as fast as the detector takes them -- use this for analysis
/app/tinytag_detect/run_live.sh --input /root/rec_mono.mp4 --input-speed 0

# Replay with the annotated live view
/app/tinytag_detect/run_live.sh --input /root/rec_mono.mp4 --rtsp-luma
```

Hardware lens correction is available in both live AprilTag detectors through
the VPSS LDC block. Calibrate the lens and choose the correction parameters
with [`tools/ldc_calibrate.py`](../../tools/ldc_calibrate.md), then pass the
reported JSON file with `--ldc-calibration ldc-calibration.json` to `run_live.sh`.
It is off unless a calibration file is supplied. The
calibration tool can read image sets, recorded video, or the board's RTSP live
preview; its fit RMS indicates how well Sophgo's single-ratio model describes
the lens.

With LDC enabled, VPSS stores the visible 1280x720 and 640x360 images in
1280x768 and 640x384 surfaces. The extra bottom rows are padding, not part of
the camera image. The live apps allocate VB blocks for the full stored sizes
and additional blocks for GDC's temporary rotated surfaces. The TinyTag path
accepts a model/full pair only when both the sensor frame counter and PTS
match; a malformed GDC frame is discarded.

The first start with a given LDC JSON and output size generates a GDC mesh and
stores it beside the JSON as `.sg2000-ldc-*.mesh`. Later starts load that mesh;
the two channels took about 13 s + 3.4 s to generate and 2 ms + 30 ms to load
on this board. Changing the LDC values automatically selects a new cache key.
If the cache directory is not writable, correction still works but generation
repeats at each start. After an SDK/firmware change, remove these mesh files
to force regeneration. Caching only improves startup, not per-frame throughput.

On this Duo S, a 1920x1080 sensor run with RTSP luma preview and the v40c
model measured about 31 fps without LDC and 14-15 fps with both corrected
VPSS channels. A single corrected channel delivered about 31 fps in the
capture-only test. The existing copied-input mode corrected only the
1280x720 channel and resized/copied into the TPU input in about 1.7 ms;
it measured about 31 fps with RTSP in this scene. To compare it on the board:

```sh
TINYTAG_LIVE_DIRECT_COMPACT_INPUT=0 ./run_live.sh --rtsp-luma \
    --ldc-calibration /root/ldc-calibration.json
```

The direct 640x384 model channel remains the default. The copied mode uses
the same corrected full-resolution frame for inference and crop decode, so
there is no cross-channel pairing step. These figures depend on scene content
and decoder crop load; use the per-second `[camera]` and `[crop-profile]`
reports for your scene.

For comparison, `--ldc-mode sw` skips VPSS/GDC LDC and corrects the
uncorrected 1280x720 detector frame on the CPU with the JSON's full OpenCV
model (`camera_matrix` and `distortion_coefficients`), not the one-ratio
Sophgo fit. The corrected frame keeps the calibrated camera matrix, so its
intrinsics are that matrix (scaled to 1280x720) with zero distortion. The model
input is resized from the corrected frame, and `--rtsp-luma` shows corrected
pixels; colour `--rtsp` does not. `--ldc-sw-interp nearest` trades
interpolation quality for speed. The `[camera]` line reports its per-frame
`ldc` time, and startup prints an isolated self-test time for the scalar and
NEON paths plus their bit-exact comparison.

```sh
TINYTAG_LIVE_OV5647_720P60=1 ./run_live.sh --rtsp-luma --max-exposure-us 10000 \
    --ldc-calibration /root/ldc-calibration.json --ldc-mode sw
```

Full-frame software correction is much slower than the hardware path on the
800 MHz A53. With 720p60 and `--max-exposure-us 10000` in one scene, the mean
`[tails]` result age was 20.6 ms at 61 fps without LDC. It was 28.7 ms at 56
fps with hardware direct input, and 36.2 ms at 49 fps with hardware
copied input. Software linear LDC took 36 ms of CPU per frame and reached 63.6
ms at 18 fps; software nearest took 21.5 ms and reached 47.6 ms at 25 fps.

Point LDC is implemented but opt-in. Run it with tag decoding enabled:

```sh
./run_live.sh --ldc-mode point --ldc-calibration /root/ldc-calibration.json
```

It leaves the frame uncorrected, then samples and undistorts tag edge points to
refine the corners of decoded raw-image quads. Its default-on fallback retries
up to eight quads whose normal bit decode failed through a distortion-aware
grid. It cannot recover a tag if the raw-image contour or quad search found no
candidate. See [the point-LDC note](../../docs/ldc-point-correction.md) and
the [recorded point-LDC measurements](../../docs/ldc-point-correction.md#recorded-validation-and-board-cost).

The OV5647 720p60 mode uses a wider, binned sensor region than 1080p30. A
calibration made from 1080p30 images is not valid for accurate 720p60 pose
estimation, even if both feed a 1280x720 VPSS channel; calibrate the uncorrected
720p60 stream separately. On this board, single-channel LDC on 720p60 gave
about 31 corrected frames/s in `--capture-only` (versus about 60 without LDC),
and roughly 13 detector frames/s with copied input and RTSP in the test scene.
Avoid assuming that removing the VPSS downscale will make this path faster:
the GDC correction still runs on each output frame.

The hardware H.264 decoder (VDEC) is bound to the same VPSS group, device
and channels that VI feeds live, so scaling, the model-input channel, the
detector, crop-decode and the previews are all unchanged. Only the source
differs: VI, the sensor and the ISP are not started.

- `--input-speed f` scales the recorded timing (default 1). `0` is lockstep:
  the feeder keeps one compressed packet ahead because VDEC needs the next
  packet to release the current picture, then waits for the detector before
  advancing. Decoded frames are not superseded. VDEC retains the final
  picture at EOF; a 279-frame no-B-frame recording processed 278 frames in
  board testing.
- The run exits at end of file and prints
  `[input] detector processed N of M frames`.
- `--mirror`/`--flip` are ignored (recordings are already oriented), as are
  `--max-exposure-us` and the interactive ISP commands.
- Other files work if they are 8-bit 4:2:0 H.264 (Baseline, Main or High).
  10-bit, 4:2:2, 4:4:4 and H.265 are rejected at open; re-encode with
  `ffmpeg -i in.mp4 -c:v libx264 -pix_fmt yuv420p -bf 0 out.mp4`. A
  non-16:9 source is stretched to 1280x720, with a warning.

Known limitations:

- The decoder driver does not flush its reorder queue at end of stream, so
  the last few B-frame pictures (3 in testing) are never processed. It also
  retained the last picture of a no-B-frame 279-frame recording (278
  processed). Use `-bf 0` when re-encoding to minimize this loss.
- VPSS numbers replayed frames in steps of 2, so the per-second `seq mean
  2.00` and the preview's `seq skipped` counts read like dropped frames.
  Trust the `[input]` summary line instead.
- Decoded tags are reported per second, as in live mode. There is no
  per-frame output mapping a detection to a frame index yet.

## Usage on the board

Sample frames ship with the app, so there is nothing to copy over. With no
arguments at all it runs against the bundled `arena-1280x800.jpg` sample, so
every other option already has a working default:

```sh
run_tinytag.sh                                                  # bundled sample, full two-stage pipeline
run_tinytag.sh /app/tinytag_detect/samples/arena-1280x800.jpg   # full two-stage pipeline
run_tinytag.sh /app/tinytag_detect/samples/frame-640x360.png    # no resize

# proposal-only mode, if needed
TINYTAG_DECODE= run_tinytag.sh /app/tinytag_detect/samples/arena-1280x800.jpg
```

Or any image of your own:

```sh
run_tinytag.sh /path/to/frame.jpg
```

The defaults use the coverage-mask A+C model. Values can be overridden from
the environment:

| variable | default | meaning |
|---|---|---|
| `TINYTAG_MODEL` | `/app/tinytag_detect/cvimodel/tinytag_v7_synthetic_area_cost.int8.cvimodel` | cvimodel to load |
| `TINYTAG_THRES_HEAT` | `0.30` | ROI center heat threshold |
| `TINYTAG_THRES_MASK` | `0` | Optional final A+C mask-score gate; 0 disables |
| `TINYTAG_MAX` | `20` | max proposals per frame |
| `TINYTAG_EXPAND` | `1.0` | legacy model ROI expansion; unused by A+C |
| `TINYTAG_IOU` | `0.5` | legacy model ROI IoU suppression; unused by A+C |
| `TINYTAG_OUT` | `/tmp/tinytag_det.jpg` | annotated output image |
| `TINYTAG_DEBUG` | `1` | 0 quiet, 1 timing, 2 verbose |
| `TINYTAG_REPEAT` | `20` | timed inference runs |
| `TINYTAG_WARMUP` | `2` | untimed runs before measuring |
| `TINYTAG_GOLDEN` | `/app/tinytag_detect/cvimodel/tinytag_v7_synthetic_area_cost.ttgold` | optional, model-matched self-test bundle; regenerate for v7 |
| `TINYTAG_MAX_MAE` | `0.06` | self-test error gate (accepted INT8 validation MAE is 0.0558) |
| `TINYTAG_DECODE` | `strict` | `strict` or `tolerant`; empty disables stage two |

```sh
TINYTAG_THRES_HEAT=0.20 run_tinytag.sh frame.jpg      # higher recall, ~2x proposals
run_tinytag.sh frame.jpg --repeat 20             # extra args pass through
```

`0.35` roughly halves the proposal count versus the training repo's frozen
`0.20`, for only a slight drop in recall.

With `--decode` on, note that `TINYTAG_MAX` is the **latency** knob and
`TINYTAG_THRES_HEAT` the **recall** knob. `decode_proposals` truncates to
`max_proposals` before IoU suppression, so the number of crops the decoder sees
is capped regardless of scene, and worst-case frame time is
`4.74 ms + N x 0.93 ms`. Once enough peaks clear the threshold to fill the cap,
lowering the threshold further is free -- it only changes which candidates take
the top-N slots. See `docs/duo-s-performance-findings.md` for the table.

Or call the binary directly:

```
tinytag_detect <cvimodel> <image_file> [--thres_heat f] [--thres_mask f] [--max n] [--expand f] [--iou f]
                                  [--decode [strict|tolerant]]
                                  [--out path] [--repeat n] [--warmup n] [--debug 0|1|2]
tinytag_detect <cvimodel> --selftest <bundle> [--repeat n] [--warmup n] [--max-mae f]
```

`--decode` adds the per-ROI CV stage. `strict` requires an exact bit match
(`errorCorrectionRate` and `maxErroneousBitsInBorderRate` both 0.0, the K230
production setting); `tolerant` raises both to 1.0, accepting marginal tags at
the cost of false positives and extra time in bit extraction.

## Timing

Every run reports min / median / mean / max per stage over `--repeat` runs,
after `--warmup` untimed runs (the first inference pays one-off setup, so
measuring it would overstate steady-state cost):

```
---- timing, ms over 20 run(s) after 2 warmup ----
  pre_process  min   ...  median   ...  mean   ...  max   ...
  inference    min   ...  median   ...  mean   ...  max   ...
  decode       min   ...  median   ...  mean   ...  max   ...
  crop_decode  min   ...  median   ...  mean   ...  max   ...   (--decode only)
  total        min   ...  median   ...  mean   ...  max   ...

  TPU inference    : ... /s at the median
  this pipeline    : ... frames/s at the median
  per-ROI decode   : ... ms over N crop(s)                      (--decode only)
```

The four stages are: `pre_process` (crop + resize + tensor fill), `inference`
(`CVI_NN_Forward` alone), `decode` (`decode_proposals`: sigmoid, NMS, top-K,
box decode) and `crop_decode` (the per-ROI CV tag decode).

`per-ROI decode` divides `crop_decode` by the number of crops, which is the
figure that scales with proposal count — so it is what a higher or lower
`--thres_heat` trades against. See "Measured on hardware" below.

## Self-test against golden references

```sh
run_tinytag.sh --selftest
```

Replays frames from a model-matched golden bundle — each carrying the exact
uint8 input, the FP32 ONNX Runtime output, and the host simulator output — and reports:

| comparison | meaning | gate |
|---|---|---|
| hardware vs **FP32** | quantization error against the source model | worst-frame MAE <= `--max-mae` (binary default 0.05; `run_tinytag.sh` passes `TINYTAG_MAX_MAE`, 0.06) |
| heatmap peak | did the argmax move? | all frames within 2 cells |
| hardware vs **simulator** | does real silicon reproduce what the model was signed off against? | worst element < 1e-3 |

That third row is the important one. The model is validated on the host against
tpu-mlir's simulator; if the board disagrees with the simulator, that validation
does not transfer — and that is precisely the K230 failure mode, where a kmodel
passed simulation and then corrupted memory on real hardware. The simulator and
the TPU execute the same instruction stream, so they should agree essentially
to the bit.

Exit status is 0 on pass, 1 on any failure, so it can gate a boot script or CI.

Regenerate the bundle whenever the cvimodel changes:

```sh
tools/tinytag_cvimodel/tpu_docker.sh run \
  tools/tinytag_cvimodel/make_golden.py \
    --onnx /assets/.../tinytag-v40c-unfrozen-moderate30ep.static.onnx \
    --cvimodel tools/tinytag_cvimodel/work/tinytag-v40c.int8.cvimodel \
    --images   tools/tinytag_cvimodel/work/frames/validation \
    --output   tools/tinytag_cvimodel/work/tinytag-v40c.ttgold
```

Each frame adds ~816 KB (`--frames`, default 4).

## Pipeline

```
  image (any size, grayscale)
        |
  [1] pre_process()
        crop bottom-left 1280x720 band  (crop-only: no scale-up, no letterbox)
        plain resize -> 640x360
        memcpy raw luma into the uint8 input tensor
        (the /255 is folded into the model by --fuse_preprocess, so the TPU does it)
        |
  [2] inference()            CVI_NN_Forward
        |
        v
  mask_and_roi: f32 [1,6,45,80]  (mask, heat, offsets x/y, log w/h)
  [3] decode_proposals()
        mask hysteresis (seed 0.4, grow 0.3) -> connected regions
        -> ROI peaks split regions -> 4 px margin + conditional warm-cell margin
        -> map boxes to the source frame
        |
        v
  proposals: [{confidence, roi}] in full-frame pixel coordinates
```

The crop-then-resize in step 1 is mirrored exactly by
`tools/tinytag_cvimodel/prepare_calibration.py`, so calibration statistics match
what the board actually sees. If you change one, change the other.

## Legacy v40c INT8 measurements

These historical figures are for the older v40c proposal model, not the current A+C model. Duo S (SG2000), image built from this tree. Full analysis, including the
K230 comparison and its caveats, is in `docs/duo-s-performance-findings.md`.

```
hardware-INT8 vs FP32 : worst frame MAE 0.04709  (gate <= 0.05)
heatmap peak match    : 4/4 within 2 cells
hardware vs simulator : worst element 0.000000      <-- bit-exact
```

The simulator row is the important one: the real TPU reproduces tpu-mlir's
cv18xx simulator exactly, so host validation transfers to silicon. That was the
open risk inherited from the K230 experience, and it is closed.

Medians over 20 runs after warmup, `arena-1280x800.jpg`, 7 proposals at
threshold 0.35:

| stage | 640x360 in | 1280x800 in | K230 @1280x720 |
|---|---|---|---|
| pre_process | 0.19 ms | 1.73 ms | 2.4 ms (hardware ai2d) |
| inference | 2.08 ms | 2.11 ms | 2.2 ms (KPU) |
| decode_proposals | 0.89 ms | 0.90 ms | <0.5 ms |
| crop_decode (`--decode`) | -- | 6.53 ms | 10.7 ms |
| **full pipeline** | -- | **11.25 ms (88.9 fps)** | ~15.25 ms (~65 fps) |

Per-ROI CV decode is **0.93 ms** against roughly 1.5 ms on K230.

The Duo S is faster than the K230 at every measured stage. The TPU is a wash
with the KPU, but the CPU resize beats the K230's dedicated ai2d block, and the
CV crop-decode -- the stage that dominates the budget -- is materially quicker.

**Read that last row carefully: it is not an A53-vs-C908 microarchitecture
result.** Both are 2-wide in-order cores, so issue width is roughly a wash. The
difference is SIMD availability, and it comes straight out of the K230 build
config. The K230 application is built under `k230_canmv_small_core_defconfig`,
and that core has no RISC-V vector extension, so its OpenCV is compiled
scalar (`buildroot-overlay/package/opencv4/opencv4.mk`):

    # The small core has no V extension; CSI-CV is a prebuilt RVV library.
    OPENCV4_CONF_OPTS += -DBUILD_CSI_CV=OFF -DCV_ENABLE_INTRINSICS=OFF
    OPENCV4_CONF_OPTS += ... -mcpu=c908 ...      # no 'v'

The Duo S side has NEON unconditionally -- aarch64 makes it mandatory, and
`libopencv_imgproc.so.3.2` contains 439 vector instructions. ArUco Nano's hot
path (`adaptiveThreshold`, `findContours`, `getPerspectiveTransform`) is almost
entirely byte-wise image processing, which is exactly where that matters.

So this measures **NEON-vectorized OpenCV against deliberately scalar OpenCV**.
The ~10.7 ms K230 figure is not that chip's best: on its big core with RVV and
the CSI-CV library the same work could be considerably faster. Do not cite this
table as evidence that the Duo S is the faster part.

An earlier revision of this file projected ~25 ms (~40 fps) by scaling K230's
crop-decode by the 0.90-vs-<0.5 ms `decode_proposals` ratio. That was wrong by
more than 2x: `decode_proposals` is a sub-millisecond loop over a 45x80 grid
and a poor proxy for a 10 ms workload. The measured numbers above replace it.

All of this is **single-threaded on a single core**. The arm64 build's device
tree declares exactly one Cortex-A53 (`cpu@0`, no `cpu@1`) -- the SG2000's C906
runs FreeRTOS in this configuration -- so OpenCV's `parallel_for_` has nothing
to scale onto. Confirm with `nproc` on the board.

The CPU clock is not in the device tree and there is no cpufreq driver, so
`scaling_cur_freq` is empty; the A53 PLL is set by fsbl before Linux starts.
Read it at runtime from `/sys/kernel/debug/clk/clk_summary` (the `clk_a53` row;
it muxes between `clk_xtal_a53` at 25 MHz and `clk_div_{0,1}_a53`). It has not
been recorded here -- do not assume a value.

**The NPU is not the bottleneck**: at 2.11 ms the network could run 3-4x per
frame and the pipeline would still be CV-bound.

Tail latency is worth watching for a real-time loop -- `pre_process` showed a
7.06 ms max against a 1.73 ms median, pushing the total max to 17.6 ms. Medians
are stable across runs, so this looks like scheduling or cache behaviour rather
than anything algorithmic.

Proposal-level agreement with FP32 (via
`tools/tinytag_cvimodel/reference_proposals.py`) is within ~1.4 px. Occasional
low-confidence peaks shift by one grid cell or drop out, because the heatmap
logit quantizes to ~0.045 steps and ties change which cells survive the 3x3
max-pool.

## Not yet done

- No live camera path. The Duo's capture stack (`cvi_mpi`/VPSS) differs
  substantially from the K230's V4L2 path and is a separate piece of work. See
  `docs/ov9281-camera-port.md` for notes on wiring up an OV9281 global-shutter
  mono sensor, whose 1280x800 output matches the model's training resolution.
  Note that a VPSS-fed model would want `--aligned_input` at compile time; this
  application rejects such a model at load, since it feeds a plain contiguous
  buffer.
- Everything else here has been run on a Duo S; see "Measured on hardware".
