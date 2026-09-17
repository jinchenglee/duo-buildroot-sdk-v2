#include <cvi_ive.h>
#include <cvi_sys.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kGuardBytes = 64;

double thread_cpu_us()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) / 1e3;
}

double wall_us()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::micro>(clock::now().time_since_epoch()).count();
}

double percentile(std::vector<double> samples, double fraction)
{
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t index = static_cast<size_t>(fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
}

struct IonBuffer
{
    CVI_U64 phy = 0;
    void *vir = nullptr;
    CVI_U32 len = 0;

    bool allocate(const char *name, CVI_U32 bytes, bool cached)
    {
        len = bytes;
        const CVI_S32 ret = cached
                                ? CVI_SYS_IonAlloc_Cached(&phy, &vir, name, bytes)
                                : CVI_SYS_IonAlloc(&phy, &vir, name, bytes);
        return ret == CVI_SUCCESS && vir != nullptr;
    }

    ~IonBuffer()
    {
        if (vir != nullptr)
            CVI_SYS_IonFree(phy, vir);
    }
};

struct Options
{
    CVI_U32 width = 1280;
    CVI_U32 height = 720;
    CVI_U32 stride = 0;
    int warmup = 5;
    int iterations = 100;
    bool cached = false;
    bool instant = false;
    bool sweep = false;
};

bool parse_u32(const char *text, CVI_U32 &value)
{
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 0);
    if (end == text || *end != '\0' || parsed > 0xffffffffUL)
        return false;
    value = static_cast<CVI_U32>(parsed);
    return true;
}

void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "Usage: %s [--width N --height N] [--stride N] [--iterations N]\n"
                 "          [--warmup N] [--cached 0|1] [--instant 0|1] [--sweep]\n"
                 "\n"
                 "bInstant=1 busy-polls; bInstant=0 waits for the completion IRQ.\n"
                 "Enable kernel timing separately with:\n"
                 "  echo 1 > /proc/ive/hw_profiling\n",
                 argv0);
}

bool validate(const uint8_t *src, const uint8_t *dst, CVI_U32 width,
              CVI_U32 height, CVI_U32 stride, CVI_U32 bytes)
{
    for (CVI_U32 y = 0; y < height; ++y)
    {
        const size_t row = static_cast<size_t>(y) * stride;
        if (std::memcmp(src + row, dst + row, width) != 0)
        {
            std::fprintf(stderr, "validation: copied bytes differ at row %u\n", y);
            return false;
        }
        for (CVI_U32 x = width; x < stride; ++x)
        {
            if (dst[row + x] != 0xa5)
            {
                std::fprintf(stderr, "validation: destination padding changed at (%u,%u)\n", x, y);
                return false;
            }
        }
    }
    const CVI_U32 guard_start = bytes - static_cast<CVI_U32>(kGuardBytes);
    for (CVI_U32 i = guard_start; i < bytes; ++i)
    {
        if (dst[i] != 0xa5)
        {
            std::fprintf(stderr, "validation: destination guard changed at byte %u\n", i);
            return false;
        }
    }
    return true;
}

