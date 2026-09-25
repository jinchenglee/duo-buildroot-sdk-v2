#include "sw_ldc.h"

#include <opencv2/imgproc/imgproc.hpp>

#include <algorithm>
#include <cmath>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace
{

constexpr int kCellShift = 4;
constexpr int kCell = 1 << kCellShift;
constexpr int kFracBits = 16;
constexpr double kFracScale = 1 << kFracBits;

// One output run: the source coordinate of its first pixel and the per-pixel
// step, all 16.16 fixed point. Coordinates are bilinear in the mesh cell,
// hence exactly linear along the run.
struct Run
{
    int32_t x, y, step_x, step_y;
};

inline Run mesh_run(const int32_t *nx, const int32_t *ny, int cols, int v, int gx)
{
    const int gy = v >> kCellShift;
    const int64_t wb = v & (kCell - 1);
    const int64_t wa = kCell - wb;
    const size_t a = static_cast<size_t>(gy) * cols + gx;
    const size_t b = a + cols;
    const int32_t lx = static_cast<int32_t>((nx[a] * wa + nx[b] * wb) >> kCellShift);
    const int32_t rx = static_cast<int32_t>((nx[a + 1] * wa + nx[b + 1] * wb) >> kCellShift);
    const int32_t ly = static_cast<int32_t>((ny[a] * wa + ny[b] * wb) >> kCellShift);
    const int32_t ry = static_cast<int32_t>((ny[a + 1] * wa + ny[b + 1] * wb) >> kCellShift);
    return Run{lx, ly, (rx - lx) >> kCellShift, (ry - ly) >> kCellShift};
}

// 7-bit bilinear sample, shared by every path so their output is identical.
// Each horizontal pass is rounded to 8 bits before the vertical pass, exactly
// as the NEON vrshrn steps do. wx and wy are 0..128.
inline uint8_t bilinear7(const uint8_t *p, size_t stride, int wx, int wy)
{
    const int top = (p[0] * (128 - wx) + p[1] * wx + 64) >> 7;
    const int bottom = (p[stride] * (128 - wx) + p[stride + 1] * wx + 64) >> 7;
    return static_cast<uint8_t>((top * (128 - wy) + bottom * wy + 64) >> 7);
}

// Clamp a 16.16 coordinate so both bilinear taps stay inside [0, size-1].
// Returns the left/top tap and a 0..128 weight for the right/bottom tap.
inline void clamp_tap(int32_t s, int size, int &i0, int &w)
{
    const int32_t max_s = (size - 1) << kFracBits;
    s = std::min(std::max(s, 0), max_s);
    i0 = s >> kFracBits;
    w = (s >> (kFracBits - 7)) & 127;
    if (i0 >= size - 1)
    {
        i0 = size - 2;
        w = 128;
    }
}

#if defined(__aarch64__) && defined(__ARM_NEON)

// Sixteen lanes of ((start + i * step) >> 9) - base_q7 as int16: source
// coordinates in 1/128 px relative to the run's gather window.
inline int16x8x2_t q7_lanes(int32_t start, int32_t step, int32_t base_q7)
{
    const int32x4_t lane = {0, 1, 2, 3};
    const int32x4_t step4 = vdupq_n_s32(4 * step);
    const int32x4_t base = vdupq_n_s32(base_q7);
    const int32x4_t c0 = vmlaq_n_s32(vdupq_n_s32(start), lane, step);
    const int32x4_t c1 = vaddq_s32(c0, step4);
    const int32x4_t c2 = vaddq_s32(c1, step4);
    const int32x4_t c3 = vaddq_s32(c2, step4);
    int16x8x2_t out;
    out.val[0] = vcombine_s16(vmovn_s32(vsubq_s32(vshrq_n_s32(c0, 9), base)),
                              vmovn_s32(vsubq_s32(vshrq_n_s32(c1, 9), base)));
    out.val[1] = vcombine_s16(vmovn_s32(vsubq_s32(vshrq_n_s32(c2, 9), base)),
                              vmovn_s32(vsubq_s32(vshrq_n_s32(c3, 9), base)));
    return out;
}

inline uint8x16_t narrow16(int16x8x2_t v)
{
    return vcombine_u8(vmovn_u16(vreinterpretq_u16_s16(v.val[0])),
                       vmovn_u16(vreinterpretq_u16_s16(v.val[1])));
}

// Gather 16 bytes by index from one 32-byte row window, or from two stacked
// windows (index = row * 32 + column).
inline uint8x16_t gather(const uint8_t *row, size_t stride, bool two_rows, uint8x16_t index)
{
    if (!two_rows)
    {
        uint8x16x2_t t;
        t.val[0] = vld1q_u8(row);
        t.val[1] = vld1q_u8(row + 16);
        return vqtbl2q_u8(t, index);
    }
    uint8x16x4_t t;
    t.val[0] = vld1q_u8(row);
    t.val[1] = vld1q_u8(row + 16);
    t.val[2] = vld1q_u8(row + stride);
    t.val[3] = vld1q_u8(row + stride + 16);
    return vqtbl4q_u8(t, index);
}

inline uint8x16_t lerp7(uint8x16_t a, uint8x16_t b, uint8x16_t w)
{
    const uint8x16_t iw = vsubq_u8(vdupq_n_u8(128), w);
    const uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(a), vget_low_u8(iw)),
                                   vget_low_u8(b), vget_low_u8(w));
    const uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(a), vget_high_u8(iw)),
                                   vget_high_u8(b), vget_high_u8(w));
    return vcombine_u8(vrshrn_n_u16(lo, 7), vrshrn_n_u16(hi, 7));
}

