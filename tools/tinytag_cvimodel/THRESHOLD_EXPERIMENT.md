# ArUco Nano threshold branch on Duo S

This experiment runs ArUco Nano's 15×15 local-mean threshold on the SG2000
TPU. It includes standalone 640×360 and 1280×720 models and a two-output
model with v4c proposals and a 640×360 threshold output from one grayscale input.
The detector consumes the proposal output; the threshold output is measured and
saved separately. It does **not** feed the threshold mask to v4c or change the
CPU AprilTag crop decoder.

## Build

Use the pinned `tinytag-tpu-mlir:v1.3.228` container. Copy the v4c ONNX into
`tools/tinytag_cvimodel/work/` or supply another container-visible path.

```sh
tools/tinytag_cvimodel/tpu_docker.sh run \
  python3 tools/tinytag_cvimodel/build_threshold_experiment.py \
  --v4c tools/tinytag_cvimodel/work/tinytag-v11_k230-v4c.onnx \
  --calibration-dir tools/tinytag_cvimodel/work/frames/calibration \
  --output-dir tools/tinytag_cvimodel/work/threshold_rebuild \
  --calibration-count 20
```

The build produces BF16 and INT8 CVIMODELs for:

- `aruco_threshold_640_sep` and `aruco_threshold_1280_sep`: standalone threshold outputs;
- `v4c_fused_input_baseline`: matched v4c baseline;
- `v4c_threshold_branch`: v4c proposals plus the threshold output.

The ONNX input is FP32 pixel values in `[0,255]`; fused preprocessing makes the
deployed CVIMODEL input a raw uint8 grayscale plane. The v4c branch divides by
255 internally. The threshold branch uses two separable convolutions with
replicate padding. The original 15×15 convolution exceeded the CV181x
compiler's tile policy at 1280×720. The exact FP32 threshold predicate
`round(local_mean) - pixel > 3` is equivalent to `local_mean - pixel > 3.5`
for an integer 15×15 sum, because 225 is odd. A clipped arithmetic ramp avoids
the compiler's unsupported `CompareConst` lowering. ONNX Runtime produced the
same binary masks as the original threshold ONNX on the tested photos at both
resolutions. The v4c proposal output was numerically identical to the original
v4c ONNX in the host FP32 check.

Build the board benchmark in the SDK's `duodocker` container:

```sh
docker exec duodocker bash -lc \
  'cd /home/work && cmake --build apps/tinytag_detect/build_milkv-duos-glibc-arm64-sd --target threshold_bench tinytag_detect -j4'
```

Copy the CVIMODELs, `threshold_bench`, and exact-size grayscale PNGs to a
scratch directory on the board. For example, the experiment used
`/tmp/threshold_experiment/` and left the installed models untouched. A
640×360 PNG was made by cropping the top 80 rows from the 1280×800 arena
sample, then resizing the remaining 1280×720 band; the 1280×720 PNG used the
band directly. `threshold_bench` requires an exact-size image and writes a
0/255 mask PNG:

```sh
LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib \
  /tmp/threshold_experiment/threshold_bench \
  /tmp/threshold_experiment/aruco_threshold_640_sep.int8.cvimodel \
  /tmp/threshold_experiment/arena_640x360.png 30 \
  /tmp/threshold_experiment/arena_640_int8_mask.png
```

**Output caveat:** The original compiled branch has an output scaling defect:
its raw values range roughly from −20 million to +14 million because the final
Clip/Mul LUT is lowered with an unexpected scale. The PNGs and error counts
below are diagnostic masks made by the benchmark's software cutoff of 127.5;
they are not directly usable raw 0/255 TPU masks. The striped signed-difference
variants below avoid this LUT but require a CPU cutoff of 3.5.

The `mismatch_pixels` printed by this binary uses the board's OpenCV 3.2
`boxFilter` implementation. For comparison with the earlier host notebook's
original ONNX mask, compare the saved PNG against that model in ONNX Runtime;
the two OpenCV versions do not produce identical reference masks on every
pixel. `fractional_raw` counts TPU outputs strictly between 0 and 255 before
the PNG binarization at 127.5.

To run the merged model through the existing launcher, build and copy the
updated `tinytag_detect` binary to the same board scratch directory, then:

```sh
TINYTAG_BIN=/tmp/threshold_experiment/tinytag_detect \
TINYTAG_MODEL=/tmp/threshold_experiment/v4c_threshold_branch.int8.cvimodel \
TINYTAG_REPEAT=50 TINYTAG_WARMUP=3 \
TINYTAG_OUT=/tmp/threshold_experiment/merged.jpg \
  /app/tinytag_detect/run_tinytag.sh \
  /app/tinytag_detect/samples/arena-1280x800.jpg
```

