#!/usr/bin/env python3
"""Work out why ArUco Nano decodes nothing on a captured live frame.

Takes a frame saved by `tinytag_detect_live --save-frame` -- which is the exact
1280x720 luma plane the on-board decoder reads -- and walks the same two stages
the board does, reporting at each point the things that stop a tag decoding:
size in pixels, sharpness, and contrast.

Stage 1 reproduces the network's proposals (FP32 ONNX, so it is independent of
the cvimodel). Stage 2 runs OpenCV's own ArUco detector over each proposal crop.
OpenCV's detector is not the same code as ArUco Nano, but it reads the same
AprilTag 36h11 dictionary, so if it also finds nothing the problem is the image
rather than the decoder.

The upscale test is the key diagnostic: if a crop decodes at 2x or 4x but not at
1x, the tag is resolution-limited, and no decoder tuning will fix it -- the
camera has to see the tag bigger.
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np
import onnxruntime

NET_W, NET_H = 640, 360
CROP_W, CROP_H = 1280, 720
STRIDE = 8
SCALE_CLAMP = (-4.0, 6.0)
# AprilTag 36h11 is 6x6 data bits plus a 1-module border on each side.
TAG_MODULES = 8


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--onnx", required=True, type=Path)
    parser.add_argument("--frames", required=True, nargs="+", type=Path)
    parser.add_argument("--thres", type=float, default=0.20)
    parser.add_argument("--max", dest="max_proposals", type=int, default=8)
    parser.add_argument("--expand", type=float, default=1.5)
    parser.add_argument("--dump-crops", type=Path,
                        help="write each proposal crop here for eyeballing")
    return parser.parse_args()


def sharpness(image: np.ndarray) -> float:
    """Variance of the Laplacian -- the standard focus measure. Blurry crops
    score low; anything under ~50 on an 8-bit image is visibly soft."""
    return float(cv2.Laplacian(image, cv2.CV_64F).var())


def proposals_from_onnx(session, gray, thres, max_proposals, expand):
    band_w, band_h = min(gray.shape[1], CROP_W), min(gray.shape[0], CROP_H)
    crop_x, crop_y = 0, gray.shape[0] - band_h
    band = gray[crop_y:crop_y + band_h, crop_x:crop_x + band_w]
    net_in = band if band.shape[:2] == (NET_H, NET_W) else cv2.resize(
        band, (NET_W, NET_H), interpolation=cv2.INTER_AREA)

    out = session.run(None, {session.get_inputs()[0].name:
                             (net_in.astype(np.float32) / 255.0)[None, None]})[0][0]
    heat, ox, oy, sw, sh = out[0], out[1], out[2], out[3], out[4]
    score = 1.0 / (1.0 + np.exp(-heat))
    H, W = score.shape
    padded = np.pad(score, 1, constant_values=-np.inf)
    pooled = np.max(np.stack([padded[dy:dy + H, dx:dx + W]
                              for dy in range(3) for dx in range(3)]), axis=0)
    peaks = sorted(((float(score[y, x]), y, x) for y in range(H) for x in range(W)
                    if score[y, x] >= pooled[y, x] and score[y, x] >= thres),
                   key=lambda t: -t[0])[:max_proposals]

    xf, yf = band_w / NET_W, band_h / NET_H
    out_list = []
    for conf, gy, gx in peaks:
        cx = (gx + ox[gy, gx]) * STRIDE * xf + crop_x
        cy = (gy + oy[gy, gx]) * STRIDE * yf + crop_y
        w = np.exp(np.clip(sw[gy, gx], *SCALE_CLAMP)) * STRIDE * expand * xf
        h = np.exp(np.clip(sh[gy, gx], *SCALE_CLAMP)) * STRIDE * expand * yf
        x0 = int(max(crop_x, cx - w / 2)); y0 = int(max(crop_y, cy - h / 2))
        x1 = int(min(crop_x + band_w, cx + w / 2)); y1 = int(min(crop_y + band_h, cy + h / 2))
        if x1 - x0 >= 8 and y1 - y0 >= 8:
            out_list.append((conf, x0, y0, x1, y1))
    return out_list


def make_detector():
    """OpenCV 4.7+ moved the ArUco API; support both spellings."""
    d = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_APRILTAG_36h11)
    if hasattr(cv2.aruco, "ArucoDetector"):
        params = cv2.aruco.DetectorParameters()
        return lambda img: cv2.aruco.ArucoDetector(d, params).detectMarkers(img)
    params = cv2.aruco.DetectorParameters_create()
    return lambda img: cv2.aruco.detectMarkers(img, d, parameters=params)


def main() -> int:
    args = parse_args()
    session = onnxruntime.InferenceSession(str(args.onnx), providers=["CPUExecutionProvider"])
    detect = make_detector()
    if args.dump_crops:
        args.dump_crops.mkdir(parents=True, exist_ok=True)

    for path in args.frames:
        gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            print(f"!! cannot read {path}")
            continue

        print(f"\n{'=' * 72}\n{path.name}  {gray.shape[1]}x{gray.shape[0]}")
        print(f"  luma range {gray.min()}-{gray.max()}  mean {gray.mean():.1f}  "
              f"std {gray.std():.1f}  sharpness {sharpness(gray):.0f}")
        if gray.max() - gray.min() < 200:
            print("  NOTE: luma does not span 0-255. If this looks like 16-235 the ISP is")
            print("        emitting limited-range Y, which flattens contrast for thresholding.")

        # Reference: can any detector find a tag in the whole frame?
        corners, ids, _ = detect(gray)
        n = 0 if ids is None else len(ids)
        print(f"  full-frame OpenCV ArUco: {n} tag(s)" + (f" -> ids {sorted(int(i) for i in ids.flatten())}" if n else ""))
        if n:
            for c in corners:
                p = c.reshape(4, 2)
                side = np.mean([np.linalg.norm(p[i] - p[(i + 1) % 4]) for i in range(4)])
                print(f"      tag side ~{side:.0f} px -> ~{side / TAG_MODULES:.1f} px per module")

        props = proposals_from_onnx(session, gray, args.thres, args.max_proposals, args.expand)
        print(f"  network proposals: {len(props)}")

        for i, (conf, x0, y0, x1, y1) in enumerate(props):
            crop = gray[y0:y1, x0:x1]
            line = (f"    [{i}] conf {conf:.3f}  {x1-x0:3d}x{y1-y0:3d}px  "
                    f"sharp {sharpness(crop):6.0f}  contrast {crop.max()-crop.min():3d}")
            hits = []
            for scale in (1, 2, 4):
                img = crop if scale == 1 else cv2.resize(
                    crop, None, fx=scale, fy=scale, interpolation=cv2.INTER_CUBIC)
                _, cids, _ = detect(img)
                if cids is not None and len(cids):
                    hits.append(f"{scale}x->{sorted(int(v) for v in cids.flatten())}")
            line += "  decode: " + (", ".join(hits) if hits else "none at 1x/2x/4x")
            print(line)
            if args.dump_crops:
                cv2.imwrite(str(args.dump_crops / f"{path.stem}_roi{i}.png"), crop)

    print("\nReading the results:")
    print("  decodes only at 2x/4x  -> resolution-limited; the tag must appear larger")
    print("  low sharpness          -> out of focus or motion blur")
    print("  low contrast           -> exposure, or limited-range Y from the ISP")
    print("  full frame finds tags but crops do not -> the ROIs are misplaced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
