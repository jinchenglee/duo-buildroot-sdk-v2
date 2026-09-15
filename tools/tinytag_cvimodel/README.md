# TinyTag cvimodel toolchain (Milk-V Duo S / SG2000)

Turns an externally trained TinyTag ONNX model into a `cvimodel` for the
cv181x TPU, and checks the result against ONNX Runtime before it ever reaches
a board. Training checkpoints, ONNX models, datasets and generated work
directories stay **outside** this repository; only a hardware-validated
cvimodel belongs in a board overlay.

This mirrors the K230 flow in `tools/tinytag_kmodel/` of the K230 SDK, retargeted
from nncase/kmodel to tpu-mlir/cvimodel.

## Application contract

The scripts are fixed to what `apps/tinytag_detect` expects:

| | |
|---|---|
| source ONNX input | float `1x1x360x640`, normalized to `[0, 1]` |
| deployed input | uint8 grayscale `1x1x360x640`, `/255` folded in by `--fuse_preprocess` |
| output | float `1x21x45x80` (`proposal_maps`) |
| target | `cv181x`, which covers the SG2000/SG2002 on the Duo S |

## 1. Build the container

```sh
tools/tinytag_cvimodel/tpu_docker.sh build
```

`sophgo/tpuc_dev:v3.1` plus `milkv-duo/tpu-mlir` (a prebuilt drop of tpu-mlir
**v1.3.228**, 2023-09), which is the combination Milk-V's own TPU documentation
pins. Do not casually upgrade it: a newer tpu-mlir can emit a cvimodel version
the `cviruntime` 1.1.1 shipped in this SDK cannot parse.

`tpu_docker.sh run <cmd>` runs a command inside the image with this repo bind
mounted at `/workspace` and `$TINYTAG_ASSETS` (default `~/Downloads`) read-only
at `/assets`. It runs as your UID, so generated files are not root-owned.

## 2. Prepare calibration and validation frames

```sh
tools/tinytag_cvimodel/tpu_docker.sh run \
  tools/tinytag_cvimodel/prepare_calibration.py \
    --source /assets/220-225.mp4 \
    --source /assets/528.198329.jpg \
    --output tools/tinytag_cvimodel/work/frames
```

Every frame goes through **exactly** the preprocessing the board performs: crop
the bottom-left 1280x720 band out of anything larger, then plain-resize to
640x360 grayscale. INT8 calibration measures activation statistics, so frames
cropped differently than deployment produce thresholds nothing downstream can
recover from.

Use raw frames. Do **not** calibrate on images with rendered boxes or text
(e.g. the `visual-*` directories next to the model): overlays change activation
statistics. Aim for >= 100 frames spanning normal, hard and negative scenes,
drawn from more than one clip — frames from a single 10 fps clip are highly
correlated and count for much less than their number suggests. Fewer than 100
requires `--allow-small-calibration` and yields an experimental model.

## 3. Compile

```sh
tools/tinytag_cvimodel/tpu_docker.sh run \
  tools/tinytag_cvimodel/compile_cvimodel.py \
    --onnx /assets/tinytag-v40c-unfrozen-moderate30ep/tinytag-v40c-unfrozen-moderate30ep.static.onnx \
    --calibration-dir tools/tinytag_cvimodel/work/frames/calibration \
    --validation-dir  tools/tinytag_cvimodel/work/frames/validation \
    --output tools/tinytag_cvimodel/work/tinytag-v40c.int8.cvimodel
```

Runs `model_transform.py` -> `run_calibration.py` -> `model_deploy.py`.
`--validation-dir` additionally feeds one held-out frame to tpu-mlir's own
per-layer similarity check during deploy.

### Dilated-depthwise rewrite (on by default)

The v40c ONNX contains 3x3 depthwise convolutions with dilation 2 and 3.
**Both** nncase (K230) and tpu-mlir (cv181x) lower that native-dilation form
incorrectly on their INT8 paths. `compile_cvimodel.py` rewrites them into
sparse 5x5 and 7x7 kernels with dilation 1 before compiling — mathematically
exact, since it only inserts zeros between the original taps.

Measured on 16 held-out frames, tpu-mlir v1.3.228 targeting cv181x:

| build | mean abs error | heatmap peak within 2 cells | size |
|---|---|---|---|
| rewritten (default) | **0.04434** | **16/16** | 43,856 B |
| `--keep-native-dilated-depthwise` | 0.34617 | 0/16 | 41,168 B |

The K230 saw the same failure at nearly the same magnitude (0.353 vs 0.028), so
this is a property of the construct, not of one vendor's compiler.
`--keep-native-dilated-depthwise` exists only to re-measure this on a future
toolchain. Do not deploy its output.

## 4. Validate before flashing

```sh
tools/tinytag_cvimodel/tpu_docker.sh run \
  tools/tinytag_cvimodel/validate_cvimodel.py \
    --onnx /assets/tinytag-v40c-unfrozen-moderate30ep/tinytag-v40c-unfrozen-moderate30ep.static.onnx \
    --cvimodel tools/tinytag_cvimodel/work/tinytag-v40c.int8.cvimodel \
    --images tools/tinytag_cvimodel/work/frames/validation
```

Runs both models on the host — the cvimodel through tpu-mlir's bundled cv18xx
simulator (`pyruntime_cvi`), the ONNX through ONNX Runtime — and gates on mean
absolute error <= 0.05 and >= 90% of heatmap maxima within 2 output cells, the
same criteria the K230 flow used.

**Passing the simulator is necessary but not sufficient.** On K230 an earlier
TinyTag model passed simulation and still corrupted memory on real hardware.
This gate can reject, never fully approve.

## 5. Build the golden bundle for on-board self-checking

```sh
tools/tinytag_cvimodel/tpu_docker.sh run \
  tools/tinytag_cvimodel/make_golden.py \
    --onnx /assets/tinytag-v40c-unfrozen-moderate30ep/tinytag-v40c-unfrozen-moderate30ep.static.onnx \
    --cvimodel tools/tinytag_cvimodel/work/tinytag-v40c.int8.cvimodel \
    --images   tools/tinytag_cvimodel/work/frames/validation \
    --output   tools/tinytag_cvimodel/work/tinytag-v40c.ttgold
```

Records, per frame, the exact uint8 input, the FP32 ONNX output, and the INT8
simulator output. `apps/tinytag_detect/build.sh` stages the result into the
image, where `run_tinytag.sh --selftest` replays it on the real TPU and checks
both the quantization error and any hardware-versus-simulator divergence. See
`apps/tinytag_detect/README.md` for what the numbers mean.

Regenerate this whenever the cvimodel changes — a stale bundle silently
validates the wrong model.

## 6. Inspect a compiled model

```sh
tools/tinytag_cvimodel/tpu_docker.sh run \
  bash -lc 'model_tool --info tools/tinytag_cvimodel/work/tinytag-v40c.int8.cvimodel'
```

Shows the declared program inputs/outputs, the tensor map with per-tensor
quantization scales, and the weight map (where you can confirm the dilation
rewrite landed: `backbone.7.2` should be a 5x5 filter and `backbone.8.2` a 7x7).

## Current status

The committed flow has been run end to end and produces a **passing** model
(MAE 0.04434, 16/16 peaks). One caveat worth carrying forward: it was
calibrated on only 50 frames, 49 of which come from a single 6.5 s clip, and
its MAE sits close to the 0.05 gate (the K230 model reached 0.028 with >= 100
frames from varied scenes). Re-run step 2 with broader raw footage and
recompile before treating the model as production-grade.
