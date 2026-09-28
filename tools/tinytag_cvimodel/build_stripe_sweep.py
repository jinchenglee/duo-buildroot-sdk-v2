#!/usr/bin/env python3
"""Compile selected striped threshold variants inside the pinned TPU container."""

import argparse
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--calibration-640", type=Path, required=True)
    parser.add_argument("--calibration-1280", type=Path, required=True)
    parser.add_argument("names", nargs="+")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    for name in args.names:
        large = "1280" in name
        height, width = (720, 1280) if large else (360, 640)
        calibration = args.calibration_1280 if large else args.calibration_640
        log = work / f"{name}_build.log"
        commands = [
            ["model_transform.py", "--model_name", name, "--model_def", f"{name}.onnx",
             "--input_shapes", f"[[1,1,{height},{width}]]", "--mean", "0.0", "--scale", "1.0",
             "--pixel_format", "gray", "--channel_format", "nchw", "--mlir", f"{name}.mlir"],
            ["run_calibration.py", f"{name}.mlir", "--dataset", str(calibration.resolve()),
             "--input_num", "20", "-o", f"{name}_cali_table"],
            ["model_deploy.py", "--mlir", f"{name}.mlir", "--quantize", "INT8", "--chip", "cv181x",
             "--fuse_preprocess", "--customization_format", "GRAYSCALE", "--quant_output",
             "--calibration_table", f"{name}_cali_table", "--model", f"{name}.int8.cvimodel"],
        ]
        with log.open("w") as out:
            for command in commands:
                print("$", " ".join(command), flush=True)
                out.write("$ " + " ".join(command) + "\n")
                out.flush()
                subprocess.run(command, cwd=work, stdout=out, stderr=subprocess.STDOUT, check=True)


if __name__ == "__main__":
    main()
