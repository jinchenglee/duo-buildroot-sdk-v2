# Duo S performance findings: TinyTag two-stage detector

Measured on a Milk-V Duo S (SG2000), image built from this tree. Companion to
`docs/ov9281-camera-port.md`, which covers the missing camera stage.

Everything here is a measurement or a citation. Where something is an
inference, it says so; where a number was never obtained, it says that too.

## Headline

The full two-stage pipeline -- TPU proposals plus ArUco Nano AprilTag 36h11
crop-decode, everything except camera capture -- runs in **11.25 ms
(88.9 fps)** on a 1280x800 frame with 7 proposals at threshold 0.35.

    ---- timing, ms over 20 run(s) after 2 warmup ----
      pre_process  min   1.666  median   1.727  mean   1.979  max   7.060
      inference    min   2.094  median   2.109  mean   2.126  max   2.299
      decode       min   0.899  median   0.902  mean   0.908  max   0.937
      crop_decode  min   6.442  median   6.532  mean   6.653  max   8.596
      total        min  11.121  median  11.251  mean  11.666  max  17.610

      TPU inference    : 474.2/s at the median
      this pipeline    : 88.9 frames/s at the median
      per-ROI decode   : 0.93 ms over 7 crop(s)

Reproduce with:

    tinytag_detect /app/tinytag_detect/cvimodel/tinytag-v40c.int8.cvimodel \
        /app/tinytag_detect/samples/arena-1280x800.jpg --decode --repeat 20

## Budget breakdown

| stage | median | share | what it is |
|---|---|---|---|
| pre_process | 1.73 ms | 15% | crop to the 1280x720 band, resize to 640x360, fill tensor |
| inference | 2.11 ms | 19% | `CVI_NN_Forward` on the TPU |
| decode | 0.90 ms | 8% | sigmoid, 3x3 NMS, top-K, box decode over a 45x80 grid |
| crop_decode | 6.53 ms | **58%** | per-ROI ArUco Nano, 0.93 ms x 7 crops |

**The NPU is not the bottleneck and will not become one.** At 2.11 ms the
network could run 3-4x per frame and the pipeline would still be CV-bound.
Optimization effort belongs in stage two, or in reducing how many ROIs reach it.

## Over-proposing is the architecture, not a defect

Proposals with no tag inside are a **design feature**, not an inefficiency to
be trained away. The two-stage split exists because precision is expensive in a
network and cheap in a verifier. Stage one's job is not to be right; it is to
reduce a 1280x800 = 1.02M-pixel search to at most 8 crops of roughly 60x80.
Stage two then buys precision at 0.93 ms per candidate -- less than half what
the whole network costs at 2.11 ms.

The measured numbers make the trade explicit. Eliminating the 3 non-decoding
proposals in the sample frame would save 2.8 ms, but would require a materially
larger network: even a 2x model costs +2.11 ms, would still not reach zero
false positives (no network does), and would therefore still need the verifier
for the residue. Most of the saving is spent, and the worst case gets worse.

This also inverts with verifier cost: the *cheaper* stage two is per ROI, the
*more* stage one should be allowed to over-propose. At 0.93 ms against a
2.11 ms network, the verifier is cheap here, which argues for pushing recall
up -- consistent with the threshold being free below the cap, below.

## The frame time is bounded, and N is the knob

The cost of over-proposing is bounded, which is what makes it safe.

What matters is that the cost is **bounded**. `decode_proposals()` sorts peaks
by confidence, truncates to `max_proposals` (`--max`, default 8), and only then
runs IoU suppression, so the number of crops reaching the decoder is capped
regardless of scene content. Worst-case frame time is therefore deterministic:

    fixed = pre_process 1.73 + inference 2.11 + decode 0.90 = 4.74 ms
    total = fixed + N x 0.93 ms

| N (`--max`) | crop_decode | total | guaranteed fps |
|---|---|---|---|
| 4 | 3.72 ms | 8.46 ms | 118 |
| 6 | 5.58 ms | 10.32 ms | 97 |
| **8** (current default) | **7.44 ms** | **12.18 ms** | **82** |
| 12 | 11.16 ms | 15.90 ms | 63 |
| 16 | 14.88 ms | 19.62 ms | 51 |
| 20 (training repo default) | 18.60 ms | 23.34 ms | 43 |

The 11.25 ms measured above was 7 crops, one under the cap.

**`--max` is the latency knob; `--thres` is the recall knob**, and they
interact in only one direction. Once enough peaks clear the threshold to fill
the cap, lowering the threshold further costs nothing -- it only changes which
candidates win the top-N slots. On K230, threshold 0.20 yielded ~14
proposals/frame against ~7 at 0.35; with N=8 both saturate, so the lower
threshold buys recall for free. Pick N from the latency budget, then pick the
threshold as low as precision tolerates.

## Other levers

1. **A newer OpenCV.** The board ships OpenCV **3.2** (2016). 4.x has
   substantially better aarch64 NEON coverage in exactly the functions ArUco
   Nano leans on. Untested here; listed as the next lever, not a promise.
2. Algorithmic work on the decoder, last. Note there is no thread parallelism
   to exploit -- see "Machine characteristics".