Swap `TINYTAG_MODEL` to `v4c_fused_input_baseline.int8.cvimodel` for the
matched baseline, or use the `.bf16.cvimodel` pair. Both outputs are retained
in the compiled merged graph. The updated app selects the 21-channel proposal
map from a multi-output model. The threshold branch output can also be read by
`threshold_bench`; its benchmark-converted PNG was byte-identical to the standalone output for
both BF16 and INT8 in this run.

## Measurements on Duo S (SG2000)

Measurements used 2 warmups and 30 TPU inferences for standalone models, and
3 warmups and 50 detector runs for each merged/baseline trial. Three order
balanced pairs were run for each precision. Times are medians in milliseconds;
the detector figures are medians of the three trial medians. Images were read
from storage before timing. The standalone rows time `CVI_NN_Forward` only;
the detector total includes image preprocessing, TPU inference, proposal
decoding, and ArUco Nano crop decoding, but excludes camera capture.

| Model | BF16 TPU ms | INT8 TPU ms | BF16 converted-mask error vs host ONNX | INT8 converted-mask error vs host ONNX |
| --- | ---: | ---: | ---: | ---: |
| 640×360 arena threshold | 8.476 | 3.831 | 4,066 / 230,400 (1.765%) | 9,251 / 230,400 (4.015%) |
| 640×360 second photo threshold | 8.470 | 3.825 | 3,077 / 230,400 (1.336%) | 4,860 / 230,400 (2.109%) |
| 1280×720 arena threshold | 33.503 | 15.013 | 16,442 / 921,600 (1.784%) | 43,781 / 921,600 (4.751%) |

| Detector on arena sample | BF16 TPU ms | BF16 total ms | INT8 TPU ms | INT8 total ms |
| --- | ---: | ---: | ---: | ---: |
| Matched v4c baseline | 11.443 | 18.387 | 2.296 | 10.023 |
| v4c plus threshold branch | 19.346 | 26.303 | 5.890 | 13.610 |
| Added cost | 7.903 | 7.916 | 3.594 | 3.587 |

For each precision, the baseline and merged model produced byte-identical
decoded-tag JSON, including IDs, centers, and corners, in all six detector
trials. BF16 detected three tags on this image. The experimental INT8 pair,
calibrated on 20 frames, detected two. The board's previously installed v40c
INT8 model detected three and ran inference in about 2.10 ms; it has a
different calibration and graph, so it is context rather than the matched
baseline. The benchmark-converted threshold mask's BF16 error was one-sided on these images
(extra foreground); INT8 had mainly missing foreground. These accuracy limits
matter if the TPU mask is later used for actual ArUco decoding.

The raw per-trial logs, masks, and calibration artifacts from this run are in
`tools/tinytag_cvimodel/work/` (ignored by Git). The board copies are under
`/tmp/threshold_experiment/` and will disappear after reboot.

## Stripe-count and bottleneck follow-up

`make_striped_threshold.py` divides the image into overlapping vertical strips,
packs them into **channels** (batch remains 1), applies grouped 15×1 and 1×15
convolutions, then reassembles the local mean and subtracts the original pixel.
Seven halo rows on each side preserve the filter at strip boundaries. When the
stripe count does not divide the image height, the bottom is replicate-padded
and cropped after filtering. In ONNX Runtime, the FP32 predicate
`difference > 3.5` was byte-identical to the original ONNX mask on the tested
arena inputs, including 12 and 16 stripes.

For example, generate and compile one variant in the pinned TPU container:

```sh
tools/tinytag_cvimodel/tpu_docker.sh run python3 \
  tools/tinytag_cvimodel/make_striped_threshold.py \
  --width 640 --height 360 --stripes 8 \
  --output tools/tinytag_cvimodel/work/aruco_striped_8.onnx
tools/tinytag_cvimodel/tpu_docker.sh run python3 \
  tools/tinytag_cvimodel/build_stripe_sweep.py \
  --work-dir tools/tinytag_cvimodel/work \
  --calibration-640 tools/tinytag_cvimodel/work/frames/calibration \
  --calibration-1280 tools/tinytag_cvimodel/work/threshold_rebuild/calibration_1280x720 \
  aruco_striped_8
```

Run `threshold_bench` on the resulting `.int8.cvimodel` with cutoff `3.5` as
its sixth argument. `--clip 8` adds a range clip to the ONNX signed difference;
it improves INT8 resolution near the threshold but adds a full-frame LUT.

| Stripes | 640×360 INT8 median | 1280×720 INT8 median |
| ---: | ---: | ---: |
| 4 | 1.352 ms | 5.586 ms |
| 8 | **1.216 ms** | **5.204 ms** |
| 12 | 1.466 ms | 5.375 ms |
| 16 | 1.470 ms | 5.292 ms |

