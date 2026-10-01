# Point-Level Lens Distortion Correction for AprilTag on Wide-FOV (Duo-S)

Status: implemented and opt-in on `roi_on_mask` (also in `origin/loop-ldc`).
Use `--ldc-mode point --ldc-calibration FILE.json` with tag decoding enabled.
The calibration needs `camera_matrix` and `distortion_coefficients`. The live
launcher does not enable point LDC by default. The implementation is in
`apps/common/point_ldc.{h,cc}` and `apps/tinytag_detect/tag_crop_decoder.cc`;
board and synthetic validation are recorded below. Hardware/software
comparisons are in [the LDC findings](ldc-performance-findings.md). This note
also records limits and possible future work.

## Why not full-frame LDC

Measured trade-off (Duo-S, 720p60 unless noted):

| Correction | fps | loop ms | note |
| --- | ---: | ---: | --- |
| none | 61 | 14.4 | detection path, camera-bound |
| HW (VPSS/GDC), direct compact | ~56 | 17.5 | ~16.6 ms submit-to-completion per full channel |
| HW (VPSS/GDC), copied input | ~48 | 20.6 | |
| SW remap (cv::remap, nearest) | ~25 | 39.8 | full-frame on one AArch64 core |
| SW remap (linear) | ~18 | 55.6 | costs more CPU than the rest of the detector |

Conclusions:
- Full-frame correction (HW or SW) is a fixed per-frame tax paid even for frames
  with zero tags, and it is too slow for a 30-60 fps target.
- SW is worse than HW, so "a faster SW kernel" cannot close the gap on the
  single-core CPU (A53 at 800 MHz).
- Hardware GDC uses a single-ratio (k-only) radial model. The
  [LDC findings](ldc-performance-findings.md) explain that this cannot represent
  the lens's higher-order (k2/k3) terms, so for
  a genuine wide-FOV lens the HW path is both slow AND the wrong model.

## Key insight: correct points, not regions

Lens distortion affects three separate stages of the detector, with different
requirements:

