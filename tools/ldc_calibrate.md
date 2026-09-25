# Duo S / SG2000 lens calibration

`ldc_calibrate.py` takes checkerboard views from images, videos, or a live
OpenCV source and fits the scalar radial model exposed by Sophgo's VPSS LDC
API. It writes the full OpenCV calibration and a best-fit `ratio`, `center_x`,
and `center_y` for the AprilTag live detector. The SG2000 applies the mesh in
the VPSS/DWA path; it does not add a CPU pass per frame.

Use a compatible Python OpenCV and NumPy pair, then capture at least 12 useful
views of a flat checkerboard. On the Ubuntu environment used for this repo,
the system OpenCV 4.11 works with the distro NumPy 1.21.5. If a user-installed
NumPy 2.x shadows it, prefix the command with `PYTHONNOUSERSITE=1` as below.
The `--pattern` value
is the number of inner corner intersections, not the number of squares. For
example, `--pattern 10 7` means 10 intersections across and 7 down; the
physical board therefore has 11 by 8 squares. Move the board
around the entire image, tilt it in both directions, and include near and far
views. Frames are resized to the live detector's 1280×720 VPSS output size
before calibration. This mirrors the pipeline, which scales the complete
1280×800 sensor frame to 1280×720; it does not discard the top 80 lines. Keep
the lens focus and camera mode the same as the eventual detector setup.

Images (directory, quoted glob, or repeated `--source`):

```sh
PYTHONNOUSERSITE=1 python3 tools/ldc_calibrate.py \
  --source 'calibration/*.jpg' --pattern 9 6 --square-size 25 \
  --output ldc-calibration.json
```

Recorded video, sampling every 30th frame:

```sh
PYTHONNOUSERSITE=1 python3 tools/ldc_calibrate.py \
  --source calibration.mp4 --stride 30 --pattern 9 6 \
  --output ldc-calibration.json
```

To review the estimated correction without an OpenCV GUI/HighGUI build, ask
the script to write corrected calibration views and input-versus-corrected
comparisons. It only saves views where the checkerboard was detected:

```sh
PYTHONNOUSERSITE=1 python3 tools/ldc_calibrate.py \
  --source calibration.mp4 --stride 30 --pattern 10 7 \
  --output ldc-calibration.json --corrected-dir ldc-review
```

Open `view-*-compare.jpg` images to compare the original on the left with the
OpenCV-undistorted result on the right; `view-*-corrected.png` contains only
the corrected image. The corrected images use OpenCV's full calibrated lens
model as a visual reference, not the SG2000's single-ratio VPSS LDC hardware
mapping, so they are not pixel-identical predictions of the board output.
The fit error in the JSON remains the measure of how closely that simpler
hardware mapping approximates the calibrated lens.

For live calibration, start a detector preview on the Duo S and point the
calibration host at that RTSP stream. The board must have its normal detector
process running because VI/VPSS is owned by that process:

```sh
# On the Duo S
/app/tinytag_detect/run_live.sh --rtsp-luma

# On the host connected to USB Ethernet
PYTHONNOUSERSITE=1 python3 tools/ldc_calibrate.py \
  --source rtsp://192.168.42.1:554/h264 --show \
  --pattern 9 6 --output ldc-calibration.json
```

Pass the generated JSON file to either live detector. The file bundles the
four tunable LDC values (radial ratio, X/Y optical-center offset, and retained
view ratio) with the calibration resolution:

```sh
# Copy the host-generated JSON onto the board first.
scp ldc-calibration.json root@192.168.42.1:/root/
/app/tinytag_detect/run_live.sh --ldc-calibration /root/ldc-calibration.json
```

The detector scales optical-center offsets from the calibration resolution to
its VPSS output and rejects a mismatched aspect ratio. The detector's VPSS
output is corrected before the TPU and AprilTag decoder;
the neural-model VPSS channel receives the same correction when direct model
input is enabled. Correction is disabled by default. A calibration ratio of
zero means no useful radial correction was measured.

This is a best-fit conversion: Sophgo exposes one radial ratio and a center,
while OpenCV may estimate multiple radial and tangential coefficients. Review
`radial_fit_rms_px` in the JSON; a large value means the lens cannot be closely
represented by this hardware parameterization. The tool reports the full
OpenCV matrix and coefficients too, but those are not consumed by the VPSS LDC
API. Check the corrected view with straight lines near the image edges before
relying on AprilTag corner geometry.
