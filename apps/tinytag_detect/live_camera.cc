// Live-camera capture for TinyTag, using VI -> VPSS (the SG2000 MPI stack)
// instead of V4L2 -- this SoC has no V4L2 camera path at all; see the
// "No /dev/video*" note this app's README doesn't yet have but the repo's
// chat history does: capture, ISP, and encode go through Sophgo's proprietary
// MPI API (cvi_mpi/) via dedicated /dev/cvi-* nodes.
//
// Mirrors the K230 reference port's design 1:1 (buildroot-overlay/package/
// ai_demo/tinytag_detect/main.cc's ai_proc in that SDK): feed
// TinyTagDet::pre_process() a live grayscale plane straight from the capture
// buffer, zero-copy, and let its existing crop-to-band + resize (unchanged,
// same code the file-based CLI path uses) do the rest. The one thing this
// port does *better* than K230's "slice the Y-plane out of an NV12 buffer"
// trick: VPSS can be told to output PIXEL_FORMAT_YUV_400 directly -- pure
// hardware grayscale, no chroma plane ever computed, let alone copied.
//
// Deliberately does NOT reuse tdl_sdk/sample_video/middleware_utils.c's
// SAMPLE_TDL_Init_WM for VI/VPSS bring-up: that unconditionally creates a
// VENC+RTSP session, unwanted overhead for a "low-latency wins" detection
// loop. Below is the subset of Init_WM's sequence that only touches
// SYS/VB/VI/VPSS, using nothing but cvi_mpi's own libraries -- no cvi_tdl,
// no cvi_rtsp.
// core/utils/vpss_helper.h (from tdl_sdk) is header-only (every function is
// `static inline`), so including it for VPSS_INIT_HELPER2 et al. costs a
// compile-time include, not a link dependency on libcvi_tdl.so.
//
// No RTSP/display here: a single-threaded capture -> detect loop, so nothing
// competes with the detector for VI/VPSS/TPU access or CPU time. A visual
// feed (VPSS Chn1 + VENC + RTSP, bound to the same VI source) can be added
// later as a second, independent channel without touching this loop -- see
// the design discussion this file's commit message links to.
//
// Verified on a Duo S with an OV5647 on J2: 30 fps (sensor-limited), ~11 ms
// busy per frame with crop-decode on, ~6 ms neural-only.

#include "tag_crop_decoder.h"
#include "tinytag_det.h"

extern "C" {
#include <core/utils/vpss_helper.h>
#include <cvi_ae.h>
#include <cvi_awb.h>
#include <cvi_comm.h>
#include <cvi_isp.h>
#include <cvi_vi.h>
#include <rtsp.h>
#include <sample_comm.h>
}

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <pthread.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <csignal>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
void handle_signal(int) { g_stop = 1; }

constexpr VPSS_GRP kVpssGrp = 0;
constexpr VPSS_CHN kVpssChn = 0;
constexpr VPSS_CHN kModelChn = 1;

// VPSS scales the full sensor frame to this in hardware before the detector
// sees it. 1280x720 is exactly TinyTagDet's crop band, so pre_process() uses
// it as-is (no crop) and only does its 2x resize to the 640x360 network
// input -- the detector covers the whole field of view instead of the
// bottom-left 1280x720 of a 1080p frame. Tags come out 2/3 the pixel size
// they would in the cropped view, so very small/distant ones lose some recall.
constexpr CVI_U32 kDetWidth = 1280;
constexpr CVI_U32 kDetHeight = 720;

// --rtsp preview: a second channel on the same VPSS group, NV21 at the
// detector's resolution so boxes need no coordinate scaling, encoded in
// hardware (VENC) and served with cvi_rtsp -- the same path the SDK's
// camera-test.sh / sample_vi_fd use.
constexpr VB_POOL kDetPool = 1;
constexpr VB_POOL kModelPool = 2;
constexpr VENC_CHN kVencChn = 0;
constexpr int kPreviewBitrateKbps = 3000;
constexpr int kVencTimeoutMs = 2000;

enum RtspSendFailure : unsigned
{
    RTSP_SEND_OK = 0,
    RTSP_SEND_SUBMIT = 1u << 0,
    RTSP_SEND_NO_PACKS = 1u << 1,
    RTSP_SEND_PACK_OVERFLOW = 1u << 2,
    RTSP_SEND_GET = 1u << 3,
    RTSP_SEND_WRITE = 1u << 4,
    RTSP_SEND_RELEASE = 1u << 5,
};

struct RtspSendResult
{
    unsigned failures = RTSP_SEND_OK;
    size_t get_timeouts = 0;
    double venc_ms = 0.0;
    double rtsp_ms = 0.0;
};

enum class PreviewSurfaceState : uint8_t
{
    FREE,
    FILLING,
    QUEUED,
    IN_WORKER,
};

// A preview surface owns an NV21 frame the detector copies luma into. Its state
// is protected by LumaRtspQueue::mutex: the detector may write only FILLING,
// and the worker may draw/encode only IN_WORKER. Allocated once at init;
// nothing here is allocated or freed in the frame loop.
//
// The scene stays monochrome -- luma is a byte-exact copy of the plane the
// detector ran on -- while the chroma plane starts neutral and is written only
// where the overlay draws, so boxes and text render in colour over a grey
// image. `dirty` records the chroma rectangles written last time round so they
// can be reset to neutral without touching the whole plane.
constexpr size_t kPreviewSurfaceCount = 2;
struct PreviewSurface
{
    CVI_U64 c_phy = 0;
    CVI_VOID *c_vir = nullptr;
    CVI_U32 c_len = 0;
    CVI_U32 c_stride = 0;
    std::vector<cv::Rect> dirty;
    PreviewSurfaceState state = PreviewSurfaceState::FREE;
    size_t index = 0;
};

// Single-frame "latest value" slot between the capture thread and the
// detector. The capture thread blocks on GetChnFrame and always overwrites the
// slot, releasing whatever it displaces, so the detector never polls, never
// drains a queue, and always picks up the freshest frame the camera has
// produced -- at most one capture period old, whatever the capture rate.
//
// Faster capture therefore lowers latency rather than building a backlog: at
// 130 fps the slot refreshes every 7.7 ms, so a 20 ms detection starts on a
// frame at most 7.7 ms stale instead of 33 ms.
struct CaptureSlot
{
    std::mutex mutex;
    std::condition_variable not_empty;
    VIDEO_FRAME_INFO_S frame{};
    double ready_ms = 0.0;
    bool full = false;
    bool stopping = false;
    size_t dropped = 0; // superseded before the detector could take them
};

struct LumaRtspItem
{
    // Ownership of both the VPSS frame and its existing cached Y mapping moves
    // to this item until encode completes or the item is superseded.
    VIDEO_FRAME_INFO_S source{};
    uint8_t *source_y_vir = nullptr;
    CVI_U32 source_map_len = 0;
    VIDEO_FRAME_INFO_S encoded{};
    // Owns the neutral chroma plane paired with the borrowed Y plane.
    PreviewSurface *surface = nullptr;
    std::vector<Proposal> proposals;
    std::vector<TinyTagResult> tags;
    std::vector<cv::Rect> crops;
    double fps = 0.0;
    double busy_ms = 0.0;
    CVI_U32 sequence = 0;
    double queued_ms = 0.0;
};

struct LumaRtspQueue
{
    std::mutex mutex;
    std::condition_variable not_empty;
    // A latest-value slot, not a FIFO. A new publication supersedes only this
    // pending item; it can never touch the item already owned by the worker.
    LumaRtspItem pending{};
    bool full = false;
    bool stopping = false;

    size_t published = 0;
    size_t superseded = 0;
    size_t no_surface = 0;
    size_t dequeued = 0;
    size_t encoded = 0;
    size_t encode_failed = 0;
    size_t submit_failed = 0;
    size_t no_packs = 0;
    size_t pack_overflow = 0;
    size_t get_failed = 0;
    size_t get_timeouts = 0;
    size_t rtsp_failed = 0;
    size_t release_failed = 0;
    size_t sequence_skipped = 0;
    size_t ownership_errors = 0;
    size_t borrowed_frames = 0;
    size_t borrowed_frames_max = 0;
    CVI_U32 sequence_gap_max = 0;
    CVI_U32 previous_sequence = 0;
    bool have_previous_sequence = false;
    double queue_age_sum_ms = 0.0;
    double queue_age_max_ms = 0.0;
    double venc_sum_ms = 0.0;
    double venc_max_ms = 0.0;
    double rtsp_sum_ms = 0.0;
    double rtsp_max_ms = 0.0;
};

struct LumaRtspStats
{
    size_t published = 0;
    size_t superseded = 0;
    size_t no_surface = 0;
    size_t dequeued = 0;
    size_t encoded = 0;
    size_t encode_failed = 0;
    size_t submit_failed = 0;
    size_t no_packs = 0;
    size_t pack_overflow = 0;
    size_t get_failed = 0;
    size_t get_timeouts = 0;
    size_t rtsp_failed = 0;
    size_t release_failed = 0;
    size_t sequence_skipped = 0;
    size_t ownership_errors = 0;
    size_t borrowed_frames = 0;
    size_t borrowed_frames_max = 0;
    CVI_U32 sequence_gap_max = 0;
    double queue_age_sum_ms = 0.0;
    double queue_age_max_ms = 0.0;
    double venc_sum_ms = 0.0;
    double venc_max_ms = 0.0;
    double rtsp_sum_ms = 0.0;
    double rtsp_max_ms = 0.0;
    bool pending = false;
};

struct CachedLumaMapping
{
    CVI_U64 phy = 0;
    uint8_t *vir = nullptr;
    CVI_U32 len = 0;
};

// Latest detections, published by the detector loop and drawn by the preview
// thread. The preview draws whatever is newest, so boxes can trail the video
// by up to one detection frame -- the same trade sample_vi_fd makes.
struct Overlay
{
    std::mutex mutex;
    std::vector<Proposal> proposals;
    std::vector<TinyTagResult> tags;
    std::vector<cv::Rect> crops;
    double fps = 0.0;
    double busy_ms = 0.0;
} g_overlay;

double now_ms()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

double thread_cpu_ms()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1e6;
}

struct TailSummary
{
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
};

TailSummary summarize_tail(std::vector<double> samples)
{
    TailSummary out;
    if (samples.empty())
        return out;
    std::sort(samples.begin(), samples.end());
    auto percentile = [&samples](double p) {
        const size_t rank = static_cast<size_t>(std::ceil(p * samples.size()));
        return samples[std::min(samples.size() - 1, std::max<size_t>(1, rank) - 1)];
    };
    out.p50 = percentile(0.50);
    out.p95 = percentile(0.95);
    out.p99 = percentile(0.99);
    out.max = samples.back();
    return out;
}

void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s <cvimodel> [--thres f] [--max n] [--expand f] [--iou f]\n"
            "       [--decode strict|tolerant] [--debug n] [--save-frame frame.png] [--rtsp]\n"
            "       [--mirror 0|1] [--flip 0|1] [--crop-align N] [--tag-output 0|1]\n"
            "       [--direct-compact-input 0|1] [--validate-compact-input 0|1]\n"
            "\n"
            "  --mirror 1  correct a horizontally mirrored sensor. Mirrored frames decode\n"
            "              ZERO tags (AprilTag markers are chiral) while proposals still\n"
            "              look correct. Applied in VI hardware: no per-frame cost.\n"
            "  --flip 1    same, vertically.\n"
            "  --crop-align N  widen each decode crop horizontally to a multiple of N\n"
            "              pixels (default 4, so every crop row starts 4-byte aligned\n"
            "              and is a whole number of 32-bit words). 0 or 1 disables.\n"
            "              Aligned regions are drawn in pink on the preview.\n"
            "  --tag-output 1  print every decoded tag to stdout (default 1). Set 0\n"
            "              when stdout is not a required result transport; synchronous\n"
            "              output can otherwise stall capture behind a slow consumer.\n"
            "  --direct-compact-input 1  experimental: bind a tightly packed VPSS\n"
            "              plane directly to an ordinary compact model (default 0).\n"
            "              Exact dimensions, stride, format and tensor size are enforced.\n"
            "  --validate-compact-input 1  once, compare copied and direct input outputs\n"
            "              bit-for-bit before continuing (default 0; diagnostic only).\n"

            "       [--rtsp-luma]  (RTSP preview as detector grayscale/luma)\n",
            argv0);
}

