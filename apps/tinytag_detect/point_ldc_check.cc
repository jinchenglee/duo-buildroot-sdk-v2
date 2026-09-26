// Offline check and benchmark for point-level LDC (apps/common/point_ldc.h).
//
//   point_ldc_check calibration.json frame.png [--rois rois.txt] [--repeat N]
//                   [--no-fallback] [--samples K] [--passes P] [--max-fallback N]
//
// Runs the stock ArUco Nano crop decoder and the point-LDC decoder on the same
// ROIs of one raw 8-bit frame whose size matches the calibration's aspect.
// rois.txt has one "x0 y0 x1 y1" per line (full-frame px); without it, the
// stock decoder runs on the whole frame and each tag's box times 1.5 becomes
// a ROI, as roi_expand does for neural proposals.
//
// Prints one line per detection, then the median time per ROI of each method
// over --repeat runs. Built for the host (to compare with
// tools/ldc_point_correction/ldcpt.py) and for the board (for timing).

#include "../common/point_ldc.h"
#include "tag_crop_decoder.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

double now_ms()
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool load_model(const std::string &path, cv::Size frame, LensModel &model)
{
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened())
    {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    const cv::FileNode size = fs["image_size"];
    const cv::FileNode camera = fs["camera_matrix"];
    const cv::FileNode dist = fs["distortion_coefficients"];
    if (size.size() != 2 || camera.size() != 3 || dist.empty())
    {
        std::fprintf(stderr, "%s lacks image_size, camera_matrix or distortion_coefficients\n",
                     path.c_str());
        return false;
    }
    double k[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            k[r * 3 + c] = static_cast<double>(camera[r][c]);
    std::vector<double> d;
    for (const auto &value : dist)
        d.push_back(static_cast<double>(value));
    std::string error;
    if (!model.init(k, d, static_cast<int>(size[0]), static_cast<int>(size[1]), frame.width,
                    frame.height, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return true;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void print_detection(const char *method, size_t roi, const TagDetection &d, const cv::Rect &crop)
{
    std::printf("det method=%s roi=%zu id=%d raw=", method, roi, d.id);
    for (int j = 0; j < 4; ++j)
        std::printf("%s%.4f,%.4f", j ? ";" : "", d.corners[j].x + crop.x, d.corners[j].y + crop.y);
    if (d.has_ideal)
    {
        std::printf(" ideal=");
        for (int j = 0; j < 4; ++j)
            std::printf("%s%.4f,%.4f", j ? ";" : "", d.ideal_corners[j].x, d.ideal_corners[j].y);
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s calibration.json frame.png [--rois file] [--repeat N]\n"
                     "       [--no-fallback] [--samples K] [--passes P] [--max-fallback N]\n",
                     argv[0]);
        return 2;
    }
    const std::string calibration = argv[1], image_path = argv[2];
    std::string rois_path;
    int repeat = 20, max_fallback = 8;
    bool fallback = true;
    PointLdcParams params;
    for (int i = 3; i < argc; ++i)
    {
        const std::string flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--rois" && has_value) rois_path = argv[++i];
        else if (flag == "--repeat" && has_value) repeat = std::max(1, std::atoi(argv[++i]));
        else if (flag == "--no-fallback") fallback = false;
        else if (flag == "--samples" && has_value) params.samples_per_edge = std::atoi(argv[++i]);
        else if (flag == "--passes" && has_value) params.passes = std::atoi(argv[++i]);
        else if (flag == "--max-fallback" && has_value) max_fallback = std::atoi(argv[++i]);
        else
        {
            std::fprintf(stderr, "unknown or incomplete option %s\n", flag.c_str());
            return 2;
        }
    }

    const cv::Mat frame = cv::imread(image_path, cv::IMREAD_GRAYSCALE);
    if (frame.empty())
    {
        std::fprintf(stderr, "cannot read %s\n", image_path.c_str());
        return 1;
    }
    auto model = std::make_shared<LensModel>();
    if (!load_model(calibration, frame.size(), *model))
        return 1;

    // Round trip over the whole frame inside the fold: a model self-test.
    double worst = 0;
    int inverted = 0, outside = 0;
    for (int y = 0; y < frame.rows; y += 8)
    {
        for (int x = 0; x < frame.cols; x += 8)
        {
            cv::Point2d ideal;
            if (!model->undistort(cv::Point2d(x, y), ideal))
            {
                ++outside;
                continue;
            }
            ++inverted;
            worst = std::max(worst, cv::norm(model->distort(ideal) - cv::Point2d(x, y)));
        }
    }
    const cv::Matx33d &k = model->camera_matrix();
    std::printf("model fx=%.3f fy=%.3f cx=%.3f cy=%.3f fold_radius=%.1f px; "
                "round trip max %.2e px over %d grid points, %d outside the fold\n",
                k(0, 0), k(1, 1), k(0, 2), k(1, 2), model->fold_radius_px(), worst, inverted,
                outside);

    auto stock = make_aruco_nano_decoder(false);
    auto point = make_point_ldc_decoder(false, model, params, fallback, max_fallback);

    std::vector<cv::Rect> rois;
    if (!rois_path.empty())
    {
        std::ifstream in(rois_path);
        int x0, y0, x1, y1;
        while (in >> x0 >> y0 >> x1 >> y1)
            rois.push_back(cv::Rect(cv::Point(x0, y0), cv::Point(x1, y1)) &
                           cv::Rect(0, 0, frame.cols, frame.rows));
    }
    else
    {
        for (const auto &d : stock->detect(frame))
        {
            float x0 = d.corners[0].x, x1 = x0, y0 = d.corners[0].y, y1 = y0;
            for (int j = 1; j < 4; ++j)
            {
                x0 = std::min(x0, d.corners[j].x);
                x1 = std::max(x1, d.corners[j].x);
                y0 = std::min(y0, d.corners[j].y);
                y1 = std::max(y1, d.corners[j].y);
            }
            const float side = std::max(x1 - x0, y1 - y0) * 1.5f;
            const cv::Point2f c((x0 + x1) / 2, (y0 + y1) / 2);
            rois.push_back(cv::Rect(cvRound(c.x - side / 2), cvRound(c.y - side / 2),
                                    cvRound(side), cvRound(side)) &
                           cv::Rect(0, 0, frame.cols, frame.rows));
        }
    }
    std::printf("rois %zu, fallback %s (max %d), %d samples/edge, %d passes\n", rois.size(),
                fallback ? "on" : "off", max_fallback, params.samples_per_edge, params.passes);

    std::vector<double> stock_ms, point_ms, point_only_ms;
    TagDecoderProfile total;
    for (int run = 0; run < repeat; ++run)
    {
        double s = 0, p = 0, extra = 0;
        for (size_t i = 0; i < rois.size(); ++i)
        {
            const cv::Mat crop = frame(rois[i]);
            double t = now_ms();
            const auto a = stock->detect(crop);
            s += now_ms() - t;
            t = now_ms();
            const auto b = point->detect(crop);
            p += now_ms() - t;
            const TagDecoderProfile &prof = point->last_profile();
            extra += prof.point_ms;
            if (run == 0)
            {
                for (const auto &d : a)
                    print_detection("stock", i, d, rois[i]);
                for (const auto &d : b)
                    print_detection("point", i, d, rois[i]);
                total.point_refined += prof.point_refined;
                total.point_refine_failed += prof.point_refine_failed;
                total.point_fallback_tried += prof.point_fallback_tried;
                total.point_fallback_decoded += prof.point_fallback_decoded;
                total.point_samples += prof.point_samples;
            }
        }
        stock_ms.push_back(s);
        point_ms.push_back(p);
        point_only_ms.push_back(extra);
    }
    const double n = std::max<size_t>(1, rois.size());
    std::printf("point stats: refined %zu, refine failed %zu, fallback tried %zu, "
                "fallback decoded %zu, edge samples %zu\n",
                total.point_refined, total.point_refine_failed, total.point_fallback_tried,
                total.point_fallback_decoded, total.point_samples);
    std::printf("timing over %d runs, median per ROI: stock %.3f ms, point %.3f ms "
                "(point-LDC stage %.3f ms)\n",
                repeat, median(stock_ms) / n, median(point_ms) / n, median(point_only_ms) / n);
    return 0;
}
