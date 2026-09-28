#!/usr/bin/env python3
"""Pack overlapping image strips into channels for a separable box filter."""

import argparse
import math
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def make_model(width: int, height: int, stripes: int, clip: float | None) -> onnx.ModelProto:
    if stripes < 1 or width < 1 or height < 1:
        raise ValueError("dimensions and stripe count must be positive")
    tile_height = math.ceil(height / stripes)
    extra_rows = tile_height * stripes - height
    initializers = []
    nodes = []

    def constant(name, values):
        initializers.append(numpy_helper.from_array(np.asarray(values), name))
        return name

    constant("pads", np.array([0, 0, 7, 7, 0, 0, 7 + extra_rows, 7], dtype=np.int64))
    constant("vertical_kernel", np.full((stripes, 1, 15, 1), 1 / 15, dtype=np.float32))
    constant("horizontal_kernel", np.full((stripes, 1, 1, 15), 1 / 15, dtype=np.float32))
    nodes.append(helper.make_node("Pad", ["image", "pads"], ["padded"], mode="edge"))
    tiles = []
    for index in range(stripes):
        start = constant(f"row_start_{index}", np.array([index * tile_height], dtype=np.int64))
        end = constant(f"row_end_{index}", np.array([index * tile_height + tile_height + 14], dtype=np.int64))
        axis = constant(f"row_axis_{index}", np.array([2], dtype=np.int64))
        name = f"tile_{index}"
        nodes.append(helper.make_node("Slice", ["padded", start, end, axis], [name]))
        tiles.append(name)
    nodes.append(helper.make_node("Concat", tiles, ["stacked"], axis=1))
    nodes.append(helper.make_node("Conv", ["stacked", "vertical_kernel"], ["vertical"],
                                  kernel_shape=[15, 1], group=stripes))
    nodes.append(helper.make_node("Conv", ["vertical", "horizontal_kernel"], ["mean_strips"],
                                  kernel_shape=[1, 15], group=stripes))
    strips = []
    for index in range(stripes):
        start = constant(f"channel_start_{index}", np.array([index], dtype=np.int64))
        end = constant(f"channel_end_{index}", np.array([index + 1], dtype=np.int64))
        axis = constant(f"channel_axis_{index}", np.array([1], dtype=np.int64))
        name = f"strip_{index}"
        nodes.append(helper.make_node("Slice", ["mean_strips", start, end, axis], [name]))
        strips.append(name)
    assembled = "mean" if not extra_rows else "mean_with_extra_rows"
    nodes.append(helper.make_node("Concat", strips, [assembled], axis=2))
    if extra_rows:
        start = constant("valid_row_start", np.array([0], dtype=np.int64))
        end = constant("valid_row_end", np.array([height], dtype=np.int64))
        axis = constant("valid_row_axis", np.array([2], dtype=np.int64))
        nodes.append(helper.make_node("Slice", [assembled, start, end, axis], ["mean"]))
    nodes.append(helper.make_node("Sub", ["mean", "image"], ["difference"]))
    output = "difference"
    if clip is not None:
        minimum = constant("minimum", np.array(-clip, dtype=np.float32))
        maximum = constant("maximum", np.array(clip, dtype=np.float32))
        nodes.append(helper.make_node("Clip", ["difference", minimum, maximum], ["clipped_difference"]))
        output = "clipped_difference"
    graph = helper.make_graph(
        nodes, f"aruco_striped_{width}x{height}_{stripes}",
        [helper.make_tensor_value_info("image", TensorProto.FLOAT, [1, 1, height, width])],
        [helper.make_tensor_value_info(output, TensorProto.FLOAT, [1, 1, height, width])],
        initializer=initializers,
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 9
    onnx.checker.check_model(model)
    return model


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--stripes", type=int, required=True)
    parser.add_argument("--clip", type=float)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(make_model(args.width, args.height, args.stripes, args.clip), args.output)
