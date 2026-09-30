# Duo S lens-correction performance and geometry findings

Migrated from the retired handover on 2026-09-30. These are historical
OV5647 measurements and calibration/deployment records, including the
2026-09-25 software and point-LDC work. They are not current binary identities
or current OV9281 performance claims. Scene, sensor mode, calibration, and
preview policy matter when comparing results.

For current point-mode behavior and its validation, use
[the point-LDC document](ldc-point-correction.md). Current build and launch
instructions are in the [app README](../apps/tinytag_detect/README.md).

## Hardware LDC implementation checkpoint

This records the opt-in VPSS lens-distortion correction work on branch
`hardware-ldc`. The branch was based on `d83c63269` (`feat(tinytag_detect):
replay MP4 recordings through VDEC with --input`).

### Implementation and invariants

- Both live AprilTag applications accept `--ldc-calibration FILE.json`;
  correction stays disabled when the option is omitted.
- TinyTag applies LDC to its 1280x720 detector/crop channel and, with direct
  compact TPU input enabled, independently to its 640x360 model channel.
  ArUco Nano applies it to its 1280x720 channel.
- The shared loader checks the calibration aspect ratio and hardware parameter
  ranges, scales center offsets to each output size, and configures VPSS LDC.
- GDC-aligned surfaces are 1280x768 for visible 1280x720 and 640x384 for
  visible 640x360. The apps allocate output and intermediate VB blocks for
  those surfaces, honor valid-area offsets, and reject malformed GDC frames.
- TinyTag requires matching `u32TimeRef` and `u64PTS` between model and full
  frames. It discards stale/mismatched pairs; never weaken this to combine
  frames that merely arrived most recently. The mismatch counter can increase
  under load because channel readiness differs; it counts skipped pairs, not
  cross-frame pairings.
- TinyTag's `--save-ldc-pair PREFIX` diagnostic saves synchronized aligned
  outputs. `--save-frame` saves a visible detector frame for review.

### Calibration and geometry

Use `tools/ldc_calibrate.py` and `tools/ldc_calibrate.md`. Inputs can be images,
recorded video, or a live OpenCV source. `--pattern W H` means internal
checkerboard intersections. The calibration code scales the complete input
frame to 1280x720; it does not crop away the first 80 lines of a 1280x800
source. `--corrected-dir` writes review images without requiring an OpenCV GUI.
Those OpenCV-undistorted images are a visual reference, not an exact simulation
of the hardware's simpler radial mapping.

At the recorded checkpoint, the untracked `tools/ldc-calibration.json` was generated from
`tools/calibration.mp4`, recorded in 1920x1080/30 mode. It contains ratio `-271`,
center `(-17,-8)`, and view ratio `100`; OpenCV RMS is about 0.79 px and the
SG2000 one-ratio fit RMS about 2.89 px. These parameters belong to this lens and
sensor mode and are not production-certified.

The OV5647 1280x720/60 mode reads a wider, binned sensor region than 1080p/30.
The identical VPSS output dimensions do not make those camera mappings
equivalent. Calibrate 720p/60 separately before using its corrected output for
pose estimation. The `tools/ldc-review/` directory and
`tools/ldc-synced-*.png` files are visual diagnostics.

### Mesh cache

The mesh path is automatic; there is no CLI or JSON field for an arbitrary
filename. Mesh files are hidden sidecars in the calibration JSON's directory,
named `.sg2000-ldc-<hash>.mesh`. The hash includes output dimensions and the
applied LDC parameters. For the calibration at that board checkpoint:

```text
/root/.sg2000-ldc-ffef43e250457966.mesh  # 1280x720, 368640 bytes
/root/.sg2000-ldc-b7a308e9db37f860.mesh  # 640x360, 92160 bytes
```

Moving the calibration JSON moves the cache directory. Delete a matching mesh
to force regeneration. The SDK-specific binary mesh should be regenerated
after an SDK or firmware change. A non-writable cache directory only disables
persistence; LDC still generates a mesh for that run. Directory selection is
in `apps/common/ldc_config.cc`; key and naming are in
`apps/common/ldc_config.h`.

The first cold run generated the 1280x720 and 640x360 meshes in about 13 s and
3.4 s. Warm loads were around 1-2 ms each after the filesystem cache warmed
(an earlier small-mesh read was about 30 ms). Caching removes startup work; it
does not lower the per-frame correction cost.

### Board performance and interpretation

These board measurements depend on scene and mode:

| Duo-S test | Approx. rate |
| --- | ---: |
| 1080p/30, no LDC, RTSP luma | 31 fps |
| 1080p/30, LDC on both TinyTag channels, RTSP luma | 14-15 fps |
| 1080p/30, one corrected full channel, capture only | 31 fps |
| 1080p/30, one corrected full channel copied/resized to TPU, RTSP | about 31 fps |
| 720p/60, no LDC, capture only | about 63 fps |
| 720p/60, one corrected channel, capture only | about 31 fps |
| 720p/60, one corrected channel, copied TPU input and RTSP | about 13 fps |

