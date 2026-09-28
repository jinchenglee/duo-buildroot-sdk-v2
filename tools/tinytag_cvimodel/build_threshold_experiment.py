#!/usr/bin/env python3
"""Build standalone and parallel-branch ArUco threshold CVIMODELs.

Run inside tpu_docker.sh. The v4c ONNX and calibration images must be mounted
inside that container. Outputs remain in --output-dir and are safe to copy to
the board without replacing its installed models.
"""

import argparse
import subprocess
import sys
from pathlib import Path

import cv2


def run(*args: object, cwd: Path) -> None:
    command = [str(arg) for arg in args]
    print("$", " ".join(command), flush=True)
    subprocess.run(command, cwd=str(cwd), check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--v4c", required=True, type=Path)
    parser.add_argument("--calibration-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--calibration-count", type=int, default=20)
    args = parser.parse_args()
    v4c = args.v4c.resolve()
    calibration_dir = args.calibration_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if not v4c.is_file():
        parser.error(f"missing v4c ONNX: {v4c}")
    images = sorted(path for path in calibration_dir.rglob("*")
                    if path.suffix.lower() in {".png", ".jpg", ".jpeg"})
    if len(images) < args.calibration_count:
        parser.error(f"need {args.calibration_count} calibration images, found {len(images)}")

    run(sys.executable, Path(__file__).with_name("make_threshold_onnx.py"),
        "--output-dir", output_dir, "--v4c", v4c, cwd=output_dir)
    calibration_1280 = output_dir / "calibration_1280x720"
    calibration_1280.mkdir(exist_ok=True)
    for index, path in enumerate(images[:args.calibration_count]):
        gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            raise RuntimeError(f"cannot read calibration image {path}")
        resized = cv2.resize(gray, (1280, 720), interpolation=cv2.INTER_LINEAR)
        cv2.imwrite(str(calibration_1280 / f"{index:04d}.png"), resized)

    models = [
        ("aruco_threshold_640_sep", "aruco_threshold_640x360.onnx", 360, 640, calibration_dir),
        ("aruco_threshold_1280_sep", "aruco_threshold_1280x720.onnx", 720, 1280, calibration_1280),
        ("v4c_fused_input_baseline", "v4c_fused_input_baseline.onnx", 360, 640, calibration_dir),
        ("v4c_threshold_branch", "v4c_with_threshold_branch.onnx", 360, 640, calibration_dir),
    ]
    for name, onnx_name, height, width, calibration in models:
        mlir = output_dir / f"{name}.mlir"
        run("model_transform.py", "--model_name", name,
            "--model_def", output_dir / onnx_name,
            "--input_shapes", f"[[1,1,{height},{width}]]",
            "--mean", "0.0", "--scale", "1.0",
            "--pixel_format", "gray", "--channel_format", "nchw",
            "--mlir", mlir, cwd=output_dir)
        run("model_deploy.py", "--mlir", mlir, "--quantize", "BF16",
            "--chip", "cv181x", "--fuse_preprocess",
            "--customization_format", "GRAYSCALE",
            "--model", output_dir / f"{name}.bf16.cvimodel", cwd=output_dir)
        table = output_dir / f"{name}_cali_table"
        run("run_calibration.py", mlir, "--dataset", calibration,
            "--input_num", args.calibration_count, "-o", table, cwd=output_dir)
        run("model_deploy.py", "--mlir", mlir, "--quantize", "INT8",
            "--chip", "cv181x", "--fuse_preprocess",
            "--customization_format", "GRAYSCALE",
            "--calibration_table", table,
            "--model", output_dir / f"{name}.int8.cvimodel", cwd=output_dir)


if __name__ == "__main__":
    main()
