#include "tag_crop_decoder.h"

#include "../common/point_ldc.h"

#include <opencv2/imgproc.hpp>

// ARUCONANO_NO_POSE drops Marker::estimatePose, the header's only user of
// opencv2/calib3d.hpp -- a module the board's OpenCV 3.2 runtime does not
// ship. Pose estimation is not part of tag ID decoding. See
// third_party/aruco_nano/0001-opencv3-no-pose.patch.
#define ARUCONANO_NO_POSE
#include "aruco_nano.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace {

// Parameters mirror the K230 application's ArucoNanoDecoder so that timings
// taken here are comparable with the figures recorded there.
aruco_nano::DetectorParameters nano_parameters(bool tolerant)
{
    aruco_nano::DetectorParameters parameters;
    parameters.minSize = 10;
    parameters.dicts = {cv::aruco::getPredefinedDictionary(cv::aruco::DICT_APRILTAG_36h11)};
    parameters.detectInvertedMarker = false;
    const double rate = tolerant ? 1.0 : 0.0;
    parameters.errorCorrectionRate = rate;
    parameters.maxErroneousBitsInBorderRate = rate;
    return parameters;
}

void copy_profile(const aruco_nano::DetectionProfile &measured, const cv::Mat &crop,
                  TagDecoderProfile &profile)
{
    profile = TagDecoderProfile{};
    profile.threshold_ms = measured.threshold_ms;
    profile.contour_ms = measured.contour_ms;
    profile.quad_ms = measured.quad_ms;
    profile.decode_ms = measured.decode_ms;
    profile.refine_ms = measured.refine_ms;
    profile.pixels = crop.total();
    profile.contours = measured.contours;
    profile.candidates = measured.candidates;
    profile.attempts = measured.attempts;
    profile.markers = measured.markers;
}

cv::Point2f mean_corner(const cv::Point2f corners[4])
{
    cv::Point2f centre(0.f, 0.f);
    for (int corner = 0; corner < 4; ++corner)
        centre += corners[corner] * 0.25f;
    return centre;
}

class ArucoNanoDecoder final : public TagCropDecoder
{
public:
    explicit ArucoNanoDecoder(bool tolerant) : parameters_(nano_parameters(tolerant)) {}

    std::vector<TagDetection> detect(const cv::Mat &crop) override
    {
        if (crop.type() != CV_8UC1)
            throw std::runtime_error("TagCropDecoder::detect expects CV_8UC1");

        aruco_nano::DetectionProfile measured;
        const auto markers = aruco_nano::MarkerDetector::detect(
            crop, parameters_, nullptr, &measured);
        copy_profile(measured, crop, profile_);

        std::vector<TagDetection> out;
        out.reserve(markers.size());
        for (const auto &marker : markers)
        {
            if (marker.id < 0 || marker.size() != 4)
                continue;
            TagDetection detection{};
            detection.id = marker.id;
            for (int corner = 0; corner < 4; ++corner)
                detection.corners[corner] = marker[corner];
            detection.center = mean_corner(detection.corners);
            out.push_back(detection);
        }
        return out;
    }

    const TagDecoderProfile &last_profile() const override { return profile_; }

private:
    aruco_nano::DetectorParameters parameters_;
    TagDecoderProfile profile_;
};

double quad_area(const cv::Point2f q[4])
{
    double twice = 0;
    for (int j = 0; j < 4; ++j)
        twice += q[j].x * q[(j + 1) % 4].y - q[(j + 1) % 4].x * q[j].y;
    return std::fabs(twice) / 2;
}

bool inside_quad(const cv::Point2f q[4], const cv::Point2f &p)
{
    const std::vector<cv::Point2f> contour(q, q + 4);
    return cv::pointPolygonTest(contour, p, false) >= 0;
}

