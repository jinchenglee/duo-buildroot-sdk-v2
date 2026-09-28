// Standalone cv181x threshold benchmark; accepts an exact-size grayscale PNG.
#include <cviruntime.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

static double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

static float value(const CVI_TENSOR &tensor, size_t index) {
    const void *ptr = CVI_NN_TensorPtr(const_cast<CVI_TENSOR *>(&tensor));
    if (tensor.fmt == CVI_FMT_FP32) return static_cast<const float *>(ptr)[index];
    if (tensor.fmt == CVI_FMT_BF16) {
        uint32_t bits = static_cast<uint32_t>(static_cast<const uint16_t *>(ptr)[index]) << 16;
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }
    if (tensor.fmt == CVI_FMT_INT8) return static_cast<const int8_t *>(ptr)[index] / tensor.qscale;
    if (tensor.fmt == CVI_FMT_UINT8) return static_cast<const uint8_t *>(ptr)[index];
    throw std::runtime_error("unsupported output format");
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 6) {
        std::fprintf(stderr, "usage: %s model.cvimodel exact-size-gray.png [repeat=30] [mask.png] [cutoff=127.5]\n", argv[0]);
        return 2;
    }
    const int repeat = argc > 3 ? std::max(1, std::atoi(argv[3])) : 30;
    const float cutoff = argc > 5 ? std::atof(argv[5]) : 127.5f;
    CVI_MODEL_HANDLE model = nullptr;
    try {
        if (CVI_NN_RegisterModel(argv[1], &model) != CVI_RC_SUCCESS) throw std::runtime_error("register model failed");
        CVI_TENSOR *inputs = nullptr, *outputs = nullptr;
        int32_t n_inputs = 0, n_outputs = 0;
        if (CVI_NN_GetInputOutputTensors(model, &inputs, &n_inputs, &outputs, &n_outputs) != CVI_RC_SUCCESS)
            throw std::runtime_error("get tensors failed");
        if (n_inputs != 1 || n_outputs < 1) throw std::runtime_error("unexpected tensor count");
        const CVI_SHAPE input_shape = CVI_NN_TensorShape(&inputs[0]);
        const int height = input_shape.dim[2], width = input_shape.dim[3];
        const cv::Mat image = cv::imread(argv[2], cv::IMREAD_GRAYSCALE);
        if (image.empty() || image.rows != height || image.cols != width)
            throw std::runtime_error("input image must match model size exactly");
        if (inputs[0].fmt != CVI_FMT_UINT8 || inputs[0].mem_size < image.total())
            throw std::runtime_error("model must have fused uint8 grayscale input");
        std::memcpy(CVI_NN_TensorPtr(&inputs[0]), image.data, image.total());

        int mask_index = -1;
        for (int i = 0; i < n_outputs; ++i) {
            const CVI_SHAPE shape = CVI_NN_TensorShape(&outputs[i]);
            std::printf("output[%d] name=%s shape=[%d,%d,%d,%d] fmt=%d\n", i, outputs[i].name,
                        shape.dim[0], shape.dim[1], shape.dim[2], shape.dim[3], outputs[i].fmt);
            if (shape.dim_size == 4 && shape.dim[1] == 1 && shape.dim[2] == height && shape.dim[3] == width)
                mask_index = i;
        }
        if (mask_index < 0) throw std::runtime_error("model lacks full-resolution threshold output");
        for (int i = 0; i < 2; ++i)
            if (CVI_NN_Forward(model, inputs, n_inputs, outputs, n_outputs) != CVI_RC_SUCCESS)
                throw std::runtime_error("warmup forward failed");
        std::vector<double> times;
        for (int i = 0; i < repeat; ++i) {
            const double started = now_ms();
            if (CVI_NN_Forward(model, inputs, n_inputs, outputs, n_outputs) != CVI_RC_SUCCESS)
                throw std::runtime_error("forward failed");
            times.push_back(now_ms() - started);
        }
        std::sort(times.begin(), times.end());
        const double sum = std::accumulate(times.begin(), times.end(), 0.0);

        cv::Mat actual(height, width, CV_8UC1);
        size_t fractional = 0;
        float raw_min = std::numeric_limits<float>::infinity();
        float raw_max = -std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < image.total(); ++i) {
            const float raw = value(outputs[mask_index], i);
            raw_min = std::min(raw_min, raw);
            raw_max = std::max(raw_max, raw);
            if (raw > 0.0f && raw < 255.0f) ++fractional;
            actual.data[i] = raw > cutoff ? 255 : 0;
        }
        cv::Mat mean, difference, expected, mismatch;
        cv::boxFilter(image, mean, image.type(), cv::Size(15, 15), cv::Point(-1, -1), true,
                      cv::BORDER_REPLICATE | cv::BORDER_ISOLATED);
        cv::subtract(mean, image, difference);
        cv::threshold(difference, expected, 3, 255, cv::THRESH_BINARY);
        cv::compare(actual, expected, mismatch, cv::CMP_NE);
        std::printf("model=%s input=%dx%d repeats=%d median_ms=%.3f mean_ms=%.3f min_ms=%.3f max_ms=%.3f mismatch_pixels=%d/%zu fractional_raw=%zu raw_range=[%.3f,%.3f] qscale=%.6g cutoff=%.3f\n",
                    argv[1], width, height, repeat, times[times.size()/2], sum/times.size(), times.front(), times.back(),
                    cv::countNonZero(mismatch), image.total(), fractional, raw_min, raw_max,
                    outputs[mask_index].qscale, cutoff);
        if (argc > 4 && !cv::imwrite(argv[4], actual)) throw std::runtime_error("mask save failed");
        CVI_NN_CleanupModel(model);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "threshold_bench: %s\n", error.what());
        if (model) CVI_NN_CleanupModel(model);
        return 1;
    }
}
