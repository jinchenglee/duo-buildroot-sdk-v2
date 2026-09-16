#include "tinytag_det.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

double now_ms()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

const char *fmt_name(CVI_FMT fmt)
{
    switch (fmt)
    {
    case CVI_FMT_FP32:  return "fp32";
    case CVI_FMT_INT8:  return "int8";
    case CVI_FMT_UINT8: return "uint8";
    case CVI_FMT_BF16:  return "bf16";
    default:            return "other";
    }
}

} // namespace

TinyTagDet::TinyTagDet(const std::string &cvimodel_path, float heatmap_thres, int max_proposals,
                       float roi_expand, float roi_iou_thres, int debug_mode)
    : heatmap_thres_(heatmap_thres), max_proposals_(max_proposals), roi_expand_(roi_expand),
      roi_iou_thres_(roi_iou_thres), debug_mode_(debug_mode)
{
    int ret = CVI_NN_RegisterModel(cvimodel_path.c_str(), &model_);
    if (ret != CVI_RC_SUCCESS)
        throw std::runtime_error("CVI_NN_RegisterModel failed for " + cvimodel_path +
                                 " (err " + std::to_string(ret) + ")");

    ret = CVI_NN_GetInputOutputTensors(model_, &input_tensors_, &input_num_,
                                       &output_tensors_, &output_num_);
    if (ret != CVI_RC_SUCCESS)
        throw std::runtime_error("CVI_NN_GetInputOutputTensors failed (err " +
                                 std::to_string(ret) + ")");

    input_ = CVI_NN_GetTensorByName(CVI_NN_DEFAULT_TENSOR, input_tensors_, input_num_);
    output_ = CVI_NN_GetTensorByName(CVI_NN_DEFAULT_TENSOR, output_tensors_, output_num_);
    if (input_ == nullptr || output_ == nullptr)
        throw std::runtime_error("cvimodel has no usable input/output tensor");

    CVI_SHAPE in_shape = CVI_NN_TensorShape(input_);
    CVI_SHAPE out_shape = CVI_NN_TensorShape(output_);
    if (in_shape.dim_size != 4 || out_shape.dim_size != 4)
        throw std::runtime_error("expected rank-4 input and output tensors");
    if (in_shape.dim[1] != 1)
        throw std::runtime_error("expected a single-channel (grayscale) input, got " +
                                 std::to_string(in_shape.dim[1]) + " channels");
    if (out_shape.dim[1] < kTrainedChannels)
        throw std::runtime_error("output has " + std::to_string(out_shape.dim[1]) +
                                 " channels, need at least " + std::to_string(kTrainedChannels));

    input_h_ = in_shape.dim[2];
    input_w_ = in_shape.dim[3];
    output_c_ = out_shape.dim[1];
    output_h_ = out_shape.dim[2];
    output_w_ = out_shape.dim[3];

    const int plane = output_h_ * output_w_;
    score_.resize(plane);
    is_peak_.resize(plane);

    if (debug_mode_ > 0)
    {
        printf("model  : %s\n", cvimodel_path.c_str());
        printf("target : %s\n", CVI_NN_GetModelTarget(model_));
        printf("input  : %s %dx%d fmt=%s qscale=%f pixel_format=%d aligned=%d\n",
               input_->name, input_w_, input_h_, fmt_name(input_->fmt),
               CVI_NN_TensorQuantScale(input_), static_cast<int>(input_->pixel_format),
               input_->aligned ? 1 : 0);
        printf("output : %s [1,%d,%d,%d] fmt=%s\n", output_->name, output_c_,
               output_h_, output_w_, fmt_name(output_->fmt));
    }

    // A width-aligned input tensor means the model was built with
    // --aligned_input, which expects VPSS-padded rows. This application feeds a
    // plain contiguous buffer, so the rows would be misinterpreted.
    if (input_->aligned)
        throw std::runtime_error(
            "cvimodel was compiled with --aligned_input; rebuild without it, or feed "
            "this model from the VPSS pipeline instead of a plain buffer");
}

TinyTagDet::~TinyTagDet()
{
    if (model_ != nullptr)
        CVI_NN_CleanupModel(model_);
}

void TinyTagDet::pre_process(const cv::Mat &ori_img_gray)
{
    const double started = now_ms();

    if (ori_img_gray.empty() || ori_img_gray.type() != CV_8UC1)
        throw std::runtime_error("pre_process expects a non-empty CV_8UC1 image");

    // Bottom-left 16:9 band, then plain resize. See the header for why this is
    // crop-only rather than letterbox.
    const int band_w = std::min(ori_img_gray.cols, kCropW);
    const int band_h = std::min(ori_img_gray.rows, kCropH);
    const cv::Mat band = ori_img_gray(cv::Rect(0, ori_img_gray.rows - band_h, band_w, band_h));

    const cv::Mat *source = &band;
    if (band_w != input_w_ || band_h != input_h_)
    {
        const int interp = (band_w >= input_w_) ? cv::INTER_AREA : cv::INTER_LINEAR;
        cv::resize(band, resized_, cv::Size(input_w_, input_h_), 0, 0, interp);
        source = &resized_;
    }

    store_input_plane(source->ptr<uint8_t>(0), source->step);

    preprocess_ms_ = now_ms() - started;
}