// One full 16-pixel run. Returns false, writing nothing, when its taps do not
// fit the gather window; the caller then uses the scalar path.
inline bool run_neon(const uint8_t *src, size_t stride, int width, int height, bool nearest,
                     const Run &run, uint8_t *out)
{
    const int32_t last_x = run.x + run.step_x * (kCell - 1);
    const int32_t last_y = run.y + run.step_y * (kCell - 1);
    const int32_t round = nearest ? 1 << (kFracBits - 1) : 0;
    const int x_lo = (std::min(run.x, last_x) + round) >> kFracBits;
    const int x_hi = (std::max(run.x, last_x) + round) >> kFracBits;
    const int y_lo = (std::min(run.y, last_y) + round) >> kFracBits;
    const int y_hi = (std::max(run.y, last_y) + round) >> kFracBits;
    // Linear reads one tap to the right and one row below each coordinate.
    const int tap = nearest ? 0 : 1;
    if (x_lo < 0 || y_lo < 0 || x_lo + 32 > width || x_hi + tap - x_lo > 31 ||
        y_hi + tap > height - 1 || y_hi - y_lo > 1)
        return false;

    int16x8x2_t rx = q7_lanes(run.x, run.step_x, x_lo << 7);
    int16x8x2_t ry = q7_lanes(run.y, run.step_y, y_lo << 7);
    if (nearest)
    {
        const int16x8_t half = vdupq_n_s16(64);
        for (int k = 0; k < 2; ++k)
        {
            rx.val[k] = vaddq_s16(rx.val[k], half);
            ry.val[k] = vaddq_s16(ry.val[k], half);
        }
    }
    int16x8x2_t index, wx, wy;
    const int16x8_t frac = vdupq_n_s16(127);
    for (int k = 0; k < 2; ++k)
    {
        index.val[k] = vaddq_s16(vshlq_n_s16(vshrq_n_s16(ry.val[k], 7), 5),
                                 vshrq_n_s16(rx.val[k], 7));
        wx.val[k] = vandq_s16(rx.val[k], frac);
        wy.val[k] = vandq_s16(ry.val[k], frac);
    }
    const uint8x16_t idx = narrow16(index);
    const bool two_rows = y_hi != y_lo;
    const uint8_t *row = src + static_cast<size_t>(y_lo) * stride + x_lo;

    if (nearest)
    {
        vst1q_u8(out, gather(row, stride, two_rows, idx));
        return true;
    }

    const uint8x16_t idx_right = vaddq_u8(idx, vdupq_n_u8(1));
    const uint8x16_t top = lerp7(gather(row, stride, two_rows, idx),
                                 gather(row, stride, two_rows, idx_right), narrow16(wx));
    const uint8_t *below = row + stride;
    const uint8x16_t bottom = lerp7(gather(below, stride, two_rows, idx),
                                    gather(below, stride, two_rows, idx_right), narrow16(wx));
    vst1q_u8(out, lerp7(top, bottom, narrow16(wy)));
    return true;
}