1. Quad detectability (can a candidate tag be fitted at all).
2. Bit-grid sampling (the tag's bit cells project to a curved grid in the image).
3. Corner/pose accuracy (corner positions).

The implemented path leaves the image uncorrected. It refines corner geometry
after a raw-image quad has been found and decoded. It can retry a raw quad that
failed bit decoding with a distortion-aware grid. It cannot recover a tag when
raw-image contour tracing or quad fitting found no candidate. Corrected-ROI
rescue for that case remains a proposal.

Iterating on only four detected corner points cannot recover edge curvature.
The implementation uses those corners to locate the edges, samples edge pixels
in the raw frame, undistorts the samples, and fits straight lines in the
corrected domain. It does not undistort the candidate contour itself.

## Pipeline

Implemented flow (no full-frame correction):

```
raw frame
  -> ArUco Nano threshold, contour trace, raw-image quad fit, bit decode
  -> for each decoded tag:
        sample the four raw-image edges near the decoded quad
        locate subpixel edge crossings and undistort those samples
        fit corrected-domain straight lines and intersect them
        return refined raw corners and ideal (pinhole) corners
  -> for selected rejected quads (fallback enabled by default):
        perform the same edge fit
        build a corrected-domain bit-grid homography
        forward-distort sample points into the raw image and retry ID decode
```

No `solvePnP` call is part of this detector path. The reported `ideal` corners
can be used by downstream pose estimation. Neither the TPU input nor the
preview is undistorted in point mode.

### Decode path

The normal first attempt is ArUco Nano's raw-image decode. Point LDC refines
its successful tag corners afterward. With `--point-ldc-fallback 1` (the
default), up to eight raw quad candidates per crop whose normal decode failed
are edge-refined and retried. Their bit samples follow a corrected-domain
homography forward-distorted into the raw frame. The fallback does not run for
a tag whose initial contour or quad was never produced.

### Pose path

The live app reports `ideal=(...)` corners in pinhole pixel coordinates when
point correction succeeds. A consumer can use them with the calibration's
camera matrix and zero distortion. Pose estimation is not implemented here.

### Missing-quad limitation

When contour tracing or raw-image quad fitting fails, point LDC has no seed
quad to refine. A corrected-ROI rescue is a possible future extension, not an
implemented fallback.

## Sampling along the edges

The current defaults are eight samples per edge, a 15% margin at both corners,
and two fitting passes. The first pass searches for subpixel edge crossings
near each raw quad side. The next pass projects the fitted corrected-domain
edge back into the raw image and searches near that curved edge. The code fits
each edge with Huber-reweighted least squares and intersects adjacent lines.

The physical tag edge is straight in the ideal image, so samples on that edge
can be fitted there without remapping the whole image. The initial raw quad
still has to be close enough for the edge searches to find the edge.

Error / balance:
- Variance (noise) of the line fit falls like ~1/K. Systematic bias (model
  residual, placement) is NOT reduced by K.
- Choose K just large enough that line-fit variance falls below the calibration
  model's own residual (the true accuracy floor on wide FOV). Beyond that, more
  samples buy nothing. Suggested K ~ 8-16 per edge; ~4-6 acceptable for clean
  edges.
- Placement matters: the implementation samples image intensity near the
  quad-guided edge and excludes the corner margins. It does not sample stored
  contour pixels.
- The robust line fit limits the influence of a stray edge sample.

Measured Duo S cost and corner-error comparisons are recorded below.

## Recorded validation and board cost

Migrated from the handover. These synthetic and OV5647 board results were
recorded on 2026-09-25; they do not establish accuracy or cost for a different
lens, calibration, camera, or scene.

Design and validation are in `tools/ldc_point_correction/`. The notebook
compares this with full-frame and ROI remapping on synthetic frames with ground
truth; `ldcpt.point_ldc_detect` is the Python reference for the C++.
Synthetic results, mean corner error in ideal px:

| Case | point LDC | ROI remap + stock | full remap (SW) | HW one-ratio sim |
| --- | ---: | ---: | ---: | ---: |
| 80 mm tags, 0.7-1.4 m | 0.135 | 0.30 | 0.27 | 3.2 |
| 120 mm tags, 0.35-0.7 m | 0.075 | 0.37 | 0.29 | 3.2 |
| lens distortion x1.5 | 0.103 | 0.45 | 0.25 | n/a |

Recall in those synthetic tests is 96-99%, against 60-74% for full remap, which loses the outer band.
C++ and Python agree to a median of 0.0001 px on the same frames. A few
small-tag outliers (up to 0.8 px) start from stock corners that differ by about
1 px between the two contour tracers; a third pass removes most of that.
`apps/tinytag_detect/point_ldc_check` (built for host and board) reruns this
check on a still frame and times it.

Duo-S cost, measured 2026-09-25:

| Measurement | Cost |
| --- | ---: |
| point stage per decoded tag, `point_ldc_check`, 80 mm synthetic tags | 0.34-0.36 ms |
| same, `test_post_ldc.png` | 0.38 ms |
| same, large tags | 0.53 ms |
| each fallback candidate | about 0.12 ms |
| live 720p60, one tag in view, `[point-ldc]` | 0.60 ms per frame |
| full-frame SW remap, for comparison | 25-36 ms per frame |

In the live run, the detector held 58-59 fps with capped AE, against 59-62 fps
without LDC; crop time varied 1-2 ms with the scene.
`cv::fitLine(DIST_HUBER)` cost 68 us per call on the A53, so the line fit is a
closed-form reweighted fit of about 2 us. The model round trip is exact to
1e-12 px on the board.

Limits:

- The specific 720p calibration used in these tests folds at a raw radius of 686 px. Three frame
  corners have no inverse, and samples there are dropped.
- Two passes do not fully converge from a stock quad that is off by about 1 px
  on small tags.
- The static-image `tinytag_detect` and ArUco Nano apps do not use this mode.

## Validation / next steps

1. Sweep K (e.g. 3, 6, 9, 12, 16) and corner-margin fraction; measure corner
   residual against a dense correction or full-ROI-remap reference on a
   checkerboard or synthetic grid.
2. Check per-frame recall and corrected corner accuracy at the frame edge on
   real scenes, including cases with no raw-image quad candidate.
3. Evaluate a corrected-ROI rescue if missing raw quads cause material misses.
4. Validate a downstream pose consumer separately if pose is needed.

## Open questions

- Whether candidate loops in far periphery need the corrected-ROI rescue; quantify
  where quad fit starts failing vs distortion.
- Whether forward-distorted bit sampling is as robust as an ROI remap for decode
  under strong radial terms; validate on the wide-FOV lens.
- Measure the speed and recall tradeoff of the optional fallback on the target
  lens and scene before changing the launch default.

## Related work and novelty framing

Point-level correction is a well-established tool in adjacent fields. The honest
contribution framing for this work is a SYSTEMS result (distortion-robust
detection under a hard single-core embedded real-time budget), not the
point-vs-image undistortion idea itself.

### Established precedent (must cite; not a novel claim)

- Feature-point undistortion (correct points, not the image) is standard in
  visual SLAM: ORB-SLAM3 undistorts keypoints via `Frame::UndistortKeyPoints`
  for matching/triangulation and does not remap the frame.
- Sparse point undistortion primitives: OpenCV `cv::undistortPoints` and Kornia
  `undistort_points` operate on a set of 2D points, iterating the inverse of the
  radial/tangential polynomial (e.g. num_iters=5) rather than rendering a map.
- Undistorting fiducial/marker corners for pose is standard: OpenCV ArUco pose
  estimation takes the camera matrix and undistorts marker corners; its docs and
  MATLAB `readArucoMarker` note that markers may fail to detect when the lens
  distorts their square shape, and recommend undistortion. AprilTag itself
  claims robustness to mild lens distortion.
- Wide-angle AprilTag corner correction, closest prior work: TartanCalib,
  "Iterative Wide-Angle Lens Calibration using Adaptive Subpixel Refinement of
  AprilTags" (2022). It explicitly targets extreme edge distortion with
  AprilTags and uses three mechanisms, one being reprojection of known target
  coordinates into the image frame via a camera model (not a homography) plus
  adaptive subpixel refinement - the direct analogue of our forward-distorted
  bit-grid sampling. Cite this.
- Distortion modeled at the point/pose level rather than image-space:
  "Camera Pose Estimation Using Implicit Distortion Models" (CVPR 2022),
  point-wise radial model with undistorted-keypoint P3P+RANSAC; and "Minimal
  Solvers for Single-View Lens-Distorted Camera Auto-Calibration" (WACV 2021),
  division-model / arc fits to distorted edges.

### A nuance that validates this design (cite)

The undistort-image vs undistort-point discussion (OpenCV; see the canonical
StackOverflow "undistort vs undistorPoints for feature matching") explains the
asymmetry: image undistortion uses inverse mapping (build the output grid, then
forward-distort into the source and interpolate), while point undistortion must
numerically invert the 4th/6th-degree polynomial and so carries higher error.
This supports correcting edge geometry in the corrected domain and
forward-distorting only the sample points, rather than trusting naive
point-undistortion of a few corners. It also grounds the "do not iterate on 4
corners" argument in prior literature.

### What is genuinely this work (claim this)

- Point-level correction applied to the fallback decode bit grid:
  forward-distorting bit sample positions so they follow the curved grid in
  the raw frame on a single-core embedded detector.
- Per-candidate-loop correction (undistort a candidate contour, then segment)
  remains an unimplemented extension for candidates the raw quad fitter misses.
- The "correct only what detection consumes" framing under a hard real-time
  budget, where full-frame correction (hardware GDC or software remap) is
  measured to exceed the frame budget on the Milk-V Duo S (SG2000).

Suggested paper framing, subject to real-scene validation: point LDC corrects
decoded corners and retries selected failed quad decodes without full-frame
remapping on a single-core embedded platform. Pose and recovery of missing raw
quads are outside the current implementation. Cite
TartanCalib, ORB-SLAM3, undistortPoints, and the inverse-mapping caveat.
Subsection "References":
- TartanCalib: "Iterative Wide-Angle Lens Calibration using Adaptive Subpixel
  Refinement of AprilTags" (2022). arXiv:2210.02511.
- ORB-SLAM3: UZ-SLAMLab/ORB_SLAM3 `Frame::UndistortKeyPoints`.
- OpenCV `cv::undistortPoints`, Kornia `kornia.geometry.calibration.undistort_points`.
- "Camera Pose Estimation Using Implicit Distortion Models" (CVPR 2022).
- "Minimal Solvers for Single-View Lens-Distorted Camera Auto-Calibration" (WACV 2021).
- Olson et al., "AprilTag: A Robust and Flexible Visual Fiducial System" (2011); Wang & Olson (2016).