// --- live ISP control (this SoC's answer to `v4l2-ctl --set-ctrl`) --------
// There is no V4L2 here, so no V4L2 controls either -- the equivalent knobs
// (manual gain, exposure time, white balance) live in the ISP driver, set
// via CVI_ISP_Set*Attr on VI pipe 0, the same pipe this process owns. These
// only work called from a process that already owns the pipe (i.e. this one,
// after setup_camera() succeeds) -- Sophgo's own answer for adjusting a pipe
// you *don't* own is isp_tool_daemon (already on the board), which pairs
// with their separate desktop "ISP Tuning Tool" GUI over the network; that
// tool owns the pipe itself, so it can't run alongside this one.
//
// Reads simple commands from stdin so you can type `gain 800` while the
// detector is running and watch [tag] output react, instead of having to
// relaunch. Runs in its own thread since main() is busy in the capture loop.
void print_isp_help()
{
    fprintf(stderr,
            "[isp] commands (stdin):\n"
            "  gain <value>       manual analog gain (0x400=1x, up to 0x7FFFFFFF)\n"
            "  exptime <us>       manual exposure time, microseconds\n"
            "  ae auto            revert exposure+gain to automatic\n"
            "  awb <r> <g> <b>    manual white balance gains (0x1-0x3FFF each)\n"
            "  awb auto           revert white balance to automatic\n"
            "  help               show this message\n");
}

void isp_set_manual_gain(CVI_U32 gain)
{
    ISP_EXPOSURE_ATTR_S attr;
    if (CVI_ISP_GetExposureAttr(0, &attr) != CVI_SUCCESS)
    {
        fprintf(stderr, "[isp] GetExposureAttr failed\n");
        return;
    }
    attr.enOpType = OP_TYPE_MANUAL;
    attr.stManual.enAGainOpType = OP_TYPE_MANUAL;
    attr.stManual.u32AGain = gain;
    if (CVI_ISP_SetExposureAttr(0, &attr) != CVI_SUCCESS)
        fprintf(stderr, "[isp] SetExposureAttr failed\n");
    else
        fprintf(stderr, "[isp] gain set to %u\n", gain);
}

void isp_set_manual_exptime(CVI_U32 us)
{
    ISP_EXPOSURE_ATTR_S attr;
    if (CVI_ISP_GetExposureAttr(0, &attr) != CVI_SUCCESS)
    {
        fprintf(stderr, "[isp] GetExposureAttr failed\n");
        return;
    }
    attr.enOpType = OP_TYPE_MANUAL;
    attr.stManual.enExpTimeOpType = OP_TYPE_MANUAL;
    attr.stManual.u32ExpTime = us;
    if (CVI_ISP_SetExposureAttr(0, &attr) != CVI_SUCCESS)
        fprintf(stderr, "[isp] SetExposureAttr failed\n");
    else
        fprintf(stderr, "[isp] exposure time set to %u us\n", us);
}

void isp_set_ae_auto()
{
    ISP_EXPOSURE_ATTR_S attr;
    if (CVI_ISP_GetExposureAttr(0, &attr) != CVI_SUCCESS)
    {
        fprintf(stderr, "[isp] GetExposureAttr failed\n");
        return;
    }
    attr.enOpType = OP_TYPE_AUTO;
    attr.stManual.enAGainOpType = OP_TYPE_AUTO;
    attr.stManual.enExpTimeOpType = OP_TYPE_AUTO;
    if (CVI_ISP_SetExposureAttr(0, &attr) != CVI_SUCCESS)
        fprintf(stderr, "[isp] SetExposureAttr failed\n");
    else
        fprintf(stderr, "[isp] exposure/gain back to auto\n");
}

void isp_set_manual_wb(CVI_U16 r, CVI_U16 g, CVI_U16 b)
{
    ISP_WB_ATTR_S attr;
    if (CVI_ISP_GetWBAttr(0, &attr) != CVI_SUCCESS)
    {
        fprintf(stderr, "[isp] GetWBAttr failed\n");
        return;
    }
    attr.enOpType = OP_TYPE_MANUAL;
    attr.stManual.u16Rgain = r;
    attr.stManual.u16Grgain = g;
    attr.stManual.u16Gbgain = g;
    attr.stManual.u16Bgain = b;
    if (CVI_ISP_SetWBAttr(0, &attr) != CVI_SUCCESS)
        fprintf(stderr, "[isp] SetWBAttr failed\n");
    else
        fprintf(stderr, "[isp] white balance set to r=%u g=%u b=%u\n", r, g, b);
}

void isp_set_awb_auto()
{
    ISP_WB_ATTR_S attr;
    if (CVI_ISP_GetWBAttr(0, &attr) != CVI_SUCCESS)
    {
        fprintf(stderr, "[isp] GetWBAttr failed\n");
        return;
    }
    attr.enOpType = OP_TYPE_AUTO;
    if (CVI_ISP_SetWBAttr(0, &attr) != CVI_SUCCESS)
        fprintf(stderr, "[isp] SetWBAttr failed\n");
    else
        fprintf(stderr, "[isp] white balance back to auto\n");
}

void isp_control_loop()
{
    print_isp_help();
    char line[256];
    while (!g_stop)
    {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = poll(&input, 1, 200);
        if (ready < 0)
            continue;
        if (ready == 0)
            continue;
        if (!(input.revents & POLLIN) || fgets(line, sizeof(line), stdin) == nullptr)
            break;

        char cmd[32] = {0};
        if (sscanf(line, "%31s", cmd) != 1)
            continue;

        if (strcmp(cmd, "gain") == 0)
        {
            unsigned long v;
            if (sscanf(line, "%*s %lu", &v) == 1) isp_set_manual_gain((CVI_U32)v);
            else fprintf(stderr, "[isp] usage: gain <value>\n");
        }
        else if (strcmp(cmd, "exptime") == 0)
        {
            unsigned long v;
            if (sscanf(line, "%*s %lu", &v) == 1) isp_set_manual_exptime((CVI_U32)v);
            else fprintf(stderr, "[isp] usage: exptime <microseconds>\n");
        }
        else if (strcmp(cmd, "ae") == 0)
        {
            char mode[16];
            if (sscanf(line, "%*s %15s", mode) == 1 && strcmp(mode, "auto") == 0) isp_set_ae_auto();
            else fprintf(stderr, "[isp] usage: ae auto\n");
        }
        else if (strcmp(cmd, "awb") == 0)
        {
            char mode[16];
            unsigned long r, g, b;
            if (sscanf(line, "%*s %15s", mode) == 1 && strcmp(mode, "auto") == 0)
                isp_set_awb_auto();
            else if (sscanf(line, "%*s %lu %lu %lu", &r, &g, &b) == 3)
                isp_set_manual_wb((CVI_U16)r, (CVI_U16)g, (CVI_U16)b);
            else
                fprintf(stderr, "[isp] usage: awb <r> <g> <b> | awb auto\n");
        }
        else if (strcmp(cmd, "help") == 0)
        {
            print_isp_help();
        }
        else
        {
            fprintf(stderr, "[isp] unknown command: %s (type 'help')\n", cmd);
        }
    }
}

// Everything TeardownCamera() needs. Populated by SetupCamera().
struct CameraContext
{
    SAMPLE_VI_CONFIG_S vi_config{};
    uint32_t width = 0;
    uint32_t height = 0;
    bool preview = false;
    bool preview_luma = false;
    bool direct_model_input = false;
    bool compact_direct_input = false;
    bool validate_compact_input = false;
    bool vi_online = false;
    bool sys_initialized = false;
    bool vi_initialized = false;
    bool vpss_created = false;
    bool vi_vpss_bound = false;
    bool venc_started = false;
    bool rtsp_started = false;
    VPSS_CHN preview_chn = 1;
    VB_POOL preview_pool = 2;
    size_t preview_item_capacity = 0;
    unsigned preview_delay_ms = 0; // test-only worker delay for backlog stress
    // Applied once to the VI channel at setup, so the capture hardware
    // delivers corrected pixels and the per-frame cost is zero. A software
    // cv::flip would cost a full pass over the ~900 KB luma plane every frame.
    bool mirror = false;
    bool flip = false;
    CaptureSlot capture;
    std::thread capture_worker;
    CaptureSlot model_capture;
    std::thread model_capture_worker;
    std::atomic<size_t> pair_mismatches{0};
    // The detector VPSS channel has a fixed five-block pool. Keep one cached
    // virtual mapping per physical block and invalidate it on each reuse,
    // instead of mmap/munmap on every frame.
    std::vector<CachedLumaMapping> luma_mappings;
    size_t luma_mapping_hits = 0;
    size_t luma_mapping_misses = 0;
    PreviewSurface preview_surfaces[kPreviewSurfaceCount];
    std::vector<VENC_PACK_S> venc_packs;
    LumaRtspQueue luma_rtsp_queue;
    std::thread luma_rtsp_worker;
    CVI_RTSP_CTX *rtsp = nullptr;
    CVI_RTSP_SESSION *session = nullptr;
};

uint8_t *map_luma_for_cpu(CameraContext &ctx, CVI_U64 phy, CVI_U32 len)
{
    for (auto &mapping : ctx.luma_mappings)
    {
        if (mapping.phy != phy)
            continue;
        if (len > mapping.len)
        {
            fprintf(stderr, "[camera] VPSS block %#llx grew from %u to %u bytes\n",
                    (unsigned long long)phy, mapping.len, len);
            return nullptr;
        }
        // VPSS has reused this block via DMA since the last CPU access.
        if (CVI_SYS_IonInvalidateCache(phy, mapping.vir, len) != CVI_SUCCESS)
        {
            fprintf(stderr, "[camera] cache invalidate failed for %#llx (%u bytes)\n",
                    (unsigned long long)phy, len);
            return nullptr;
        }
        ++ctx.luma_mapping_hits;
        return mapping.vir;
    }

    // CVI_SYS_MmapCache performs the initial invalidation internally.
    uint8_t *vir = static_cast<uint8_t *>(CVI_SYS_MmapCache(phy, len));
    if (vir == nullptr)
        return nullptr;
    CachedLumaMapping mapping;
    mapping.phy = phy;
    mapping.vir = vir;
    mapping.len = len;
    ctx.luma_mappings.push_back(mapping);
    ++ctx.luma_mapping_misses;
    return vir;
}

void unmap_cached_luma(CameraContext &ctx)
{
    for (auto &mapping : ctx.luma_mappings)
    {
        if (mapping.vir != nullptr)
            CVI_SYS_Munmap(mapping.vir, mapping.len);
    }
    ctx.luma_mappings.clear();
}

void on_rtsp_connect(const char *ip, void *) { fprintf(stderr, "[rtsp] client connected from %s\n", ip); }
void on_rtsp_disconnect(const char *ip, void *) { fprintf(stderr, "[rtsp] client disconnected from %s\n", ip); }

