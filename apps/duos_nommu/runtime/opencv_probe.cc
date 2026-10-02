// Local-buffer correctness and timing: no mailbox or live camera dependency.
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
// Match this probe's OpenCV 4.12 normalizer; production reference stays at 16.
#define TT_THRESHOLD_FIXED_SHIFT 23u
#include "../../common/roi_threshold.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#ifdef DUOS_HARDFLOAT
#define DUOS_ABI_LABEL "RV64IMAFDC LP64D hardware FP"
#else
#define DUOS_ABI_LABEL "RV64IMAC LP64 software FP"
#endif
static int logfd;
static unsigned long long ns()
{
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t)) throw std::runtime_error("clock_gettime");
    return (unsigned long long)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void opencv(const cv::Mat& in, cv::Mat& out, int k, int t)
{
    // Exact existing A53 detector/benchmark operations, not adaptiveThreshold.
    cv::boxFilter(in, out, in.type(), cv::Size(k,k), cv::Point(-1,-1), true,
                  cv::BORDER_REPLICATE | cv::BORDER_ISOLATED);
    out = out - in;
    cv::threshold(out, out, t, 255, cv::THRESH_BINARY);
}
static void scalar(const cv::Mat& in, cv::Mat& out, int k, int t)
{
    out.create(in.size(), CV_8UC1);
    std::array<uint32_t, TT_THRESHOLD_MAX_WIDTH> columns;
    if (tt_roi_threshold(in.ptr<uint8_t>(), in.step, out.ptr<uint8_t>(), out.step,
                         in.cols, in.rows, k, t, columns.data()))
        throw std::runtime_error("scalar rejected input");
}
static cv::Mat synthetic(int w, int h, int pattern)
{
    cv::Mat parent(h+4, w+8, CV_8UC1, cv::Scalar(173));
    cv::Mat in = parent(cv::Rect(3,2,w,h));
    uint32_t state = 0x92811234;
    for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) {
        state ^= state<<13; state ^= state>>17; state ^= state<<5;
        unsigned value = state & 255;
        if (pattern==0) value=0;
        if (pattern==1) value=255;
        if (pattern==2) value=((x/8+y/8)&1) ? 240 : 12;
        if (pattern==3) value=125+(state%7);
        in.at<uint8_t>(y,x)=value;
    }
    return in;
}
static void same(const cv::Mat& a, const cv::Mat& b, int k, int t, int pattern)
{
    for (int y=0; y<a.rows; ++y) for (int x=0; x<a.cols; ++x)
        if (a.at<uint8_t>(y,x)!=b.at<uint8_t>(y,x)) {
            dprintf(logfd, "<3>duos-opencv: mismatch %dx%d k=%d t=%d pattern=%d x=%d y=%d cv=%u scalar=%u\n",
                    a.cols,a.rows,k,t,pattern,x,y,a.at<uint8_t>(y,x),b.at<uint8_t>(y,x));
            throw std::runtime_error("output differs; STOP, timings invalid");
        }
}
int main()
{
    logfd = open("/dev/kmsg", O_WRONLY);
    if (logfd<0) return 1;
    bool passed=false;
    try {
        cv::setNumThreads(1);
        dprintf(logfd,"<6>duos-opencv: begin OpenCV=%s\n",CV_VERSION);
        dprintf(logfd,"<6>duos-opencv: scalar fixed-point shift=%u (OpenCV 4.12)\n",TT_THRESHOLD_FIXED_SHIFT);
        dprintf(logfd,"<6>duos-opencv: ABI %s\n",DUOS_ABI_LABEL);
        const cv::Size validation[]={{1,1},{1,19},{19,1},{2,3},{7,9},{31,17},{201,99},{640,360},{1280,800}};
        unsigned cases=0;
        for (auto size: validation) {
            for (int k: {1,3,5,15,31,63}) for (int t: {-1,0,1,3,127,254,255})
                for (int pattern=0; pattern<5; ++pattern) {
                    auto in=synthetic(size.width,size.height,pattern); cv::Mat a,b;
                    opencv(in,a,k,t); scalar(in,b,k,t); same(a,b,k,t,pattern); ++cases;
                }
            dprintf(logfd,"<6>duos-opencv: validation progress cases=%u size=%dx%d\n",cases,size.width,size.height);
        }
        dprintf(logfd,"<6>duos-opencv: validation PASS cases=%u; timing 100 samples, 5 warmup, k=15 t=3\n",cases);
        const cv::Size sizes[]={{32,32},{64,64},{128,128},{200,200},{320,240},{640,360},{1280,800}};
        for (auto size: sizes) {
            auto in=synthetic(size.width,size.height,4); cv::Mat reference,out;
            opencv(in,reference,15,3);
            for (int mode=0; mode<2; ++mode) {
                std::array<unsigned long long,100> times{};
                unsigned long long sum=0;
                for (int i=-5; i<100; ++i) {
                    auto start=ns();
                    if (mode==0) opencv(in,out,15,3); else scalar(in,out,15,3);
                    auto elapsed=ns()-start;
                    same(reference,out,15,3,4);
                    if (i>=0) { times[i]=elapsed; sum+=elapsed; }
                }
                std::sort(times.begin(),times.end());
                // Keep both records short: the captured long records lost percentile tails.
                dprintf(logfd,"<6>duos-opencv: timing %s %dx%d n=100\n",
                        mode==0?"opencv":"scalar-c906l",size.width,size.height);
                dprintf(logfd,"<6>duos-opencv: mean_ns=%llu p50_ns=%llu p95_ns=%llu\n",
                        sum/100,times[49],times[94]);
            }
        }
        passed=true;
        dprintf(logfd,"<6>duos-opencv: DONE PASS; local compute only, no handover timing\n");
    } catch (const std::exception& e) {
        dprintf(logfd,"<3>duos-opencv: FAIL %s\n",e.what());
    }
    for (;;) { sleep(10); dprintf(logfd,"<6>duos-opencv: alive result=%s\n",passed?"PASS":"FAIL"); }
}