## Comparison with K230 -- read carefully

| stage | Duo S @1280x800 | K230 @1280x720 |
|---|---|---|
| pre_process | 1.73 ms | 2.4 ms (hardware ai2d) |
| inference | 2.11 ms | 2.2 ms (KPU) |
| decode_proposals | 0.90 ms | <0.5 ms |
| crop_decode | 6.53 ms | 10.7 ms |
| total | 11.25 ms (88.9 fps) | ~15.25 ms (~65 fps) |

The Duo S is faster at every measured stage, **but this is not an
A53-vs-C908 microarchitecture result and must not be cited as one.**

Both cores are 2-wide in-order superscalar, so issue width is a wash. The
difference is SIMD availability, and it comes straight out of the K230 build
config. The K230 application is built under `k230_canmv_small_core_defconfig`;
that core has no RISC-V vector extension, so its OpenCV is compiled scalar
(`buildroot-overlay/package/opencv4/opencv4.mk` in the K230 SDK):

    # The small core has no V extension; CSI-CV is a prebuilt RVV library.
    OPENCV4_CONF_OPTS += -DBUILD_CSI_CV=OFF -DCV_ENABLE_INTRINSICS=OFF
    OPENCV4_CONF_OPTS += ... -mcpu=c908 ...      # no 'v'

The Duo S has NEON unconditionally -- aarch64 makes it mandatory, `asimd` is in
`/proc/cpuinfo`, and `libopencv_imgproc.so.3.2` contains 439 vector
instructions. ArUco Nano's hot path (`adaptiveThreshold`, `findContours`,
`getPerspectiveTransform`) is almost entirely byte-wise image processing, which
is exactly where that matters.

So this measures **NEON-vectorized OpenCV against deliberately scalar OpenCV**.
The 10.7 ms K230 figure is not that chip's best: on its big core with RVV and
the CSI-CV library, the same work could be considerably faster.

### A superseded projection, kept as a caution

Before the decoder was ported, this work projected ~25 ms (~40 fps) by scaling
K230's crop-decode by the 0.90-vs-<0.5 ms `decode_proposals` ratio, reasoning
that scalar CPU work was ~2x slower here. That was wrong by more than 2x in the
pessimistic direction: `decode_proposals` is a sub-millisecond loop over a
45x80 grid and a poor proxy for a 10 ms workload, and the underlying premise
(slower scalar CPU) was backwards once SIMD was accounted for. Measure the
stage you care about.

## Machine characteristics

- **One core.** The arm64 device tree declares exactly one Cortex-A53
  (`cpu@0`, no `cpu@1`) in
  `build/boards/default/dts/cv181x_arm64/cv181x_base_arm.dtsi`. The SG2000's
  C906 runs FreeRTOS in this configuration (`CONFIG_ENABLE_FREERTOS=y`). All
  timings above are single-threaded with no parallelism available.
- **NEON present**: `Features : fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid`.
- **CPU clock: not recorded.** It is absent from the device tree, and there is
  no cpufreq driver, so `scaling_cur_freq` is empty -- the A53 PLL is set by
  fsbl before Linux starts. Read it from
  `/sys/kernel/debug/clk/clk_summary`, row `clk_a53` (a mux over
  `clk_xtal_a53` at 25 MHz and `clk_div_{0,1}_a53`). **Do not assume a value**:
  earlier notes in this work asserted 1 GHz from memory and it was never
  verified.

## Numerical correctness

The TPU reproduces tpu-mlir's cv18xx simulator **bit-exactly**:

    hardware-INT8 vs FP32 : worst frame MAE 0.04709  (gate <= 0.05)
    heatmap peak match    : 4/4 within 2 cells
    hardware vs simulator : worst element 0.000000

That closes the risk inherited from K230, where a kmodel passed simulation and
then misbehaved on real silicon. Host validation transfers to this hardware.

Proposal-level agreement with the FP32 ONNX reference (via
`tools/tinytag_cvimodel/reference_proposals.py`) is within ~1.4 px. Occasional
low-confidence peaks shift one grid cell or drop out, because the heatmap logit
quantizes to ~0.045 steps and ties change which cells survive the 3x3
max-pool -- an INT8 effect, not a port defect.

Decoded ids on `arena-1280x800.jpg` are 23, 21, 25, 24, at plausible positions
(id 23 on the left wall, 21/24/25 clustered on the hub structure).

## Tail latency

`pre_process` showed a 7.06 ms max against a 1.73 ms median, pushing total max
to 17.6 ms -- a ~6x spike. Medians are stable across runs, so this looks like
scheduling or cache behaviour rather than anything algorithmic. It has not been
investigated. For a real-time camera loop tail latency is what drops frames, so
it is worth a longer `--repeat` and a look at whether it correlates with other
system activity.

## Not yet measured

- Camera capture. See `docs/ov9281-camera-port.md`.
- OSD compositing. The application draws into an output JPEG, but there is no
  display path; the K230's DRM overlay work is not ported.
- `--decode tolerant`, which accepts marginal tags and should be slower.
- Any threshold/IoU sweep.
- Whether a newer OpenCV helps.