// VENC channel + RTSP server for the preview. Encoder parameters mirror
// tdl_sdk/sample_video/middleware_utils.c (SAMPLE_TDL_Get_Input_Config +
// the VENC/RTSP half of SAMPLE_TDL_Init_WM), with a lower bitrate: 720p
// over the USB/Ethernet link doesn't need the 8 Mbps that sample uses, and
// sending less costs less of this board's single CPU core.
bool start_preview_stream(CameraContext &ctx)
{
    chnInputCfg ic{};
    strcpy(ic.codec, "h264");
    ic.initialDelay = CVI_INITIAL_DELAY_DEFAULT;
    ic.width = kDetWidth;
    ic.height = kDetHeight;
    ic.vpssGrp = kVpssGrp;
    ic.vpssChn = ctx.preview_luma ? kVpssChn : ctx.preview_chn;
    ic.num_frames = -1;
    ic.bsMode = 0;
    ic.rcMode = SAMPLE_RC_CBR;
    ic.iqp = DEF_IQP;
    ic.pqp = DEF_PQP;
    ic.gop = DEF_264_GOP;
    ic.maxIprop = CVI_H26X_MAX_I_PROP_DEFAULT;
    ic.minIprop = CVI_H26X_MIN_I_PROP_DEFAULT;
    ic.bitrate = kPreviewBitrateKbps;
    ic.firstFrmstartQp = 30;
    ic.minIqp = DEF_264_MINIQP;
    ic.maxIqp = DEF_264_MAXIQP;
    ic.minQp = DEF_264_MINQP;
    ic.maxQp = DEF_264_MAXQP;
    ic.srcFramerate = 30;
    ic.framerate = 30;
    ic.bVariFpsEn = 0;
    ic.maxbitrate = -1;
    ic.statTime = -1;
    ic.chgNum = -1;
    ic.quality = -1;
    ic.pixel_format = 0;
    ic.bitstreamBufSize = 0;
    ic.single_LumaBuf = 0;
    ic.single_core = 0;
    ic.forceIdr = -1;
    ic.tempLayer = 0;
    ic.testRoi = 0;
    ic.bgInterval = 0;

    VENC_GOP_ATTR_S gop{};
    if (SAMPLE_COMM_VENC_GetGopAttr(VENC_GOPMODE_NORMALP, &gop) != CVI_SUCCESS ||
        SAMPLE_COMM_VENC_Start(&ic, kVencChn, PT_H264, PIC_720P, SAMPLE_RC_CBR, 0, CVI_FALSE, &gop) !=
            CVI_SUCCESS)
    {
        fprintf(stderr, "[rtsp] VENC start failed\n");
        return false;
    }
    ctx.venc_started = true;

    CVI_RTSP_CONFIG rtsp_config{};
    rtsp_config.port = 554;
    if (CVI_RTSP_Create(&ctx.rtsp, &rtsp_config) < 0)
    {
        fprintf(stderr, "[rtsp] cannot create RTSP server\n");
        return false;
    }
    CVI_RTSP_SESSION_ATTR attr{};
    attr.video.codec = RTSP_VIDEO_H264;
    snprintf(attr.name, sizeof(attr.name), "h264");
    if (CVI_RTSP_CreateSession(ctx.rtsp, &attr, &ctx.session) < 0)
    {
        fprintf(stderr, "[rtsp] cannot create RTSP session\n");
        return false;
    }

    CVI_RTSP_STATE_LISTENER listener{};
    listener.onConnect = on_rtsp_connect;
    listener.onDisconnect = on_rtsp_disconnect;
    CVI_RTSP_SetListener(ctx.rtsp, &listener);

    if (CVI_RTSP_Start(ctx.rtsp) < 0)
    {
        fprintf(stderr, "[rtsp] cannot start RTSP server\n");
        return false;
    }
    ctx.rtsp_started = true;
    // VENC normally emits one pack per frame, but reserve a fixed packet
    // array during setup so send_to_rtsp() never allocates in the frame loop.
    ctx.venc_packs.resize(64);
    return true;
}

// Sensor VI config from /mnt/data/sensor_cfg.ini. Same ini file and parser
// tdl_sdk's demos use (SAMPLE_TDL_Get_VI_Config in
// tdl_sdk/sample_video/middleware_utils.c is this exact body) -- copied
// rather than linking that file in, to avoid pulling in the rest of it
// (SAMPLE_TDL_Init_WM et al., which do reach for cvi_rtsp).
bool get_vi_config(SAMPLE_VI_CONFIG_S &vi_config)
{
    SAMPLE_INI_CFG_S ini_cfg = {};
    ini_cfg.enSource = VI_PIPE_FRAME_SOURCE_DEV;
    ini_cfg.devNum = 1;
    ini_cfg.enSnsType[0] = SONY_IMX327_MIPI_2M_30FPS_12BIT;
    ini_cfg.enWDRMode[0] = WDR_MODE_NONE;
    ini_cfg.s32BusId[0] = 3;
    ini_cfg.s32SnsI2cAddr[0] = -1;
    ini_cfg.MipiDev[0] = 0xFF;
    ini_cfg.u8UseMultiSns = 0;

    if (SAMPLE_COMM_VI_ParseIni(&ini_cfg))
        fprintf(stderr, "[camera] sensor info loaded from /mnt/data/sensor_cfg.ini\n");

    if (SAMPLE_COMM_VI_IniToViCfg(&ini_cfg, &vi_config) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] cannot convert ini to VI config\n");
        return false;
    }
    return vi_config.s32WorkingViNum > 0;
}

bool setup_camera(CameraContext &ctx)
{
    if (!get_vi_config(ctx.vi_config))
        return false;

    CVI_VI_SetDevNum(ctx.vi_config.s32WorkingViNum);

    PIC_SIZE_E pic_size;
    if (SAMPLE_COMM_VI_GetSizeBySensor(ctx.vi_config.astViInfo[0].stSnsInfo.enSnsType, &pic_size) !=
        CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] cannot get sensor size\n");
        return false;
    }
    SIZE_S sensor_size;
    if (SAMPLE_COMM_SYS_GetPicSize(pic_size, &sensor_size) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] cannot resolve sensor pic size\n");
        return false;
    }
    ctx.width = sensor_size.u32Width;
    ctx.height = sensor_size.u32Height;
    fprintf(stderr, "[camera] sensor %ux%u\n", ctx.width, ctx.height);

    // VB pools: pool 0 for VI's native NV21 capture, pool 1 for the 1280x720
    // detector/decode channel, optional pool 2 for direct 640x360 TPU input,
    // and the next pool for the ordinary color preview channel.
    ctx.preview_chn = ctx.direct_model_input ? 2 : 1;
    ctx.preview_pool = ctx.direct_model_input ? 3 : 2;
    VB_CONFIG_S vb_config{};
    vb_config.u32MaxPoolCnt = 2 + (ctx.direct_model_input ? 1 : 0) +
                              ((ctx.preview && !ctx.preview_luma) ? 1 : 0);
    vb_config.astCommPool[0].u32BlkSize = COMMON_GetPicBufferSize(
        ctx.width, ctx.height, VI_PIXEL_FORMAT, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    vb_config.astCommPool[0].u32BlkCnt = 5;

    vb_config.astCommPool[kDetPool].u32BlkSize = COMMON_GetPicBufferSize(
        kDetWidth, kDetHeight, PIXEL_FORMAT_YUV_400, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    vb_config.astCommPool[kDetPool].u32BlkCnt = 5;

    if (ctx.direct_model_input)
    {
        vb_config.astCommPool[kModelPool].u32BlkSize = COMMON_GetPicBufferSize(
            640, 360, PIXEL_FORMAT_YUV_400, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
        vb_config.astCommPool[kModelPool].u32BlkCnt = 5;
    }

    if (ctx.preview && !ctx.preview_luma)
    {
        vb_config.astCommPool[ctx.preview_pool].u32BlkSize = COMMON_GetPicBufferSize(
            kDetWidth, kDetHeight, VI_PIXEL_FORMAT, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
        vb_config.astCommPool[ctx.preview_pool].u32BlkCnt = 5;
    }

    if (SAMPLE_COMM_SYS_Init(&vb_config) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] SAMPLE_COMM_SYS_Init failed\n");
        return false;
    }
    ctx.sys_initialized = true;

    if (ctx.preview_luma)
    {
        VB_CAL_CONFIG_S chroma_cfg{};
        COMMON_GetPicBufferConfig(kDetWidth, kDetHeight, VI_PIXEL_FORMAT,
                                  DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN,
                                  &chroma_cfg);
        // The Y plane is borrowed directly from the detector's VPSS frame.
        // Only neutral NV21 chroma surfaces are allocated here; the worker
        // colours overlay regions and reuses them via the ownership states.
        for (size_t i = 0; i < kPreviewSurfaceCount; ++i)
        {
            PreviewSurface &sfc = ctx.preview_surfaces[i];
            sfc.index = i;
            sfc.c_len = chroma_cfg.u32MainCSize;
            sfc.c_stride = chroma_cfg.u32CStride;
            char name[32];
            snprintf(name, sizeof(name), "tinytag_preview_c%zu", i);
            if (CVI_SYS_IonAlloc(&sfc.c_phy, &sfc.c_vir, name, sfc.c_len) != CVI_SUCCESS)
            {
                fprintf(stderr, "[camera] preview chroma allocation failed\n");
                return false;
            }
            // 128/128 is neutral: the scene renders grey. The overlay writes
            // real chroma only where it draws, and those rectangles are reset
            // here on the next pass -- see PreviewSurface::dirty.
            std::memset(sfc.c_vir, 128, sfc.c_len);
            CVI_SYS_IonFlushCache(sfc.c_phy, sfc.c_vir, sfc.c_len);
            sfc.dirty.reserve(64);
        }
        fprintf(stderr, "[camera] preview transport: borrowed VPSS Y (zero copy)\n");
    }

    VI_VPSS_MODE_S vi_vpss_mode{};
    vi_vpss_mode.aenMode[0] = ctx.vi_online ? VI_ONLINE_VPSS_ONLINE
                                            : VI_OFFLINE_VPSS_ONLINE;
    if (CVI_SYS_SetVIVPSSMode(&vi_vpss_mode) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] CVI_SYS_SetVIVPSSMode failed\n");
        return false;
    }
    fprintf(stderr, "[camera] VI/VPSS mode: VI %s, VPSS online\n",
            ctx.vi_online ? "online" : "offline");

    if (SAMPLE_PLAT_VI_INIT(&ctx.vi_config) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] VI init failed\n");
        return false;
    }
    ctx.vi_initialized = true;

    ISP_PUB_ATTR_S pub_attr{};
    CVI_ISP_GetPubAttr(0, &pub_attr);
    pub_attr.f32FrameRate = 30;
    CVI_ISP_SetPubAttr(0, &pub_attr);

    // Orientation is fixed in the VI channel rather than per frame. Mirroring
    // matters beyond cosmetics here: AprilTag/ArUco markers are chiral, so a
    // mirrored frame decodes to nothing at all -- the network still proposes
    // ROIs on tag-like texture, but no quad ever matches the dictionary.
    // Runtime-selectable because which setting is correct depends on the
    // camera module, and getting it wrong is silent apart from zero decodes.
    if (ctx.mirror || ctx.flip)
    {
        const CVI_S32 rc = CVI_VI_SetChnFlipMirror(
            0, 0, ctx.flip ? CVI_TRUE : CVI_FALSE, ctx.mirror ? CVI_TRUE : CVI_FALSE);
        if (rc != CVI_SUCCESS)
            fprintf(stderr, "[camera] CVI_VI_SetChnFlipMirror(flip=%d mirror=%d) failed: %#x\n",
                    ctx.flip, ctx.mirror, rc);
        else
            fprintf(stderr, "[camera] orientation: flip=%d mirror=%d\n", ctx.flip, ctx.mirror);
    }

    // VPSS device/mode setup. u8VpssDev is only meaningful in VPSS_MODE_DUAL
    // (see cvi_comm_vpss.h), and this board's VI runs offline-into-VPSS, so a
    // group needs an explicit device mapped to VPSS_INPUT_ISP on ViPipe 0 --
    // CVI_VPSS_CreateGrp fails outright without this (confirmed on hardware:
    // it's what a first attempt without this call hit). Mirrors
    // tdl_sdk/sample_video/sample_vi_fd.c's config exactly (down to using
    // device 1, not 0 -- its own comment: "device1 has 3 outputs in dual
    // mode"), since that's the one combination already proven to work on
    // this board, rather than guessing at an untested simpler variant.
    VPSS_MODE_S vpss_mode{};
    vpss_mode.enMode = VPSS_MODE_DUAL;
    vpss_mode.aenInput[0] = VPSS_INPUT_MEM;
    vpss_mode.aenInput[1] = VPSS_INPUT_ISP;
    vpss_mode.ViPipe[1] = 0;
    CVI_SYS_SetVPSSModeEx(&vpss_mode);

    // One VPSS group (on device 1, see above). Channel 0 scales the full sensor
    // frame to kDetWidth x kDetHeight for crop-decode. The ordinary model uses
    // a CPU 2x resize from that plane; an aligned-input model adds channel 1 at
    // 640x360 and binds its physical address directly to the TPU input.
    //
    // Verified on hardware: VPSS accepts YUV_400 as a channel output with
    // NV21 group input, and returns a single W*H-byte luma plane.
    VPSS_GRP_ATTR_S vpss_grp_attr{};
    VPSS_GRP_DEFAULT_HELPER2(&vpss_grp_attr, ctx.width, ctx.height, VI_PIXEL_FORMAT, /*dev=*/1);
    VPSS_CHN_ATTR_S vpss_chn_attr{};
    VPSS_CHN_DEFAULT_HELPER(&vpss_chn_attr, kDetWidth, kDetHeight, PIXEL_FORMAT_YUV_400, CVI_FALSE);

    CVI_S32 vpss_ret = CVI_VPSS_CreateGrp(kVpssGrp, &vpss_grp_attr);
    if (vpss_ret == CVI_SUCCESS)
        ctx.vpss_created = true;
    if (vpss_ret == CVI_SUCCESS)
        vpss_ret = CVI_VPSS_ResetGrp(kVpssGrp);
    if (vpss_ret == CVI_SUCCESS)
        vpss_ret = CVI_VPSS_SetChnAttr(kVpssGrp, kVpssChn, &vpss_chn_attr);
    if (vpss_ret == CVI_SUCCESS)
        vpss_ret = CVI_VPSS_EnableChn(kVpssGrp, kVpssChn);
    if (vpss_ret == CVI_SUCCESS && ctx.direct_model_input)
    {
        VPSS_CHN_ATTR_S model_attr{};
        VPSS_CHN_DEFAULT_HELPER(&model_attr, 640, 360, PIXEL_FORMAT_YUV_400, CVI_FALSE);
        vpss_ret = CVI_VPSS_SetChnAttr(kVpssGrp, kModelChn, &model_attr);
        if (vpss_ret == CVI_SUCCESS)
            vpss_ret = CVI_VPSS_EnableChn(kVpssGrp, kModelChn);
    }
    if (vpss_ret == CVI_SUCCESS && ctx.preview && !ctx.preview_luma)
    {
        VPSS_CHN_ATTR_S preview_attr{};
        VPSS_CHN_DEFAULT_HELPER(&preview_attr, kDetWidth, kDetHeight, VI_PIXEL_FORMAT, CVI_FALSE);
        vpss_ret = CVI_VPSS_SetChnAttr(kVpssGrp, ctx.preview_chn, &preview_attr);
        if (vpss_ret == CVI_SUCCESS)
            vpss_ret = CVI_VPSS_EnableChn(kVpssGrp, ctx.preview_chn);
    }
    if (vpss_ret == CVI_SUCCESS)
        vpss_ret = CVI_VPSS_StartGrp(kVpssGrp);
    if (vpss_ret != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] VPSS init failed: %#x\n", vpss_ret);
        return false;
    }

    if (SAMPLE_COMM_VI_Bind_VPSS(0, 0, kVpssGrp) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] VI->VPSS bind failed\n");
        return false;
    }
    ctx.vi_vpss_bound = true;

    // After the bind, matching SAMPLE_TDL_Init_WM's order (VPSS start ->
    // bind VI -> attach VB pools).
    vpss_ret = CVI_VPSS_AttachVbPool(kVpssGrp, kVpssChn, kDetPool);
    if (vpss_ret == CVI_SUCCESS && ctx.direct_model_input)
        vpss_ret = CVI_VPSS_AttachVbPool(kVpssGrp, kModelChn, kModelPool);
    if (vpss_ret == CVI_SUCCESS && ctx.preview && !ctx.preview_luma)
        vpss_ret = CVI_VPSS_AttachVbPool(kVpssGrp, ctx.preview_chn, ctx.preview_pool);
    if (vpss_ret != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] VPSS attach VB pool failed: %#x\n", vpss_ret);
        return false;
    }

    if (ctx.preview && !start_preview_stream(ctx))
        return false;

    return true;
}