#endif

} // namespace

bool SoftwareLdc::init(const AppLdcConfig &config, int width, int height, Interp interp,
                       std::string &error)
{
    if (!config.has_opencv_model)
    {
        error = "software LDC needs camera_matrix and distortion_coefficients in the calibration JSON";
        return false;
    }
    if (width < 2 || height < 2)
    {
        error = "software LDC frame is too small";
        return false;
    }
    const double aspect_in = static_cast<double>(config.calibration_width) /
                             config.calibration_height;
    const double aspect_out = static_cast<double>(width) / height;
    if (std::fabs(aspect_in - aspect_out) > 0.005 * aspect_out)
    {
        error = "calibration aspect does not match the software LDC frame size";
        return false;
    }
    std::vector<double> d = config.distortion;
    if (d.size() == 14 && (d[12] != 0.0 || d[13] != 0.0))
    {
        error = "software LDC does not support the tilted-sensor distortion terms";
        return false;
    }
    distortion_ = d;
    d.resize(12, 0.0);

    // Scale intrinsics from the calibration image to this frame. Principal
    // point uses pixel-centre coordinates, so offset by half a pixel.
    const double sx = static_cast<double>(width) / config.calibration_width;
    const double sy = static_cast<double>(height) / config.calibration_height;
    const double *k = config.camera_matrix;
    camera_ = cv::Matx33d(k[0] * sx, k[1] * sx, (k[2] + 0.5) * sx - 0.5,
                          k[3] * sy, k[4] * sy, (k[5] + 0.5) * sy - 0.5,
                          k[6], k[7], k[8]);

    width_ = width;
    height_ = height;
    interp_ = interp;
    node_cols_ = (width + kCell - 1) / kCell + 1;
    const int node_rows = (height + kCell - 1) / kCell + 1;
    node_x_.assign(static_cast<size_t>(node_cols_) * node_rows, 0);
    node_y_.assign(node_x_.size(), 0);

    // Same forward model as cv::initUndistortRectifyMap with R = I and the
    // unchanged camera matrix as the new one. Nodes past the last pixel
    // extrapolate the model so partial edge cells interpolate normally.
    const double fx = camera_(0, 0), fy = camera_(1, 1);
    const double cx = camera_(0, 2), cy = camera_(1, 2), skew = camera_(0, 1);
    const double k1 = d[0], k2 = d[1], p1 = d[2], p2 = d[3], k3 = d[4];
    const double k4 = d[5], k5 = d[6], k6 = d[7];
    const double s1 = d[8], s2 = d[9], s3 = d[10], s4 = d[11];
    const double limit = 1 << 14; // keeps 16.16 values and mesh sums in range
    for (int r = 0; r < node_rows; ++r)
    {
        for (int c = 0; c < node_cols_; ++c)
        {
            const double u = c * kCell, v = r * kCell;
            const double y = (v - cy) / fy;
            const double x = (u - cx - skew * y) / fx;
            const double x2 = x * x, y2 = y * y, xy2 = 2 * x * y;
            const double r2 = x2 + y2;
            const double radial = (1 + ((k3 * r2 + k2) * r2 + k1) * r2) /
                                  (1 + ((k6 * r2 + k5) * r2 + k4) * r2);
            const double xd = x * radial + p1 * xy2 + p2 * (r2 + 2 * x2) + s1 * r2 + s2 * r2 * r2;
            const double yd = y * radial + p1 * (r2 + 2 * y2) + p2 * xy2 + s3 * r2 + s4 * r2 * r2;
            const double su = std::min(std::max(fx * xd + cx, -limit), limit);
            const double sv = std::min(std::max(fy * yd + cy, -limit), limit);
            const size_t i = static_cast<size_t>(r) * node_cols_ + c;
            node_x_[i] = static_cast<int32_t>(std::lround(su * kFracScale));
            node_y_[i] = static_cast<int32_t>(std::lround(sv * kFracScale));
        }
    }
    return true;
}

