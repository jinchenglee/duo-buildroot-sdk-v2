#pragma once
#include <opencv2/core.hpp>
#include <memory>
#include <cstdint>

struct RoiThresholdTiming {
    double prepare_us = 0, wait_us = 0, finish_us = 0, total_us = 0;
    double remote_compute_us = 0, remote_service_us = 0;
    uint64_t remote_cycles = 0;
};
class RoiThresholdOffload {
public:
    explicit RoiThresholdOffload(unsigned poll_us = 50, unsigned timeout_ms = 2000);
    ~RoiThresholdOffload();
    RoiThresholdOffload(const RoiThresholdOffload &) = delete;
    RoiThresholdOffload &operator=(const RoiThresholdOffload &) = delete;
    void run(const cv::Mat &input, cv::Mat &output, int kernel, int threshold,
             RoiThresholdTiming &timing);
    void nop(RoiThresholdTiming &timing);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void roi_threshold_opencv(const cv::Mat &input, cv::Mat &output, int kernel, int threshold);
bool roi_threshold_scalar(const cv::Mat &input, cv::Mat &output, int kernel, int threshold);
using RoiThresholdHook = bool (*)(const cv::Mat &, cv::Mat &, int, int);
/* TINYTAG_THRESHOLD_BACKEND=opencv(default)|scalar|freertos. */
RoiThresholdHook roi_threshold_runtime_hook();