// NV21 colors (BT.601 limited range). Boxes are drawn into both planes so
// they show in color; NV21's chroma plane is half resolution, V before U.
struct Nv21Color
{
    uint8_t y, u, v;
};
constexpr Nv21Color kProposalColor{210, 16, 146}; // yellow
constexpr Nv21Color kTagColor{145, 54, 34};       // green
constexpr Nv21Color kAlignColor{158, 140, 197};   // pink

void draw_box(cv::Mat &y, cv::Mat &vu, const cv::Rect &r, Nv21Color c, int thickness,
              std::vector<cv::Rect> *dirty = nullptr)
{
    cv::rectangle(y, r, cv::Scalar(c.y), thickness);
    cv::Rect half(r.x / 2, r.y / 2, std::max(1, r.width / 2), std::max(1, r.height / 2));
    const int half_thick = std::max(1, thickness / 2);
    cv::rectangle(vu, half, cv::Scalar(c.v, c.u), half_thick);
    if (dirty != nullptr)
    {
        // Widen by the stroke so the reset covers the whole drawn outline.
        const int pad = half_thick + 1;
        dirty->push_back(cv::Rect(half.x - pad, half.y - pad,
                                  half.width + 2 * pad, half.height + 2 * pad));
    }
}

// Dark outline under light text so labels read on any background.
void draw_label(cv::Mat &y, const std::string &text, cv::Point org, double scale)
{
    cv::putText(y, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0), 4);
    cv::putText(y, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(255), 2);
}

// Pulls one VPSS output as fast as it produces frames and keeps only the
// newest. Direct-input mode runs this independently for channels 0 and 1, so
// full-resolution capture proceeds while synchronous TPU inference is active.
void capture_loop(VPSS_CHN channel, CaptureSlot *slot)
{
    while (!g_stop)
    {
        VIDEO_FRAME_INFO_S frame{};
        if (CVI_VPSS_GetChnFrame(kVpssGrp, channel, &frame, 1000) != CVI_SUCCESS)
            continue;

        VIDEO_FRAME_INFO_S superseded{};
        bool release_superseded = false;
        bool stopping = false;
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            stopping = slot->stopping;
            if (!stopping)
            {
                if (slot->full)
                {
                    superseded = slot->frame;
                    release_superseded = true;
                    ++slot->dropped;
                }
                slot->frame = frame;
                slot->ready_ms = now_ms();
                slot->full = true;
            }
        }
        if (stopping)
        {
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, channel, &frame);
            return;
        }
        if (release_superseded)
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, channel, &superseded);
        slot->not_empty.notify_one();
    }
}

// Blocks only when the camera has not produced a frame yet -- the healthy case
// when detection outruns capture.
bool take_latest_frame(CaptureSlot &slot, VIDEO_FRAME_INFO_S &out, double *ready_ms = nullptr)
{
    std::unique_lock<std::mutex> lock(slot.mutex);
    slot.not_empty.wait(lock, [&slot] { return slot.full || slot.stopping || g_stop; });
    if (!slot.full)
        return false;
    out = slot.frame;
    if (ready_ms != nullptr)
        *ready_ms = slot.ready_ms;
    slot.full = false;
    return true;
}

// Called only after direct TPU inference. Channel 0 has therefore had the
// whole inference interval to produce the requested full-resolution frame.
bool take_matching_full_frame(CameraContext &ctx, CVI_U32 sequence,
                              VIDEO_FRAME_INFO_S &out, double &ready_ms)
{
    CaptureSlot &slot = ctx.capture;
    for (;;)
    {
        VIDEO_FRAME_INFO_S older{};
        {
            std::unique_lock<std::mutex> lock(slot.mutex);
            slot.not_empty.wait(lock, [&slot] { return slot.full || slot.stopping || g_stop; });
            if (!slot.full)
                return false;
            const int32_t delta = static_cast<int32_t>(slot.frame.stVFrame.u32TimeRef - sequence);
            if (delta == 0)
            {
                out = slot.frame;
                ready_ms = slot.ready_ms;
                slot.full = false;
                return true;
            }
            if (delta > 0)
            {
                ctx.pair_mismatches.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            older = slot.frame;
            slot.full = false;
        }
        ctx.pair_mismatches.fetch_add(1, std::memory_order_relaxed);
        CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &older);
    }
}

void stop_capture_slot(CaptureSlot &slot, std::thread &worker, VPSS_CHN channel)
{
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.stopping = true;
    }
    slot.not_empty.notify_all();
    if (worker.joinable())
        worker.join();
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (slot.full)
    {
        CVI_VPSS_ReleaseChnFrame(kVpssGrp, channel, &slot.frame);
        slot.full = false;
    }
}

void stop_capture(CameraContext &ctx)
{
    if (ctx.direct_model_input)
        stop_capture_slot(ctx.model_capture, ctx.model_capture_worker, kModelChn);
    stop_capture_slot(ctx.capture, ctx.capture_worker, kVpssChn);
}

// Defined below, next to the preview loop; declared here because the luma RTSP
// worker draws before that point in the file.
void draw_overlay_nv21(cv::Mat &y, cv::Mat &vu, const std::vector<Proposal> &proposals,
                       const std::vector<TinyTagResult> &tags,
                       const std::vector<cv::Rect> &crops, double fps, double busy_ms,
                       std::vector<cv::Rect> *dirty);

// Encode one NV21 frame and hand the bitstream to the RTSP server. Same
// sequence as SAMPLE_TDL_Send_Frame_RTSP in tdl_sdk's middleware_utils.c.
RtspSendResult send_to_rtsp(CameraContext &ctx, VIDEO_FRAME_INFO_S &frame)
{
    RtspSendResult result;
    const double venc_start = now_ms();
    // Preview latency is deliberately subordinate to detector isolation. The
    // worker may wait here; its latest-value input slot and two-frame borrowing
    // bound prevent that wait from propagating back into detection.
    if (CVI_VENC_SendFrame(kVencChn, &frame, kVencTimeoutMs) != CVI_SUCCESS)
    {
        result.failures |= RTSP_SEND_SUBMIT;
        result.venc_ms = now_ms() - venc_start;
        return result;
    }
    // SDK samples query status only to size a per-frame allocation. This app's
    // persistent array has 64 entries while the driver MAX_NUM_PACKS is 12, so
    // the query ioctl and its post-submit failure path are both unnecessary.
    VENC_STREAM_S stream{};
    stream.pstPack = ctx.venc_packs.data();
    CVI_S32 get_rc;
    do
    {
        get_rc = CVI_VENC_GetStream(kVencChn, &stream, kVencTimeoutMs);
        if (get_rc == CVI_ERR_VENC_BUSY)
            ++result.get_timeouts;
        // An accepted encode may still DMA from the borrowed Y plane. Drain it
        // before the caller releases that VPSS frame, even when it has become
        // too stale to publish.
    } while (get_rc == CVI_ERR_VENC_BUSY);
    result.venc_ms = now_ms() - venc_start;
    if (get_rc != CVI_SUCCESS)
    {
        result.failures |= RTSP_SEND_GET;
        return result;
    }

    if (stream.u32PackCount == 0)
        result.failures |= RTSP_SEND_NO_PACKS;
    if (stream.u32PackCount > ctx.venc_packs.size())
    {
        fprintf(stderr, "[rtsp] too many VENC packs: %u\n", stream.u32PackCount);
        result.failures |= RTSP_SEND_PACK_OVERFLOW;
    }

    // The preview may be late without harming detection; publish the completed
    // bitstream even if an earlier wait slice timed out.
    if ((result.failures & (RTSP_SEND_NO_PACKS | RTSP_SEND_PACK_OVERFLOW)) == 0)
    {
        CVI_RTSP_DATA data{};
        data.blockCnt = stream.u32PackCount;
        for (CVI_U32 i = 0; i < stream.u32PackCount; ++i)
        {
            data.dataPtr[i] = stream.pstPack[i].pu8Addr + stream.pstPack[i].u32Offset;
            data.dataLen[i] = stream.pstPack[i].u32Len - stream.pstPack[i].u32Offset;
        }
        const double rtsp_start = now_ms();
        if (CVI_RTSP_WriteFrame(ctx.rtsp, ctx.session->video, &data) != 0)
            result.failures |= RTSP_SEND_WRITE;
        result.rtsp_ms = now_ms() - rtsp_start;
    }
    if (CVI_VENC_ReleaseStream(kVencChn, &stream) != CVI_SUCCESS)
        result.failures |= RTSP_SEND_RELEASE;
    return result;
}

void reserve_luma_item(LumaRtspItem &item, size_t capacity)
{
    item.proposals.reserve(capacity);
    item.tags.reserve(capacity);
    item.crops.reserve(capacity);
}