bool SoftwareLdc::simd_available()
{
#if defined(__aarch64__) && defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
}

void SoftwareLdc::apply(const uint8_t *src, size_t src_stride, uint8_t *dst,
                        size_t dst_stride) const
{
    const int32_t *nx = node_x_.data();
    const int32_t *ny = node_y_.data();
    // Scalar fast-path bounds: both linear taps inside, so x0 <= width - 2.
    const int32_t max_x = (width_ - 1) << kFracBits;
    const int32_t max_y = (height_ - 1) << kFracBits;
    const bool nearest = interp_ == Interp::Nearest;
    constexpr int32_t kHalf = 1 << (kFracBits - 1);
#if defined(__aarch64__) && defined(__ARM_NEON)
    const bool simd = simd_;
#endif

    for (int v = 0; v < height_; ++v)
    {
        uint8_t *out = dst + static_cast<size_t>(v) * dst_stride;
        for (int gx = 0, u0 = 0; u0 < width_; ++gx, u0 += kCell)
        {
            const Run run = mesh_run(nx, ny, node_cols_, v, gx);
            const int n = std::min(kCell, width_ - u0);
            uint8_t *o = out + u0;
#if defined(__aarch64__) && defined(__ARM_NEON)
            if (simd && n == kCell &&
                run_neon(src, src_stride, width_, height_, nearest, run, o))
                continue;
#endif
            const int32_t end_x = run.x + run.step_x * (n - 1);
            const int32_t end_y = run.y + run.step_y * (n - 1);
            const bool inside = std::min(run.x, end_x) >= 0 && std::max(run.x, end_x) < max_x &&
                                std::min(run.y, end_y) >= 0 && std::max(run.y, end_y) < max_y;
            int32_t sx = run.x, sy = run.y;

            if (nearest)
            {
                for (int i = 0; i < n; ++i, sx += run.step_x, sy += run.step_y)
                {
                    int x = (sx + kHalf) >> kFracBits;
                    int y = (sy + kHalf) >> kFracBits;
                    if (!inside)
                    {
                        x = std::min(std::max(x, 0), width_ - 1);
                        y = std::min(std::max(y, 0), height_ - 1);
                    }
                    o[i] = src[static_cast<size_t>(y) * src_stride + x];
                }
                continue;
            }

            if (inside)
            {
                for (int i = 0; i < n; ++i, sx += run.step_x, sy += run.step_y)
                {
                    const uint8_t *p = src + static_cast<size_t>(sy >> kFracBits) * src_stride +
                                       (sx >> kFracBits);
                    o[i] = bilinear7(p, src_stride, (sx >> (kFracBits - 7)) & 127,
                                     (sy >> (kFracBits - 7)) & 127);
                }
                continue;
            }

            for (int i = 0; i < n; ++i, sx += run.step_x, sy += run.step_y)
            {
                int x0, y0, wx, wy;
                clamp_tap(sx, width_, x0, wx);
                clamp_tap(sy, height_, y0, wy);
                o[i] = bilinear7(src + static_cast<size_t>(y0) * src_stride + x0,
                                 src_stride, wx, wy);
            }
        }
    }
}

double SoftwareLdc::max_mesh_error_px() const
{
    cv::Mat map_x, map_y;
    cv::initUndistortRectifyMap(camera_, cv::Mat(distortion_, true), cv::noArray(), camera_,
                                cv::Size(width_, height_), CV_32FC1, map_x, map_y);
    double worst = 0.0;
    for (int v = 0; v < height_; ++v)
    {
        const float *mx = map_x.ptr<float>(v);
        const float *my = map_y.ptr<float>(v);
        for (int gx = 0, u0 = 0; u0 < width_; ++gx, u0 += kCell)
        {
            const Run run = mesh_run(node_x_.data(), node_y_.data(), node_cols_, v, gx);
            const int n = std::min(kCell, width_ - u0);
            for (int i = 0; i < n; ++i)
            {
                const double ex = (run.x + run.step_x * i) / kFracScale - mx[u0 + i];
                const double ey = (run.y + run.step_y * i) / kFracScale - my[u0 + i];
                worst = std::max(worst, std::sqrt(ex * ex + ey * ey));
            }
        }
    }
    return worst;
}
