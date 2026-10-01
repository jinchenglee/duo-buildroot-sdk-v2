#include "roi_threshold_offload.h"
#include "../common/roi_threshold_protocol.h"
#include <cvi_sys.h>
#include <opencv2/imgproc.hpp>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <cerrno>

namespace {
double now_us()
{
    return std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t align64(uint32_t n) { return (n + 63u) & ~63u; }
/* Matches the stock 8-byte mailbox ABI on the supported 64-bit Linux builds. */
struct __attribute__((packed, aligned(8))) Command {
    uint8_t ip_id, command_and_block;
    uint16_t reserved;
    uint32_t param;
};
static_assert(sizeof(Command) == 8, "mailbox ABI");
constexpr unsigned kSystemIp = 6;
constexpr unsigned long kSendIoctl = _IOW('r', 1, unsigned long);
void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
bool supported(const cv::Mat &in, int kernel)
{
    return in.type() == CV_8UC1 && !in.empty() &&
           in.cols <= static_cast<int>(TT_THRESHOLD_MAX_WIDTH) &&
           in.rows <= static_cast<int>(TT_THRESHOLD_MAX_HEIGHT) &&
           kernel > 0 && (kernel & 1) && kernel <= static_cast<int>(TT_THRESHOLD_MAX_KERNEL);
}
}

void roi_threshold_opencv(const cv::Mat &input, cv::Mat &output, int kernel, int threshold)
{
    cv::boxFilter(input, output, input.type(), cv::Size(kernel, kernel),
                  cv::Point(-1, -1), true, cv::BORDER_REPLICATE | cv::BORDER_ISOLATED);
    output = output - input;
    cv::threshold(output, output, threshold, 255, cv::THRESH_BINARY);
}
bool roi_threshold_scalar(const cv::Mat &input, cv::Mat &output, int kernel, int threshold)
{
    if (!supported(input, kernel)) return false;
    output.create(input.size(), CV_8UC1);
    std::array<uint32_t, TT_THRESHOLD_MAX_WIDTH> columns;
    return tt_roi_threshold(input.ptr<uint8_t>(), input.step, output.ptr<uint8_t>(),
                            output.step, input.cols, input.rows, kernel, threshold,
                            columns.data()) == 0;
}

struct RoiThresholdOffload::Impl {
    int command_fd = -1, lock_fd = -1;
    CVI_U64 physical = 0;
    uint8_t *memory = nullptr;
    const uint32_t output_offset = TT_THRESHOLD_HEADER_BYTES +
        align64(TT_THRESHOLD_MAX_WIDTH * TT_THRESHOLD_MAX_HEIGHT);
    const uint32_t allocation_bytes = output_offset +
        align64(TT_THRESHOLD_MAX_WIDTH * TT_THRESHOLD_MAX_HEIGHT);
    unsigned poll_us, timeout_ms;
    uint32_t sequence = 0;
    bool pending = false, poisoned = false;

