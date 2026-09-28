#!/usr/bin/env python3
"""Create TPU-friendly ArUco Nano threshold graphs and a parallel v4c graph.

Input is grayscale pixel values in [0, 255], represented as FP32 in ONNX.
The deployed cvimodel uses fused preprocessing to accept raw uint8 luma.
"""

import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def threshold_nodes(height: int, width: int):
    # OpenCV's 8-bit 15x15 boxFilter rounds the average before subtracting
    # an integer pixel. Since 225 is odd, an integer sum / 225 is never a
    # half-integer. Thus round(mean) - pixel > 3 iff mean - pixel > 3.5.
    vertical = numpy_helper.from_array(np.full((1, 1, 15, 1), 1 / 15, np.float32), "threshold_vertical")
    horizontal = numpy_helper.from_array(np.full((1, 1, 1, 15), 1 / 15, np.float32), "threshold_horizontal")
    bias = numpy_helper.from_array(np.array([3.5], np.float32), "threshold_offset")
    white = numpy_helper.from_array(np.array([255], np.float32), "white_value")
    steepness = numpy_helper.from_array(np.array([500], np.float32), "threshold_steepness")
    zero = numpy_helper.from_array(np.array([0], np.float32), "zero_value")
    one = numpy_helper.from_array(np.array([1], np.float32), "one_value")
    pads = numpy_helper.from_array(np.array([0, 0, 7, 7, 0, 0, 7, 7], np.int64), "edge_pads")
    nodes = [
        helper.make_node("Pad", ["image", "edge_pads"], ["threshold_padded"], mode="edge"),
        helper.make_node("Conv", ["threshold_padded", "threshold_vertical"], ["threshold_vertical_mean"], kernel_shape=[15, 1]),
        helper.make_node("Conv", ["threshold_vertical_mean", "threshold_horizontal"], ["threshold_mean"], kernel_shape=[1, 15]),
        helper.make_node("Sub", ["threshold_mean", "image"], ["threshold_difference"]),
        helper.make_node("Sub", ["threshold_difference", "threshold_offset"], ["threshold_margin"]),
        helper.make_node("Mul", ["threshold_margin", "threshold_steepness"], ["threshold_ramp"]),
        helper.make_node("Clip", ["threshold_ramp", "zero_value", "one_value"], ["threshold_clipped"]),
        helper.make_node("Mul", ["threshold_clipped", "white_value"], ["threshold_mask"]),
    ]
    return nodes, [pads, vertical, horizontal, bias, steepness, zero, one, white]


def make_model(height: int, width: int, v4c_path: Path | None):
    nodes, initializers = threshold_nodes(height, width)
    outputs = []
    if v4c_path is not None:
        if (height, width) != (360, 640):
            raise ValueError("v4c supports only 640x360")
        v4c = onnx.load(str(v4c_path))
        # Prefix every internal value to avoid collisions with the threshold branch.
        for item in v4c.graph.initializer:
            item.name = "v4c_" + item.name
            initializers.append(item)
        value_names = {value for node in v4c.graph.node
                       for value in (*node.input, *node.output) if value}
        value_names.discard(v4c.graph.input[0].name)
        rename = {name: "v4c_" + name for name in value_names}
        nodes.insert(0, helper.make_node("Div", ["image", "v4c_scale"], ["v4c_input"]))
        initializers.append(numpy_helper.from_array(np.array([255], np.float32), "v4c_scale"))
        for node in v4c.graph.node:
            for i, name in enumerate(node.input):
                node.input[i] = "v4c_input" if name == v4c.graph.input[0].name else rename.get(name, name)
            for i, name in enumerate(node.output):
                node.output[i] = rename.get(name, name)
            nodes.append(node)
        outputs.append(helper.make_tensor_value_info(rename[v4c.graph.output[0].name], TensorProto.FLOAT, [1, 21, 45, 80]))
    outputs.append(helper.make_tensor_value_info("threshold_mask", TensorProto.FLOAT, [1, 1, height, width]))
    graph = helper.make_graph(nodes, "aruco_nano_threshold" + ("_v4c" if v4c_path else ""),
                              [helper.make_tensor_value_info("image", TensorProto.FLOAT, [1, 1, height, width])],
                              outputs, initializer=initializers)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 9
    onnx.checker.check_model(model)
    return model


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--v4c", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for height, width in [(360, 640), (720, 1280)]:
        output = args.output_dir / f"aruco_threshold_{width}x{height}.onnx"
        onnx.save(make_model(height, width, None), output)
        print(output)
    output = args.output_dir / "v4c_with_threshold_branch.onnx"
    merged = make_model(360, 640, args.v4c)
    onnx.save(merged, output)
    print(output)
    # Same v4c input normalization and compiler contract, without the extra
    # branch, for a like-for-like BF16 hardware timing baseline.
    baseline = onnx.ModelProto()
    baseline.CopyFrom(merged)
    threshold_values = {"edge_pads", "threshold_vertical", "threshold_horizontal", "threshold_offset",
                        "threshold_steepness", "zero_value", "one_value", "white_value"}
    retained = [node for node in baseline.graph.node if not any(
        output.startswith("threshold_") for output in node.output)]
    del baseline.graph.node[:]
    baseline.graph.node.extend(retained)
    kept_initializers = [item for item in baseline.graph.initializer if item.name not in threshold_values]
    del baseline.graph.initializer[:]
    baseline.graph.initializer.extend(kept_initializers)
    del baseline.graph.output[1:]
    onnx.checker.check_model(baseline)
    baseline_path = args.output_dir / "v4c_fused_input_baseline.onnx"
    onnx.save(baseline, baseline_path)
    print(baseline_path)


if __name__ == "__main__":
    main()
