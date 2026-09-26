# Point-Level Lens Distortion Correction for AprilTag on Wide-FOV (Duo-S)

Status: design note. Complements `docs/handover.md` and the `hardware-ldc` work.
Scope: an AprilTag ROI->decode->pose pipeline on the SG2000 (Milk-V Duo S), where
full-frame lens-distortion correction (hardware GDC or software remap) is measured
to be too slow for real-time.

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
- Hardware GDC uses a single-ratio (k-only) radial model. The handover already
  flags that this cannot represent the lens's higher-order (k2/k3) terms, so for
  a genuine wide-FOV lens the HW path is both slow AND the wrong model.

## Key insight: correct points, not regions

Lens distortion affects three separate stages of the detector, with different
requirements:

1. Quad detectability (can a candidate tag be fitted at all).
2. Bit-grid sampling (the tag's bit cells project to a curved grid in the image).
3. Corner/pose accuracy (corner positions).

Only stage (1), if it fails, forces image-space correction (a corrected ROI).
Stages (2) and (3) are exactly solvable by cheap point-level correction in the
corrected (pinhole) domain. So we do not render any corrected region for the
common path.

Note on the "whole ROI first?" trap: iterating on the 4 detected *corner points*
does NOT converge to the remap->fit->corners answer, because those 4 points are a
lossy summary that already discarded the curvature that causes the sagitta bias.
The correction must be applied to the *edge geometry* (contour/edge sample
points) BEFORE the line fit. Then the fitted lines are true straight lines and the
corners equal what remap->fit would give, up to shared residuals (calibration
model residual, threshold/contour noise, pixel quantization).

## Pipeline

Overall flow (no full-frame correction):

```
raw frame
  -> threshold
  -> connected-component / candidate loops   [topological; no undistortion needed]
  -> per candidate loop:
        sample along the loop (arc-length; skip corner margins)
        undistort samples with D^-1            [full distortion coefficients]
        fit straight edges (least squares / robust) in corrected domain
        intersect adjacent edges -> refined corners
  -> corrected-domain homography H'  (refined corners -> tag bit grid)
  -> for each bit center b: sample raw image at D(H'(b))     [forward-distort]
  -> decode ID
  -> refinement / solvePnP  (pose from refined corners)
```

### Decode path
- Use the raw-frame corners only to LOCALISE the bit grid. Bit-cell tolerance is
  large, and the quad-fit sagitta bias is small relative to a readable tag's cell
  size (and largely common-mode across the four corners, so it mostly cancels in
  the grid).
- The wide-FOV decoding fix is to build the bit-grid homography in the corrected
  domain (from refined corners) and forward-distort each bit-center with D.
  Because the bit cells form a curved grid in the raw image, forward-distortion
  makes the samples follow that curve — the same effect an ROI undistortion would
  give, but with O(number of bits) point lookups, no region, no rectangle.

### Pose path
- Once a tag is decoded, the refined corners (from corrected edge fits) go to
  solvePnP. They are geometrically accurate (equal to remap->fit->corners within
  the shared residual floor), so pose is correct for edge tags under wide-FOV
  distortion.

### When a corrected ROI is unavoidable
- Only if candidate generation / quad fitting outright fails in the far periphery
  (no usable convex candidate). Then fall back to a corrected-domain ROI remap
  sized from the undistorted corners + source margin, invoked as a rescue rather
  than the default.

## Sampling along the loop (computation reduction)

Instead of undistorting every contour pixel of a candidate loop, sample K points
per edge (approximately arc-length spaced) and undistort only those.

Why it is safe: the physical edge is straight; undistortion maps it to a straight
line regardless of apparent curvature. So dense sampling is not needed to capture
curvature — only to average out noise. K points per edge is sufficient.

Error / balance:
- Variance (noise) of the line fit falls like ~1/K. Systematic bias (model
  residual, placement) is NOT reduced by K.
- Choose K just large enough that line-fit variance falls below the calibration
  model's own residual (the true accuracy floor on wide FOV). Beyond that, more
  samples buy nothing. Suggested K ~ 8-16 per edge; ~4-6 acceptable for clean
  edges.
- Placement matters more than count:
  * sample actual contour pixels at even arc-length intervals (on the border), and
  * exclude a margin near each corner (skip ~15-20% of each edge at both ends) so
    no sample sits in the bend region.
- Use least-squares with light outlier rejection (or RANSAC) so a stray sample
  cannot drag the line.
- Anchor the samples with a cheap coarse quad fit on the raw loop first so edge /
  corner regions are known.

Cost: ~K*4 ~ 30-60 D^-1 calls per tag (each a few multiply/adds) — effectively
free on one core vs. correcting the whole loop (hundreds of contour points) or any
region remap.

## Validation / next steps

1. Implement: threshold -> loops -> sample -> undistort samples -> robust edge fit
   -> refined corners -> corrected-domain homography -> forward-distort bit
   sampling -> decode -> solvePnP.
2. Sweep K (e.g. 3, 6, 9, 12, 16) and corner-margin fraction; measure corner
   residual vs. the full-range... reference (dense correction and/or full ROI
   remap) on a checkerboard / synthetic grid.
3. Compare estimated corners to the remap->fit->corners reference to confirm the
   point-level result matches within the shared residual floor.
4. Measure per-tag added CPU time and confirm the decode/pose accuracy holds at
   the frame edge with the wide-FOV lens.

## Open questions

- Whether candidate loops in far periphery need the corrected-ROI rescue; quantify
  where quad fit starts failing vs distortion.
- Whether forward-distorted bit sampling is as robust as an ROI remap for decode
  under strong radial terms; validate on the wide-FOV lens.
- Positioning: this is a "correct only what detection consumes" design that
  preserves frame rate and is exact at the sampling level for wide FOV.

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

- Point-level correction applied to the DECODE bit-grid: forward-distorting the
  bit-sample grid under distortion so the samples follow the curved grid in the
  raw frame - in a real-time, single-core, NPU-constrained embedded detector.
- Per-candidate-loop correction (undistort a candidate contour, then segment)
  as a way to avoid pre-identifying the tag's edges under distortion.
- The "correct only what detection consumes" framing under a hard real-time
  budget, where full-frame correction (hardware GDC or software remap) is
  measured to exceed the frame budget on the Milk-V Duo S (SG2000).

Suggested paper framing: "on a single-core embedded platform, full-frame
lens-distortion correction (HW GDC or SW remap) exceeds the real-time budget; we
achieve distortion-robust detection and pose by correcting only the decoded
corners and bit samples in the corrected domain, matching region-level accuracy
at negligible cost." Lead with the constraint and decode-level bit sampling; cite
TartanCalib, ORB-SLAM3, undistortPoints, and the inverse-mapping caveat.
Subsection "References":
- TartanCalib: "Iterative Wide-Angle Lens Calibration using Adaptive Subpixel
  Refinement of AprilTags" (2022). arXiv:2210.02511.
- ORB-SLAM3: UZ-SLAMLab/ORB_SLAM3 `Frame::UndistortKeyPoints`.
- OpenCV `cv::undistortPoints`, Kornia `kornia.geometry.calibration.undistort_points`.
- "Camera Pose Estimation Using Implicit Distortion Models" (CVPR 2022).
- "Minimal Solvers for Single-View Lens-Distorted Camera Auto-Calibration" (WACV 2021).
- Olson et al., "AprilTag: A Robust and Flexible Visual Fiducial System" (2011); Wang & Olson (2016).