    Impl(unsigned poll, unsigned timeout) : poll_us(poll), timeout_ms(timeout) {}
    ~Impl()
    {
        if (memory && !pending)
            CVI_SYS_IonFree(physical, memory);
        else if (memory)
            std::fprintf(stderr, "[threshold-offload] pending job: retaining ION allocation at 0x%llx; reboot before retesting\n",
                         static_cast<unsigned long long>(physical));
        if (command_fd >= 0) close(command_fd);
        if (lock_fd >= 0) close(lock_fd);
    }
    void initialize()
    {
        require(timeout_ms > 0 && poll_us <= 100000, "invalid threshold wait settings");
        lock_fd = open("/tmp/tinytag-threshold.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        require(lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) == 0,
                "another threshold offload client is running, or lock unavailable");
        command_fd = open("/dev/cvi-rtos-cmdqu", O_RDWR | O_CLOEXEC);
        require(command_fd >= 0, "open /dev/cvi-rtos-cmdqu failed");
        void *virtual_address = nullptr;
        require(CVI_SYS_IonAlloc_Cached(&physical, &virtual_address, "tt-threshold",
                                       allocation_bytes) == CVI_SUCCESS && virtual_address,
                "threshold ION allocation failed");
        memory = static_cast<uint8_t *>(virtual_address);
        require(!(physical & 63) && physical + allocation_bytes <= UINT64_C(0x100000000),
                "threshold allocation outside 32-bit aligned mailbox address space");
        memset(memory, 0, allocation_bytes);
        require(CVI_SYS_IonFlushCache(physical, memory, allocation_bytes) == CVI_SUCCESS,
                "initial threshold cache flush failed");
    }
    void flush(uint32_t offset, uint32_t bytes)
    {
        require(CVI_SYS_IonFlushCache(physical + offset, memory + offset, bytes) == CVI_SUCCESS,
                "threshold cache flush failed");
    }
    void invalidate(uint32_t offset, uint32_t bytes)
    {
        require(CVI_SYS_IonInvalidateCache(physical + offset, memory + offset, bytes) == CVI_SUCCESS,
                "threshold cache invalidate failed");
    }
    void run(const cv::Mat *input, cv::Mat *output, int kernel, int threshold,
             RoiThresholdTiming &timing)
    {
        require(!poisoned && !pending, "threshold session unavailable after timeout/failure");
        require(!input || supported(*input, kernel), "unsupported threshold crop/kernel");
        timing = RoiThresholdTiming{};
        const double start = now_us();
        auto *job = reinterpret_cast<tt_threshold_job *>(memory);
        if (++sequence == 0) ++sequence;
        memset(job, 0, sizeof(*job));
        auto &request = job->request;
        request.magic = TT_THRESHOLD_MAGIC;
        request.version = TT_THRESHOLD_VERSION;
        request.sequence = sequence;
        request.operation = input ? TT_THRESHOLD_OP_COMPUTE : TT_THRESHOLD_OP_NOP;
        request.width = input ? input->cols : 0;
        request.height = input ? input->rows : 0;
        request.kernel = kernel;
        request.threshold = threshold;
        request.input_offset = TT_THRESHOLD_HEADER_BYTES;
        request.output_offset = output_offset;
        request.total_bytes = allocation_bytes;
        const uint32_t pixels = request.width * request.height;
        if (input) {
            output->create(input->size(), CV_8UC1);
            for (int y = 0; y < input->rows; ++y)
                memcpy(memory + request.input_offset + y * input->cols, input->ptr(y), input->cols);
            flush(request.input_offset, align64(pixels));
        }
        flush(0, TT_THRESHOLD_HEADER_BYTES);
        const double prepared = now_us();
        Command cmd{static_cast<uint8_t>(kSystemIp), TT_THRESHOLD_COMMAND, 0,
                    static_cast<uint32_t>(physical)};
        /* Once a send is attempted, conservatively keep ownership on failure:
         * an error does not prove firmware cannot still access this buffer. */
        pending = true;
        try {
            require(ioctl(command_fd, kSendIoctl, &cmd) == 0, "threshold mailbox send failed");
            for (;;) {
                invalidate(64, 64);
                if (__atomic_load_n(&job->completion.sequence, __ATOMIC_ACQUIRE) == sequence)
                    break;
                if (now_us() - prepared >= timeout_ms * 1000.0)
                    throw std::runtime_error("threshold completion timed out: install matching threshold FIP and reboot");
                if (poll_us) usleep(poll_us);
            }
        } catch (...) {
            poisoned = true;
            throw;
        }
        pending = false;
        const double received = now_us();
        const auto result = job->completion;
        require(result.magic == TT_THRESHOLD_MAGIC && result.version == TT_THRESHOLD_VERSION &&
                result.status == TT_THRESHOLD_OK && result.timer_hz,
                "invalid threshold firmware response");
        if (input) {
            invalidate(output_offset, align64(pixels));
            for (int y = 0; y < input->rows; ++y)
                memcpy(output->ptr(y), memory + output_offset + y * input->cols, input->cols);
        }
        timing.prepare_us = prepared - start;
        timing.wait_us = received - prepared;
        timing.finish_us = now_us() - received;
        timing.total_us = timing.prepare_us + timing.wait_us + timing.finish_us;
        timing.remote_compute_us = result.compute_ticks * (1e6 / result.timer_hz);
        timing.remote_service_us = result.service_ticks * (1e6 / result.timer_hz);
        timing.remote_cycles = result.compute_cycles;
        timing.firmware_id = result.reserved[0];
        timing.cache_control = result.reserved[1];
        timing.prefetch_control = result.reserved[2];
    }
};

RoiThresholdOffload::RoiThresholdOffload(unsigned poll_us, unsigned timeout_ms)
    : impl_(new Impl(poll_us, timeout_ms)) { impl_->initialize(); }
RoiThresholdOffload::~RoiThresholdOffload() = default;
void RoiThresholdOffload::run(const cv::Mat &in, cv::Mat &out, int k, int t, RoiThresholdTiming &timing)
{ impl_->run(&in, &out, k, t, timing); }
void RoiThresholdOffload::nop(RoiThresholdTiming &timing)
{ impl_->run(nullptr, nullptr, 1, 0, timing); }

namespace {
bool remote_hook(const cv::Mat &in, cv::Mat &out, int kernel, int threshold)
{
    if (!supported(in, kernel)) return false;
    static std::mutex mutex;
    static std::unique_ptr<RoiThresholdOffload> client;
    static bool disabled = false;
    static uint64_t jobs = 0;
    static double remote_us = 0, total_us = 0;
    std::lock_guard<std::mutex> lock(mutex);
    if (disabled) return false;
    try {
        if (!client) {
            const char *poll = getenv("TINYTAG_THRESHOLD_POLL_US");
            client.reset(new RoiThresholdOffload(poll ? std::stoul(poll) : 50));
            std::fprintf(stderr, "[threshold-offload] FreeRTOS backend enabled; full pipeline remains on Linux except ROI thresholding\n");
        }
        RoiThresholdTiming timing;
        client->run(in, out, kernel, threshold, timing);
        const char *verify = getenv("TINYTAG_THRESHOLD_VERIFY");
        if (!verify || strcmp(verify, "0") != 0) {
            cv::Mat reference;
            roi_threshold_opencv(in, reference, kernel, threshold);
            require(cv::countNonZero(reference != out) == 0, "threshold differs from OpenCV");
        }
        ++jobs;
        remote_us += timing.remote_compute_us;
        total_us += timing.total_us;
        if (jobs == 1 || jobs % 100 == 0)
            std::fprintf(stderr, "[threshold-offload] jobs %llu | mean compute %.3f ms | mean transfer+compute %.3f ms | verification %s\n",
                         static_cast<unsigned long long>(jobs), remote_us / jobs / 1000,
                         total_us / jobs / 1000, (!verify || strcmp(verify, "0")) ? "on" : "off");
        return true;
    } catch (const std::exception &e) {
        disabled = true;
        std::fprintf(stderr, "[threshold-offload] DISABLED: %s; using Linux OpenCV fallback\n", e.what());
        return false;
    }
}
}
RoiThresholdHook roi_threshold_runtime_hook()
{
    const char *backend = getenv("TINYTAG_THRESHOLD_BACKEND");
    if (!backend || strcmp(backend, "opencv") == 0) return nullptr;
    if (strcmp(backend, "scalar") == 0) return roi_threshold_scalar;
    if (strcmp(backend, "freertos") == 0) return remote_hook;
    throw std::runtime_error("TINYTAG_THRESHOLD_BACKEND must be opencv, scalar or freertos");
}
