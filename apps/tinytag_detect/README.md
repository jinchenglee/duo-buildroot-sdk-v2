# tinytag_detect

Still-image TinyTag proposal detector for the Milk-V Duo S, running on the
SG2000 TPU via `cviruntime`.

This is the **neural half** of the K230 two-stage AprilTag detector (see
`buildroot-overlay/package/ai_demo/tinytag_detect` in the K230 SDK): a small
network proposes tag ROIs on a downscaled frame. The second stage there — a
traditional-CV AprilTag 36h11 crop decoder — is deliberately not ported, so the
TPU path can be proven end to end without pulling in an ArUco dependency.

## Build and install

```sh
apps/tinytag_detect/build.sh [board]        # defaults to the board in device/target
```

Cross-compiles against the cvitek TPU SDK that a full `./build.sh <board>` drops
in `install/soc_<project>/tpu_64bit/`, then installs into
`device/<board>/overlay/`:

```
usr/local/bin/tinytag_detect             the binary                  (69 KB)
usr/local/bin/run_tinytag.sh             launcher with defaults baked in
mnt/cvimodel/tinytag-v40c.int8.cvimodel  the model                    (43 KB)
mnt/cvimodel/tinytag-v40c.ttgold         golden references            (3.2 MB)
mnt/data/tinytag-samples/                sample frames from samples/  (740 KB)
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
ifneq ($(wildcard $(TOP_DIR)/device/$(MV_BOARD)/overlay),)
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

Sample frames ship with the app, so there is nothing to copy over:

```sh
run_tinytag.sh /mnt/data/tinytag-samples/arena-1280x800.jpg   # real crop path
run_tinytag.sh /mnt/data/tinytag-samples/frame-640x360.png    # no resize
```

Or any image of your own:

```sh
run_tinytag.sh /path/to/frame.jpg
```

Defaults are the K230 production operating point and can be overridden from the
environment:

| variable | default | meaning |
|---|---|---|
| `TINYTAG_MODEL` | `/mnt/cvimodel/tinytag-v40c.int8.cvimodel` | cvimodel to load |
| `TINYTAG_THRES` | `0.35` | heatmap threshold |
| `TINYTAG_MAX` | `8` | max proposals per frame |
| `TINYTAG_EXPAND` | `1.5` | ROI expansion factor |
| `TINYTAG_IOU` | `0.5` | ROI IoU suppression (<= 0 disables) |
| `TINYTAG_OUT` | `/tmp/tinytag_det.jpg` | annotated output image |
| `TINYTAG_DEBUG` | `1` | 0 quiet, 1 timing, 2 verbose |
| `TINYTAG_REPEAT` | `20` | timed inference runs |
| `TINYTAG_WARMUP` | `2` | untimed runs before measuring |
| `TINYTAG_GOLDEN` | `/mnt/cvimodel/tinytag-v40c.ttgold` | self-test bundle |
| `TINYTAG_MAX_MAE` | `0.05` | self-test error gate |

```sh
TINYTAG_THRES=0.20 run_tinytag.sh frame.jpg      # higher recall, ~2x proposals
run_tinytag.sh frame.jpg --repeat 20             # extra args pass through
```

`0.35` roughly halves the proposal count versus the training repo's frozen
`0.20`, for only a slight drop in recall.

Or call the binary directly:

```
tinytag_detect <cvimodel> <image> [--thres f] [--max n] [--expand f] [--iou f]
                                  [--out path] [--repeat n] [--warmup n] [--debug 0|1|2]
tinytag_detect <cvimodel> --selftest <bundle> [--repeat n] [--warmup n] [--max-mae f]
```

## Timing

Every run reports min / median / mean / max per stage over `--repeat` runs,
after `--warmup` untimed runs (the first inference pays one-off setup, so
measuring it would overstate steady-state cost):

```
---- timing, ms over 20 run(s) after 2 warmup ----
  pre_process  min   ...  median   ...  mean   ...  max   ...
  inference    min   ...  median   ...  mean   ...  max   ...
  decode       min   ...  median   ...  mean   ...  max   ...
  total        min   ...  median   ...  mean   ...  max   ...

  TPU inference    : ... /s at the median
  this pipeline    : ... frames/s at the median
```

`inference` is the TPU stage alone (`CVI_NN_Forward`) — the figure to use when
projecting what a larger application can afford. Remember that a full two-stage
tinytag detector adds a per-ROI CV decode stage, which on K230 dominated the
budget at ~10.7 ms of ~15.25 ms per frame; the neural side was only ~2.2 ms
there. So TPU time is likely to be the *small* term in the final system.

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

Duo S (SG2000), image built from this tree:

```
hardware-INT8 vs FP32 : worst frame MAE 0.04709  (gate <= 0.05)
heatmap peak match    : 4/4 within 2 cells
hardware vs simulator : worst element 0.000000      <-- bit-exact
```

The simulator row is the important one: the real TPU reproduces tpu-mlir's
cv18xx simulator exactly, so host validation transfers to silicon. That was the
open risk inherited from the K230 experience, and it is closed.

| stage | 640x360 in | 1280x800 in | K230 @1280x720 |
|---|---|---|---|
| pre_process | 0.19 ms | 1.69 ms | 2.4 ms (hardware ai2d) |
| inference | 2.08 ms | 2.10 ms | 2.2 ms (KPU) |
| decode_proposals | 0.89 ms | 0.90 ms | <0.5 ms |
| **neural total** | 3.16 ms | **4.70 ms** | ~5.1 ms |

The TPU is a wash with the K230 KPU, and the CPU resize here is actually faster
than the K230's dedicated ai2d block. Scalar CPU work is roughly 2x slower
though (0.90 vs <0.5 ms for the same decode), consistent with a 1 GHz
Cortex-A53 against K230's 1.6 GHz C908.

That matters for projecting the full two-stage detector, whose CV crop-decode
stage is pure scalar CPU and cost ~10.7 of ~15.25 ms on K230. Scaling by ~2x
puts a complete pipeline near ~25 ms (~40 fps) against K230's ~65 fps. Treat
that as an extrapolation from one CPU-bound datapoint, not a measurement. The
firm conclusion is that **the NPU is not the bottleneck and will not become
one** -- at 2.1 ms the network could run 3-4x per frame and still be CPU-bound.

Proposal-level agreement with FP32 (via
`tools/tinytag_cvimodel/reference_proposals.py`) is within ~1.4 px. Occasional
low-confidence peaks shift by one grid cell or drop out, because the heatmap
logit quantizes to ~0.045 steps and ties change which cells survive the 3x3
max-pool.

## Not yet done

- No live camera path (see below). Everything else here has been run on a
  Duo S; see "Measured on hardware".
- No live camera path. The Duo's capture stack (`cvi_mpi`/VPSS) differs
  substantially from the K230's V4L2 path and is a separate piece of work. Note
  that a VPSS-fed model would want `--aligned_input` at compile time; this
  application rejects such a model at load, since it feeds a plain contiguous
  buffer.
- No AprilTag ID decode — proposals only.