void clear_luma_item(LumaRtspItem &item)
{
    item.source = VIDEO_FRAME_INFO_S{};
    item.source_y_vir = nullptr;
    item.source_map_len = 0;
    item.encoded = VIDEO_FRAME_INFO_S{};
    item.surface = nullptr;
    item.proposals.clear();
    item.tags.clear();
    item.crops.clear();
    item.fps = 0.0;
    item.busy_ms = 0.0;
    item.sequence = 0;
    item.queued_ms = 0.0;
}

struct BorrowedYResources
{
    VIDEO_FRAME_INFO_S frame{};
    uint8_t *mapped_y = nullptr;
    CVI_U32 map_len = 0;
    bool valid = false;
};

BorrowedYResources detach_borrowed_y(LumaRtspItem &item)
{
    BorrowedYResources resources;
    if (item.source_y_vir != nullptr)
    {
        resources.frame = item.source;
        resources.mapped_y = item.source_y_vir;
        resources.map_len = item.source_map_len;
        resources.valid = true;
        item.source = VIDEO_FRAME_INFO_S{};
        item.source_y_vir = nullptr;
        item.source_map_len = 0;
    }
    return resources;
}

void release_borrowed_y(BorrowedYResources &resources)
{
    if (!resources.valid)
        return;
    CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &resources.frame);
    resources.valid = false;
}

// Reserve a surface without ever taking one away from the worker. If both are
// occupied, a still-pending item may be superseded and its QUEUED surface
// reclaimed; an IN_WORKER surface is never writable by the detector.
PreviewSurface *acquire_luma_surface(CameraContext &ctx)
{
    LumaRtspQueue &queue = ctx.luma_rtsp_queue;
    BorrowedYResources superseded;
    std::unique_lock<std::mutex> lock(queue.mutex);
    if (queue.stopping)
        return nullptr;

    for (size_t i = 0; i < kPreviewSurfaceCount; ++i)
    {
        PreviewSurface &sfc = ctx.preview_surfaces[i];
        if (sfc.state == PreviewSurfaceState::FREE)
        {
            sfc.state = PreviewSurfaceState::FILLING;
            return &sfc;
        }
    }

    if (queue.full && queue.pending.surface != nullptr)
    {
        PreviewSurface *sfc = queue.pending.surface;
        if (sfc->state != PreviewSurfaceState::QUEUED)
        {
            ++queue.ownership_errors;
        }
        else
        {
            superseded = detach_borrowed_y(queue.pending);
            if (superseded.valid)
                --queue.borrowed_frames;
            clear_luma_item(queue.pending);
            queue.full = false;
            ++queue.superseded;
            sfc->state = PreviewSurfaceState::FILLING;
            lock.unlock();
            release_borrowed_y(superseded);
            return sfc;
        }
    }

    ++queue.no_surface;
    return nullptr;
}

bool enqueue_luma_rtsp(CameraContext &ctx, const VIDEO_FRAME_INFO_S &encoded,
                       PreviewSurface *surface, const std::vector<Proposal> &proposals,
                       const std::vector<TinyTagResult> &tags,
                       const std::vector<cv::Rect> &crops, double fps, double busy_ms,
                       const VIDEO_FRAME_INFO_S &source, uint8_t *source_y_vir,
                       CVI_U32 source_map_len)
{
    LumaRtspQueue &queue = ctx.luma_rtsp_queue;
    BorrowedYResources superseded;
    std::unique_lock<std::mutex> lock(queue.mutex);
    if (surface == nullptr || surface->state != PreviewSurfaceState::FILLING)
    {
        ++queue.ownership_errors;
        return false;
    }
    if (source_y_vir == nullptr || source_map_len == 0)
    {
        ++queue.ownership_errors;
        surface->state = PreviewSurfaceState::FREE;
        return false;
    }
    if (queue.stopping)
    {
        surface->state = PreviewSurfaceState::FREE;
        return false;
    }

    // The slot keeps the newest frame. Returning the displaced surface here is
    // safe because QUEUED means the worker has not acquired it.
    if (queue.full)
    {
        PreviewSurface *old = queue.pending.surface;
        if (old == nullptr || old->state != PreviewSurfaceState::QUEUED)
            ++queue.ownership_errors;
        else
            old->state = PreviewSurfaceState::FREE;
        superseded = detach_borrowed_y(queue.pending);
        if (superseded.valid)
            --queue.borrowed_frames;
        clear_luma_item(queue.pending);
        ++queue.superseded;
    }

    LumaRtspItem &item = queue.pending;
    item.encoded = encoded;
    item.surface = surface;
    item.source = source;
    item.source_y_vir = source_y_vir;
    item.source_map_len = source_map_len;
    ++queue.borrowed_frames;
    queue.borrowed_frames_max = std::max(queue.borrowed_frames_max,
                                          queue.borrowed_frames);
    item.proposals = proposals;
    item.tags = tags;
    item.crops = crops;
    item.fps = fps;
    item.busy_ms = busy_ms;
    item.sequence = encoded.stVFrame.u32TimeRef;
    item.queued_ms = now_ms();
    surface->state = PreviewSurfaceState::QUEUED;
    queue.full = true;
    ++queue.published;
    lock.unlock();
    release_borrowed_y(superseded);
    queue.not_empty.notify_one();
    return true;
}

void luma_rtsp_loop(CameraContext *ctx)
{
    LumaRtspQueue &queue = ctx->luma_rtsp_queue;
    LumaRtspItem item;
    reserve_luma_item(item, ctx->preview_item_capacity);
    for (;;)
    {
        BorrowedYResources rejected;
        bool item_valid = true;
        {
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.not_empty.wait(lock, [&queue] { return queue.full || queue.stopping; });
            if (!queue.full && queue.stopping)
                return;

            std::swap(item, queue.pending);
            queue.full = false;
            if (item.surface == nullptr || item.surface->state != PreviewSurfaceState::QUEUED)
            {
                ++queue.ownership_errors;
                rejected = detach_borrowed_y(item);
                if (rejected.valid)
                    --queue.borrowed_frames;
                clear_luma_item(item);
                item_valid = false;
            }
            else
            {
                item.surface->state = PreviewSurfaceState::IN_WORKER;

                const double age_ms = now_ms() - item.queued_ms;
                queue.queue_age_sum_ms += age_ms;
                queue.queue_age_max_ms = std::max(queue.queue_age_max_ms, age_ms);
                ++queue.dequeued;
                if (queue.have_previous_sequence)
                {
                    const CVI_U32 gap = item.sequence - queue.previous_sequence;
                    queue.sequence_gap_max = std::max(queue.sequence_gap_max, gap);
                    if (gap > 1)
                        queue.sequence_skipped += gap - 1;
                }
                queue.previous_sequence = item.sequence;
                queue.have_previous_sequence = true;
            }
        }
        if (!item_valid)
        {
            release_borrowed_y(rejected);
            continue;
        }

        if (item.surface != nullptr)
        {
            PreviewSurface &sfc = *item.surface;
            const VIDEO_FRAME_S &vf = item.encoded.stVFrame;
            uint8_t *y_vir = item.source_y_vir;
            cv::Mat y(vf.u32Height, vf.u32Width, CV_8UC1,
                      y_vir, vf.u32Stride[0]);
            cv::Mat vu(vf.u32Height / 2, vf.u32Width / 2, CV_8UC2,
                       static_cast<uint8_t *>(sfc.c_vir), sfc.c_stride);

            // Reset only the chroma this surface coloured last time, so the
            // scene returns to neutral grey without rewriting the whole plane.
            for (const auto &r : sfc.dirty)
                vu(r & cv::Rect(0, 0, vu.cols, vu.rows)).setTo(cv::Scalar(128, 128));
            sfc.dirty.clear();

            draw_overlay_nv21(y, vu, item.proposals, item.tags, item.crops, item.fps,
                              item.busy_ms, &sfc.dirty);

            CVI_SYS_IonFlushCache(vf.u64PhyAddr[0], y_vir, item.source_map_len);
            CVI_SYS_IonFlushCache(sfc.c_phy, sfc.c_vir, sfc.c_len);
        }

        if (ctx->preview_delay_ms != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(ctx->preview_delay_ms));

        const RtspSendResult send = send_to_rtsp(*ctx, item.encoded);
        BorrowedYResources completed = detach_borrowed_y(item);
        const bool completed_borrowed = completed.valid;
        release_borrowed_y(completed);

        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            if (item.surface != nullptr)
            {
                if (item.surface->state != PreviewSurfaceState::IN_WORKER)
                    ++queue.ownership_errors;
                item.surface->state = PreviewSurfaceState::FREE;
            }
            queue.venc_sum_ms += send.venc_ms;
            queue.venc_max_ms = std::max(queue.venc_max_ms, send.venc_ms);
            queue.rtsp_sum_ms += send.rtsp_ms;
            queue.rtsp_max_ms = std::max(queue.rtsp_max_ms, send.rtsp_ms);
            queue.get_timeouts += send.get_timeouts;
            if (send.failures == RTSP_SEND_OK)
                ++queue.encoded;
            else
            {
                ++queue.encode_failed;
                if (send.failures & RTSP_SEND_SUBMIT) ++queue.submit_failed;
                if (send.failures & RTSP_SEND_NO_PACKS) ++queue.no_packs;
                if (send.failures & RTSP_SEND_PACK_OVERFLOW) ++queue.pack_overflow;
                if (send.failures & RTSP_SEND_GET) ++queue.get_failed;
                if (send.failures & RTSP_SEND_WRITE) ++queue.rtsp_failed;
                if (send.failures & RTSP_SEND_RELEASE) ++queue.release_failed;
            }
            if (completed_borrowed)
                --queue.borrowed_frames;
        }
        clear_luma_item(item);
    }
}

LumaRtspStats luma_rtsp_stats(CameraContext &ctx)
{
    LumaRtspQueue &queue = ctx.luma_rtsp_queue;
    std::lock_guard<std::mutex> lock(queue.mutex);
    size_t filling = 0, queued = 0, in_worker = 0;
    for (size_t i = 0; i < kPreviewSurfaceCount; ++i)
    {
        switch (ctx.preview_surfaces[i].state)
        {
        case PreviewSurfaceState::FILLING: ++filling; break;
        case PreviewSurfaceState::QUEUED: ++queued; break;
        case PreviewSurfaceState::IN_WORKER: ++in_worker; break;
        case PreviewSurfaceState::FREE: break;
        }
    }
    // Called by the sole producer between frames, so no surface may still be
    // FILLING. There can be at most one latest-value item and one worker item.
    if (filling != 0 || queued != (queue.full ? 1u : 0u) || in_worker > 1)
        ++queue.ownership_errors;

    LumaRtspStats stats;
    stats.published = queue.published;
    stats.superseded = queue.superseded;
    stats.no_surface = queue.no_surface;
    stats.dequeued = queue.dequeued;
    stats.encoded = queue.encoded;
    stats.encode_failed = queue.encode_failed;
    stats.submit_failed = queue.submit_failed;
    stats.no_packs = queue.no_packs;
    stats.pack_overflow = queue.pack_overflow;
    stats.get_failed = queue.get_failed;
    stats.get_timeouts = queue.get_timeouts;
    stats.rtsp_failed = queue.rtsp_failed;
    stats.release_failed = queue.release_failed;
    stats.sequence_skipped = queue.sequence_skipped;
    stats.ownership_errors = queue.ownership_errors;
    stats.borrowed_frames = queue.borrowed_frames;
    stats.borrowed_frames_max = queue.borrowed_frames_max;
    stats.sequence_gap_max = queue.sequence_gap_max;
    stats.queue_age_sum_ms = queue.queue_age_sum_ms;
    stats.queue_age_max_ms = queue.queue_age_max_ms;
    stats.venc_sum_ms = queue.venc_sum_ms;
    stats.venc_max_ms = queue.venc_max_ms;
    stats.rtsp_sum_ms = queue.rtsp_sum_ms;
    stats.rtsp_max_ms = queue.rtsp_max_ms;
    stats.pending = queue.full;
    queue.sequence_gap_max = 0;
    queue.queue_age_max_ms = 0.0;
    queue.venc_max_ms = 0.0;
    queue.rtsp_max_ms = 0.0;
    return stats;
}

