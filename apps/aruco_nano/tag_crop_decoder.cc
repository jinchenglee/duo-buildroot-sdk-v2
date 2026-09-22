#include "tag_crop_decoder.h"

#include <opencv2/imgproc.hpp>

// ARUCONANO_NO_POSE drops Marker::estimatePose, the header's only user of
// opencv2/calib3d.hpp -- a module the board's OpenCV 3.2 runtime does not
// ship. Pose estimation is not part of tag ID decoding. See
// third_party/aruco_nano/0001-opencv3-no-pose.patch.
#define ARUCONANO_NO_POSE
#include "aruco_nano.h"

#include <stdexcept>

namespace {

class ArucoNanoDecoder final : public TagCropDecoder
{
public:
    explicit ArucoNanoDecoder(bool tolerant)
    {
        // Parameters mirror the K230 application's ArucoNanoDecoder so that
        // timings taken here are comparable with the figures recorded there.
        parameters_.minSize = 10;
        parameters_.dicts = {cv::aruco::getPredefinedDictionary(
            cv::aruco::DICT_APRILTAG_36h11)};
        parameters_.detectInvertedMarker = false;
        const double rate = tolerant ? 1.0 : 0.0;
        parameters_.errorCorrectionRate = rate;
        parameters_.maxErroneousBitsInBorderRate = rate;
    }

    std::vector<TagDetection> detect(const cv::Mat &crop) override
    {
        if (crop.type() != CV_8UC1)
            throw std::runtime_error("TagCropDecoder::detect expects CV_8UC1");

        aruco_nano::DetectionProfile measured;
        const auto markers = aruco_nano::MarkerDetector::detect(
            crop, parameters_, nullptr, &measured);
        profile_.threshold_ms = measured.threshold_ms;
        profile_.contour_ms = measured.contour_ms;
        profile_.quad_ms = measured.quad_ms;
        profile_.decode_ms = measured.decode_ms;
        profile_.refine_ms = measured.refine_ms;
        profile_.pixels = crop.total();
        profile_.contours = measured.contours;
        profile_.candidates = measured.candidates;
        profile_.attempts = measured.attempts;
        profile_.markers = measured.markers;

        std::vector<TagDetection> out;
        out.reserve(markers.size());
        for (const auto &marker : markers)
        {
            if (marker.id < 0 || marker.size() != 4)
                continue;
            TagDetection detection{};
            detection.id = marker.id;
            detection.center = cv::Point2f(0.f, 0.f);
            for (int corner = 0; corner < 4; ++corner)
            {
                detection.corners[corner] = marker[corner];
                detection.center += marker[corner] * 0.25f;
            }
            out.push_back(detection);
        }
        return out;
    }

    const TagDecoderProfile &last_profile() const override { return profile_; }

private:
    aruco_nano::DetectorParameters parameters_;
    TagDecoderProfile profile_;
};

} // namespace

std::shared_ptr<TagCropDecoder> make_aruco_nano_decoder(bool tolerant)
{
    return std::make_shared<ArucoNanoDecoder>(tolerant);
}