bool run_case(IVE_HANDLE handle, CVI_U32 width, CVI_U32 height, CVI_U32 requested_stride,
              int warmup, int iterations, bool cached, bool instant)
{
    const CVI_U32 stride = requested_stride ? requested_stride : ((width + 15u) & ~15u);
    if (width < 32 || height == 0 || width > 1920 || height > 1080 || stride < width ||
        warmup < 0 || iterations <= 0)
    {
        std::fprintf(stderr, "invalid case: %ux%u stride %u warmup %d iterations %d\n",
                     width, height, stride, warmup, iterations);
        return false;
    }
    const uint64_t payload64 = static_cast<uint64_t>(stride) * height;
    if (payload64 + kGuardBytes > 0xffffffffULL)
        return false;
    const CVI_U32 allocation = static_cast<CVI_U32>(payload64 + kGuardBytes);

    IonBuffer src, dst;
    if (!src.allocate("ive_bench_src", allocation, cached) ||
        !dst.allocate("ive_bench_dst", allocation, cached))
    {
        std::fprintf(stderr, "ION allocation failed for %u bytes x2\n", allocation);
        return false;
    }

    uint8_t *src_bytes = static_cast<uint8_t *>(src.vir);
    uint8_t *dst_bytes = static_cast<uint8_t *>(dst.vir);
    std::memset(src_bytes, 0x5a, allocation);
    std::memset(dst_bytes, 0xa5, allocation);
    for (CVI_U32 y = 0; y < height; ++y)
        for (CVI_U32 x = 0; x < width; ++x)
            src_bytes[static_cast<size_t>(y) * stride + x] =
                static_cast<uint8_t>((x * 17u + y * 31u + (x >> 3)) & 0xffu);

    double flush_us = 0.0;
    if (cached)
    {
        const double start = wall_us();
        CVI_SYS_IonFlushCache(src.phy, src.vir, allocation);
        CVI_SYS_IonFlushCache(dst.phy, dst.vir, allocation);
        flush_us = wall_us() - start;
    }

    IVE_DATA_S src_data{};
    src_data.u64PhyAddr = src.phy;
    src_data.u64VirAddr = reinterpret_cast<CVI_U64>(src.vir);
    src_data.u32Stride = stride;
    src_data.u32Width = width;
    src_data.u32Height = height;
    IVE_DST_DATA_S dst_data{};
    dst_data.u64PhyAddr = dst.phy;
    dst_data.u64VirAddr = reinterpret_cast<CVI_U64>(dst.vir);
    dst_data.u32Stride = stride;
    dst_data.u32Width = width;
    dst_data.u32Height = height;
    IVE_DMA_CTRL_S control{};
    control.enMode = IVE_DMA_MODE_DIRECT_COPY;

    for (int i = 0; i < warmup; ++i)
    {
        const CVI_S32 ret = CVI_IVE_DMA(
            handle, &src_data, &dst_data, &control, instant ? CVI_TRUE : CVI_FALSE);
        if (ret != CVI_SUCCESS)
        {
            std::fprintf(stderr, "CVI_IVE_DMA warm-up failed: %#x\n", ret);
            return false;
        }
    }

    std::vector<double> wall_samples, cpu_samples;
    wall_samples.reserve(static_cast<size_t>(iterations));
    cpu_samples.reserve(static_cast<size_t>(iterations));
    for (int i = 0; i < iterations; ++i)
    {
        const double wall_start = wall_us();
        const double cpu_start = thread_cpu_us();
        const CVI_S32 ret = CVI_IVE_DMA(
            handle, &src_data, &dst_data, &control, instant ? CVI_TRUE : CVI_FALSE);
        const double cpu_end = thread_cpu_us();
        const double wall_end = wall_us();
        if (ret != CVI_SUCCESS)
        {
            std::fprintf(stderr, "CVI_IVE_DMA iteration %d failed: %#x\n", i, ret);
            return false;
        }
        wall_samples.push_back(wall_end - wall_start);
        cpu_samples.push_back(cpu_end - cpu_start);
    }

    double invalidate_us = 0.0;
    if (cached)
    {
        const double start = wall_us();
        CVI_SYS_IonInvalidateCache(dst.phy, dst.vir, allocation);
        invalidate_us = wall_us() - start;
    }
    const bool valid = validate(src_bytes, dst_bytes, width, height, stride, allocation);
    const double p50 = percentile(wall_samples, 0.50);
    const double p95 = percentile(wall_samples, 0.95);
    const double p99 = percentile(wall_samples, 0.99);
    const double cpu50 = percentile(cpu_samples, 0.50);
    const double payload_mb = static_cast<double>(width) * height / 1e6;

    std::printf("dma width=%u height=%u stride=%u bytes=%u cached=%d instant=%d "
                "n=%d wall_us[p50=%.1f p95=%.1f p99=%.1f max=%.1f] "
                "cpu_us[p50=%.1f] payload_MBps=%.1f cache_us[flush=%.1f invalidate=%.1f] "
                "valid=%d\n",
                width, height, stride, width * height, cached ? 1 : 0, instant ? 1 : 0,
                iterations, p50, p95, p99,
                *std::max_element(wall_samples.begin(), wall_samples.end()), cpu50,
                p50 > 0.0 ? payload_mb * 1e6 / p50 : 0.0, flush_us, invalidate_us,
                valid ? 1 : 0);
    return valid;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--width" && has_value && parse_u32(argv[++i], options.width)) {}
        else if (flag == "--height" && has_value && parse_u32(argv[++i], options.height)) {}
        else if (flag == "--stride" && has_value && parse_u32(argv[++i], options.stride)) {}
        else if (flag == "--iterations" && has_value) options.iterations = std::atoi(argv[++i]);
        else if (flag == "--warmup" && has_value) options.warmup = std::atoi(argv[++i]);
        else if (flag == "--cached" && has_value) options.cached = std::atoi(argv[++i]) != 0;
        else if (flag == "--instant" && has_value) options.instant = std::atoi(argv[++i]) != 0;
        else if (flag == "--sweep") options.sweep = true;
        else
        {
            usage(argv[0]);
            return 2;
        }
    }

    IVE_HANDLE handle = CVI_IVE_CreateHandle();
    if (handle == nullptr)
    {
        std::fprintf(stderr, "CVI_IVE_CreateHandle returned null\n");
        return 1;
    }

    bool ok = true;
    if (options.sweep)
    {
        const CVI_U32 sizes[][2] = {{32, 1}, {320, 180}, {640, 360},
                                    {1280, 720}, {1920, 1080}};
        for (const auto &size : sizes)
            ok = run_case(handle, size[0], size[1], 0, options.warmup,
                          options.iterations, options.cached, options.instant) && ok;
    }
    else
    {
        ok = run_case(handle, options.width, options.height, options.stride, options.warmup,
                      options.iterations, options.cached, options.instant);
    }
    CVI_IVE_DestroyHandle(handle);
    return ok ? 0 : 1;
}
