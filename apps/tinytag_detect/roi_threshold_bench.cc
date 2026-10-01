#include "roi_threshold_offload.h"
#include "../common/roi_threshold.h"
#include "../common/roi_threshold_protocol.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
double wall_us()
{
    return std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
double cpu_us()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}
double mean(const std::vector<double> &v)
{
    double sum = 0;
    for (double x : v) sum += x;
    return v.empty() ? 0 : sum / v.size();
}
double percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}
void same(const cv::Mat &a, const cv::Mat &b, const char *name)
{
    if (a.size() != b.size() || a.type() != b.type())
        throw std::runtime_error(std::string(name) + " output shape/type mismatch");
    if (cv::countNonZero(a != b)) {
        for (int y=0; y<a.rows; ++y) for (int x=0; x<a.cols; ++x)
            if (a.at<uint8_t>(y,x) != b.at<uint8_t>(y,x)) {
                std::fprintf(stderr, "%s: first mismatch (%d,%d): OpenCV=%u actual=%u; different pixels=%d\n",
                             name,x,y,a.at<uint8_t>(y,x),b.at<uint8_t>(y,x),cv::countNonZero(a!=b));
                goto reported;
            }
reported:
        throw std::runtime_error(std::string(name) + " differs from OpenCV (STOP; no valid timing conclusion)");
    }
}
cv::Mat synthetic(int w, int h, int pattern)
{
    /* Return a non-contiguous ROI with a parent whose pixels differ: verifies
     * that border handling is isolated from surrounding parent content. */
    cv::Mat parent(h + 4, w + 8, CV_8UC1, cv::Scalar(173));
    cv::Mat in = parent(cv::Rect(3, 2, w, h));
    uint32_t state = 0x92811234;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        unsigned value = state & 255;
        if (pattern == 0) value = 0;
        if (pattern == 1) value = 255;
        if (pattern == 2) value = ((x / 8 + y / 8) & 1) ? 240 : 12;
        if (pattern == 3) value = 125 + (state % 7);
        in.at<uint8_t>(y, x) = value;
    }
    return in;
}
void local_validation()
{
    const cv::Size sizes[] = {{1,1}, {1,19}, {19,1}, {2,3}, {7,9}, {31,17}, {201,99}, {640,360}, {1280,800}};
    const int kernels[] = {1,3,5,15,31,63};
    const int thresholds[] = {-1,0,1,3,127,254,255};
    size_t cases = 0;
    for (const auto size : sizes) for (int k : kernels)
        for (int t : thresholds) for (int pattern = 0; pattern < 5; ++pattern) {
            cv::Mat in = synthetic(size.width, size.height, pattern), expected, actual;
            roi_threshold_opencv(in, expected, k, t);
            if (!roi_threshold_scalar(in, actual, k, t))
                throw std::runtime_error("scalar validation rejected supported input");
            try { same(expected, actual, "scalar threshold"); }
            catch (...) {
                cv::Mat mean;
                cv::boxFilter(in,mean,CV_8U,cv::Size(k,k),cv::Point(-1,-1),true,
                              cv::BORDER_REPLICATE|cv::BORDER_ISOLATED);
                std::fprintf(stderr,"Validation case: %dx%d kernel=%d threshold=%d pattern=%d OpenCV=%s\n",
                             size.width,size.height,k,t,pattern,CV_VERSION);
                for (int y=0; y<in.rows; ++y) for (int x=0; x<in.cols; ++x)
                    if (expected.at<uint8_t>(y,x)!=actual.at<uint8_t>(y,x)) {
                        std::fprintf(stderr,"Mismatch input=%u OpenCV mean=%u\n",
                                     in.at<uint8_t>(y,x),mean.at<uint8_t>(y,x));
                        throw;
                    }
                throw;
            }
            ++cases;
        }
    std::fprintf(stderr, "Validation: %zu scalar/OpenCV cases passed (including strides, isolated borders, tiny ROIs and rounding)\n", cases);
}
struct Samples {
    std::vector<double> total, cpu, compute, service, prepare, wait, finish, cycles;
    void add(double elapsed, double cpu_elapsed, const RoiThresholdTiming &t)
    {
        total.push_back(elapsed); cpu.push_back(cpu_elapsed);
        compute.push_back(t.remote_compute_us); service.push_back(t.remote_service_us);
        prepare.push_back(t.prepare_us); wait.push_back(t.wait_us); finish.push_back(t.finish_us);
        cycles.push_back(static_cast<double>(t.remote_cycles));
    }
    void print(const char *mode, int w, int h, int kernel)
    {
        std::printf("%s\t%d\t%d\t%d\t%zu\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.0f\n",
                    mode, w, h, kernel, total.size(), mean(total), percentile(total, .50),
                    percentile(total, .95), mean(cpu), mean(compute), mean(service),
                    mean(prepare), mean(wait), mean(finish), mean(cycles));
        std::fflush(stdout);
    }
};
int number(const char *s, int minimum, int maximum)
{
    char *end;
    long n = std::strtol(s, &end, 10);
    if (!*s || *end || n < minimum || n > maximum) throw std::runtime_error("invalid numeric argument");
    return static_cast<int>(n);
}
}
int main(int argc, char **argv)
{
    try {
        int iterations = 100, warmup = 5, kernel = 15, poll = 50;
        bool local = false, bare_metal = false;
        std::string input_path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--local-only") local = true;
            else if (arg == "--bare-metal") bare_metal = true;
            else if (arg == "--help") {
                std::puts("Usage: tinytag_threshold_bench [--local-only] [--input grayscale-image]\n"
                          "       [--iterations 100] [--warmup 5] [--kernel 15] [--poll-us 50]\n"
                          "       [--bare-metal] verifies and labels bare-metal firmware.\n"
                          "Full mode needs matching firmware. --poll-us 0 busy-polls.\n"
                          "Validation is outside timed regions; all remote outputs are checked.");
                return 0;
            } else {
                if (++i >= argc) throw std::runtime_error("missing argument");
                if (arg == "--iterations") iterations = number(argv[i], 1, 100000);
                else if (arg == "--warmup") warmup = number(argv[i], 0, 10000);
                else if (arg == "--kernel") kernel = number(argv[i], 1, TT_THRESHOLD_MAX_KERNEL);
                else if (arg == "--poll-us") poll = number(argv[i], 0, 100000);
                else if (arg == "--input") input_path = argv[i];
                else throw std::runtime_error("unknown option: " + arg);
            }
        }
        if (!(kernel & 1)) throw std::runtime_error("kernel must be odd");
        local_validation();
        cv::Mat image;
        if (!input_path.empty()) {
            image = cv::imread(input_path, cv::IMREAD_GRAYSCALE);
            if (image.empty()) throw std::runtime_error("cannot read input image");
        }
        std::unique_ptr<RoiThresholdOffload> remote;
        if (!local) remote.reset(new RoiThresholdOffload(poll));
        std::fprintf(stderr, "Benchmark: %d iterations, %d warmup, kernel %d, poll %d us, input %s; OpenCV threads %d\n",
                     iterations, warmup, kernel, poll, input_path.empty() ? "synthetic texture" : input_path.c_str(), cv::getNumThreads());
        std::puts("mode\twidth\theight\tkernel\tsamples\ttotal_mean_us\ttotal_p50_us\ttotal_p95_us\tlinux_cpu_us\tremote_compute_us\tremote_service_us\tprepare_us\twait_us\tfinish_us\tremote_cycles");
        if (remote) {
            Samples s;
            for (int i = -warmup; i < iterations; ++i) {
                RoiThresholdTiming t;
                double cpu = cpu_us();
                remote->nop(t);
                if (bare_metal != (t.firmware_id == TT_THRESHOLD_BARE_METAL_MAGIC))
                    throw std::runtime_error("firmware identity mismatch: use --bare-metal only with bare-metal FIP");
                if (i == -warmup || (warmup == 0 && i == 0))
                    std::fprintf(stderr, "Remote firmware=%s cache_control=0x%x prefetch_control=0x%x (zero: not reported by stock experiment)\n",
                                 bare_metal ? "bare-metal" : "FreeRTOS", t.cache_control, t.prefetch_control);
                if (i >= 0) s.add(t.total_us, cpu_us() - cpu, t);
            }
            s.print(bare_metal ? "bare-metal-nop" : "freertos-nop", 0, 0, 0);
        }
        const cv::Size sizes[] = {{32,32}, {64,64}, {128,128}, {200,200}, {320,240}, {640,360}, {1280,800}};
        for (const auto size : sizes) {
            if (!image.empty() && (size.width > image.cols || size.height > image.rows)) continue;
            cv::Mat in = image.empty() ? synthetic(size.width, size.height, 4) :
                image(cv::Rect((image.cols-size.width)/2, (image.rows-size.height)/2, size.width, size.height));
            cv::Mat expected, out;
            roi_threshold_opencv(in, expected, kernel, 3);
            for (int mode = 0; mode < (remote ? 3 : 2); ++mode) {
                Samples s;
                const char *name = mode == 0 ? "opencv" : mode == 1 ? "scalar-a53" : bare_metal ? "bare-metal" : "freertos";
                for (int i = -warmup; i < iterations; ++i) {
                    RoiThresholdTiming t;
                    const double cpu = cpu_us(), begin = wall_us();
                    if (mode == 0) roi_threshold_opencv(in, out, kernel, 3);
                    else if (mode == 1) {
                        if (!roi_threshold_scalar(in, out, kernel, 3)) throw std::runtime_error("scalar failed");
                    } else remote->run(in, out, kernel, 3, t);
                    const double elapsed = wall_us() - begin, used_cpu = cpu_us() - cpu;
                    same(expected, out, name);
                    if (i >= 0) s.add(elapsed, used_cpu, t);
                }
                s.print(name, size.width, size.height, kernel);
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "threshold benchmark FAILED: %s\n", e.what());
        return 1;
    }
}