void TinyTagDet::store_input_plane(const uint8_t *src, size_t src_stride)
{
    uint8_t *dst = reinterpret_cast<uint8_t *>(CVI_NN_TensorPtr(input_));

    if (input_->fmt == CVI_FMT_UINT8)
    {
        // --fuse_preprocess build: the TPU does the /255 itself, so hand it the
        // raw luma plane. Row-wise copy because the source may be a cv::Mat view
        // whose step is larger than its width.
        for (int y = 0; y < input_h_; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * input_w_, src + y * src_stride, input_w_);
    }
    else if (input_->fmt == CVI_FMT_INT8)
    {
        // Non-fused build: apply the model's own normalization (x/255) and the
        // input quantization scale by hand, exactly as the SDK's classifier
        // sample does for its mean/scale.
        const float qscale = CVI_NN_TensorQuantScale(input_);
        const float factor = (1.0f / 255.0f) * qscale;
        int8_t *qdst = reinterpret_cast<int8_t *>(dst);
        for (int y = 0; y < input_h_; ++y)
        {
            const uint8_t *row = src + y * src_stride;
            int8_t *qrow = qdst + static_cast<size_t>(y) * input_w_;
            for (int x = 0; x < input_w_; ++x)
            {
                const float v = std::round(row[x] * factor);
                qrow[x] = static_cast<int8_t>(std::max(-128.f, std::min(127.f, v)));
            }
        }
    }
    else if (input_->fmt == CVI_FMT_FP32)
    {
        float *fdst = reinterpret_cast<float *>(dst);
        for (int y = 0; y < input_h_; ++y)
        {
            const uint8_t *row = src + y * src_stride;
            float *frow = fdst + static_cast<size_t>(y) * input_w_;
            for (int x = 0; x < input_w_; ++x)
                frow[x] = row[x] / 255.0f;
        }
    }
    else
    {
        throw std::runtime_error(std::string("unsupported input tensor format: ") +
                                 fmt_name(input_->fmt));
    }
}

void TinyTagDet::set_input(const uint8_t *gray, size_t count)
{
    const size_t expected = static_cast<size_t>(input_w_) * input_h_;
    if (count != expected)
        throw std::runtime_error("set_input expects " + std::to_string(expected) +
                                 " pixels, got " + std::to_string(count));
    const double started = now_ms();
    store_input_plane(gray, static_cast<size_t>(input_w_));
    preprocess_ms_ = now_ms() - started;
}

void TinyTagDet::copy_output(std::vector<float> &out) const
{
    out.resize(output_count());
    if (output_->fmt == CVI_FMT_FP32)
    {
        std::memcpy(out.data(), CVI_NN_TensorPtr(output_), out.size() * sizeof(float));
    }
    else if (output_->fmt == CVI_FMT_INT8)
    {
        const float qscale = CVI_NN_TensorQuantScale(output_);
        const float inv = (qscale != 0.0f) ? (1.0f / qscale) : 1.0f;
        const int8_t *raw = reinterpret_cast<const int8_t *>(CVI_NN_TensorPtr(output_));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = raw[i] * inv;
    }
    else
    {
        throw std::runtime_error(std::string("unsupported output tensor format: ") +
                                 fmt_name(output_->fmt));
    }
}

void TinyTagDet::inference()
{
    const double started = now_ms();
    const int ret = CVI_NN_Forward(model_, input_tensors_, input_num_,
                                   output_tensors_, output_num_);
    if (ret != CVI_RC_SUCCESS)
        throw std::runtime_error("CVI_NN_Forward failed (err " + std::to_string(ret) + ")");
    inference_ms_ = now_ms() - started;
}

