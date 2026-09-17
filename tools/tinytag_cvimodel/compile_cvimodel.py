#!/usr/bin/env python3
"""Compile an external TinyTag ONNX model into a cv181x cvimodel.

Drives tpu-mlir's three-stage flow (model_transform -> run_calibration ->
model_deploy) against the fixed TinyTag application contract:

  - source ONNX input : float 1x1x360x640, normalized to [0, 1]
  - deployed input    : uint8 grayscale 1x1x360x640, with /255 folded into the
                        model by --fuse_preprocess (so the application memcpys a
                        raw luma plane straight into the input tensor)
  - output            : float 1x21x45x80
  - target            : cv181x (covers SG2000/SG2002 as used on the Duo S)

Run this inside the toolchain container:

  tools/tinytag_cvimodel/tpu_docker.sh run \\
    tools/tinytag_cvimodel/compile_cvimodel.py \\
      --onnx /assets/tinytag-v40c-unfrozen-moderate30ep/tinytag-v40c-unfrozen-moderate30ep.static.onnx \\
      --calibration-dir build/tinytag/frames/calibration \\
      --output build/tinytag/tinytag-v40c.int8.cvimodel
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper, shape_inference

INPUT_SHAPE = [1, 1, 360, 640]
OUTPUT_SHAPE = [1, 21, 45, 80]
IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png", ".pgm", ".webp"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--onnx", required=True, type=Path,
                        help="external TinyTag ONNX model")
    parser.add_argument("--calibration-dir", required=True, action="append", type=Path,
                        help="directory of prepared 640x360 grayscale frames; repeat as needed")
    parser.add_argument("--output", required=True, type=Path,
                        help="output .cvimodel path")
    parser.add_argument("--validation-dir", type=Path,
                        help="held-out frames; the first is used for tpu-mlir's built-in "
                             "per-layer similarity check during model_deploy")
    parser.add_argument("--model-name", default="tinytag",
                        help="model name recorded in the cvimodel (default: tinytag)")
    parser.add_argument("--chip", default="cv181x",
                        choices=("cv181x", "cv180x", "cv182x", "cv183x"),
                        help="target chip (default: cv181x, which covers SG2000/SG2002)")
    parser.add_argument("--quantize", default="INT8", choices=("INT8", "BF16", "F32"),
                        help="quantization mode (default: INT8)")
    parser.add_argument("--input-num", type=int, default=0,
                        help="calibration images to use; 0 means all available")
    parser.add_argument("--tolerance", default="0.90,0.60",
                        help="model_deploy cosine/euclidean similarity floor (default: 0.90,0.60)")
    parser.add_argument("--keep-native-dilated-depthwise", dest="expand_dilated_depthwise",
                        action="store_false",
                        help="diagnostic opt-out: do not apply the default dilation rewrite")
    parser.set_defaults(expand_dilated_depthwise=True)
    parser.add_argument("--allow-small-calibration", action="store_true",
                        help="allow fewer than 100 calibration frames (experimental builds only)")
    parser.add_argument("--aligned-input", action="store_true",
                        help="compile an input tensor that can bind a width-aligned VPSS "
                             "physical frame directly (experimental live-camera path)")
    parser.add_argument("--workdir", type=Path,
                        help="keep intermediates here instead of a temporary directory")
    return parser.parse_args()


def set_attribute(node: onnx.NodeProto, name: str, value: list[int]) -> None:
    replacement = onnx.helper.make_attribute(name, value)
    for index, attribute in enumerate(node.attribute):
        if attribute.name == name:
            node.attribute[index].CopyFrom(replacement)
            return
    node.attribute.append(replacement)


def expand_dilated_depthwise(model: onnx.ModelProto) -> int:
    """Rewrite depthwise dilation without changing the represented function.

    A 3x3 kernel with dilation D is rewritten as a sparse (2D+1)x(2D+1) kernel
    with dilation 1, by inserting zeros between the original taps. This is
    mathematically exact -- the two graphs compute the same function in FP32.

    Why it is on by default: nncase lowered this exact construct incorrectly for
    the K230 INT8 path (see tools/tinytag_kmodel/README.md in the K230 SDK), and
    whether tpu-mlir's cv181x INT8 path has the same weakness is not established.
    Compiling the dense form costs a little weight memory (48 3x3 depthwise taps
    become 5x5 and 7x7) and removes the question entirely. Use
    --keep-native-dilated-depthwise to build the other variant and compare.
    """
    initializers = {item.name: (index, item)
                    for index, item in enumerate(model.graph.initializer)}
    expanded = 0
    for node in model.graph.node:
        if node.op_type != "Conv":
            continue
        attributes = {item.name: onnx.helper.get_attribute_value(item)
                      for item in node.attribute}
        dilation = attributes.get("dilations", [1, 1])
        if list(dilation) == [1, 1]:
            continue

        index, initializer = initializers[node.input[1]]
        weight = numpy_helper.to_array(initializer)
        group = attributes.get("group", 1)
        if (len(dilation) != 2 or min(dilation) < 1 or weight.ndim != 4
                or weight.shape[1] != 1 or group != weight.shape[0]):
            raise RuntimeError(
                "dilation rewrite encountered an unsupported non-depthwise "
                f"convolution: {node.output[0]}. Re-run with "
                "--keep-native-dilated-depthwise and validate the result carefully."
            )

        height = (weight.shape[2] - 1) * dilation[0] + 1
        width = (weight.shape[3] - 1) * dilation[1] + 1
        dense = np.zeros((weight.shape[0], weight.shape[1], height, width),
                         dtype=weight.dtype)
        dense[:, :, ::dilation[0], ::dilation[1]] = weight
        model.graph.initializer[index].CopyFrom(
            numpy_helper.from_array(dense, initializer.name))
        set_attribute(node, "dilations", [1, 1])
        set_attribute(node, "kernel_shape", [height, width])
        expanded += 1
        print(f"expanded {node.output[0]}: {tuple(weight.shape[2:])} "
              f"dilation {tuple(dilation)} -> {(height, width)}")
    return expanded


def prepare_onnx(source: Path, destination: Path, expand_dilation: bool) -> None:
    model = onnx.load(str(source))
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise RuntimeError("TinyTag conversion expects exactly one input and one output")

    if expand_dilation:
        count = expand_dilated_depthwise(model)
        print(f"expanded dilated depthwise convolutions: {count}")
    else:
        print("dilation rewrite DISABLED (--keep-native-dilated-depthwise)")

    input_dims = model.graph.input[0].type.tensor_type.shape.dim
    if len(input_dims) != len(INPUT_SHAPE):
        raise RuntimeError(f"expected rank-4 input, got rank {len(input_dims)}")
    for dim, value in zip(input_dims, INPUT_SHAPE):
        dim.ClearField("dim_param")
        dim.dim_value = value

    model = shape_inference.infer_shapes(model)
    actual = [dim.dim_value for dim in model.graph.output[0].type.tensor_type.shape.dim]
    if actual != OUTPUT_SHAPE:
        raise RuntimeError(f"expected output {OUTPUT_SHAPE}, got {actual}")
    onnx.checker.check_model(model)
    onnx.save(model, str(destination))


def list_images(directories: list[Path]) -> list[Path]:
    paths = sorted(path
                   for directory in directories
                   for path in directory.rglob("*")
                   if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES)
    if not paths:
        raise RuntimeError(f"no images found under {[str(d) for d in directories]}")
    return paths


def run(command: list[str], cwd: Path) -> None:
    """Run a tpu-mlir tool with `cwd` as its working directory.

    These tools scatter intermediates (.mlir, .npz, .onnx, weight maps) into the
    current directory with no way to redirect them, so they are run inside the
    work directory rather than wherever the user invoked this script -- which
    would otherwise be the repository root.
    """
    printable = " ".join(str(part) for part in command)
    print(f"\n$ {printable}\n", flush=True)
    result = subprocess.run([str(part) for part in command], cwd=str(cwd))
    if result.returncode != 0:
        raise SystemExit(f"command failed with exit code {result.returncode}: {printable}")


def main() -> int:
    args = parse_args()
    if args.output.suffix != ".cvimodel":
        raise RuntimeError("--output must end in .cvimodel")

    # Absolute throughout, because the tpu-mlir tools below run with a different
    # working directory than this script was invoked from.
    args.onnx = args.onnx.resolve()
    args.output = args.output.resolve()
    args.calibration_dir = [d.resolve() for d in args.calibration_dir]
    if args.validation_dir:
        args.validation_dir = args.validation_dir.resolve()
    if args.workdir:
        args.workdir = args.workdir.resolve()

    calibration_images = list_images(args.calibration_dir)
    if len(calibration_images) < 100 and not args.allow_small_calibration:
        raise RuntimeError(
            f"only {len(calibration_images)} calibration frames found, want >=100; "
            "add raw footage or pass --allow-small-calibration for an experimental build")

    workdir_context = None
    if args.workdir:
        args.workdir.mkdir(parents=True, exist_ok=True)
        workdir = args.workdir
    else:
        workdir_context = tempfile.TemporaryDirectory(prefix="tinytag-cvimodel-")
        workdir = Path(workdir_context.name)

    try:
        prepared_onnx = workdir / "prepared.onnx"
        mlir = workdir / f"{args.model_name}.mlir"
        calibration_table = workdir / f"{args.model_name}_cali_table"

        print("== stage 0: prepare ONNX ==")
        prepare_onnx(args.onnx, prepared_onnx, args.expand_dilated_depthwise)

        # The application feeds a raw uint8 luma plane, so the network's own
        # normalization is x/255. tpu-mlir applies (x - mean) * scale.
        print("\n== stage 1: model_transform ==")
        transform = [
            "model_transform.py",
            "--model_name", args.model_name,
            "--model_def", prepared_onnx,
            "--input_shapes", f"[[{','.join(str(d) for d in INPUT_SHAPE)}]]",
            "--mean", "0.0",
            "--scale", repr(1.0 / 255.0),
            "--pixel_format", "gray",
            "--channel_format", "nchw",
            "--mlir", mlir,
        ]

        reference_image = None
        reference_outputs = workdir / f"{args.model_name}_top_outputs.npz"
        if args.validation_dir:
            validation_images = list_images([args.validation_dir])
            reference_image = validation_images[0]
            print(f"reference frame for the deploy-time similarity check: {reference_image}")
            transform += ["--test_input", reference_image,
                          "--test_result", reference_outputs]
        run(transform, workdir)

        if args.quantize == "INT8":
            print("\n== stage 2: run_calibration ==")
            calibrate = [
                "run_calibration.py", mlir,
                "--dataset", args.calibration_dir[0],
                "--input_num", str(args.input_num or len(calibration_images)),
                "-o", calibration_table,
            ]
            if len(args.calibration_dir) > 1:
                # run_calibration takes a single --dataset; stage everything into one.
                merged = workdir / "calibration_merged"
                merged.mkdir(exist_ok=True)
                for index, path in enumerate(calibration_images):
                    shutil.copy2(path, merged / f"{index:05d}{path.suffix}")
                calibrate[calibrate.index("--dataset") + 1] = merged
            run(calibrate, workdir)

        print("\n== stage 3: model_deploy ==")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        deploy = [
            "model_deploy.py",
            "--mlir", mlir,
            "--quantize", args.quantize,
            "--chip", args.chip,
            # fuse_preprocess folds the /255 into the network and makes the input
            # tensor uint8 GRAYSCALE. The default model accepts a plain
            # contiguous CPU buffer; the experimental aligned variant binds a
            # VPSS physical frame directly on the board.
            "--fuse_preprocess",
            "--customization_format", "GRAYSCALE",
            "--model", args.output,
        ]
        if args.aligned_input:
            deploy.append("--aligned_input")
        if args.quantize == "INT8":
            deploy += ["--calibration_table", calibration_table]
        if reference_image is not None:
            deploy += ["--test_input", reference_image,
                       "--test_reference", reference_outputs,
                       "--tolerance", args.tolerance,
                       "--compare_all"]
        run(deploy, workdir)

        print(f"\nwrote {args.output} ({args.output.stat().st_size} bytes)")
        print("Next: validate against ONNX Runtime before flashing:\n"
              f"  tools/tinytag_cvimodel/validate_cvimodel.py --onnx {args.onnx} "
              f"--cvimodel {args.output} --images <validation dir>")
        return 0
    finally:
        if workdir_context is not None:
            workdir_context.cleanup()


if __name__ == "__main__":
    sys.exit(main())
