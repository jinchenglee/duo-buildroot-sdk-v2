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
#include <rtsp.h>
#include <sample_comm.h>
}

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
void handle_signal(int) { g_stop = 1; }

constexpr VPSS_GRP kVpssGrp = 0;
constexpr VPSS_CHN kVpssChn = 0;

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
constexpr VPSS_CHN kPreviewChn = 1;
constexpr VB_POOL kDetPool = 1;
constexpr VB_POOL kPreviewPool = 2;
constexpr VENC_CHN kVencChn = 0;
constexpr int kPreviewBitrateKbps = 3000;
constexpr size_t kLumaRtspQueueDepth = 4;

struct LumaRtspItem
{
    VIDEO_FRAME_INFO_S source{};
    VIDEO_FRAME_INFO_S encoded{};
};

struct LumaRtspQueue
{
    std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    LumaRtspItem items[kLumaRtspQueueDepth]{};
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    bool stopping = false;
};

// Latest detections, published by the detector loop and drawn by the preview
// thread. The preview draws whatever is newest, so boxes can trail the video
// by up to one detection frame -- the same trade sample_vi_fd makes.
struct Overlay
{
    std::mutex mutex;
    std::vector<Proposal> proposals;
    std::vector<TinyTagResult> tags;
    double fps = 0.0;
    double busy_ms = 0.0;
} g_overlay;

double now_ms()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s <cvimodel> [--thres f] [--max n] [--expand f] [--iou f]\n"
            "       [--decode strict|tolerant] [--debug n] [--save-frame frame.png] [--rtsp]\n"
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