void TinyTagDet::decode_proposals(cv::Size frame_size, std::vector<Proposal> &proposals)
{
    const double started = now_ms();
    proposals.clear();

    const int H = output_h_;
    const int W = output_w_;
    const int plane = H * W;

    // Channels 0-4 of an NCHW [1,21,H,W] tensor. Anything but fp32 is
    // dequantized once into scratch so the decode below is format-agnostic.
    const float *out = nullptr;
    if (output_->fmt == CVI_FMT_FP32)
    {
        out = reinterpret_cast<const float *>(CVI_NN_TensorPtr(output_));
    }
    else if (output_->fmt == CVI_FMT_INT8)
    {
        // cvitek convention: qscale = 128 / threshold, so dequant divides by it.
        const float qscale = CVI_NN_TensorQuantScale(output_);
        const float inv = (qscale != 0.0f) ? (1.0f / qscale) : 1.0f;
        const int8_t *raw = reinterpret_cast<const int8_t *>(CVI_NN_TensorPtr(output_));
        dequantized_.resize(static_cast<size_t>(plane) * kTrainedChannels);
        for (size_t i = 0; i < dequantized_.size(); ++i)
            dequantized_[i] = raw[i] * inv;
        out = dequantized_.data();
    }
    else
    {
        throw std::runtime_error(std::string("unsupported output tensor format: ") +
                                 fmt_name(output_->fmt));
    }

    const float *heatmap = out + 0 * plane;
    const float *offset_x = out + 1 * plane;
    const float *offset_y = out + 2 * plane;
    const float *scale_w = out + 3 * plane;
    const float *scale_h = out + 4 * plane;

    for (int i = 0; i < plane; ++i)
        score_[i] = 1.f / (1.f + std::exp(-heatmap[i]));

    // 3x3 max-pool local-maxima NMS: a cell survives only if it equals the max
    // of its own 3x3 neighborhood (clamped at the border). Reads score_ only --
    // never mutated mid-scan, so neighbor reads stay correct in any scan order.
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const float v = score_[y * W + x];
            bool peak = true;
            for (int dy = -1; dy <= 1 && peak; ++dy)
            {
                const int ny = y + dy;
                if (ny < 0 || ny >= H)
                    continue;
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = x + dx;
                    if (nx < 0 || nx >= W)
                        continue;
                    if (score_[ny * W + nx] > v)
                    {
                        peak = false;
                        break;
                    }
                }
            }
            is_peak_[y * W + x] = peak ? 1 : 0;
        }
    }

    std::vector<std::pair<float, int>> peaks;
    for (int i = 0; i < plane; ++i)
        if (is_peak_[i] && score_[i] >= heatmap_thres_)
            peaks.emplace_back(score_[i], i);
    std::sort(peaks.begin(), peaks.end(),
              [](const std::pair<float, int> &a, const std::pair<float, int> &b)
              { return a.first > b.first; });
    if (static_cast<int>(peaks.size()) > max_proposals_)
        peaks.resize(max_proposals_);

    // The network was fed the bottom-left 16:9 band, so map network coordinates
    // into that band's space and then offset back to full-frame coordinates.
    // For sources at or below the band size this reduces to identity offsets.
    const int band_w = std::min(frame_size.width, kCropW);
    const int band_h = std::min(frame_size.height, kCropH);
    const int crop_x = 0;
    const int crop_y = frame_size.height - band_h;
    const float x_factor = static_cast<float>(band_w) / input_w_;
    const float y_factor = static_cast<float>(band_h) / input_h_;

    proposals.reserve(peaks.size());
    for (const auto &pk : peaks)
    {
        const int idx = pk.second;
        const int gy = idx / W;
        const int gx = idx % W;

        float cx = (gx + offset_x[idx]) * kStride;
        float cy = (gy + offset_y[idx]) * kStride;
        float w = std::exp(std::max(kScaleClampLo, std::min(kScaleClampHi, scale_w[idx]))) * kStride;
        float h = std::exp(std::max(kScaleClampLo, std::min(kScaleClampHi, scale_h[idx]))) * kStride;
        w *= roi_expand_;
        h *= roi_expand_;

        cx = cx * x_factor + crop_x;
        cy = cy * y_factor + crop_y;
        w *= x_factor;
        h *= y_factor;

        const float lo_x = static_cast<float>(crop_x);
        const float hi_x = static_cast<float>(crop_x + band_w);
        const float lo_y = static_cast<float>(crop_y);
        const float hi_y = static_cast<float>(crop_y + band_h);
        const float x0 = std::max(lo_x, std::min(hi_x, cx - w / 2.f));
        const float y0 = std::max(lo_y, std::min(hi_y, cy - h / 2.f));
        const float x1 = std::max(lo_x, std::min(hi_x, cx + w / 2.f));
        const float y1 = std::max(lo_y, std::min(hi_y, cy + h / 2.f));
        if (x1 <= x0 || y1 <= y0)
            continue;

        proposals.push_back({pk.first, cv::Rect2f(x0, y0, x1 - x0, y1 - y0)});
    }

    // Greedy box-level IoU suppression. `proposals` is already in descending
    // confidence order (peaks were sorted before the top-K cut and pushed in
    // that order), so keep-first is correct without re-sorting.
    if (roi_iou_thres_ > 0.f && proposals.size() > 1)
    {
        std::vector<Proposal> kept;
        kept.reserve(proposals.size());
        for (const auto &cand : proposals)
        {
            bool suppressed = false;
            for (const auto &k : kept)
            {
                if (rect_iou(cand.roi, k.roi) > roi_iou_thres_)
                {
                    suppressed = true;
                    break;
                }
            }
            if (!suppressed)
                kept.push_back(cand);
        }
        if (debug_mode_ > 1)
            printf("roi_iou_suppress: %zu -> %zu proposals\n", proposals.size(), kept.size());
        proposals.swap(kept);
    }

    decode_ms_ = now_ms() - started;
}