// Read the cell grid through the corrected-domain homography, sampling the raw
// frame at forward-distorted positions (3x3 per cell, inner half), then apply
// aruco_nano's border and dictionary checks. `rotation` has the meaning of
// aruco_nano::MarkerDetector::getMarkerId's nrotations.
bool forward_decode(const LensModel &model, const cv::Mat &frame, const cv::Point2d ideal[4],
                    const aruco_nano::DetectorParameters &parameters, int &id, int &rotation)
{
    const cv::aruco::Dictionary &dict = parameters.dicts.front();
    const int border = static_cast<int>(parameters.markerBorderBits);
    const int cells = dict.markerSize + 2 * border;
    constexpr int kSamples = 3;
    std::vector<cv::Point2f> points;
    forward_grid_points(model, ideal, cells, kSamples, points);
    const int n = cells * kSamples;
    cv::Mat values(cells, cells, CV_8UC1);
    for (int r = 0; r < cells; ++r)
    {
        for (int c = 0; c < cells; ++c)
        {
            float sum = 0;
            for (int i = 0; i < kSamples; ++i)
            {
                for (int j = 0; j < kSamples; ++j)
                {
                    const cv::Point2f &p =
                        points[static_cast<size_t>(r * kSamples + i) * n + c * kSamples + j];
                    sum += sample_bilinear(frame, p.x, p.y);
                }
            }
            values.at<uchar>(r, c) = cv::saturate_cast<uchar>(cvRound(sum / (kSamples * kSamples)));
        }
    }
    cv::Mat bits;
    cv::threshold(values, bits, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    int border_errors = 0;
    for (int r = 0; r < cells; ++r)
    {
        for (int c = 0; c < cells; ++c)
        {
            const bool on_border = r < border || c < border || r >= cells - border ||
                                   c >= cells - border;
            if (on_border && bits.at<uchar>(r, c) != 0)
                ++border_errors;
        }
    }
    if (border_errors > static_cast<int>(dict.markerSize * dict.markerSize *
                                         parameters.maxErroneousBitsInBorderRate))
        return false;
    const cv::Mat only_bits =
        bits(cv::Range(border, cells - border), cv::Range(border, cells - border)) / 255;
    return dict.identify(only_bits, id, rotation, parameters.errorCorrectionRate);
}

class PointLdcDecoder final : public TagCropDecoder
{
public:
    PointLdcDecoder(bool tolerant, std::shared_ptr<const LensModel> model,
                    const PointLdcParams &params, bool fallback, int max_fallback)
        : parameters_(nano_parameters(tolerant)), model_(std::move(model)), params_(params),
          fallback_(fallback), max_fallback_(std::max(0, max_fallback))
    {
        if (!model_)
            throw std::invalid_argument("point LDC decoder needs a lens model");
    }

    std::vector<TagDetection> detect(const cv::Mat &crop) override
    {
        if (crop.type() != CV_8UC1)
            throw std::runtime_error("TagCropDecoder::detect expects CV_8UC1");
        cv::Size whole;
        cv::Point origin;
        crop.locateROI(whole, origin);
        if (whole != model_->size())
            throw std::runtime_error("point LDC crop is not a view of the calibrated frame");
        cv::Mat frame = crop;
        frame.adjustROI(origin.y, whole.height - origin.y - crop.rows,
                        origin.x, whole.width - origin.x - crop.cols);
        const cv::Point2f offset(static_cast<float>(origin.x), static_cast<float>(origin.y));

        aruco_nano::DetectionProfile measured;
        std::vector<aruco_nano::Marker> rejected;
        const auto markers = aruco_nano::MarkerDetector::detect(
            crop, parameters_, fallback_ ? &rejected : nullptr, &measured);
        copy_profile(measured, crop, profile_);
        const auto started = std::chrono::steady_clock::now();

        std::vector<TagDetection> out;
        out.reserve(markers.size());
        std::vector<std::vector<cv::Point2f>> decoded_raw;
        for (const auto &marker : markers)
        {
            if (marker.id < 0 || marker.size() != 4)
                continue;
            cv::Point2f raw[4];
            for (int j = 0; j < 4; ++j)
                raw[j] = marker[j] + offset;
            decoded_raw.emplace_back(raw, raw + 4);

            TagDetection detection{};
            detection.id = marker.id;
            cv::Point2d ideal[4];
            PointLdcRefineStats stats;
            if (refine_quad_ideal(*model_, frame, raw, params_, ideal, &stats))
            {
                ++profile_.point_refined;
                set_from_ideal(detection, ideal, offset);
            }
            else
            {
                // Keep the stock corners and undistort them where the model allows.
                ++profile_.point_refine_failed;
                bool all = true;
                for (int j = 0; j < 4 && all; ++j)
                    all = model_->undistort(raw[j], ideal[j]);
                for (int j = 0; j < 4; ++j)
                    detection.corners[j] = marker[j];
                detection.has_ideal = all;
                for (int j = 0; j < 4 && all; ++j)
                    detection.ideal_corners[j] = cv::Point2f(ideal[j]);
            }
            profile_.point_samples += stats.samples;
            detection.center = mean_corner(detection.corners);
            out.push_back(detection);
        }

        if (fallback_ && !rejected.empty())
            recover(frame, offset, rejected, decoded_raw, out);

        profile_.point_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started).count();
        return out;
    }

    const TagDecoderProfile &last_profile() const override { return profile_; }

private:
    void set_from_ideal(TagDetection &detection, const cv::Point2d ideal[4],
                        const cv::Point2f &offset) const
    {
        detection.has_ideal = true;
        for (int j = 0; j < 4; ++j)
        {
            detection.ideal_corners[j] = cv::Point2f(ideal[j]);
            detection.corners[j] = cv::Point2f(model_->distort(ideal[j])) - offset;
        }
    }

    // Try rejected candidates, largest first, through corrected-domain
    // refinement and the forward-distorted grid.
    void recover(const cv::Mat &frame, const cv::Point2f &offset,
                 const std::vector<aruco_nano::Marker> &rejected,
                 const std::vector<std::vector<cv::Point2f>> &decoded_raw,
                 std::vector<TagDetection> &out)
    {
        std::vector<std::pair<double, size_t>> order;
        order.reserve(rejected.size());
        for (size_t i = 0; i < rejected.size(); ++i)
        {
            if (rejected[i].size() != 4)
                continue;
            order.emplace_back(quad_area(rejected[i].data()), i);
        }
        std::sort(order.begin(), order.end(),
                  [](const std::pair<double, size_t> &a, const std::pair<double, size_t> &b) {
                      return a.first > b.first;
                  });

        std::vector<std::pair<double, TagDetection>> recovered; // (raw area, detection)
        int tried = 0;
        for (const auto &entry : order)
        {
            if (tried >= max_fallback_)
                break;
            const auto &candidate = rejected[entry.second];
            cv::Point2f raw[4];
            for (int j = 0; j < 4; ++j)
                raw[j] = candidate[j] + offset;
            const cv::Point2f centre = mean_corner(raw);
            bool covered = false;
            for (const auto &decoded : decoded_raw)
                covered = covered || inside_quad(decoded.data(), centre);
            if (covered)
                continue;
            ++tried;
            ++profile_.point_fallback_tried;
            cv::Point2d ideal[4];
            PointLdcRefineStats stats;
            const bool refined = refine_quad_ideal(*model_, frame, raw, params_, ideal, &stats);
            profile_.point_samples += stats.samples;
            int id = -1, rotation = 0;
            if (!refined || !forward_decode(*model_, frame, ideal, parameters_, id, rotation))
                continue;
            std::rotate(ideal, ideal + 4 - rotation, ideal + 4);
            TagDetection detection{};
            detection.id = id;
            set_from_ideal(detection, ideal, offset);
            detection.center = mean_corner(detection.corners);
            recovered.emplace_back(entry.first, detection);
        }

        // An outer and an inner border contour can both decode; keep the outer.
        for (size_t i = 0; i < recovered.size(); ++i)
        {
            bool dominated = false;
            for (size_t j = 0; j < recovered.size() && !dominated; ++j)
            {
                if (i == j || recovered[i].second.id != recovered[j].second.id)
                    continue;
                dominated = recovered[j].first > recovered[i].first &&
                            inside_quad(recovered[j].second.corners, recovered[i].second.center);
            }
            if (!dominated)
            {
                out.push_back(recovered[i].second);
                ++profile_.point_fallback_decoded;
            }
        }
    }

    aruco_nano::DetectorParameters parameters_;
    std::shared_ptr<const LensModel> model_;
    PointLdcParams params_;
    bool fallback_;
    int max_fallback_;
    TagDecoderProfile profile_;
};

} // namespace

std::shared_ptr<TagCropDecoder> make_aruco_nano_decoder(bool tolerant)
{
    return std::make_shared<ArucoNanoDecoder>(tolerant);
}

std::shared_ptr<TagCropDecoder> make_point_ldc_decoder(bool tolerant,
                                                       std::shared_ptr<const LensModel> model,
                                                       const PointLdcParams &params,
                                                       bool fallback, int max_fallback)
{
    return std::make_shared<PointLdcDecoder>(tolerant, std::move(model), params, fallback,
                                             max_fallback);
}