These use two warmups and 30 `CVI_NN_Forward` measurements per model. All four
counts produced the same board INT8 mask at a given resolution. The 640×360
eight-stripe mask differed from original FP32 ONNX at 19,982 / 230,400 pixels
(8.673%) on the arena sample. Clipping the output to ±8 changed inference to
2.138 ms and reduced error to 8,224 pixels (3.569%). The 1280×720 eight-stripe
mask differed at 69,560 / 921,600 pixels (7.548%). These signed outputs still
need CPU binarization, and none has been fed into the tag crop decoder.

PMU reports the original 640×360 full-mask INT8 model at 1.92 ms TDMA and
2.10 ms TIU; 1280×720 used 8.99 and 8.29 ms. The eight-stripe signed-difference
graph used 1.02 ms TDMA and 0.33 ms TIU at 640×360, and 4.74 and 1.30 ms at
1280×720. TDMA and TIU overlap, so these do not sum to wall time. The optimized
1280×720 graph is mainly limited by tensor movement (15.91 MB reported by PMU),
including halo and reassembly traffic.

The merged original graph has one `image_raw` input and one TPU core. Its
preprocessing scale LUT feeds both branches; v4c uses an additional normalization
LUT. The 640×360 merged INT8 inference cost was 3.594 ms over its matched v4c
baseline, about 0.24 ms less than the standalone branch's 3.831 ms. This shows
modest input/preprocessing reuse, not concurrent TPU convolution. A 1280×720
branch would need a separate input surface or a new graph, and its end-to-end
overlap with v4c has not been benchmarked.

The live camera's default compact physical-input path already binds a dense
640×360 VPSS luma plane to the model tensor if stride is 640. Its crop-local
threshold work measured about 1.65 ms on 14 arena crops with the installed v4c
model. Running a 640×360 CPU threshold during TPU inference while reserving the
NPU for a 1280×720 threshold is a future scheduling experiment. Replacing
crop-local thresholding with a full-frame mask also needs a geometry/border
accuracy check, because crop warping changes the pixels seen by the local mean.

## End-to-end external-mask Nano decoder

The still-image and live detector binaries now accept
`--threshold-model /path/to/aruco_striped_1280_8.int8.cvimodel`. Use it alongside
`--decode strict` or `--decode tolerant`. The feature is opt-in; point-level
LDC decoder mode rejects an external mask. For example on the board:

```sh
TINYTAG_BIN=/tmp/threshold_experiment/tinytag_threshold_e2e \
TINYTAG_REPEAT=40 TINYTAG_WARMUP=3 \
  /app/tinytag_detect/run_tinytag.sh \
  /app/tinytag_detect/samples/arena-1280x800.jpg \
  --threshold-model /tmp/threshold_experiment/aruco_striped_1280_8.int8.cvimodel \
  --bench-json /tmp/threshold_experiment/e2e_fast.jsonl
```

At initialization, the second CVIMODEL registers its tensors and allocates one
1280×720 binary mask surface. Its input/output tensor pointers are cached. A
full-resolution frame contributes its bottom-left 1280×720 band to the second
model; the NPU returns signed INT8 local-mean-minus-pixel values, which a NEON
integer cutoff converts to 0/255 in the persistent surface. Proposal crops
map to that surface with the band offset. ArUco Nano contour tracing receives
a copied mask ROI in scratch storage allocated at decoder construction, since
contour tracing modifies its input. Marker-bit sampling and subpixel refinement
still read the original grayscale ROI. The threshold-model input copy and
full-frame conversion are per-frame data movement; neither allocates a new
surface. The existing proposal model remains at 640×360.

Three order-balanced matched still-image pairs used the installed v4c INT8
model on the arena 1280×800 image, 40 repeats and three warmups. Median total
time was **14.916 ms** with stock crop-local thresholding and **19.430 ms** with
the 1280×720 NPU mask; crop stage medians were 10.372 and 14.852 ms. The NPU
mask path spent medians of 0.758 ms on input copy, 5.365 ms on its inference,
and 0.679 ms on signed-output binarization. All runs detected IDs 25, 21, 24,
23 with byte-identical JSON, including corners.

Four deterministic full-resolution arena variants checked geometry beyond the
original sample. Brightness +20 and Gaussian blur σ0.7 gave exact output JSON.
Contrast ×0.75 changed matched corners by at most 0.0018 px. Gaussian noise
σ5 changed the tag set: stock returned 21,24,23; the NPU-mask path also found
25. These differences can come from both full-frame versus crop-local border
semantics and INT8 mask error; this experiment has not separated them.

A bounded recorded-video replay through `run_live.sh --input
/app/tinytag_detect/rec_mono.mp4 --threshold-model ...` initialized the mask
once and processed 158 frames in five seconds with no VPSS pairing mismatch.
Per-frame profile reported roughly 0.82 ms input copy, 5.4–5.7 ms second TPU
inference and 0.8 ms mask conversion. The full board results, logs, JSON and
annotated images are copied into the notebook's `board_threshold_results/`.