void stop_luma_rtsp(CameraContext &ctx)
{
    if (!ctx.luma_rtsp_worker.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(ctx.luma_rtsp_queue.mutex);
        ctx.luma_rtsp_queue.stopping = true;
    }
    ctx.luma_rtsp_queue.not_empty.notify_one();
    ctx.luma_rtsp_worker.join();

    std::lock_guard<std::mutex> lock(ctx.luma_rtsp_queue.mutex);
    if (ctx.luma_rtsp_queue.borrowed_frames != 0)
    {
        ++ctx.luma_rtsp_queue.ownership_errors;
        fprintf(stderr, "[preview] %zu borrowed VPSS frame(s) remain at shutdown\n",
                ctx.luma_rtsp_queue.borrowed_frames);
    }
    for (size_t i = 0; i < kPreviewSurfaceCount; ++i)
    {
        if (ctx.preview_surfaces[i].state != PreviewSurfaceState::FREE)
        {
            ++ctx.luma_rtsp_queue.ownership_errors;
            fprintf(stderr, "[preview] surface %zu not FREE at shutdown (state %u)\n", i,
                    static_cast<unsigned>(ctx.preview_surfaces[i].state));
        }
    }
}

// Pulls preview frames, draws the newest detections on them, and streams.
// Draws the overlay into an NV21 pair. Boxes and text go into Y; colour comes
// from writing the chroma plane, so this works over a monochrome scene.
// When `dirty` is non-null every chroma rectangle touched is appended to it, so
// the caller can reset exactly those regions next frame instead of clearing the
// whole plane.
void draw_overlay_nv21(cv::Mat &y, cv::Mat &vu, const std::vector<Proposal> &proposals,
                       const std::vector<TinyTagResult> &tags,
                       const std::vector<cv::Rect> &crops, double fps, double busy_ms,
                       std::vector<cv::Rect> *dirty)
{
    // Aligned crop regions actually handed to the decoder -- at most --max of
    // them. Drawn first and thin so the proposal boxes stay readable on top.
    for (const auto &c : crops)
        draw_box(y, vu, c, kAlignColor, 1, dirty);

    // One label per ROI: a decoded tag appends " id N" to its proposal's
    // confidence rather than drawing a second label at the same anchor, which
    // used to overprint and leave both unreadable.
    for (const auto &p : proposals)
    {
        const TinyTagResult *hit = nullptr;
        for (const auto &t : tags)
        {
            if (t.roi == p.roi)
            {
                hit = &t;
                break;
            }
        }

        draw_box(y, vu, p.roi, hit ? kTagColor : kProposalColor, hit ? 4 : 2, dirty);
        const int label_x = std::max(0, static_cast<int>(p.roi.x));
        const int label_y = p.roi.y >= 28.0f
                                ? static_cast<int>(p.roi.y) - 8
                                : std::min(y.rows - 4, static_cast<int>(p.roi.y + p.roi.height) + 22);
        char label[48];
        if (hit)
            snprintf(label, sizeof(label), "%.2f id %d", p.confidence, hit->id);
        else
            snprintf(label, sizeof(label), "%.2f", p.confidence);
        draw_label(y, label, cv::Point(label_x, std::max(20, label_y)), hit ? 0.9 : 0.65);
    }

    char status[96];
    snprintf(status, sizeof(status), "tinytag %.1f fps  %.1f ms  %zu prop  %zu tags", fps, busy_ms,
             proposals.size(), tags.size());
    draw_label(y, status, cv::Point(16, 40), 0.9);
}

// Runs beside the detector loop, which only has to publish into g_overlay.
void preview_loop(CameraContext *ctx)
{
    std::vector<Proposal> proposals;
    std::vector<TinyTagResult> tags;
    std::vector<cv::Rect> crops;
    while (!g_stop)
    {
        VIDEO_FRAME_INFO_S frame{};
        if (CVI_VPSS_GetChnFrame(kVpssGrp, ctx->preview_chn, &frame, 1000) != CVI_SUCCESS)
            continue;

        // Map both NV21 planes in one span (they are contiguous, but compute
        // the plane-1 offset from the physical addresses rather than assume).
        const VIDEO_FRAME_S &vf = frame.stVFrame;
        const CVI_U64 base = vf.u64PhyAddr[0];
        const CVI_U32 span = static_cast<CVI_U32>(vf.u64PhyAddr[1] + vf.u32Length[1] - base);
        uint8_t *mem = static_cast<uint8_t *>(CVI_SYS_MmapCache(base, span));
        if (mem == nullptr)
        {
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, ctx->preview_chn, &frame);
            continue;
        }
        cv::Mat y(vf.u32Height, vf.u32Width, CV_8UC1, mem, vf.u32Stride[0]);
        cv::Mat vu(vf.u32Height / 2, vf.u32Width / 2, CV_8UC2,
                   mem + (vf.u64PhyAddr[1] - base), vf.u32Stride[1]);

        double fps, busy_ms;
        {
            std::lock_guard<std::mutex> lock(g_overlay.mutex);
            proposals = g_overlay.proposals;
            tags = g_overlay.tags;
            crops = g_overlay.crops;
            fps = g_overlay.fps;
            busy_ms = g_overlay.busy_ms;
        }
        draw_overlay_nv21(y, vu, proposals, tags, crops, fps, busy_ms, nullptr);

        // We wrote through a cached mapping; flush so VENC, which reads the
        // physical buffer via DMA, sees the boxes.
        CVI_SYS_IonFlushCache(base, mem, span);
        CVI_SYS_Munmap(mem, span);

        (void)send_to_rtsp(*ctx, frame);
        CVI_VPSS_ReleaseChnFrame(kVpssGrp, ctx->preview_chn, &frame);
    }
}

