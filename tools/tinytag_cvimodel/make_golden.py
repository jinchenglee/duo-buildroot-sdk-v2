#!/usr/bin/env python3
"""Build a golden-reference bundle for on-board self-checking.

For each held-out frame this records three things:

  1. the exact uint8 input the board will feed the TPU,
  2. the FP32 ONNX Runtime output  -- the ground truth the model is supposed
     to approximate,
  3. the INT8 host-simulator output -- what tpu-mlir's cv18xx simulator
     produced for the same input.

On the board, `tinytag_detect --selftest` replays all of them and reports two
different errors:

  hardware-INT8 vs FP32   quantization error. Should match what
                          validate_cvimodel.py measured on the host.
  hardware-INT8 vs sim    hardware/simulator divergence. Should be ~0. Anything
                          else means the real TPU disagrees with the simulator
                          the model was signed off on -- which is exactly the
                          K230 failure mode, where a kmodel passed simulation
                          and then corrupted memory on real silicon.

Format (little-endian throughout; every target here is little-endian):

    magic   char[8]   "TTGOLD01"
    counts  uint32[6] frames, in_h, in_w, out_c, out_h, out_w
    per frame:
        name    char[64]  NUL-padded
        input   uint8 [in_h * in_w]
        fp32    float32[out_c * out_h * out_w]
        sim8    float32[out_c * out_h * out_w]
"""

import argparse
import importlib
import os
import struct
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np
import onnxruntime

MAGIC = b"TTGOLD01"
NAME_LEN = 64
IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png", ".pgm", ".webp"}
NET_W, NET_H = 640, 360


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--onnx", required=True, type=Path, help="FP32 reference ONNX model")
    parser.add_argument("--cvimodel", required=True, type=Path, help="compiled cvimodel")
    parser.add_argument("--images", required=True, type=Path,
                        help="directory of held-out 640x360 grayscale frames")
    parser.add_argument("--output", required=True, type=Path, help="output .ttgold bundle")
    parser.add_argument("--frames", type=int, default=4,
                        help="how many frames to include (default: 4; each adds ~816 KB)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.frames < 1:
        raise RuntimeError("--frames must be >= 1")

    # The cv18xx simulator drops scratch files into the current directory; keep
    # them out of the repository. Paths are resolved first so the chdir is safe.
    args.onnx = args.onnx.resolve()
    args.cvimodel = args.cvimodel.resolve()
    args.images = args.images.resolve()
    args.output = args.output.resolve()
    os.chdir(tempfile.mkdtemp(prefix="tinytag-sim-"))

    paths = sorted(p for p in args.images.rglob("*")
                   if p.is_file() and p.suffix.lower() in IMAGE_SUFFIXES)
    if not paths:
        raise RuntimeError(f"no images found under {args.images}")
    # Spread the selection across the available frames instead of taking a
    # consecutive run, which in a 10 fps clip would be near-identical images.
    step = max(1, len(paths) // args.frames)
    paths = paths[::step][:args.frames]

    session = onnxruntime.InferenceSession(str(args.onnx), providers=["CPUExecutionProvider"])
    onnx_input = session.get_inputs()[0].name
    reference_shape = tuple(session.get_outputs()[0].shape)

    pyruntime_cvi = importlib.import_module("pyruntime_cvi")
    model = pyruntime_cvi.Model(str(args.cvimodel), output_all_tensors=False)
    cvi_input = model.inputs[0]
    head = next((o for o in model.outputs if tuple(o.data.shape) == reference_shape), None)
    if head is None:
        raise RuntimeError(
            f"no cvimodel output matches the ONNX output shape {reference_shape}")
    if cvi_input.data.dtype != np.uint8:
        raise RuntimeError(
            f"cvimodel input is {cvi_input.data.dtype}, expected uint8. The golden bundle "
            "stores raw uint8 frames, which assumes a --fuse_preprocess build.")

    out_c, out_h, out_w = reference_shape[1], reference_shape[2], reference_shape[3]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as handle:
        handle.write(MAGIC)
        handle.write(struct.pack("<6I", len(paths), NET_H, NET_W, out_c, out_h, out_w))

        for path in paths:
            gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
            if gray is None:
                raise RuntimeError(f"cannot decode {path}")
            if gray.shape != (NET_H, NET_W):
                raise RuntimeError(
                    f"{path} is {gray.shape[1]}x{gray.shape[0]}, expected {NET_W}x{NET_H}; "
                    "run prepare_calibration.py first")

            fp32 = session.run(None, {
                onnx_input: (gray.astype(np.float32) / 255.0)[None, None]
            })[0].astype(np.float32)

            cvi_input.data[:] = gray[None, None]
            model.forward()
            sim8 = np.asarray(head.data, dtype=np.float32)

            name = path.name.encode("utf-8")[:NAME_LEN - 1]
            handle.write(name.ljust(NAME_LEN, b"\0"))
            handle.write(gray.astype(np.uint8).tobytes())
            handle.write(np.ascontiguousarray(fp32).tobytes())
            handle.write(np.ascontiguousarray(sim8).tobytes())

            mae = float(np.mean(np.abs(sim8 - fp32)))
            print(f"{path.name}: simulator-vs-fp32 MAE {mae:.5f}")

    size = args.output.stat().st_size
    print(f"\nwrote {args.output} ({len(paths)} frames, {size} bytes)")
    print("Stage it into the image and run on the board:\n  run_tinytag.sh --selftest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
