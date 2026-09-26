#ifndef DUO_APPS_COMMON_POINT_LDC_H
#define DUO_APPS_COMMON_POINT_LDC_H

#include <opencv2/core/core.hpp>

#include <string>
#include <vector>

// Point-level lens-distortion correction: instead of remapping pixels, correct
// only the geometry a tag detector consumes. See docs/ldc-point-correction.md
// and tools/ldc_point_correction/ (ldcpt.point_ldc_detect is the Python
// reference for this code).
//
// Coordinates:
//   raw px    the uncorrected detector frame;
//   ideal px  the zero-distortion pinhole image with camera_matrix(), the same
//             view --ldc-mode sw renders.

// The calibration's OpenCV model (k1 k2 p1 p2 [k3]), scaled to the frame the
// detector sees. Forward distortion is closed form; undistortion is Newton's
// method with the analytic Jacobian.
class LensModel
{
public:
    // camera_matrix is row-major 3x3 at calibration_width x calibration_height.
    // Accepts 4 or 5 coefficients, or longer vectors whose extra terms are 0.
    bool init(const double camera_matrix[9], const std::vector<double> &distortion,
              int calibration_width, int calibration_height, int width, int height,
              std::string &error);

    // ideal px -> raw px.
    cv::Point2d distort(const cv::Point2d &ideal) const;

    // raw px -> ideal px. False outside the model's invertible range (past its
    // fold radius, where raw radius stops growing with ideal radius) or if the
    // iteration does not converge.
    bool undistort(const cv::Point2d &raw, cv::Point2d &ideal) const;

    const cv::Matx33d &camera_matrix() const { return camera_; }
    cv::Size size() const { return size_; }
    // Raw-pixel radius from the principal point beyond which undistort fails.
    double fold_radius_px() const { return fold_radius_px_; }

private:
    void distort_norm(double x, double y, double &xd, double &yd) const;

    cv::Matx33d camera_;
    cv::Size size_;
    double fx_ = 1, fy_ = 1, cx_ = 0, cy_ = 0, skew_ = 0;
    double k1_ = 0, k2_ = 0, p1_ = 0, p2_ = 0, k3_ = 0;
    double r2_ideal_max_ = 0; // squared normalized ideal radius at the fold
    double fold_radius_px_ = 0;
};

struct PointLdcParams
{
    int samples_per_edge = 8;
    double corner_margin = 0.15;  // fraction of each edge skipped at both ends
    double first_pass_cap = 8.0;  // px; first-pass search is also <= 0.45 cell
    double later_pass_half = 1.5; // px; search half-length on curve-guided passes
    int passes = 2;
    int min_edge_contrast = 15; // grey levels between the profile's two plateaus
};

struct PointLdcRefineStats
{
    int samples = 0;    // edge samples that located the edge (all passes)
    int undistorts = 0; // undistort() calls
};

// Refine a tag quad in the corrected domain.
//
// `raw_corners` are full-frame raw px, in visually clockwise order (the stock
// detector's order). Pass 1 samples each edge along the raw chord between two
// corners and moves each sample to the subpixel mid-grey crossing along the
// chord normal. Samples are undistorted, and a straight line is fitted per edge
// (Huber-reweighted least squares). Later passes put samples on the previous corrected-domain edge,
// forward-distort them onto the curved raw edge, and search along that curve's
// local normal with a short window. Adjacent lines intersect for the corners.
//
// `ideal_corners[j]` corresponds to raw_corners[j]. False if an edge has too
// few samples or the lines do not intersect.
bool refine_quad_ideal(const LensModel &model, const cv::Mat &frame,
                       const cv::Point2f raw_corners[4], const PointLdcParams &params,
                       cv::Point2d ideal_corners[4], PointLdcRefineStats *stats = nullptr);

// Raw-px sample positions for reading a tag's cells through the corrected-domain
// homography: cells x cells cells, samples x samples points per cell, placed in
// the inner half of each cell. Row-major by cell row, then sample row, then
// cell column, then sample column: index ((r*samples + i)*cells + c)*samples + j.
void forward_grid_points(const LensModel &model, const cv::Point2d ideal_corners[4],
                         int cells, int samples, std::vector<cv::Point2f> &raw_points);

// Bilinear grey value at a raw position; 0 outside the frame.
float sample_bilinear(const cv::Mat &frame, float x, float y);

#endif
