# tinytag_detect

Still-image TinyTag proposal detector for the Milk-V Duo S, running on the
SG2000 TPU via `cviruntime`.

A port of the K230 two-stage AprilTag detector (see
`buildroot-overlay/package/ai_demo/tinytag_detect` in the K230 SDK): a small
network proposes tag ROIs on a downscaled frame, then a traditional-CV decoder
reads the AprilTag 36h11 id out of each proposal at full resolution.

Stage one always runs. Stage two is opt-in via `--decode`, so the TPU path can
be exercised on its own, and the CV cost can be measured separately from it.
ArUco Nano is the decoder, matching the K230 production default.

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
app/tinytag_detect/tinytag_detect             the binary                  (284 KB)
app/tinytag_detect/run_tinytag.sh             launcher with defaults baked in
app/tinytag_detect/cvimodel/tinytag-v40c.int8.cvimodel  the model          (43 KB)
app/tinytag_detect/cvimodel/tinytag-v40c.ttgold         golden references  (3.2 MB)
app/tinytag_detect/samples/                   sample frames from samples/  (740 KB)
usr/local/bin/run_tinytag.sh -> /app/tinytag_detect/run_tinytag.sh  (symlink, keeps it on PATH)
```

`TINYTAG_CVIMODEL` and `TINYTAG_GOLDEN` select different artifacts to stage.
The golden bundle is optional: without it the app still detects, it just cannot
self-check.

Then rebuild the image; the overlay is picked up automatically:

```sh
./build.sh <board>
```

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
- VPSS device 1 has only three output channels, so the recording shares the
  preview's. The preview worker encodes each frame for the file *before*
  drawing the overlay on it, adding about 7 ms per frame to that worker.
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
  the next frame is fed only after the detector has taken the previous one,
  so no frame is skipped. A 67 s recording replays in about 29 s this way.
- The run exits at end of file and prints
  `[input] detector processed N of M frames`.
- `--mirror`/`--flip` are ignored (recordings are already oriented), as are
  `--max-exposure-us` and the interactive ISP commands.
- Other files work if they are 8-bit 4:2:0 H.264 (Baseline, Main or High).
  10-bit, 4:2:2, 4:4:4 and H.265 are rejected at open; re-encode with
  `ffmpeg -i in.mp4 -c:v libx264 -pix_fmt yuv420p -bf 0 out.mp4`. A
  non-16:9 source is stretched to 1280x720, with a warning.

Known limitations:

- With B-frames, the decoder driver does not flush its reorder queue at end
  of stream, so the last few frames (3 in testing) are never processed, and
  lockstep can skip about one frame. `--record` output has no B-frames and
  loses nothing; use `-bf 0` when re-encoding.
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

Defaults are the K230 production operating point and can be overridden from the
environment:

| variable | default | meaning |
|---|---|---|
| `TINYTAG_MODEL` | `/app/tinytag_detect/cvimodel/tinytag-v40c.int8.cvimodel` | cvimodel to load |
| `TINYTAG_THRES` | `0.35` | heatmap threshold |
| `TINYTAG_MAX` | `8` | max proposals per frame |
| `TINYTAG_EXPAND` | `1.5` | ROI expansion factor |
| `TINYTAG_IOU` | `0.5` | ROI IoU suppression (<= 0 disables) |
| `TINYTAG_OUT` | `/tmp/tinytag_det.jpg` | annotated output image |
| `TINYTAG_DEBUG` | `1` | 0 quiet, 1 timing, 2 verbose |
| `TINYTAG_REPEAT` | `20` | timed inference runs |
| `TINYTAG_WARMUP` | `2` | untimed runs before measuring |
| `TINYTAG_GOLDEN` | `/app/tinytag_detect/cvimodel/tinytag-v40c.ttgold` | self-test bundle |
| `TINYTAG_MAX_MAE` | `0.05` | self-test error gate |
| `TINYTAG_DECODE` | `strict` | `strict` or `tolerant`; empty disables stage two |

```sh
TINYTAG_THRES=0.20 run_tinytag.sh frame.jpg      # higher recall, ~2x proposals
run_tinytag.sh frame.jpg --repeat 20             # extra args pass through
```

`0.35` roughly halves the proposal count versus the training repo's frozen
`0.20`, for only a slight drop in recall.

With `--decode` on, note that `TINYTAG_MAX` is the **latency** knob and
`TINYTAG_THRES` the **recall** knob. `decode_proposals` truncates to
`max_proposals` before IoU suppression, so the number of crops the decoder sees
is capped regardless of scene, and worst-case frame time is
`4.74 ms + N x 0.93 ms`. Once enough peaks clear the threshold to fill the cap,
lowering the threshold further is free -- it only changes which candidates take
the top-N slots. See `docs/duo-s-performance-findings.md` for the table.

Or call the binary directly:

```
tinytag_detect <cvimodel> <image_file> [--thres f] [--max n] [--expand f] [--iou f]
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
`--thres` trades against. See "Measured on hardware" below.

## Self-test against golden references

```sh
run_tinytag.sh --selftest
```

Replays frames from the golden bundle — each carrying the exact uint8 input,
the FP32 ONNX Runtime output, and the host simulator's INT8 output — and reports:

| comparison | meaning | gate |
|---|---|---|
| hardware-INT8 vs **FP32** | quantization error: how much accuracy the INT8 model gives up | worst-frame MAE <= `--max-mae` (0.05) |
| heatmap peak | did the argmax move? | all frames within 2 cells |
| hardware-INT8 vs **simulator** | does real silicon reproduce what the model was signed off against? | worst element < 1e-3 |

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
  proposal_maps: f32 [1,21,45,80]   (channels 0-4 trained: heatmap,
        |                            offset_x/y, scale_w/h; 5-20 dormant)
  [3] decode_proposals()
        sigmoid -> 3x3 max-pool NMS -> threshold -> top-K
        -> center/size decode (exp, clamped to [-4,6]) -> roi_expand
        -> clamp to band -> greedy IoU suppression
        |
        v
  proposals: [{confidence, roi}] in full-frame pixel coordinates
```

The crop-then-resize in step 1 is mirrored exactly by
`tools/tinytag_cvimodel/prepare_calibration.py`, so calibration statistics match
what the board actually sees. If you change one, change the other.

## Measured on hardware

Duo S (SG2000), image built from this tree. Full analysis, including the
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
