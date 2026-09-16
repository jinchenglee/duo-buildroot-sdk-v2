#ifndef TAG_CROP_DECODER_H
#define TAG_CROP_DECODER_H

#include <memory>
#include <opencv2/core.hpp>
#include <vector>

// One decoded tag, in the coordinate space of whatever crop was passed to
// TagCropDecoder::detect(). The caller offsets into full-frame coordinates.
struct TagDetection
{
    int id;
    cv::Point2f center;
    cv::Point2f corners[4];
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
};

// ArUco Nano reading AprilTag 36h11, the K230 production default.
//
// tolerant=false (strict) matches the K230 launcher: errorCorrectionRate and
// maxErroneousBitsInBorderRate both 0.0, so a tag must decode exactly. Setting
// tolerant=true raises both to 1.0, which accepts more marginal tags at the
// cost of false positives -- and is slower, since more candidates survive to
// the bit-extraction stage.
std::shared_ptr<TagCropDecoder> make_aruco_nano_decoder(bool tolerant);

#endif // TAG_CROP_DECODER_H
