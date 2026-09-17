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

## Usage on the board

Sample frames ship with the app, so there is nothing to copy over. With no
arguments at all it runs against the bundled `arena-1280x800.jpg` sample, so
every other option already has a working default:

```sh
run_tinytag.sh                                                  # bundled sample, all defaults
run_tinytag.sh /app/tinytag_detect/samples/arena-1280x800.jpg   # proposals only
run_tinytag.sh /app/tinytag_detect/samples/frame-640x360.png    # no resize

# full two-stage pipeline: proposals + AprilTag 36h11 ids
TINYTAG_DECODE=strict run_tinytag.sh /app/tinytag_detect/samples/arena-1280x800.jpg
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
| `TINYTAG_DECODE` | *(off)* | `strict` or `tolerant` — enables stage two |

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
tinytag_detect <cvimodel> <image> [--thres f] [--max n] [--expand f] [--iou f]
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
