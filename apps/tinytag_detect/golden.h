#ifndef TINYTAG_GOLDEN_H
#define TINYTAG_GOLDEN_H

#include <cstdint>
#include <string>
#include <vector>

// A golden-reference bundle produced by tools/tinytag_cvimodel/make_golden.py.
// See that script for the on-disk format and for what the two reference
// tensors mean.
struct GoldenFrame
{
    std::string name;
    std::vector<uint8_t> input; // in_h * in_w, exactly what the TPU is fed
    std::vector<float> fp32;    // ONNX Runtime FP32 ground truth
    std::vector<float> sim8;    // tpu-mlir host simulator, INT8
};

struct GoldenBundle
{
    uint32_t in_h = 0, in_w = 0;
    uint32_t out_c = 0, out_h = 0, out_w = 0;
    std::vector<GoldenFrame> frames;

    size_t input_count() const { return static_cast<size_t>(in_h) * in_w; }
    size_t output_count() const
    {
        return static_cast<size_t>(out_c) * out_h * out_w;
    }
};

// Throws std::runtime_error on a missing, truncated, or foreign-format file.
GoldenBundle load_golden(const std::string &path);

#endif // TINYTAG_GOLDEN_H