void *isp_control_thread(void *)
{
    print_isp_help();
    char line[256];
    while (fgets(line, sizeof(line), stdin) != nullptr)
    {
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
    return nullptr;
}

// Everything TeardownCamera() needs. Populated by SetupCamera().
struct CameraContext
{
    SAMPLE_VI_CONFIG_S vi_config{};
    uint32_t width = 0;
    uint32_t height = 0;
    bool preview = false;
    bool preview_luma = false;
    CVI_U64 rtsp_chroma_phy = 0;
    CVI_VOID *rtsp_chroma_vir = nullptr;
    CVI_U32 rtsp_chroma_len = 0;
    CVI_U32 rtsp_chroma_stride = 0;
    std::vector<VENC_PACK_S> venc_packs;
    LumaRtspQueue luma_rtsp_queue;
    std::thread luma_rtsp_worker;
    CVI_RTSP_CTX *rtsp = nullptr;
    CVI_RTSP_SESSION *session = nullptr;
};

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
    ic.vpssChn = ctx.preview_luma ? kVpssChn : kPreviewChn;
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
    CVI_RTSP_CreateSession(ctx.rtsp, &attr, &ctx.session);

    CVI_RTSP_STATE_LISTENER listener{};
    listener.onConnect = on_rtsp_connect;
    listener.onDisconnect = on_rtsp_disconnect;
    CVI_RTSP_SetListener(ctx.rtsp, &listener);

    if (CVI_RTSP_Start(ctx.rtsp) < 0)
    {
        fprintf(stderr, "[rtsp] cannot start RTSP server\n");
        return false;
    }
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

    // VB pools: pool 0 for VI's native NV21 capture, pool 1 for the detector
    // channel (YUV_400), and pool 2 for the ordinary color preview channel.
    VB_CONFIG_S vb_config{};
    vb_config.u32MaxPoolCnt = (ctx.preview && !ctx.preview_luma) ? 3 : 2;
    vb_config.astCommPool[0].u32BlkSize = COMMON_GetPicBufferSize(
        ctx.width, ctx.height, VI_PIXEL_FORMAT, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    vb_config.astCommPool[0].u32BlkCnt = 5;

    vb_config.astCommPool[kDetPool].u32BlkSize = COMMON_GetPicBufferSize(
        kDetWidth, kDetHeight, PIXEL_FORMAT_YUV_400, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    vb_config.astCommPool[kDetPool].u32BlkCnt = 5;

    if (ctx.preview && !ctx.preview_luma)
    {
        vb_config.astCommPool[kPreviewPool].u32BlkSize = COMMON_GetPicBufferSize(
            kDetWidth, kDetHeight, VI_PIXEL_FORMAT, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
        vb_config.astCommPool[kPreviewPool].u32BlkCnt = 5;
    }

    if (SAMPLE_COMM_SYS_Init(&vb_config) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] SAMPLE_COMM_SYS_Init failed\n");
        return false;
    }

    if (ctx.preview_luma)
    {
        VB_CAL_CONFIG_S chroma_cfg{};
        COMMON_GetPicBufferConfig(kDetWidth, kDetHeight, VI_PIXEL_FORMAT,
                                  DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN,
                                  &chroma_cfg);
        ctx.rtsp_chroma_len = chroma_cfg.u32MainCSize;
        ctx.rtsp_chroma_stride = chroma_cfg.u32CStride;
        if (CVI_SYS_IonAlloc(&ctx.rtsp_chroma_phy, &ctx.rtsp_chroma_vir,
                             "tinytag_rtsp_chroma", ctx.rtsp_chroma_len) != CVI_SUCCESS)
        {
            fprintf(stderr, "[camera] neutral RTSP chroma allocation failed\n");
            return false;
        }
        // 128/128 is neutral chroma. It is initialized once and never
        // modified in the frame loop, so the encoded image remains luma-only.
        std::memset(ctx.rtsp_chroma_vir, 128, ctx.rtsp_chroma_len);
        CVI_SYS_IonFlushCache(ctx.rtsp_chroma_phy, ctx.rtsp_chroma_vir,
                              ctx.rtsp_chroma_len);
    }

    VI_VPSS_MODE_S vi_vpss_mode{};
    vi_vpss_mode.aenMode[0] = VI_OFFLINE_VPSS_ONLINE;
    CVI_SYS_SetVIVPSSMode(&vi_vpss_mode);

    if (SAMPLE_PLAT_VI_INIT(&ctx.vi_config) != CVI_SUCCESS)
    {
        fprintf(stderr, "[camera] VI init failed\n");
        return false;
    }

    ISP_PUB_ATTR_S pub_attr{};
    CVI_ISP_GetPubAttr(0, &pub_attr);
    pub_attr.f32FrameRate = 30;
    CVI_ISP_SetPubAttr(0, &pub_attr);

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

    // One VPSS group (on device 1, see above), one channel: the full sensor
    // frame scaled to kDetWidth x kDetHeight and converted to grayscale, both
    // in hardware. The remaining 2x resize to the 640x360 network input stays
    // in TinyTagDet::pre_process(), so crop-decode still reads 1280x720
    // pixels rather than the network's 640x360.
    //
    // Verified on hardware: VPSS accepts YUV_400 as a channel output with
    // NV21 group input, and returns a single W*H-byte luma plane.
    VPSS_GRP_ATTR_S vpss_grp_attr{};
    VPSS_GRP_DEFAULT_HELPER2(&vpss_grp_attr, ctx.width, ctx.height, VI_PIXEL_FORMAT, /*dev=*/1);
    VPSS_CHN_ATTR_S vpss_chn_attr{};
    VPSS_CHN_DEFAULT_HELPER(&vpss_chn_attr, kDetWidth, kDetHeight, PIXEL_FORMAT_YUV_400, CVI_FALSE);

    CVI_S32 vpss_ret = CVI_VPSS_CreateGrp(kVpssGrp, &vpss_grp_attr);
    if (vpss_ret == CVI_SUCCESS)
        vpss_ret = CVI_VPSS_ResetGrp(kVpssGrp);
    if (vpss_ret == CVI_SUCCESS)
        vpss_ret = CVI_VPSS_SetChnAttr(kVpssGrp, kVpssChn, &vpss_chn_attr);
    if (vpss_ret == CVI_SUCCESS)
        vpss_ret = CVI_VPSS_EnableChn(kVpssGrp, kVpssChn);
    if (vpss_ret == CVI_SUCCESS && ctx.preview && !ctx.preview_luma)
    {
        VPSS_CHN_ATTR_S preview_attr{};
        VPSS_CHN_DEFAULT_HELPER(&preview_attr, kDetWidth, kDetHeight, VI_PIXEL_FORMAT, CVI_FALSE);
        vpss_ret = CVI_VPSS_SetChnAttr(kVpssGrp, kPreviewChn, &preview_attr);
        if (vpss_ret == CVI_SUCCESS)
            vpss_ret = CVI_VPSS_EnableChn(kVpssGrp, kPreviewChn);
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

    // After the bind, matching SAMPLE_TDL_Init_WM's order (VPSS start ->
    // bind VI -> attach VB pools).
    vpss_ret = CVI_VPSS_AttachVbPool(kVpssGrp, kVpssChn, kDetPool);
    if (vpss_ret == CVI_SUCCESS && ctx.preview && !ctx.preview_luma)
        vpss_ret = CVI_VPSS_AttachVbPool(kVpssGrp, kPreviewChn, kPreviewPool);
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

void draw_box(cv::Mat &y, cv::Mat &vu, const cv::Rect &r, Nv21Color c, int thickness)
{
    cv::rectangle(y, r, cv::Scalar(c.y), thickness);
    cv::Rect half(r.x / 2, r.y / 2, std::max(1, r.width / 2), std::max(1, r.height / 2));
    cv::rectangle(vu, half, cv::Scalar(c.v, c.u), std::max(1, thickness / 2));
}

// Dark outline under light text so labels read on any background.
void draw_label(cv::Mat &y, const std::string &text, cv::Point org, double scale)
{
    cv::putText(y, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0), 4);
    cv::putText(y, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(255), 2);
}

// Encode one NV21 frame and hand the bitstream to the RTSP server. Same
// sequence as SAMPLE_TDL_Send_Frame_RTSP in tdl_sdk's middleware_utils.c.
void send_to_rtsp(CameraContext &ctx, VIDEO_FRAME_INFO_S &frame)
{
    if (CVI_VENC_SendFrame(kVencChn, &frame, 2000) != CVI_SUCCESS)
        return;
    VENC_CHN_STATUS_S status{};
    if (CVI_VENC_QueryStatus(kVencChn, &status) != CVI_SUCCESS || status.u32CurPacks == 0)
        return;

    if (status.u32CurPacks > ctx.venc_packs.size())
    {
        fprintf(stderr, "[rtsp] too many VENC packs: %u\n", status.u32CurPacks);
        return;
    }
    VENC_STREAM_S stream{};
    stream.pstPack = ctx.venc_packs.data();
    if (CVI_VENC_GetStream(kVencChn, &stream, 2000) != CVI_SUCCESS)
        return;

    CVI_RTSP_DATA data{};
    data.blockCnt = stream.u32PackCount;
    for (CVI_U32 i = 0; i < stream.u32PackCount; ++i)
    {
        data.dataPtr[i] = stream.pstPack[i].pu8Addr + stream.pstPack[i].u32Offset;
        data.dataLen[i] = stream.pstPack[i].u32Len - stream.pstPack[i].u32Offset;
    }
    CVI_RTSP_WriteFrame(ctx.rtsp, ctx.session->video, &data);
    CVI_VENC_ReleaseStream(kVencChn, &stream);
}

bool enqueue_luma_rtsp(CameraContext &ctx, const VIDEO_FRAME_INFO_S &source,
                       const VIDEO_FRAME_INFO_S &encoded)
{
    LumaRtspQueue &queue = ctx.luma_rtsp_queue;
    std::unique_lock<std::mutex> lock(queue.mutex);
    queue.not_full.wait(lock, [&queue] { return queue.count < kLumaRtspQueueDepth || queue.stopping; });
    if (queue.stopping)
        return false;

    LumaRtspItem &item = queue.items[queue.tail];
    item.source = source;
    item.encoded = encoded;
    queue.tail = (queue.tail + 1) % kLumaRtspQueueDepth;
    ++queue.count;
    lock.unlock();
    queue.not_empty.notify_one();
    return true;
}

void luma_rtsp_loop(CameraContext *ctx)
{
    LumaRtspQueue &queue = ctx->luma_rtsp_queue;
    for (;;)
    {
        LumaRtspItem item;
        {
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.not_empty.wait(lock, [&queue] { return queue.count != 0 || queue.stopping; });
            if (queue.count == 0 && queue.stopping)
                return;

            item = queue.items[queue.head];
            queue.head = (queue.head + 1) % kLumaRtspQueueDepth;
            --queue.count;
        }
        queue.not_full.notify_one();

        send_to_rtsp(*ctx, item.encoded);
        CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &item.source);
    }
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
    ctx.luma_rtsp_queue.not_full.notify_all();
    ctx.luma_rtsp_worker.join();
}

// Pulls preview frames, draws the newest detections on them, and streams.
// Runs beside the detector loop, which only has to publish into g_overlay.
void preview_loop(CameraContext *ctx)
{
    std::vector<Proposal> proposals;
    std::vector<TinyTagResult> tags;
    while (!g_stop)
    {
        VIDEO_FRAME_INFO_S frame{};
        if (CVI_VPSS_GetChnFrame(kVpssGrp, kPreviewChn, &frame, 1000) != CVI_SUCCESS)
            continue;

        // Map both NV21 planes in one span (they are contiguous, but compute
        // the plane-1 offset from the physical addresses rather than assume).
        const VIDEO_FRAME_S &vf = frame.stVFrame;
        const CVI_U64 base = vf.u64PhyAddr[0];
        const CVI_U32 span = static_cast<CVI_U32>(vf.u64PhyAddr[1] + vf.u32Length[1] - base);
        uint8_t *mem = static_cast<uint8_t *>(CVI_SYS_MmapCache(base, span));
        if (mem == nullptr)
        {
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, kPreviewChn, &frame);
            continue;
        }
        CVI_SYS_IonInvalidateCache(base, mem, span);

        cv::Mat y(vf.u32Height, vf.u32Width, CV_8UC1, mem, vf.u32Stride[0]);
        cv::Mat vu(vf.u32Height / 2, vf.u32Width / 2, CV_8UC2,
                   mem + (vf.u64PhyAddr[1] - base), vf.u32Stride[1]);

        double fps, busy_ms;
        {
            std::lock_guard<std::mutex> lock(g_overlay.mutex);
            proposals = g_overlay.proposals;
            tags = g_overlay.tags;
            fps = g_overlay.fps;
            busy_ms = g_overlay.busy_ms;
        }
        for (const auto &p : proposals)
        {
            draw_box(y, vu, p.roi, kProposalColor, 2);
            const int label_x = std::max(0, static_cast<int>(p.roi.x));
            const int label_y = p.roi.y >= 28.0f
                                    ? static_cast<int>(p.roi.y) - 8
                                    : std::min(static_cast<int>(vf.u32Height) - 4,
                                               static_cast<int>(p.roi.y + p.roi.height) + 22);
            char label[32];
            snprintf(label, sizeof(label), "%.2f", p.confidence);
            draw_label(y, label, cv::Point(label_x, std::max(20, label_y)), 0.65);
        }
        for (const auto &t : tags)
        {
            cv::Rect r = t.roi;
            draw_box(y, vu, r, kTagColor, 6);
            char label[48];
            snprintf(label, sizeof(label), "id %d %.2f", t.id, t.proposal_confidence);
            draw_label(y, label, cv::Point(std::max(0, r.x), std::max(24, r.y - 8)), 0.9);
        }
        char status[96];
        snprintf(status, sizeof(status), "tinytag %.1f fps  %.1f ms  %zu prop  %zu tags", fps, busy_ms,
                 proposals.size(), tags.size());
        draw_label(y, status, cv::Point(16, 40), 0.9);

        // We wrote through a cached mapping; flush so VENC, which reads the
        // physical buffer via DMA, sees the boxes.
        CVI_SYS_IonFlushCache(base, mem, span);
        CVI_SYS_Munmap(mem, span);

        send_to_rtsp(*ctx, frame);
        CVI_VPSS_ReleaseChnFrame(kVpssGrp, kPreviewChn, &frame);
    }
}

// Luma-only overlay for --rtsp-luma. The detector frame is sent directly to
// VENC; only its Y plane is modified. The neutral chroma buffer allocated at
// setup remains untouched and is reused for every encoded frame.
void draw_luma_overlay(cv::Mat &y, const std::vector<Proposal> &proposals,
                       const std::vector<TinyTagResult> &tags, double fps, double busy_ms)
{
    for (const auto &p : proposals)
    {
        cv::rectangle(y, p.roi, cv::Scalar(210), 2);
        const int label_x = std::max(0, static_cast<int>(p.roi.x));
        const int label_y = p.roi.y >= 28.0f
                                ? static_cast<int>(p.roi.y) - 8
                                : std::min(y.rows - 4,
                                           static_cast<int>(p.roi.y + p.roi.height) + 22);
        char label[32];
        snprintf(label, sizeof(label), "%.2f", p.confidence);
        draw_label(y, label, cv::Point(label_x, std::max(20, label_y)), 0.65);
    }
    for (const auto &t : tags)
    {
        const cv::Rect r = t.roi;
        cv::rectangle(y, r, cv::Scalar(145), 6);
        char label[48];
        snprintf(label, sizeof(label), "id %d %.2f", t.id, t.proposal_confidence);
        draw_label(y, label, cv::Point(std::max(0, r.x), std::max(24, r.y - 8)), 0.9);
    }
    char status[96];
    snprintf(status, sizeof(status), "tinytag %.1f fps  %.1f ms  %zu prop  %zu tags", fps, busy_ms,
             proposals.size(), tags.size());
    draw_label(y, status, cv::Point(16, 40), 0.9);
}

void teardown_camera(CameraContext &ctx)
{
    if (ctx.preview)
    {
        if (ctx.rtsp)
        {
            CVI_RTSP_Stop(ctx.rtsp);
            if (ctx.session)
                CVI_RTSP_DestroySession(ctx.rtsp, ctx.session);
            CVI_RTSP_Destroy(&ctx.rtsp);
        }
        SAMPLE_COMM_VENC_Stop(kVencChn);
    }

    SAMPLE_COMM_VI_DestroyIsp(&ctx.vi_config);
    SAMPLE_COMM_VI_DestroyVi(&ctx.vi_config);

    CVI_BOOL chn_enable[VPSS_MAX_PHY_CHN_NUM + 1] = {0};
    chn_enable[kVpssChn] = CVI_TRUE;
    if (ctx.preview && !ctx.preview_luma)
        chn_enable[kPreviewChn] = CVI_TRUE;
    SAMPLE_COMM_VPSS_Stop(kVpssGrp, chn_enable);

    if (ctx.rtsp_chroma_vir)
    {
        CVI_SYS_IonFree(ctx.rtsp_chroma_phy, ctx.rtsp_chroma_vir);
        ctx.rtsp_chroma_phy = 0;
        ctx.rtsp_chroma_vir = nullptr;
    }

    CVI_SYS_Exit();
    CVI_VB_Exit();
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

    CameraContext ctx;
    ctx.preview = rtsp;
    ctx.preview_luma = rtsp_luma;
    if (!setup_camera(ctx))
        return 1;

    pthread_t isp_thread;
    pthread_create(&isp_thread, nullptr, isp_control_thread, nullptr);
    pthread_detach(isp_thread);

    std::thread preview;
    if (ctx.preview && !ctx.preview_luma)
        preview = std::thread(preview_loop, &ctx);
    if (ctx.preview_luma)
        ctx.luma_rtsp_worker = std::thread(luma_rtsp_loop, &ctx);

    TinyTagDet detector(cvimodel_path, heatmap_thres, max_proposals, roi_expand, roi_iou_thres,
                        debug_mode);
    if (decode)
    {
        detector.set_decoder(make_aruco_nano_decoder(decode_tolerant));
        fprintf(stderr, "[camera] decoder: ArUco Nano, AprilTag 36h11, %s\n",
                decode_tolerant ? "tolerant" : "strict");
    }

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
        double wait = 0, map = 0, pre = 0, infer = 0, decode = 0, crop = 0, release = 0;
        size_t proposals = 0;
    } win;
    double fps_window_start = now_ms();
    double overlay_fps = 0.0;
    double overlay_busy_ms = 0.0;

    fprintf(stderr, "[camera] capture started: sensor %ux%u -> VPSS %ux%u %s -> tinytag_detect\n",
            ctx.width, ctx.height, kDetWidth, kDetHeight,
            ctx.preview_luma ? "YUV_400 + direct RTSP Y" : "YUV_400");

    bool logged_first_frame = false;
    constexpr int kSaveFrameIndex = 30;
    int frames_seen = 0;
    while (!g_stop)
    {
        VIDEO_FRAME_INFO_S frame{};
        const double t_wait = now_ms();
        CVI_S32 ret = CVI_VPSS_GetChnFrame(kVpssGrp, kVpssChn, &frame, 1000);
        const double t_got = now_ms();
        if (ret != CVI_SUCCESS)
        {
            // The first call or two time out (0xc006800e) while the ISP is
            // still starting up; harmless.
            fprintf(stderr, "[camera] CVI_VPSS_GetChnFrame failed: %#x\n", ret);
            continue;
        }

        // GetChnFrame returns *physical* addresses only -- pu8VirAddr is left
        // NULL. The caller maps the plane itself, then invalidates the CPU
        // cache so it sees what VPSS just wrote via DMA. Same sequence as the
        // SDK's own frame-dump code (cvi_mpi/sample/sensor_test/
        // sample_sensor_test.c, "dump vi yuv frame").
        //
        // MmapCache, not Mmap: the plain variant is a non-cached mapping, and
        // every CPU read through it is slow. Measured on a Duo S at 1080p:
        // pre_process 20-23 ms -> 2.1 ms, 8-ROI crop-decode 11-13 ms -> ~5 ms.
        const VIDEO_FRAME_S &vf = frame.stVFrame;
        const CVI_U32 map_len = vf.u32Length[0] ? vf.u32Length[0] : vf.u32Stride[0] * vf.u32Height;
        uint8_t *luma = static_cast<uint8_t *>(CVI_SYS_MmapCache(vf.u64PhyAddr[0], map_len));
        if (luma == nullptr)
        {
            fprintf(stderr, "[camera] CVI_SYS_Mmap failed for %#llx (%u bytes)\n",
                    (unsigned long long)vf.u64PhyAddr[0], map_len);
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &frame);
            continue;
        }
        CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], luma, map_len);
        const double t_mapped = now_ms();

        // Wrap the mapped plane without copying. u32Stride may exceed width
        // (alignment padding), so the Mat uses the real stride.
        cv::Mat gray(vf.u32Height, vf.u32Width, CV_8UC1, luma, vf.u32Stride[0]);

        if (!logged_first_frame)
        {
            fprintf(stderr, "[camera] first frame: %ux%u stride=%u len=%u fmt=%d\n", vf.u32Width,
                    vf.u32Height, vf.u32Stride[0], map_len, vf.enPixelFormat);
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

        detector.detect(gray, proposals);
        if (decode)
            detector.post_process(gray, proposals, results);
        const double t_detected = now_ms();

        VIDEO_FRAME_INFO_S rtsp_frame{};
        if (ctx.preview_luma)
        {
            draw_luma_overlay(gray, proposals, results, overlay_fps, overlay_busy_ms);
            CVI_SYS_IonFlushCache(vf.u64PhyAddr[0], luma, map_len);

            // Reuse the detector frame's Y plane directly. Only the frame
            // descriptor is copied; the neutral chroma plane was allocated
            // and initialized once during setup.
            rtsp_frame = frame;
            VIDEO_FRAME_S &rf = rtsp_frame.stVFrame;
            rf.enPixelFormat = PIXEL_FORMAT_NV21;
            rf.u32Stride[1] = ctx.rtsp_chroma_stride;
            rf.u32Length[1] = ctx.rtsp_chroma_len;
            rf.u64PhyAddr[1] = ctx.rtsp_chroma_phy;
            rf.pu8VirAddr[1] = static_cast<CVI_U8 *>(ctx.rtsp_chroma_vir);
            rf.u32Stride[2] = 0;
            rf.u32Length[2] = 0;
            rf.u64PhyAddr[2] = 0;
            rf.pu8VirAddr[2] = nullptr;
        }

        // The luma RTSP worker owns the frame until VENC has finished with it;
        // the normal path releases it immediately after detection.
        CVI_SYS_Munmap(luma, map_len);
        if (ctx.preview_luma)
        {
            if (!enqueue_luma_rtsp(ctx, frame, rtsp_frame))
                CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &frame);
        }
        else
        {
            CVI_VPSS_ReleaseChnFrame(kVpssGrp, kVpssChn, &frame);
        }
        const double t_released = now_ms();

        ++win.frames;
        win.wait += t_got - t_wait;
        win.map += t_mapped - t_got;
        win.pre += detector.last_preprocess_ms();
        win.infer += detector.last_inference_ms();
        win.decode += detector.last_decode_ms();
        win.crop += decode ? detector.last_crop_decode_ms() : 0.0;
        win.release += t_released - t_detected;
        win.proposals += proposals.size();

        if (ctx.preview && !ctx.preview_luma)
        {
            std::lock_guard<std::mutex> lock(g_overlay.mutex);
            g_overlay.proposals = proposals;
            if (decode)
                g_overlay.tags = results;
        }

        for (const auto &r : results)
            fprintf(stdout, "[tag] id=%d confidence=%.2f roi=(%.0f,%.0f,%.0fx%.0f)\n", r.id,
                    r.proposal_confidence, r.roi.x, r.roi.y, r.roi.width, r.roi.height);
        if (!results.empty())
            fflush(stdout);

        const double t_now = now_ms();
        if (t_now - fps_window_start >= 1000.0)
        {
            const double n = static_cast<double>(win.frames);
            const double busy = (win.map + win.pre + win.infer + win.decode + win.crop + win.release) / n;
            fprintf(stderr,
                    "[camera] %.1f fps | per frame ms: wait %.2f map %.2f pre %.2f infer %.2f "
                    "decode %.2f crop %.2f release %.2f = busy %.2f | %.1f proposals\n",
                    n * 1000.0 / (t_now - fps_window_start), win.wait / n, win.map / n, win.pre / n,
                    win.infer / n, win.decode / n, win.crop / n, win.release / n, busy,
                    win.proposals / n);
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
            win = StageTotals{};
            fps_window_start = t_now;
        }
    }

    fprintf(stderr, "[camera] stopping\n");
    if (preview.joinable())
        preview.join();
    stop_luma_rtsp(ctx);
    teardown_camera(ctx);
    return 0;
}
