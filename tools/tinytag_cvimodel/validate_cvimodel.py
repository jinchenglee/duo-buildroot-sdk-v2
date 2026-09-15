#!/usr/bin/env python3
"""Check a compiled TinyTag cvimodel against its FP32 ONNX reference.

Runs both models over held-out frames on the host -- the cvimodel through
tpu-mlir's bundled cv18xx simulator (pyruntime_cvi), the ONNX through ONNX
Runtime -- and gates on the same criteria the K230 flow used:

  - mean absolute output error <= --max-mae (default 0.05)
  - >= --min-peak-match of frames have their heatmap maximum within
    --peak-tolerance output cells of the FP32 maximum (default 90% within 2)

Passing the simulator is necessary but NOT sufficient. On K230 an earlier
TinyTag model passed simulation and still corrupted memory on real hardware, so
treat this as a gate that can only reject, never fully approve. Real-board
smoke testing is still required.
"""

import argparse
import importlib
import os
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np
import onnxruntime

IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png", ".pgm", ".webp"}
NET_W, NET_H = 640, 360


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--onnx", required=True, type=Path, help="FP32 reference ONNX model")
    parser.add_argument("--cvimodel", required=True, type=Path, help="compiled cvimodel")
    parser.add_argument("--images", required=True, type=Path,
                        help="directory of held-out 640x360 grayscale frames")
    parser.add_argument("--max-mae", type=float, default=0.05,
                        help="maximum tolerated mean absolute error (default: 0.05)")
    parser.add_argument("--peak-tolerance", type=int, default=2,
                        help="heatmap peak may move this many output cells (default: 2)")
    parser.add_argument("--min-peak-match", type=float, default=0.90,
                        help="fraction of frames whose peak must match (default: 0.90)")
    parser.add_argument("--limit", type=int, default=0,
                        help="only check the first N frames (0 = all)")
    return parser.parse_args()


def load_frames(directory: Path, limit: int) -> list[tuple[Path, np.ndarray]]:
    paths = sorted(p for p in directory.rglob("*")
                   if p.is_file() and p.suffix.lower() in IMAGE_SUFFIXES)
    if not paths:
        raise RuntimeError(f"no images found under {directory}")
    if limit:
        paths = paths[:limit]

    frames = []
    for path in paths:
        gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            raise RuntimeError(f"cannot decode {path}")
        if gray.shape != (NET_H, NET_W):
            raise RuntimeError(
                f"{path} is {gray.shape[1]}x{gray.shape[0]}, expected {NET_W}x{NET_H}. "
                "Run prepare_calibration.py so validation frames get the same "
                "crop-and-resize the board applies.")
        frames.append((path, gray))
    return frames


def peak_cell(heatmap: np.ndarray) -> tuple[int, int]:
    index = int(np.argmax(heatmap))
    return divmod(index, heatmap.shape[1])


def main() -> int:
    args = parse_args()
    frames = load_frames(args.images, args.limit)
    # The cv18xx simulator drops scratch files into the current directory; keep
    # them out of the repository. Paths are resolved first so the chdir is safe.
    args.onnx = args.onnx.resolve()
    args.cvimodel = args.cvimodel.resolve()
    args.images = args.images.resolve()
    os.chdir(tempfile.mkdtemp(prefix="tinytag-sim-"))

    print(f"validating over {len(frames)} held-out frames")

    session = onnxruntime.InferenceSession(str(args.onnx),
                                           providers=["CPUExecutionProvider"])
    onnx_input = session.get_inputs()[0].name

    pyruntime_cvi = importlib.import_module("pyruntime_cvi")
    # output_all_tensors defaults to True in this binding, which exposes every
    # intermediate activation as an "output" and puts a backbone Clip at index 0.
    # We want only the tensors the cvimodel actually declares as program outputs.
    model = pyruntime_cvi.Model(str(args.cvimodel), output_all_tensors=False)
    if model is None:
        raise RuntimeError(f"cannot load {args.cvimodel}")

    cvi_input = model.inputs[0]
    print(f"cvimodel input : {cvi_input.name} shape={tuple(cvi_input.data.shape)} "
          f"dtype={cvi_input.data.dtype}")
    for out in model.outputs:
        print(f"cvimodel output: {out.name} shape={tuple(out.data.shape)} "
              f"dtype={out.data.dtype}")

    # Select the head by shape rather than trusting index 0.
    reference_shape = tuple(session.get_outputs()[0].shape)
    head = next((o for o in model.outputs if tuple(o.data.shape) == reference_shape), None)
    if head is None:
        raise RuntimeError(
            f"no cvimodel output matches the ONNX output shape {reference_shape}; "
            f"got {[tuple(o.data.shape) for o in model.outputs]}")
    print(f"comparing against : {head.name}")

    absolute_errors = []
    peak_hits = 0
    worst = (0.0, None)

    for path, gray in frames:
        reference = session.run(None, {
            onnx_input: (gray.astype(np.float32) / 255.0)[None, None]
        })[0]

        # fuse_preprocess means the cvimodel takes the raw uint8 luma plane and
        # does the /255 itself; feed it exactly what the board will feed it.
        payload = gray[None, None]
        if cvi_input.data.dtype != np.uint8:
            # Non-fused build: quantize by hand the way the application would.
            payload = (gray.astype(np.float32) / 255.0)[None, None]
        cvi_input.data[:] = payload.astype(cvi_input.data.dtype).reshape(cvi_input.data.shape)
        model.forward()
        actual = np.asarray(head.data, dtype=np.float32)

        if actual.shape != reference.shape:
            raise RuntimeError(f"shape mismatch: cvimodel {actual.shape} vs onnx {reference.shape}")

        mae = float(np.mean(np.abs(actual - reference)))
        absolute_errors.append(mae)
        if mae > worst[0]:
            worst = (mae, path)

        ref_y, ref_x = peak_cell(reference[0, 0])
        act_y, act_x = peak_cell(actual[0, 0])
        if max(abs(ref_y - act_y), abs(ref_x - act_x)) <= args.peak_tolerance:
            peak_hits += 1

    mean_mae = float(np.mean(absolute_errors))
    max_mae = float(np.max(absolute_errors))
    peak_rate = peak_hits / len(frames)

    print("\n---- results ----")
    print(f"mean absolute error : {mean_mae:.5f} (gate <= {args.max_mae})")
    print(f"worst frame MAE     : {max_mae:.5f} ({worst[1].name if worst[1] else '-'})")
    print(f"peak within {args.peak_tolerance} cells : {peak_hits}/{len(frames)} "
          f"= {peak_rate:.1%} (gate >= {args.min_peak_match:.0%})")

    failures = []
    if mean_mae > args.max_mae:
        failures.append(f"mean absolute error {mean_mae:.5f} exceeds {args.max_mae}")
    if peak_rate < args.min_peak_match:
        failures.append(f"peak match rate {peak_rate:.1%} below {args.min_peak_match:.0%}")

    if failures:
        print("\nFAIL:")
        for failure in failures:
            print(f"  - {failure}")
        print("\nIf the dilation rewrite was disabled, re-compile with it enabled "
              "(the default) and compare. That was the K230 failure mode.")
        return 1

    print("\nPASS (simulator only -- still smoke-test on real hardware)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