The `/proc/cvitek/gdc` snapshot for a 1280x720 LDC job showed about 16.6 ms
`CostTime` and 4.2 ms `HwTime`. `CostTime` is submit-to-completion wall time;
`HwTime` accumulates measured task-execution intervals. Thus the 16.6 ms is
not 16.6 ms of accelerator pixel math. The remaining elapsed time is outside
those hardware intervals; this counter does not further split queue, driver,
scheduler, and interrupt costs. At 60 fps the frame budget is 16.7 ms, leaving
almost no headroom, consistent with the measured ~31 corrected fps.

Sophgo documents two sequential 90/270-degree tasks per LDC correction. The
CV181x driver builds a job with both tasks, allocates an intermediate VB
surface, and processes jobs with one `gdc_work` worker. A proc sample with both
TinyTag channels showed about 16-17 ms for the full channel and 7-9 ms for the
compact channel. This explains why mesh caching does not restore live
throughput.

For throughput, the current option is one corrected 1280x720 channel followed
by CPU copy/resize to TPU input:

```sh
TINYTAG_LIVE_DIRECT_COMPACT_INPUT=0 \
  /app/tinytag_detect/run_live.sh --rtsp-luma \
  --ldc-calibration /root/ldc-calibration.json
```

It avoids the second GDC job and keeps inference and crop decode on the same
corrected full frame. Direct corrected 640x384 input remains the default. If
60 fps corrected output is required, investigate the VPSS/GDC path and its
task/queue costs; mesh persistence will not improve frame rate.

### Software LDC comparison

`--ldc-mode sw` (TinyTag only) leaves VPSS/GDC uncorrected and remaps the
1280x720 detector frame on the CPU into a private four-block VB pool, so
detection and the borrowed `--rtsp-luma` Y plane both use corrected pixels.
It uses the calibration's full OpenCV model rather than the one-ratio fit.
The output camera matrix equals the calibrated matrix. The remap stores
source coordinates on a 16-pixel mesh (29 KB, max 0.044 px from
`cv::initUndistortRectifyMap`) and uses 7-bit bilinear weights. On AArch64,
NEON table lookups gather full 16-pixel runs; about 12% of runs use scalar
code because they span three source rows. At startup, the NEON and scalar
results must be bit-identical. Software LDC forces copied model input.

The board's OpenCV 3.2 `cv::remap` took 74 ms per frame on an idle core, and
its worker threads compete on the single core. The mesh remap took 44 ms
scalar and 25 ms NEON while idle, or 36 ms in the live loop. The CPU runs
at 800 MHz (`clk_a53`), so the NEON path costs about 22 cycles per pixel.
2026-09-25, 720p60, `--rtsp-luma --max-exposure-us 10000`, 15 s per case,
same scene; means exclude the first three windows:

| Case | fps | loop | ldc | detCPU | procCPU | acqAge | result-age p50/p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| no LDC | 61.1 | 14.40 | 0 | 9.16 | 14.66 | 5.25 | 20.6 / 28.3 |
| HW LDC, direct compact | 55.7 | 17.47 | 0 | 9.80 | 15.16 | 12.80 | 28.7 / 38.7 |
| HW LDC, copied input | 48.5 | 20.56 | 0 | 10.65 | 18.03 | 14.22 | 36.2 / 42.6 |
| SW LDC linear | 18.0 | 55.59 | 36.03 | 37.21 | 51.74 | 9.45 | 63.6 / 73.1 |
| SW LDC nearest | 25.1 | 39.79 | 21.50 | 25.60 | 36.92 | 9.09 | 47.6 / 58.5 |

In this bright-enough capped-AE run, hardware LDC with both channels ran at
56 fps, not the 14-15 fps of the earlier 1080p30 test. Hardware correction
adds roughly 7.5 ms of acquisition age but little CPU. Full-frame software
correction costs more CPU than the rest of the detector and loses on both
latency and throughput. A faster kernel alone cannot close that gap on this
single core. This experiment motivated correcting only the geometry detection consumes.
[Point LDC](ldc-point-correction.md) is now implemented as an opt-in path.
Corrected-ROI rescue remains an extension, not the implemented fallback.

### HW versus SW correction geometry

With the same 720p calibration JSON, the HW-corrected preview showed visible
distortion at the far left, while the SW-corrected preview did not. The two
modes read different parts of the JSON and show different fields of view:

| | HW (VPSS/GDC) | SW (`--ldc-mode sw`) |
| --- | --- | --- |
| Model | `sophgo_vpss_ldc`: one ratio (-250), center offset (-66,-50) | `camera_matrix` + `distortion_coefficients` (k1,k2,p1,p2,k3) |
| Checkerboard fit | 5.0 px RMS | 1.58 px RMS |
| Output view | Aspect gain 0.94; shows source x 69..1134, y -3..705 | Calibrated matrix; shows source x 89..1129, y 18..684 |

The SDK mesh generator (`cvi_mpi/modules/sys/src/gdc_mesh.c`, around line
1240) maps each output point with `r * (1 + k * r^2 / norm^2)`. `norm` is the
half-diagonal (734 px at 1280x720), and `r` is clamped to `norm`. It cannot
represent this lens's higher-order terms, and the clamp changes the edge
slope abruptly. For the current calibration, the source radius predicted by
each model is:

| Ideal radius | OpenCV source radius | HW source radius | Difference |
| ---: | ---: | ---: | ---: |
| 500 px | 437 | 442 | +5 px |
| 600 px | 500 | 500 | 0 px |
| 700 px | 553 | 541 | -12 px |
| 734 px | 570 | 551 | -20 px |

A host simulation, `gdc_mesh.c` compared with the OpenCV model, fits a
similarity on the central region and reports residual geometric error for
HW output pixels:

| Region of the HW output | Share | Mean | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| Also visible in SW | 90% | 4.6 px | 28 px | 48 px |
| HW-only band cropped by SW | 10% | 8.5 px | 31 px | 47 px |
| SW view, left 10% | | 5.3 px | 23 px | 48 px |
| SW view, right 10% | | 22.4 px | 41 px | 45 px |

Interpretation:

- Inside the shared view, SW is more accurate; this is not caused by cropping
  alone. The comparison uses the OpenCV model as the reference, so it is
  reliable only where the calibration views provided support.
- SW keeps the calibrated camera matrix, as `cv::undistort` does by default,
  and so discards about 10% of the HW view: roughly 89 px on the left, 150 px
  on the right, 18 px at the top, and 36 px at the bottom of the source. The
  distortion there remains; SW simply does not display it. A tag in that band
  is detectable with HW but not with SW.
- In that outer band, both models are probably extrapolating beyond the
  checkerboard coverage, so SW accuracy there is unverified.
- The simulation predicts that HW error is worst on the right, yet the
  observed artifact was on the left. That remains unexplained. A mirror
  applied after LDC, which would reflect the center offset, predicts about
  100 px errors on both edges, so it does not match either.

Next checks:

1. Save one frame of the same scene in each mode (`--save-frame`), with
   straight lines reaching all four edges. Compare line straightness on the
   left and right to confirm or refute the simulation.
2. For an equal field-of-view comparison, add a SW output zoom (for example,
   the HW aspect gain of 0.94) so that SW also shows the outer band. If SW
   lines bend there, recalibrate with checkerboard views that reach the edges
   and corners.
3. Refitting the HW ratio over the full visible radius, or reducing
   `view_ratio` to hide the outer band, can reduce HW error. A single
   coefficient cannot remove the k2/k3 shape mismatch.

## Historical build, deployment, and follow-up record

At the 2026-09-25 checkpoint, these incremental cross-builds produced the
recorded binaries. Use the current app build scripts for new deployments:

```sh
docker exec duodocker /bin/bash -c \
  'cd /home/work && cmake --build apps/tinytag_detect/build_ldc_milkv-duos-glibc-arm64-sd -j4'
docker exec duodocker /bin/bash -c \
  'cd /home/work && cmake --build apps/aruco_nano/build_ldc_milkv-duos-glibc-arm64-sd -j4'
```

The board is `root@192.168.42.1` on USB Ethernet (password `milkv`). Deployed
cache-enabled binary hashes were:

```text
/app/tinytag_detect/tinytag_detect_live  md5 2302867945627dbba82f8f2d785badfb  (point LDC, 2026-09-25)
/app/aruco_nano/aruco_nano              md5 44b69326e86fe58e3140ad75f3d1e36a
```

The previous live binary (md5 affb9109217a84660fa9c52e556bdcf1, with SW LDC)
is kept as `tinytag_detect_live.before-point-ldc`. Recoverable backups are beside the binaries with suffix
`.before-ldc-mesh-cache`; earlier backups with `.before-ldc-vb-fix` also exist.
At that checkpoint, the last bounded smoke test exited cleanly and left no app process running.
Board trial logs are under `/tmp/ldc-cache-*.log`,
`/tmp/aruco-cache-warm.log`, `/tmp/bench-720-*.log`, and
`/tmp/ldc-production-smoke.log`; `/tmp` may be cleared on reboot. Compare
`md5sum` on board binaries before relying on those measurements.

Remaining work:

1. Validate corrected straight-line residuals and AprilTag corner/pose error
   across the center, edges, and corners. Checkerboard fit alone is not enough.
2. Calibrate 720p/60 separately if that mode remains a target.
3. Profile `CostTime` versus `HwTime` across one 640x360 channel, one 1280x720
   channel, and both channels; locate reducible driver, task, or queue overhead
   without changing geometry or exact frame pairing.
4. Keep LDC opt-in until its geometry benefit and performance trade-off are
   accepted. Decide separately whether a configurable cache directory is
   worth adding; currently moving the calibration JSON moves the cache.

### Local camera artifacts

The checkpoint recorded camera-specific untracked data: `tools/calibration.mp4`
(about 39 MB), `tools/ldc-review/` (about 116 MB),
`tools/ldc-calibration.json`, and synchronized PNG captures. Preserve these
while working, but review deliberately before staging. The video and review
images should generally stay out of source commits. Include the JSON only if
this branch should carry this particular lens/mode's calibration.
