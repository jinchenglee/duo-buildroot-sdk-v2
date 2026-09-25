#!/usr/bin/env python3
"""Calibrate a checkerboard camera and fit Sophgo's scalar VPSS LDC model.

Sources may be image paths/directories/globs, video files, or a live OpenCV
source such as rtsp://192.168.42.1:554/h264 or cam:0. Requires Python OpenCV.
"""

import argparse
import glob
import json
import math
from pathlib import Path
import sys
import tempfile

import cv2
import numpy as np


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff"}


def expand_sources(sources):
    expanded = []
    for source in sources:
        matches = sorted(glob.glob(source))
        candidates = matches or [source]
        for candidate in candidates:
            path = Path(candidate)
            if path.is_dir():
                expanded.extend(str(p) for p in sorted(path.iterdir())
                                if p.suffix.lower() in IMAGE_SUFFIXES)
            else:
                expanded.append(candidate)
    return expanded


def open_source(source):
    if source.startswith("cam:"):
        source = int(source[4:])
    cap = cv2.VideoCapture(source)
    if not cap.isOpened():
        raise RuntimeError(f"cannot open source: {source}")
    return cap


def frames_from(source, stride, live, show):
    path = Path(source)
    if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES:
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if image is not None:
            yield image, str(path)
        return

    cap = open_source(source)
    index = 0
    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            if index % stride == 0:
                yield frame, f"{source} frame {index}"
            index += 1
            if live and show:
                cv2.imshow("LDC checkerboard capture (q to stop)", frame)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
    finally:
        cap.release()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", action="append", required=True,
                        help="image, image directory/glob, video, RTSP URL, or cam:N; repeatable")
    parser.add_argument("--pattern", nargs=2, type=int, metavar=("COLS", "ROWS"),
                        required=True, help="checkerboard inner-corner count")
    parser.add_argument("--square-size", type=float, default=1.0,
                        help="checkerboard square size in any consistent unit (default 1)")
    parser.add_argument("--target-size", nargs=2, type=int, metavar=("WIDTH", "HEIGHT"),
                        default=(1280, 720),
                        help="detector VPSS output size; inputs are resized to this, default 1280 720")
    parser.add_argument("--stride", type=int, default=15,
                        help="read every Nth video/live frame (default 15)")
    parser.add_argument("--min-views", type=int, default=12,
                        help="minimum distinct checkerboard views (default 12)")
    parser.add_argument("--max-views", type=int, default=100,
                        help="maximum accepted views (default 100)")
    parser.add_argument("--show", action="store_true", help="show live/video frames; press q to stop")
    parser.add_argument("--corrected-dir", type=Path,
                        help="save headless OpenCV-undistorted calibration views and side-by-side comparisons")
    parser.add_argument("--output", type=Path, default=Path("ldc-calibration.json"))
    args = parser.parse_args()

    cols, rows = args.pattern
    target_width, target_height = args.target_size
    if (cols < 3 or rows < 3 or args.square_size <= 0 or args.stride < 1 or
            target_width < 1 or target_height < 1):
        parser.error("pattern dimensions must be >=3, square size positive, stride >=1, and target dimensions positive")

    pattern = (cols, rows)
    obj = np.zeros((rows * cols, 3), np.float32)
    obj[:, :2] = np.mgrid[0:cols, 0:rows].T.reshape(-1, 2) * args.square_size
    object_points, image_points = [], []
    staged_views = []
    # Calibration parameters are only known after all views have been collected.
    # Stage accepted frames on disk (not RAM) so live and long video sources can
    # still produce corrected review images without a GUI/highgui backend.
    review_staging = (tempfile.TemporaryDirectory(prefix="ldc-calibrate-review-")
                      if args.corrected_dir else None)
    image_size = None
    found_sources = expand_sources(args.source)
    live_sources = {s for s in found_sources if s.startswith(("rtsp://", "http://", "cam:"))}

    for source in found_sources:
        try:
            for frame, label in frames_from(source, args.stride,
                                             source in live_sources, args.show):
                if (frame.shape[1], frame.shape[0]) != (target_width, target_height):
                    # The live detector VPSS channel stretches the complete
                    # sensor frame to 1280x720; it does not crop a 16:10 input.
                    frame = cv2.resize(frame, (target_width, target_height),
                                       interpolation=cv2.INTER_LINEAR)
                gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                size = (gray.shape[1], gray.shape[0])
                if image_size is not None and size != image_size:
                    raise RuntimeError(f"mixed frame sizes: {image_size} and {size} ({label})")
                image_size = size
                flags = cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE
                ok, corners = cv2.findChessboardCorners(gray, pattern, flags)
                if ok:
                    corners = cv2.cornerSubPix(
                        gray, corners, (11, 11), (-1, -1),
                        (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001))
                    object_points.append(obj.copy())
                    image_points.append(corners)
                    if review_staging:
                        staged_path = Path(review_staging.name) / f"view-{len(object_points):03d}.png"
                        if not cv2.imwrite(str(staged_path), frame):
                            raise RuntimeError(f"could not stage calibration view: {staged_path}")
                        staged_views.append((staged_path, label))
                    print(f"found view {len(object_points):3d}: {label}")
                    if len(object_points) >= args.max_views:
                        break
                elif args.show:
                    cv2.drawChessboardCorners(frame, pattern, corners, ok)
                if len(object_points) >= args.max_views:
                    break
        except (cv2.error, RuntimeError) as exc:
            print(f"warning: {exc}", file=sys.stderr)
        if len(object_points) >= args.max_views:
            break

    if args.show:
        cv2.destroyAllWindows()
    if len(object_points) < args.min_views:
        raise SystemExit(f"need at least {args.min_views} checkerboard views; found {len(object_points)}")

    rms, camera, distortion, rvecs, tvecs = cv2.calibrateCamera(
        object_points, image_points, image_size, None, None)
    width, height = image_size
    center_x = int(round(float(camera[0, 2]) - width / 2.0))
    center_y = int(round(float(camera[1, 2]) - height / 2.0))
    center_x = max(-511, min(511, center_x))
    center_y = max(-511, min(511, center_y))
    cx, cy = width / 2.0 + center_x, height / 2.0 + center_y
    norm = math.hypot(width / 2.0, height / 2.0)

    # Fit the SDK's documented mapping q=p*(1 + ratio/1000 * r^2),
    # where r is normalized by the image half-diagonal. This gives the best
    # least-squares scalar radial fit to OpenCV's calibrated reprojections.
    zero_dist = np.zeros_like(distortion)
    numerator = denominator = 0.0
    for op, measured, rv, tv in zip(object_points, image_points, rvecs, tvecs):
        ideal, _ = cv2.projectPoints(op, rv, tv, camera, zero_dist)
        ideal = ideal.reshape(-1, 2)
        measured = measured.reshape(-1, 2)
        delta = measured - ideal
        rel = ideal - np.array([cx, cy], dtype=np.float64)
        radius2 = np.minimum(1.0, np.sum(rel * rel, axis=1) / (norm * norm))
        basis = rel * radius2[:, None]
        numerator += float(np.sum(delta * basis))
        denominator += float(np.sum(basis * basis))
    ratio_float = 1000.0 * numerator / denominator if denominator else 0.0
    ratio = int(round(ratio_float))
    ratio_limited = max(-300, min(500, ratio))
    predicted = []
    observed = []
    for op, measured, rv, tv in zip(object_points, image_points, rvecs, tvecs):
        ideal, _ = cv2.projectPoints(op, rv, tv, camera, zero_dist)
        ideal = ideal.reshape(-1, 2)
        measured = measured.reshape(-1, 2)
        rel = ideal - np.array([cx, cy], dtype=np.float64)
        r2 = np.minimum(1.0, np.sum(rel * rel, axis=1) / (norm * norm))
        predicted.append(ideal + rel * (ratio_limited / 1000.0 * r2[:, None]))
        observed.append(measured)
    residual = np.concatenate(predicted) - np.concatenate(observed)
    radial_rms = float(np.sqrt(np.mean(np.sum(residual * residual, axis=1))))

    result = {
        "image_size": [width, height],
        "checkerboard_inner_corners": [cols, rows],
        "views": len(object_points),
        "opencv_rms_px": float(rms),
        "camera_matrix": camera.tolist(),
        "distortion_coefficients": distortion.reshape(-1).tolist(),
        "sophgo_vpss_ldc": {
            "ratio": ratio_limited,
            "center_x": center_x,
            "center_y": center_y,
            "view_ratio": 100,
            "fitted_ratio_unclamped": ratio_float,
            "radial_fit_rms_px": radial_rms,
        },
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if review_staging:
        args.corrected_dir.mkdir(parents=True, exist_ok=True)
        # alpha=1 retains the full field of view; black corners are expected
        # where the undistortion remap has no source pixels.
        review_camera, _ = cv2.getOptimalNewCameraMatrix(
            camera, distortion, image_size, 1.0, image_size)
        for index, (staged_path, label) in enumerate(staged_views, 1):
            frame = cv2.imread(str(staged_path), cv2.IMREAD_COLOR)
            if frame is None:
                print(f"warning: could not read staged view {staged_path}", file=sys.stderr)
                continue
            corrected = cv2.undistort(frame, camera, distortion, None, review_camera)
            corrected_path = args.corrected_dir / f"view-{index:03d}-corrected.png"
            compare_path = args.corrected_dir / f"view-{index:03d}-compare.jpg"
            if not cv2.imwrite(str(corrected_path), corrected):
                raise RuntimeError(f"could not write corrected review image: {corrected_path}")
            comparison = np.hstack((frame, corrected))
            cv2.putText(comparison, "input", (16, 32), cv2.FONT_HERSHEY_SIMPLEX,
                        0.8, (0, 255, 0), 2, cv2.LINE_AA)
            cv2.putText(comparison, "OpenCV undistorted", (width + 16, 32),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2, cv2.LINE_AA)
            if not cv2.imwrite(str(compare_path), comparison,
                               [cv2.IMWRITE_JPEG_QUALITY, 95]):
                raise RuntimeError(f"could not write comparison image: {compare_path}")
            print(f"review view {index:03d}: {label} -> {corrected_path}, {compare_path}")
        review_staging.cleanup()
    ldc = result["sophgo_vpss_ldc"]
    print(f"\nOpenCV calibration RMS: {rms:.3f} px; accepted views: {len(object_points)}")
    print(f"LDC radial-fit RMS: {radial_rms:.3f} px")
    print(f"Saved: {args.output}")
    if args.corrected_dir:
        print(f"Saved {len(staged_views)} corrected and side-by-side review views in: {args.corrected_dir}")
    print(f"Apply with: --ldc-calibration {args.output}")
    if ratio != ratio_limited:
        print(f"WARNING: fitted ratio {ratio} exceeds hardware limits; clamped to {ratio_limited}")
    if radial_rms > 2.0:
        print("WARNING: the lens distortion is not well represented by Sophgo's single-ratio model.",
              "Use the reported RMS to judge the fit; this is not a full OpenCV undistortion mesh.")


if __name__ == "__main__":
    main()