void TinyTagDet::detect(const cv::Mat &ori_img_gray, std::vector<Proposal> &proposals)
{
    pre_process(ori_img_gray);
    inference();
    decode_proposals(ori_img_gray.size(), proposals);
}

float TinyTagDet::rect_iou(const cv::Rect2f &a, const cv::Rect2f &b)
{
    const float inter = (a & b).area();
    const float uni = a.area() + b.area() - inter;
    return (uni > 0.f) ? (inter / uni) : 0.f;
}

void TinyTagDet::draw_proposals(cv::Mat &bgr, const std::vector<Proposal> &proposals)
{
    for (const auto &p : proposals)
    {
        cv::rectangle(bgr, p.roi, cv::Scalar(0, 255, 0), 1);
        char label[32];
        std::snprintf(label, sizeof(label), "%.2f", p.confidence);
        cv::putText(bgr, label, cv::Point(static_cast<int>(p.roi.x),
                                          std::max(10, static_cast<int>(p.roi.y) - 3)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(0, 255, 0), 1);
    }
}

void TinyTagDet::post_process(const cv::Mat &full_res_gray,
                              const std::vector<Proposal> &proposals,
                              std::vector<TinyTagResult> &results)
{
    const double started = now_ms();
    results.clear();
    crop_count_ = 0;

    if (!decoder_)
    {
        crop_decode_ms_ = 0.0;
        return;
    }
    if (full_res_gray.empty() || full_res_gray.type() != CV_8UC1)
        throw std::runtime_error("post_process expects a non-empty CV_8UC1 image");

    for (const auto &proposal : proposals)
    {
        // Clamp to integer pixels inside the frame. decode_proposals() already
        // clamped to the band, but rounding can still push a box one pixel out.
        const int x0 = std::max(0, static_cast<int>(std::floor(proposal.roi.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(proposal.roi.y)));
        const int x1 = std::min(full_res_gray.cols,
                                static_cast<int>(std::ceil(proposal.roi.x + proposal.roi.width)));
        const int y1 = std::min(full_res_gray.rows,
                                static_cast<int>(std::ceil(proposal.roi.y + proposal.roi.height)));
        if (x1 - x0 < 8 || y1 - y0 < 8)
            continue; // too small for the decoder's minSize to ever accept

        // Zero-copy view; the decoder respects .step so no clone is needed.
        const cv::Mat crop = full_res_gray(cv::Rect(x0, y0, x1 - x0, y1 - y0));
        ++crop_count_;

        for (const auto &tag : decoder_->detect(crop))
        {
            TinyTagResult result{};
            result.id = tag.id;
            result.proposal_confidence = proposal.confidence;
            result.roi = proposal.roi;
            result.center = tag.center + cv::Point2f(static_cast<float>(x0), static_cast<float>(y0));
            for (int corner = 0; corner < 4; ++corner)
                result.corners[corner] =
                    tag.corners[corner] + cv::Point2f(static_cast<float>(x0), static_cast<float>(y0));
            results.push_back(result);
        }
    }

    // Two proposals can overlap the same physical tag, so the same id decodes
    // twice. Proposals arrive in descending confidence order, so keeping the
    // first occurrence keeps the one backed by the stronger proposal.
    std::vector<TinyTagResult> deduped;
    deduped.reserve(results.size());
    for (const auto &candidate : results)
    {
        bool duplicate = false;
        for (const auto &kept : deduped)
        {
            if (kept.id != candidate.id)
                continue;
            const float dx = kept.center.x - candidate.center.x;
            const float dy = kept.center.y - candidate.center.y;
            if (std::sqrt(dx * dx + dy * dy) < kDedupeDistPx)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            deduped.push_back(candidate);
    }
    results.swap(deduped);

    crop_decode_ms_ = now_ms() - started;
}

void TinyTagDet::draw_detections(cv::Mat &bgr, const std::vector<TinyTagResult> &results)
{
    for (const auto &result : results)
    {
        for (int corner = 0; corner < 4; ++corner)
            cv::line(bgr, result.corners[corner], result.corners[(corner + 1) % 4],
                     cv::Scalar(0, 0, 255), 2);
        char label[32];
        std::snprintf(label, sizeof(label), "id %d", result.id);
        cv::putText(bgr, label, result.center + cv::Point2f(-14.f, -6.f),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 0, 255), 1);
    }
}
