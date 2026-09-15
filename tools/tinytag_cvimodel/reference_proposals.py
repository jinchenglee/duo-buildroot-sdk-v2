#!/usr/bin/env python3
"""FP32 reference implementation of the application's proposal decode.

Mirrors apps/tinytag_detect/tinytag_det.cc (pre_process + decode_proposals) in
numpy against the ONNX model, so the C++ port can be checked against something
other than itself. The self-test compares raw output tensors; this compares the
decoded proposals, which is where an indexing or scaling mistake would hide.
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--onnx", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--thres", type=float, default=0.35)
    parser.add_argument("--max", dest="max_proposals", type=int, default=8)
    parser.add_argument("--expand", type=float, default=1.5)
    parser.add_argument("--iou", type=float, default=0.5)
    return parser.parse_args()


def iou(a, b):
    ax0, ay0, aw, ah = a
    bx0, by0, bw, bh = b
    ix0, iy0 = max(ax0, bx0), max(ay0, by0)
    ix1, iy1 = min(ax0 + aw, bx0 + bw), min(ay0 + ah, by0 + bh)
    inter = max(0.0, ix1 - ix0) * max(0.0, iy1 - iy0)
    union = aw * ah + bw * bh - inter
    return inter / union if union > 0 else 0.0


def main() -> int:
    args = parse_args()
    gray = cv2.imread(str(args.image), cv2.IMREAD_GRAYSCALE)
    if gray is None:
        raise RuntimeError(f"cannot decode {args.image}")
    height, width = gray.shape[:2]

    band_w, band_h = min(width, CROP_W), min(height, CROP_H)
    crop_x, crop_y = 0, height - band_h
    band = gray[crop_y:crop_y + band_h, crop_x:crop_x + band_w]
    if band.shape[:2] != (NET_H, NET_W):
        interp = cv2.INTER_AREA if band_w >= NET_W else cv2.INTER_LINEAR
        net_in = cv2.resize(band, (NET_W, NET_H), interpolation=interp)
    else:
        net_in = band

    session = onnxruntime.InferenceSession(str(args.onnx), providers=["CPUExecutionProvider"])
    out = session.run(None, {session.get_inputs()[0].name:
                             (net_in.astype(np.float32) / 255.0)[None, None]})[0][0]

    heatmap, off_x, off_y, sc_w, sc_h = out[0], out[1], out[2], out[3], out[4]
    score = 1.0 / (1.0 + np.exp(-heatmap))
    H, W = score.shape

    # 3x3 max-pool local-maxima NMS, border-clamped
    padded = np.pad(score, 1, mode="constant", constant_values=-np.inf)
    pooled = np.max(np.stack([padded[dy:dy + H, dx:dx + W]
                              for dy in range(3) for dx in range(3)]), axis=0)
    peaks = [(float(score[y, x]), y, x)
             for y in range(H) for x in range(W)
             if score[y, x] >= pooled[y, x] and score[y, x] >= args.thres]
    peaks.sort(key=lambda t: -t[0])
    peaks = peaks[:args.max_proposals]

    x_factor = band_w / NET_W
    y_factor = band_h / NET_H

    proposals = []
    for conf, gy, gx in peaks:
        cx = (gx + off_x[gy, gx]) * STRIDE
        cy = (gy + off_y[gy, gx]) * STRIDE
        w = np.exp(np.clip(sc_w[gy, gx], *SCALE_CLAMP)) * STRIDE * args.expand
        h = np.exp(np.clip(sc_h[gy, gx], *SCALE_CLAMP)) * STRIDE * args.expand
        cx, cy = cx * x_factor + crop_x, cy * y_factor + crop_y
        w, h = w * x_factor, h * y_factor
        x0 = np.clip(cx - w / 2, crop_x, crop_x + band_w)
        y0 = np.clip(cy - h / 2, crop_y, crop_y + band_h)
        x1 = np.clip(cx + w / 2, crop_x, crop_x + band_w)
        y1 = np.clip(cy + h / 2, crop_y, crop_y + band_h)
        if x1 <= x0 or y1 <= y0:
            continue
        proposals.append((conf, (float(x0), float(y0), float(x1 - x0), float(y1 - y0))))

    if args.iou > 0:
        kept = []
        for conf, roi in proposals:
            if not any(iou(roi, k[1]) > args.iou for k in kept):
                kept.append((conf, roi))
        proposals = kept

    print(f"image  : {args.image.name} ({width}x{height})")
    print(f"\n{len(proposals)} proposal(s) at threshold {args.thres:.2f}  [FP32 ONNX reference]")
    for i, (conf, (x, y, w, h)) in enumerate(proposals):
        print(f"  [{i}] conf {conf:.3f}  roi x={x:.1f} y={y:.1f} w={w:.1f} h={h:.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
