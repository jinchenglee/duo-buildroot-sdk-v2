#ifndef TAG_CROP_DECODER_H
#define TAG_CROP_DECODER_H

#include <memory>
#include <opencv2/core.hpp>
#include <vector>

class LensModel;
struct PointLdcParams;

// One decoded tag, in the coordinate space of whatever crop was passed to
// TagCropDecoder::detect(). The caller offsets into full-frame coordinates.
struct TagDetection
{
    int id;
    cv::Point2f center;
    cv::Point2f corners[4];
    // Point-level LDC only: corners in full-frame ideal (undistorted pinhole)
    // pixels. These are not crop-relative; there is no crop in that domain.
    bool has_ideal = false;
    cv::Point2f ideal_corners[4];
};

// Behavior-neutral instrumentation for one decoder call. Times partition the
// ArUco Nano pipeline; counts explain scene-dependent cost.
struct TagDecoderProfile
{
    double threshold_ms = 0, contour_ms = 0, quad_ms = 0, decode_ms = 0, refine_ms = 0;
    size_t pixels = 0, contours = 0, candidates = 0, attempts = 0, markers = 0;
    // Point-level LDC: time spent after the stock decoder, tags refined in the
    // corrected domain, refinements that failed (raw corners undistorted
    // instead), rejected candidates tried, and tags recovered by the
    // forward-distorted grid.
    double point_ms = 0;
    size_t point_refined = 0, point_refine_failed = 0, point_fallback_tried = 0,
           point_fallback_decoded = 0, point_samples = 0;
};

// Stage two of the two-stage detector: crop each neural proposal out of the
// full-resolution frame and hand it to a *traditional* CV tag decoder --
// nothing neural past this point.
//
// Kept as an interface, matching the K230 application's design, so a different
// backend can be dropped in without touching proposal code. Only ArUco Nano is
// wired up here: the K230's other backends are an RVV-vectorized AprilTag
// (RISC-V only, useless on this arm64 board) and the AprilTag C library (not
// packaged in this SDK).
class TagCropDecoder
{
public:
    virtual ~TagCropDecoder() = default;

    // `crop` must be CV_8UC1. It does NOT need to be contiguous -- a plain
    // sub-Mat view is expected, and implementations must respect .step rather
    // than assuming packed rows.
    virtual std::vector<TagDetection> detect(const cv::Mat &crop) = 0;
    virtual const TagDecoderProfile &last_profile() const = 0;
};

// ArUco Nano reading AprilTag 36h11, the K230 production default.
//
// tolerant=false (strict) matches the K230 launcher: errorCorrectionRate and
// maxErroneousBitsInBorderRate both 0.0, so a tag must decode exactly. Setting
// tolerant=true raises both to 1.0, which accepts more marginal tags at the
// cost of false positives -- and is slower, since more candidates survive to
// the bit-extraction stage.
std::shared_ptr<TagCropDecoder> make_aruco_nano_decoder(bool tolerant);

// ArUco Nano plus point-level lens-distortion correction (apps/common/
// point_ldc.h). The frame stays raw. Tags the stock decoder finds get corners
// refined in the corrected domain (TagDetection::ideal_corners); their raw
// corners become the distorted image of those. With `fallback`, up to
// `max_fallback` rejected candidates per crop (largest first, skipping any
// inside a decoded tag) are refined too and read through a forward-distorted
// bit grid, which recovers tags whose raw-quad homography misreads the bits
// under strong distortion.
//
// `crop` must be a view into the full frame the model was built for:
// cv::Mat::locateROI() supplies the crop's full-frame origin, and edge
// profiles may read just outside the crop.
std::shared_ptr<TagCropDecoder> make_point_ldc_decoder(bool tolerant,
                                                       std::shared_ptr<const LensModel> model,
                                                       const PointLdcParams &params,
                                                       bool fallback, int max_fallback = 8);

#endif // TAG_CROP_DECODER_H
