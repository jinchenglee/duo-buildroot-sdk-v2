#ifndef DUO_APPS_COMMON_SW_LDC_H
#define DUO_APPS_COMMON_SW_LDC_H

#include "ldc_config.h"

#include <opencv2/core/core.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// CPU lens-distortion correction from the calibration's full OpenCV model
// (camera matrix plus distortion coefficients), as an alternative to the
// VPSS/GDC path and its one-ratio radial fit.
//
// The output keeps the calibrated camera matrix, scaled to the frame size, so
// image-centre scale is unchanged and corrected pixels follow a zero-distortion
// pinhole model with camera_matrix(). This is cv::undistort's default view:
// barrel correction trims the outer source field instead of adding black
// corners.
//
// The board's OpenCV 3.2 cv::remap measured about 74 ms per 1280x720 frame,
// and a dense per-pixel table would stream 5 MB per frame. Instead, source
// coordinates are stored on a 16-pixel mesh (about 30 KB) and interpolated
// in 16.16 fixed point: exact bilinear across rows, then constant steps along
// each 16-pixel output run. max_mesh_error_px() checks this against OpenCV's
// dense map.
//
// Sampling uses 7-bit weights (1/128 px; OpenCV's fixed-point remap uses
// 1/32). On AArch64 a full 16-pixel run whose taps fit a 32-byte window over
// at most three source rows is gathered with NEON table lookups, which avoids
// the in-order A53 stalling on one dependent load chain per pixel. Other runs
// use scalar code with identical arithmetic.
class SoftwareLdc
{
public:
    enum class Interp
    {
        Linear,
        Nearest,
    };

    bool init(const AppLdcConfig &config, int width, int height, Interp interp,
              std::string &error);

    // Both planes are width x height 8-bit luma and must not overlap.
    // Coordinates that fall outside the source replicate its edge.
    void apply(const uint8_t *src, size_t src_stride, uint8_t *dst, size_t dst_stride) const;

    // Largest distance, in source pixels, between the interpolated mesh
    // coordinates and cv::initUndistortRectifyMap. Startup diagnostic only.
    double max_mesh_error_px() const;

    // Self-test control: false forces the portable scalar path, which
    // produces bit-identical output to the NEON path.
    void set_simd(bool enabled) { simd_ = enabled; }
    static bool simd_available();

    const cv::Matx33d &camera_matrix() const { return camera_; }
    Interp interp() const { return interp_; }
    size_t table_bytes() const { return (node_x_.size() + node_y_.size()) * sizeof(int32_t); }

private:
    cv::Matx33d camera_;
    std::vector<double> distortion_;
    std::vector<int32_t> node_x_; // 16.16 source x at each mesh node
    std::vector<int32_t> node_y_;
    int width_ = 0;
    int height_ = 0;
    int node_cols_ = 0;
    Interp interp_ = Interp::Linear;
    bool simd_ = true;
};

#endif