void teardown_camera(CameraContext &ctx)
{
    if (ctx.rtsp)
    {
        if (ctx.rtsp_started)
            CVI_RTSP_Stop(ctx.rtsp);
        ctx.rtsp_started = false;
        if (ctx.session)
            CVI_RTSP_DestroySession(ctx.rtsp, ctx.session);
        ctx.session = nullptr;
        CVI_RTSP_Destroy(&ctx.rtsp);
    }
    if (ctx.venc_started)
    {
        SAMPLE_COMM_VENC_Stop(kVencChn);
        ctx.venc_started = false;
    }

    // All detector and preview workers have released their frames before
    // teardown reaches here, so no cached mapping can still be in use.
    unmap_cached_luma(ctx);

    if (ctx.vi_vpss_bound)
    {
        SAMPLE_COMM_VI_UnBind_VPSS(0, 0, kVpssGrp);
        ctx.vi_vpss_bound = false;
    }
    if (ctx.vpss_created)
    {
        CVI_BOOL chn_enable[VPSS_MAX_PHY_CHN_NUM + 1] = {0};
        chn_enable[kVpssChn] = CVI_TRUE;
        if (ctx.direct_model_input)
            chn_enable[kModelChn] = CVI_TRUE;
        if (ctx.preview && !ctx.preview_luma)
            chn_enable[ctx.preview_chn] = CVI_TRUE;
        SAMPLE_COMM_VPSS_Stop(kVpssGrp, chn_enable);
        ctx.vpss_created = false;
    }
    if (ctx.vi_initialized)
    {
        SAMPLE_COMM_VI_DestroyIsp(&ctx.vi_config);
        SAMPLE_COMM_VI_DestroyVi(&ctx.vi_config);
        ctx.vi_initialized = false;
    }

    for (size_t i = 0; i < kPreviewSurfaceCount; ++i)
    {
        PreviewSurface &sfc = ctx.preview_surfaces[i];
        if (sfc.c_vir)
        {
            CVI_SYS_IonFree(sfc.c_phy, sfc.c_vir);
            sfc.c_phy = 0;
            sfc.c_vir = nullptr;
        }
    }

    if (ctx.sys_initialized)
    {
        CVI_SYS_Exit();
        CVI_VB_Exit();
        ctx.sys_initialized = false;
    }
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }

    const std::string cvimodel_path = argv[1];
    float heatmap_thres = 0.35f, roi_expand = 1.5f, roi_iou_thres = 0.5f;
    int max_proposals = 8, debug_mode = 1;
    bool decode = false, decode_tolerant = false;
    std::string save_frame_path;
    bool rtsp = false, rtsp_luma = false;
    bool mirror = false, flip = false;
    bool tag_output = true;
    bool direct_compact_input = false;
    bool validate_compact_input = false;
    int crop_align = 4;

    for (int i = 2; i < argc; ++i)
    {
        std::string flag = argv[i];
        bool has_value = i + 1 < argc;
        if (flag == "--thres" && has_value) heatmap_thres = std::atof(argv[++i]);
        else if (flag == "--max" && has_value) max_proposals = std::atoi(argv[++i]);
        else if (flag == "--expand" && has_value) roi_expand = std::atof(argv[++i]);
        else if (flag == "--iou" && has_value) roi_iou_thres = std::atof(argv[++i]);
        else if (flag == "--debug" && has_value) debug_mode = std::atoi(argv[++i]);
        else if (flag == "--save-frame" && has_value) save_frame_path = argv[++i];
        else if (flag == "--crop-align" && has_value) crop_align = std::atoi(argv[++i]);
        else if (flag == "--mirror" && has_value) mirror = std::atoi(argv[++i]) != 0;
        else if (flag == "--flip" && has_value) flip = std::atoi(argv[++i]) != 0;
        else if (flag == "--tag-output" && has_value) tag_output = std::atoi(argv[++i]) != 0;
        else if (flag == "--direct-compact-input" && has_value)
            direct_compact_input = std::atoi(argv[++i]) != 0;
        else if (flag == "--validate-compact-input" && has_value)
            validate_compact_input = std::atoi(argv[++i]) != 0;
        else if (flag == "--rtsp") rtsp = true;
        else if (flag == "--rtsp-luma") rtsp = rtsp_luma = true;
        else if (flag == "--decode" && has_value)
        {
            decode = true;
            std::string mode = argv[++i];
            if (mode == "tolerant") decode_tolerant = true;
            else if (mode == "strict") decode_tolerant = false;
            else
            {
                fprintf(stderr, "--decode mode must be 'strict' or 'tolerant'\n");
                return 1;
            }
        }
        else
        {
            fprintf(stderr, "Unknown or incomplete option: %s\n\n", flag.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Validate and construct the model before acquiring camera resources. An
    // aligned model selects the optional independent 640x360 VPSS input path.
    std::unique_ptr<TinyTagDet> detector_storage;
    try
    {
        detector_storage.reset(new TinyTagDet(cvimodel_path, heatmap_thres, max_proposals,
                                              roi_expand, roi_iou_thres, debug_mode));
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "[camera] detector initialization failed: %s\n", e.what());
        return 1;
    }
    TinyTagDet &detector = *detector_storage;
    detector.set_crop_align(crop_align);
    if (detector.crop_align() > 1)
        fprintf(stderr, "[camera] crop align: %d px\n", detector.crop_align());
    if (decode)
    {
        detector.set_decoder(make_aruco_nano_decoder(decode_tolerant));
        fprintf(stderr, "[camera] decoder: ArUco Nano, AprilTag 36h11, %s\n",
                decode_tolerant ? "tolerant" : "strict");
    }

    CameraContext ctx;
    ctx.preview = rtsp;
    ctx.preview_luma = rtsp_luma;
    ctx.preview_item_capacity = static_cast<size_t>(std::max(1, max_proposals));
    ctx.mirror = mirror;
    ctx.flip = flip;
    ctx.compact_direct_input = direct_compact_input && !detector.uses_aligned_input();
    ctx.validate_compact_input = validate_compact_input && ctx.compact_direct_input;
    ctx.direct_model_input = detector.uses_aligned_input() || ctx.compact_direct_input;
    if (const char *online = std::getenv("TINYTAG_LIVE_VI_ONLINE"))
        ctx.vi_online = std::atoi(online) != 0;
    if (const char *delay = std::getenv("TINYTAG_LIVE_PREVIEW_DELAY_MS"))
        ctx.preview_delay_ms = static_cast<unsigned>(std::max(0, std::atoi(delay)));
    if (!setup_camera(ctx))
    {
        teardown_camera(ctx);
        return 1;
    }

    // Matches the detector VPSS pool. Avoid even the few vector reallocations
    // that would otherwise occur while its fixed physical blocks are learned.
    ctx.luma_mappings.reserve(5);

    if (ctx.preview_luma)
    {
        reserve_luma_item(ctx.luma_rtsp_queue.pending, ctx.preview_item_capacity);
        for (size_t i = 0; i < kPreviewSurfaceCount; ++i)
            ctx.preview_surfaces[i].dirty.reserve(ctx.preview_item_capacity * 2);
        if (ctx.preview_delay_ms != 0)
            fprintf(stderr, "[camera] preview worker stress delay: %u ms\n", ctx.preview_delay_ms);
    }

    ctx.capture_worker = std::thread(capture_loop, kVpssChn, &ctx.capture);
    if (ctx.direct_model_input)
        ctx.model_capture_worker = std::thread(capture_loop, kModelChn, &ctx.model_capture);

    std::thread isp_control(isp_control_loop);

    std::thread preview;
    if (ctx.preview && !ctx.preview_luma)
        preview = std::thread(preview_loop, &ctx);
    if (ctx.preview_luma)
        ctx.luma_rtsp_worker = std::thread(luma_rtsp_loop, &ctx);

    std::vector<Proposal> proposals;
    std::vector<TinyTagResult> results;
    proposals.reserve(static_cast<size_t>(std::max(1, max_proposals)));
    results.reserve(static_cast<size_t>(std::max(1, max_proposals)));

    // Per-stage times summed over a one-second window and printed as
    // per-frame averages, so the line shows where latency actually goes
    // rather than a single noisy last-frame number.
    struct StageTotals
    {
        long frames = 0;
        double wait = 0, pair = 0, map = 0, pre = 0, infer = 0, decode = 0, crop = 0;
        double release = 0;
        double output = 0;
        double service_cpu = 0;
        double crop_threshold = 0, crop_contour = 0, crop_quad = 0;
        double crop_marker_decode = 0, crop_refine = 0;
        size_t crop_pixels = 0, crop_contours = 0, crop_candidates = 0;
        size_t crop_attempts = 0, crop_markers = 0;
        double ready_delta_sum = 0, ready_delta_max = -10000.0;
        long ready_delta_samples = 0;
        double age_sum = 0, age_max = 0;
        long age_samples = 0;
        size_t proposals = 0;
        // Gap in the camera's own frame counter between consecutive frames the
        // detector processed. 1 means nothing was skipped; >1 means the capture
        // thread superseded that many. Summing the gaps over a second should
        // come out at the capture rate, which is what proves the detector is
        // taking the newest frame rather than dropping arbitrary ones.
        unsigned seq_sum = 0;
        unsigned seq_max = 0;
        long seq_samples = 0;
    } win;
    CVI_U32 prev_seq = 0;
    bool have_prev_seq = false;
    double fps_window_start = now_ms();
    double tail_window_start = fps_window_start;
    double overlay_fps = 0.0;
    double overlay_busy_ms = 0.0;
    std::vector<double> tail_service, tail_cpu, tail_crop, tail_acquisition_age, tail_result_age;
    for (auto *samples : {&tail_service, &tail_cpu, &tail_crop,
                          &tail_acquisition_age, &tail_result_age})
        samples->reserve(512);

    fprintf(stderr, "[camera] capture started: sensor %ux%u -> VPSS %ux%u %s -> tinytag_detect\n",
            ctx.width, ctx.height, kDetWidth, kDetHeight,
            ctx.preview_luma ? "YUV_400 + borrowed RTSP Y" : "YUV_400");
    fprintf(stderr, "[camera] model input: %s\n",
            ctx.compact_direct_input
                ? "independent VPSS 640x360 Y (guarded compact physical experiment)"
                : (ctx.direct_model_input ? "independent VPSS 640x360 Y (aligned physical)"
                                          : "CPU resize/copy to 640x360"));

    bool logged_first_frame = false;
    constexpr int kSaveFrameIndex = 30;
    int frames_seen = 0;
    size_t stale_prev = 0;
    size_t pair_mismatch_prev = 0;
    bool compact_needs_validation = ctx.validate_compact_input;
    LumaRtspStats preview_prev;
    while (!g_stop)
    {
        VIDEO_FRAME_INFO_S frame{};
        VIDEO_FRAME_INFO_S model_frame{};
        double model_ready_ms = 0.0;
        double full_ready_ms = 0.0;
        const double t_wait = now_ms();
        // Direct mode is driven by the newest small model frame. The larger
        // full-resolution channel is deliberately not awaited here: its
        // capture proceeds concurrently while the synchronous TPU call runs.
        if (ctx.direct_model_input)
        {
            if (!take_latest_frame(ctx.model_capture, model_frame, &model_ready_ms))
                break;
        }
        else if (!take_latest_frame(ctx.capture, frame))
        {
            break;
        }
        const double t_got = now_ms();
        const double cpu_got = thread_cpu_ms();

        VIDEO_FRAME_S model_info{};
        if (ctx.direct_model_input)
        {
            const VIDEO_FRAME_S &mf = model_frame.stVFrame;
            const cv::Size expected = detector.input_size();
            if (mf.u32Width != static_cast<CVI_U32>(expected.width) ||
                mf.u32Height != static_cast<CVI_U32>(expected.height) ||
                mf.u32Stride[0] != static_cast<CVI_U32>(expected.width) ||
                mf.u32Length[0] < mf.u32Stride[0] * mf.u32Height ||
                mf.enPixelFormat != PIXEL_FORMAT_YUV_400)
            {
                fprintf(stderr,
                        "[camera] direct model frame contract mismatch: %ux%u stride=%u len=%u fmt=%d, "
                        "expected %dx%d tightly packed YUV400\n",
                        mf.u32Width, mf.u32Height, mf.u32Stride[0], mf.u32Length[0],
                        mf.enPixelFormat, expected.width, expected.height);
                CVI_VPSS_ReleaseChnFrame(kVpssGrp, kModelChn, &model_frame);
                break;
            }
            model_info = mf;
        }

        // u32TimeRef is the camera's frame counter. Unsigned subtraction wraps
        // correctly, so no special case at the 32-bit boundary.
        const VIDEO_FRAME_S &timing_frame =
            ctx.direct_model_input ? model_frame.stVFrame : frame.stVFrame;
        const CVI_U32 seq = timing_frame.u32TimeRef;
        if (have_prev_seq)
        {
            const unsigned gap = static_cast<unsigned>(seq - prev_seq);
            win.seq_sum += gap;
            if (gap > win.seq_max)
                win.seq_max = gap;
            ++win.seq_samples;
        }
        prev_seq = seq;
        have_prev_seq = true;

        double acquisition_age_ms = -1.0;
        if (timing_frame.u64PTS != 0)
        {
            const double age_ms = t_got - static_cast<double>(timing_frame.u64PTS) / 1000.0;
            if (age_ms >= 0.0 && age_ms < 10000.0)
            {
                acquisition_age_ms = age_ms;
                win.age_sum += age_ms;
                win.age_max = std::max(win.age_max, age_ms);
                ++win.age_samples;
            }
        }

        double pair_wait_ms = 0.0;
        if (ctx.direct_model_input)
        {
            uint8_t *compact_validation_copy = nullptr;
            if (compact_needs_validation)
            {
                compact_validation_copy = static_cast<uint8_t *>(CVI_SYS_MmapCache(
                    model_frame.stVFrame.u64PhyAddr[0], model_frame.stVFrame.u32Length[0]));
                if (compact_validation_copy == nullptr)
                {
                    fprintf(stderr, "[model-input] cannot map first compact frame for validation\n");
                    CVI_VPSS_ReleaseChnFrame(kVpssGrp, kModelChn, &model_frame);
                    g_stop = 1;
                    break;
                }
            }
            try
            {
                if (ctx.compact_direct_input)
                    detector.detect_compact_physical(
                        model_frame.stVFrame.u64PhyAddr[0],
                        cv::Size(model_frame.stVFrame.u32Width,
                                 model_frame.stVFrame.u32Height),
                        model_frame.stVFrame.u32Stride[0], model_frame.stVFrame.u32Length[0],
                        compact_validation_copy,
                        cv::Size(kDetWidth, kDetHeight), proposals);
                else
                    detector.detect_physical(model_frame.stVFrame.u64PhyAddr[0],
                                             cv::Size(kDetWidth, kDetHeight), proposals);
            }
            catch (const std::exception &e)
            {
                if (compact_validation_copy != nullptr)
                    CVI_SYS_Munmap(compact_validation_copy, model_frame.stVFrame.u32Length[0]);
                fprintf(stderr, "[camera] direct inference failed: %s\n", e.what());
                CVI_VPSS_ReleaseChnFrame(kVpssGrp, kModelChn, &model_frame);
                g_stop = 1;
                break;
            }
            if (compact_validation_copy != nullptr)
            {
                CVI_SYS_Munmap(compact_validation_copy, model_frame.stVFrame.u32Length[0]);
                compact_needs_validation = false;
            }
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, kModelChn, &model_frame);
            model_frame = VIDEO_FRAME_INFO_S{};

            const double pair_started = now_ms();
            if (!take_matching_full_frame(ctx, seq, frame, full_ready_ms))
                continue;
            pair_wait_ms = now_ms() - pair_started;
            const double ready_delta_ms = full_ready_ms - model_ready_ms;
            win.ready_delta_sum += ready_delta_ms;
            win.ready_delta_max = std::max(win.ready_delta_max, ready_delta_ms);
            ++win.ready_delta_samples;
        }

        // GetChnFrame returns *physical* addresses only -- pu8VirAddr is left
        // NULL. CVI_SYS_MmapCache maps the plane and internally invalidates
        // the mapped range, so the CPU sees what VPSS just wrote via DMA. Do
        // not immediately invalidate it a second time here.
        //
        // MmapCache, not Mmap: the plain variant is a non-cached mapping, and
        // every CPU read through it is slow. Measured on a Duo S at 1080p:
        // pre_process 20-23 ms -> 2.1 ms, 8-ROI crop-decode 11-13 ms -> ~5 ms.
        const VIDEO_FRAME_S &vf = frame.stVFrame;
        const CVI_U32 map_len = vf.u32Length[0] ? vf.u32Length[0] : vf.u32Stride[0] * vf.u32Height;
        const double t_map_started = now_ms();
        uint8_t *luma = map_luma_for_cpu(ctx, vf.u64PhyAddr[0], map_len);
        if (luma == nullptr)
        {
            fprintf(stderr, "[camera] CVI_SYS_Mmap failed for %#llx (%u bytes)\n",
                    (unsigned long long)vf.u64PhyAddr[0], map_len);
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &frame);
            continue;
        }
        const double t_mapped = now_ms();

        // Wrap the mapped plane without copying. u32Stride may exceed width
        // (alignment padding), so the Mat uses the real stride.
        cv::Mat gray(vf.u32Height, vf.u32Width, CV_8UC1, luma, vf.u32Stride[0]);

        if (!logged_first_frame)
        {
            // If u64PTS is non-zero the kernel populates it and true frame age
            // could be measured directly; nothing in this tree sets it.
            fprintf(stderr, "[camera] first frame: timeRef=%u pts=%llu\n", vf.u32TimeRef,
                    (unsigned long long)vf.u64PTS);
            fprintf(stderr, "[camera] first frame: %ux%u stride=%u len=%u fmt=%d\n", vf.u32Width,
                    vf.u32Height, vf.u32Stride[0], map_len, vf.enPixelFormat);
            if (ctx.direct_model_input)
            {
                fprintf(stderr,
                        "[camera] first model frame: timeRef=%u %ux%u stride=%u len=%u fmt=%d\n",
                        model_info.u32TimeRef, model_info.u32Width, model_info.u32Height,
                        model_info.u32Stride[0], model_info.u32Length[0], model_info.enPixelFormat);
            }
            logged_first_frame = true;
        }
        // Frame 1 comes out black while auto-exposure is still converging,
        // so save one from about a second in.
        if (!save_frame_path.empty() && ++frames_seen == kSaveFrameIndex)
        {
            if (cv::imwrite(save_frame_path, gray))
                fprintf(stderr, "[camera] saved frame %d to %s\n", kSaveFrameIndex, save_frame_path.c_str());
            else
                fprintf(stderr, "[camera] could not write %s\n", save_frame_path.c_str());
        }

        try
        {
            if (!ctx.direct_model_input)
                detector.detect(gray, proposals);
            if (decode)
                detector.post_process(gray, proposals, results);
        }
        catch (const std::exception &e)
        {
            fprintf(stderr, "[camera] detector runtime failed: %s\n", e.what());
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &frame);
            g_stop = 1;
            break;
        }
        const double t_detected = now_ms();

        VIDEO_FRAME_INFO_S rtsp_frame{};
        PreviewSurface *surface = nullptr;
        if (ctx.preview_luma)
        {
            // Reserve a neutral-chroma/overlay surface. The exact detector Y
            // mapping and VPSS frame transfer to the worker below.
            surface = acquire_luma_surface(ctx);

            if (surface != nullptr)
            {
                rtsp_frame = frame;
                VIDEO_FRAME_S &rf = rtsp_frame.stVFrame;
                rf.enPixelFormat = PIXEL_FORMAT_NV21;
                // Keep the exact detector plane and its existing mapping.
                // Ownership moves to the preview item after enqueue.
                rf.pu8VirAddr[0] = luma;
                rf.u32Stride[1] = surface->c_stride;
                rf.u32Length[1] = surface->c_len;
                rf.u64PhyAddr[1] = surface->c_phy;
                rf.pu8VirAddr[1] = static_cast<CVI_U8 *>(surface->c_vir);
                rf.u32Stride[2] = 0;
                rf.u32Length[2] = 0;
                rf.u64PhyAddr[2] = 0;
                rf.pu8VirAddr[2] = nullptr;
            }
        }

        if (surface != nullptr)
        {
            // Transfer both the VPSS frame and its current cached mapping. The
            // worker draws, flushes, encodes, then releases the frame. Its
            // cached mapping remains valid until application teardown.
            const bool preview_queued = enqueue_luma_rtsp(
                ctx, rtsp_frame, surface, proposals, results, detector.last_crop_rects(),
                overlay_fps, overlay_busy_ms, frame, luma, map_len);
            if (!preview_queued)
            {
                CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &frame);
            }
        }
        else
        {
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &frame);
        }
        const double t_released = now_ms();
        ++win.frames;
        win.wait += t_got - t_wait;
        win.pair += pair_wait_ms;
        win.map += t_mapped - t_map_started;
        win.pre += detector.last_preprocess_ms();
        win.infer += detector.last_inference_ms();
        win.decode += detector.last_decode_ms();
        win.crop += decode ? detector.last_crop_decode_ms() : 0.0;
        if (decode)
        {
            const TagDecoderProfile &profile = detector.last_decoder_profile();
            win.crop_threshold += profile.threshold_ms;
            win.crop_contour += profile.contour_ms;
            win.crop_quad += profile.quad_ms;
            win.crop_marker_decode += profile.decode_ms;
            win.crop_refine += profile.refine_ms;
            win.crop_pixels += profile.pixels;
            win.crop_contours += profile.contours;
            win.crop_candidates += profile.candidates;
            win.crop_attempts += profile.attempts;
            win.crop_markers += profile.markers;
        }
        win.release += t_released - t_detected;
        win.proposals += proposals.size();

        if (ctx.preview && !ctx.preview_luma)
        {
            std::lock_guard<std::mutex> lock(g_overlay.mutex);
            g_overlay.proposals = proposals;
            g_overlay.crops = detector.last_crop_rects();
            if (decode)
                g_overlay.tags = results;
        }

        if (tag_output)
        {
            for (const auto &r : results)
                fprintf(stdout, "[tag] id=%d confidence=%.2f roi=(%.0f,%.0f,%.0fx%.0f)\n", r.id,
                        r.proposal_confidence, r.roi.x, r.roi.y, r.roi.width, r.roi.height);
            if (!results.empty())
                fflush(stdout);
        }

        const double t_output_done = now_ms();
        const double cpu_output_done = thread_cpu_ms();
        win.output += t_output_done - t_released;
        win.service_cpu += cpu_output_done - cpu_got;

        const double service_ms = t_output_done - t_got;
        tail_service.push_back(service_ms);
        tail_cpu.push_back(cpu_output_done - cpu_got);
        tail_crop.push_back(decode ? detector.last_crop_decode_ms() : 0.0);
        if (acquisition_age_ms >= 0.0)
        {
            tail_acquisition_age.push_back(acquisition_age_ms);
            tail_result_age.push_back(acquisition_age_ms + service_ms);
        }

        const double t_now = now_ms();
        if (t_now - tail_window_start >= 10000.0)
        {
            const TailSummary service = summarize_tail(tail_service);
            const TailSummary cpu = summarize_tail(tail_cpu);
            const TailSummary crop_tail = summarize_tail(tail_crop);
            const TailSummary acquisition = summarize_tail(tail_acquisition_age);
            const TailSummary result_age = summarize_tail(tail_result_age);
            fprintf(stderr,
                    "[tails] %.1fs n=%zu | service p50 %.2f p95 %.2f p99 %.2f max %.2f | "
                    "cpu %.2f/%.2f/%.2f/%.2f | crop %.2f/%.2f/%.2f/%.2f | "
                    "acq-age %.2f/%.2f/%.2f/%.2f | result-age %.2f/%.2f/%.2f/%.2f ms\n",
                    (t_now - tail_window_start) / 1000.0, tail_service.size(),
                    service.p50, service.p95, service.p99, service.max,
                    cpu.p50, cpu.p95, cpu.p99, cpu.max,
                    crop_tail.p50, crop_tail.p95, crop_tail.p99, crop_tail.max,
                    acquisition.p50, acquisition.p95, acquisition.p99, acquisition.max,
                    result_age.p50, result_age.p95, result_age.p99, result_age.max);
            tail_service.clear();
            tail_cpu.clear();
            tail_crop.clear();
            tail_acquisition_age.clear();
            tail_result_age.clear();
            tail_window_start = t_now;
        }
        if (t_now - fps_window_start >= 1000.0)
        {
            size_t stale_now;
            {
                CaptureSlot &freshness_slot =
                    ctx.direct_model_input ? ctx.model_capture : ctx.capture;
                std::lock_guard<std::mutex> lock(freshness_slot.mutex);
                stale_now = freshness_slot.dropped;
            }
            const double n = static_cast<double>(win.frames);
            const double busy = (win.pair + win.map + win.pre + win.infer + win.decode +
                                 win.crop + win.release) / n;
            const double loop = busy + win.output / n;
            fprintf(stderr,
                    "[camera] %.1f fps | per frame ms: wait %.2f pair %.2f map %.2f pre %.2f infer %.2f "
                    "decode %.2f crop %.2f release %.2f output %.2f = busy %.2f loop %.2f cpu %.2f | "
                    "%.1f proposals | %zu stale"
                    " | seq mean %.2f max %u sum %u | age mean %.2f max %.2f ms | "
                    "map-cache hit %zu miss %zu blocks %zu\n",
                    n * 1000.0 / (t_now - fps_window_start), win.wait / n, win.pair / n,
                    win.map / n, win.pre / n,
                    win.infer / n, win.decode / n, win.crop / n, win.release / n, win.output / n,
                    busy, loop, win.service_cpu / n,
                    win.proposals / n, stale_now - stale_prev,
                    win.seq_samples ? static_cast<double>(win.seq_sum) / win.seq_samples : 0.0,
                    win.seq_max, win.seq_sum,
                    win.age_samples ? win.age_sum / win.age_samples : 0.0, win.age_max,
                    ctx.luma_mapping_hits,
                    ctx.luma_mapping_misses, ctx.luma_mappings.size());
            if (decode)
            {
                const double profiled = (win.crop_threshold + win.crop_contour + win.crop_quad +
                                         win.crop_marker_decode + win.crop_refine) / n;
                fprintf(stderr,
                        "[crop-profile] per frame ms: threshold %.2f contour %.2f quad %.2f "
                        "marker %.2f refine %.2f = %.2f of crop %.2f | "
                        "pixels %.0f contours %.1f candidates %.1f attempts %.1f markers %.2f\n",
                        win.crop_threshold / n, win.crop_contour / n, win.crop_quad / n,
                        win.crop_marker_decode / n, win.crop_refine / n, profiled,
                        win.crop / n, static_cast<double>(win.crop_pixels) / n,
                        static_cast<double>(win.crop_contours) / n,
                        static_cast<double>(win.crop_candidates) / n,
                        static_cast<double>(win.crop_attempts) / n,
                        static_cast<double>(win.crop_markers) / n);
            }
            if (ctx.direct_model_input)
            {
                const size_t pair_mismatches =
                    ctx.pair_mismatches.load(std::memory_order_relaxed);
                fprintf(stderr, "[model-input] pair mismatches %zu total %zu\n",
                        pair_mismatches - pair_mismatch_prev, pair_mismatches);
                fprintf(stderr, "[model-input] full-minus-model ready mean %.2f max %.2f ms\n",
                        win.ready_delta_samples ? win.ready_delta_sum / win.ready_delta_samples : 0.0,
                        win.ready_delta_samples ? win.ready_delta_max : 0.0);
                pair_mismatch_prev = pair_mismatches;
            }
            if (ctx.preview_luma)
            {
                const LumaRtspStats current = luma_rtsp_stats(ctx);
                const size_t dequeued = current.dequeued - preview_prev.dequeued;
                const double age_sum = current.queue_age_sum_ms - preview_prev.queue_age_sum_ms;
                fprintf(stderr,
                        "[preview] published %zu dequeued %zu encoded %zu fail %zu | "
                        "replaced %zu no-surface %zu | "
                        "seq skipped %zu max-gap %u | queue age mean %.2f max %.2f ms | "
                        "venc mean %.2f max %.2f rtsp mean %.2f max %.2f ms | "
                        "drop submit %zu empty %zu packs %zu get %zu timeout %zu "
                        "rtsp %zu release %zu | pending %d borrowed %zu max %zu ownership-errors %zu\n",
                        current.published - preview_prev.published, dequeued,
                        current.encoded - preview_prev.encoded,
                        current.encode_failed - preview_prev.encode_failed,
                        current.superseded - preview_prev.superseded,
                        current.no_surface - preview_prev.no_surface,
                        current.sequence_skipped - preview_prev.sequence_skipped,
                        current.sequence_gap_max, dequeued ? age_sum / dequeued : 0.0,
                        current.queue_age_max_ms,
                        dequeued ? (current.venc_sum_ms - preview_prev.venc_sum_ms) / dequeued : 0.0,
                        current.venc_max_ms,
                        dequeued ? (current.rtsp_sum_ms - preview_prev.rtsp_sum_ms) / dequeued : 0.0,
                        current.rtsp_max_ms,
                        current.submit_failed - preview_prev.submit_failed,
                        current.no_packs - preview_prev.no_packs,
                        current.pack_overflow - preview_prev.pack_overflow,
                        current.get_failed - preview_prev.get_failed,
                        current.get_timeouts - preview_prev.get_timeouts,
                        current.rtsp_failed - preview_prev.rtsp_failed,
                        current.release_failed - preview_prev.release_failed,
                        current.pending ? 1 : 0,
                        current.borrowed_frames, current.borrowed_frames_max,
                        current.ownership_errors);
                preview_prev = current;
            }
            if (ctx.preview && !ctx.preview_luma)
            {
                if (!ctx.preview_luma)
                {
                    std::lock_guard<std::mutex> lock(g_overlay.mutex);
                    g_overlay.fps = n * 1000.0 / (t_now - fps_window_start);
                    g_overlay.busy_ms = busy;
                }
            }
            overlay_fps = n * 1000.0 / (t_now - fps_window_start);
            overlay_busy_ms = busy;
            stale_prev = stale_now;
            win = StageTotals{};
            fps_window_start = t_now;
        }
    }

    fprintf(stderr, "[camera] stopping\n");
    g_stop = 1;
    if (preview.joinable())
        preview.join();
    stop_capture(ctx);
    stop_luma_rtsp(ctx);
    if (isp_control.joinable())
        isp_control.join();
    teardown_camera(ctx);
    return 0;
}
