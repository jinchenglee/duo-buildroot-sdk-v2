#!/usr/bin/env python3
"""Turn raw frames and video into TinyTag calibration / validation sets.

Every frame is put through *exactly* the preprocessing the deployed application
performs (see tinytag_det.cc::pre_process): crop the bottom-left 1280x720 band
out of anything larger, then plain-resize to the 640x360 network input. INT8
calibration measures activation statistics, so feeding the compiler differently
shaped or differently cropped images than the board will actually see produces
thresholds that are wrong in a way nothing downstream can recover from.

Sources may be image files, video files, or directories of either.
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

# Network input, and the 16:9 band the application crops before resizing into
# it. Keep in sync with tinytag_det.h (kNetW/kNetH/kCropW/kCropH).
NET_W, NET_H = 640, 360
CROP_W, CROP_H = 1280, 720

IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png", ".pgm", ".tif", ".tiff", ".webp"}
VIDEO_SUFFIXES = {".avi", ".mkv", ".mov", ".mp4", ".webm"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, action="append", type=Path,
                        help="raw image, video, or directory; repeat as needed")
    parser.add_argument("--output", required=True, type=Path,
                        help="output directory (calibration/ and validation/ are created inside)")
    parser.add_argument("--video-stride", type=int, default=1,
                        help="keep every Nth video frame (default: 1, keep all)")
    parser.add_argument("--validation-fraction", type=float, default=0.25,
                        help="fraction of frames held out for validation (default: 0.25)")
    parser.add_argument("--seed", type=int, default=0,
                        help="shuffle seed for the calibration/validation split")
    return parser.parse_args()


def deployment_preprocess(frame: np.ndarray) -> np.ndarray:
    """Crop the bottom-left 16:9 band, then resize to the network input.

    Mirrors tinytag_det.cc::pre_process exactly. Sources at or below the band
    size are used as-is (crop-only by design: no scale-up, no letterbox pad).
    """
    if frame.ndim == 3:
        frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

    height, width = frame.shape[:2]
    band_w = min(width, CROP_W)
    band_h = min(height, CROP_H)
    band = frame[height - band_h:height, 0:band_w]

    if band.shape[:2] == (NET_H, NET_W):
        return band
    # INTER_AREA when shrinking matches what the board's resize converges to
    # closely enough for calibration statistics, and avoids aliasing.
    interp = cv2.INTER_AREA if band_w >= NET_W else cv2.INTER_LINEAR
    return cv2.resize(band, (NET_W, NET_H), interpolation=interp)


def expand_sources(sources: list[Path]) -> tuple[list[Path], list[Path]]:
    images: list[Path] = []
    videos: list[Path] = []
    for source in sources:
        if not source.exists():
            raise RuntimeError(f"source not found: {source}")
        candidates = sorted(p for p in source.rglob("*") if p.is_file()) if source.is_dir() else [source]
        for path in candidates:
            suffix = path.suffix.lower()
            if suffix in IMAGE_SUFFIXES:
                images.append(path)
            elif suffix in VIDEO_SUFFIXES:
                videos.append(path)
    if not images and not videos:
        raise RuntimeError("no image or video files found in the given sources")
    return images, videos


def collect_frames(images: list[Path], videos: list[Path],
                   video_stride: int) -> list[tuple[str, np.ndarray]]:
    frames: list[tuple[str, np.ndarray]] = []

    for path in images:
        raw = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if raw is None:
            raise RuntimeError(f"cannot decode image: {path}")
        frames.append((f"img_{path.stem}", deployment_preprocess(raw)))

    for path in videos:
        capture = cv2.VideoCapture(str(path))
        if not capture.isOpened():
            raise RuntimeError(f"cannot open video: {path}")
        index = 0
        kept = 0
        while True:
            ok, raw = capture.read()
            if not ok:
                break
            if index % video_stride == 0:
                frames.append((f"vid_{path.stem}_{index:05d}", deployment_preprocess(raw)))
                kept += 1
            index += 1
        capture.release()
        print(f"{path.name}: {kept} of {index} frames kept (stride {video_stride})")

    return frames


def main() -> int:
    args = parse_args()
    if not 0.0 <= args.validation_fraction < 1.0:
        raise RuntimeError("--validation-fraction must be in [0, 1)")
    if args.video_stride < 1:
        raise RuntimeError("--video-stride must be >= 1")

    images, videos = expand_sources(args.source)
    frames = collect_frames(images, videos, args.video_stride)

    order = np.random.default_rng(args.seed).permutation(len(frames))
    split = int(round(len(frames) * (1.0 - args.validation_fraction)))
    split = max(1, min(split, len(frames)))

    calibration_dir = args.output / "calibration"
    validation_dir = args.output / "validation"
    for directory in (calibration_dir, validation_dir):
        directory.mkdir(parents=True, exist_ok=True)

    for rank, index in enumerate(order):
        name, frame = frames[int(index)]
        target = calibration_dir if rank < split else validation_dir
        cv2.imwrite(str(target / f"{name}.png"), frame)

    calibration_count = split
    validation_count = len(frames) - split
    print(f"\ncalibration: {calibration_count} frames -> {calibration_dir}")
    print(f"validation:  {validation_count} frames -> {validation_dir}")

    if calibration_count < 100:
        print(
            f"\nWARNING: only {calibration_count} calibration frames. The K230 flow's rule of "
            "thumb is >=100 covering normal, hard, and negative scenes. Fewer frames -- "
            "especially frames drawn from a single clip, which are highly correlated -- give "
            "activation thresholds that may not generalize. Treat the resulting cvimodel as "
            "experimental and re-run this with more raw footage before relying on it.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
