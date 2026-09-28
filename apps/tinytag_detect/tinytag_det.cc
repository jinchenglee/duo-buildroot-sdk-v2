#include "tinytag_det.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

float probability_to_logit(float probability)
{
    if (std::isnan(probability))
        return probability;
    if (probability <= 0.f)
        return -std::numeric_limits<float>::infinity();
    if (probability >= 1.f)
        return std::numeric_limits<float>::infinity();
    return std::log(probability / (1.f - probability));
}

float sigmoid(float logit)
{
    return 1.f / (1.f + std::exp(-logit));
}

float bfloat16_to_float(uint16_t bits)
{
    const uint32_t word = static_cast<uint32_t>(bits) << 16;
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

} // namespace

TinyTagDet::TinyTagDet(const std::string &cvimodel_path, float heatmap_thres, int max_proposals,
                       float roi_expand, float roi_iou_thres, int debug_mode)
    : heatmap_logit_thres_(probability_to_logit(heatmap_thres)),
      max_proposals_(max_proposals), roi_expand_(roi_expand),
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
    peak_logits_.reserve(plane);

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

}

TinyTagDet::~TinyTagDet()
{
    if (model_ != nullptr)
    {
        // A direct-input live run rebinds this tensor to a VPSS-owned block.
        // Do not leave an external physical address installed during cleanup.
        if (input_ != nullptr && physical_input_bound_)
            CVI_NN_SetTensorPhysicalAddr(input_, 0);
        CVI_NN_CleanupModel(model_);
    }
}

void TinyTagDet::pre_process(const cv::Mat &ori_img_gray)
{
    const double started = now_ms();

    if (input_->aligned)
        throw std::runtime_error(
            "pre_process cannot feed an aligned-input model; use a direct VPSS frame");

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
    if (input_->aligned)
        throw std::runtime_error(
            "set_input cannot feed an aligned-input model; use a direct VPSS frame");
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
    else if (output_->fmt == CVI_FMT_BF16)
    {
        const uint16_t *raw = reinterpret_cast<const uint16_t *>(CVI_NN_TensorPtr(output_));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = bfloat16_to_float(raw[i]);
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
        dequantized_.resize(static_cast<size_t>(plane) * output_c_);
        for (size_t i = 0; i < dequantized_.size(); ++i)
            dequantized_[i] = raw[i] * inv;
        out = dequantized_.data();
    }
    else if (output_->fmt == CVI_FMT_BF16)
    {
        const uint16_t *raw = reinterpret_cast<const uint16_t *>(CVI_NN_TensorPtr(output_));
        dequantized_.resize(static_cast<size_t>(plane) * output_c_);
        for (size_t i = 0; i < dequantized_.size(); ++i)
            dequantized_[i] = bfloat16_to_float(raw[i]);
        out = dequantized_.data();
    }
    else
    {
        throw std::runtime_error(std::string("unsupported output tensor format: ") +
                                 fmt_name(output_->fmt));
    }

    // The current coverage+ROI checkpoint exposes six channels in this order:
    // mask logit, heat logit, offsets x/y, and log width/height. Decode blobs
    // first, then use ROI peaks inside each blob to split neighbouring tags.
    if (output_c_ == 6)
    {
        const float *mask_logit = out;
        const float *heat_logit = out + plane;
        const float *off_x = out + 2 * plane;
        const float *off_y = out + 3 * plane;
        const float *log_w = out + 4 * plane;
        const float *log_h = out + 5 * plane;
        const float seed_thr = 0.4f;
        const float grow_thr = 0.3f;
        const float heat_logit_thr = heatmap_logit_thres_; // deployed default: probability .30
        const float warm_thr = 0.2f;
        const float margin = 4.f;
        auto prob = [](float v) { return 1.f / (1.f + std::exp(-v)); };
        std::vector<float> mask_prob(static_cast<size_t>(plane));
        std::vector<int> labels(static_cast<size_t>(plane), -1);
        for (int i = 0; i < plane; ++i)
            mask_prob[i] = prob(mask_logit[i]);

        struct Cell { int x, y; };
        struct Peak { int x, y; float heat, cx, cy, w, h; };
        std::vector<Proposal> decoded;
        int component_id = 0;
        std::vector<int> queue;
        queue.reserve(plane);
        for (int start = 0; start < plane; ++start)
        {
            if (labels[start] >= 0 || mask_prob[start] < grow_thr)
                continue;
            queue.clear();
            queue.push_back(start);
            labels[start] = component_id;
            std::vector<Cell> cells;
            cells.reserve(32);
            float component_score = 0.f;
            int min_x = W, min_y = H, max_x = -1, max_y = -1;
            for (size_t qi = 0; qi < queue.size(); ++qi)
            {
                const int idx = queue[qi], y = idx / W, x = idx % W;
                cells.push_back({x, y});
                component_score = std::max(component_score, mask_prob[idx]);
                min_x = std::min(min_x, x); min_y = std::min(min_y, y);
                max_x = std::max(max_x, x); max_y = std::max(max_y, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                        const int ni = ny * W + nx;
                        if (labels[ni] < 0 && mask_prob[ni] >= grow_thr)
                        { labels[ni] = component_id; queue.push_back(ni); }
                    }
            }
            ++component_id;
            if (component_score < seed_thr)
                continue;

            std::vector<Peak> peaks;
            for (const Cell &cell : cells)
            {
                const int idx = cell.y * W + cell.x;
                if (heat_logit[idx] < heat_logit_thr) continue;
                bool local_max = true;
                for (int dy = -1; dy <= 1 && local_max; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = cell.x + dx, ny = cell.y + dy;
                        if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                        if (heat_logit[ny * W + nx] > heat_logit[idx])
                        { local_max = false; break; }
                    }
                if (!local_max) continue;
                const float bw = std::exp(std::max(kScaleClampLo, std::min(kScaleClampHi, log_w[idx]))) * kStride;
                const float bh = std::exp(std::max(kScaleClampLo, std::min(kScaleClampHi, log_h[idx]))) * kStride;
                peaks.push_back({cell.x, cell.y, heat_logit[idx],
                                 (cell.x + off_x[idx]) * kStride,
                                 (cell.y + off_y[idx]) * kStride, bw, bh});
            }
            std::sort(peaks.begin(), peaks.end(), [](const Peak &a, const Peak &b) { return a.heat > b.heat; });
            std::vector<Peak> unique_peaks;
            for (const Peak &p : peaks)
            {
                bool duplicate = false;
                for (const Peak &k : unique_peaks)
                    if (std::abs(p.cx - k.cx) <= k.w * .5f && std::abs(p.cy - k.cy) <= k.h * .5f)
                    { duplicate = true; break; }
                if (!duplicate) unique_peaks.push_back(p);
            }

            if (unique_peaks.empty())
            {
                const float x0 = std::max(0.f, min_x * kStride - margin);
                const float y0 = std::max(0.f, min_y * kStride - margin);
                const float x1 = std::min(static_cast<float>(input_w_), (max_x + 1) * kStride + margin);
                const float y1 = std::min(static_cast<float>(input_h_), (max_y + 1) * kStride + margin);
                decoded.push_back({component_score, cv::Rect2f(x0,y0,x1-x0,y1-y0)});
                continue;
            }

            std::vector<std::vector<Cell>> owned(unique_peaks.size());
            for (const Cell &cell : cells)
            {
                int owner = 0;
                float best = std::numeric_limits<float>::infinity();
                for (size_t j = 0; j < unique_peaks.size(); ++j)
                {
                    const Peak &p = unique_peaks[j];
                    const float scale = std::max(p.w, p.h);
                    const float dx = (cell.x + .5f) * kStride - p.cx;
                    const float dy = (cell.y + .5f) * kStride - p.cy;
                    const float d = std::hypot(dx, dy) / std::max(scale, 1.f);
                    if (d < best) { best = d; owner = static_cast<int>(j); }
                }
                owned[owner].push_back(cell);
            }
            for (size_t j = 0; j < unique_peaks.size(); ++j)
            {
                const Peak &p = unique_peaks[j];
                float x0 = p.cx - p.w * .5f, y0 = p.cy - p.h * .5f;
                float x1 = p.cx + p.w * .5f, y1 = p.cy + p.h * .5f;
                float score = 0.f;
                if (!owned[j].empty())
                {
                    int bx0=W, by0=H, bx1=-1, by1=-1;
                    for (const Cell &cell : owned[j])
                    {
                        bx0=std::min(bx0,cell.x); by0=std::min(by0,cell.y);
                        bx1=std::max(bx1,cell.x); by1=std::max(by1,cell.y);
                        score=std::max(score,mask_prob[cell.y*W+cell.x]);
                    }
                    x0=std::min(x0,bx0*static_cast<float>(kStride)); y0=std::min(y0,by0*static_cast<float>(kStride));
                    x1=std::max(x1,(bx1+1)*static_cast<float>(kStride)); y1=std::max(y1,(by1+1)*static_cast<float>(kStride));
                }
                else score=mask_prob[p.y*W+p.x];

                // One conditional cell of quiet-zone margin, only on ROI boxes.
                int c0=std::max(0,static_cast<int>(std::floor(x0/kStride)));
                int r0=std::max(0,static_cast<int>(std::floor(y0/kStride)));
                int c1=std::min(W,static_cast<int>(std::ceil(x1/kStride)));
                int r1=std::min(H,static_cast<int>(std::ceil(y1/kStride)));
                auto warm = [&](int x,int y) { const float q=mask_prob[y*W+x]; return q>=warm_thr && q<grow_thr; };
                if (r0>0) for(int x=c0;x<c1;++x) if(warm(x,r0-1)){--r0;y0=std::min(y0,r0*static_cast<float>(kStride));break;}
                if (r1<H) for(int x=c0;x<c1;++x) if(warm(x,r1)){++r1;y1=std::max(y1,r1*static_cast<float>(kStride));break;}
                if (c0>0) for(int y=r0;y<r1;++y) if(warm(c0-1,y)){--c0;x0=std::min(x0,c0*static_cast<float>(kStride));break;}
                if (c1<W) for(int y=r0;y<r1;++y) if(warm(c1,y)){++c1;x1=std::max(x1,c1*static_cast<float>(kStride));break;}
                x0=std::max(0.f,x0-margin); y0=std::max(0.f,y0-margin);
                x1=std::max(0.f,std::min(static_cast<float>(input_w_),x1+margin));
                y1=std::max(0.f,std::min(static_cast<float>(input_h_),y1+margin));
                x0=std::min(x0,static_cast<float>(input_w_));
                y0=std::min(y0,static_cast<float>(input_h_));
                decoded.push_back({score,cv::Rect2f(x0,y0,x1-x0,y1-y0)});
            }
        }
        std::sort(decoded.begin(), decoded.end(), [](const Proposal &a,const Proposal &b){return a.confidence>b.confidence;});
        if (max_proposals_ > 0 && decoded.size() > static_cast<size_t>(max_proposals_)) decoded.resize(max_proposals_);

        const int band_w=std::min(frame_size.width,kCropW), band_h=std::min(frame_size.height,kCropH);
        const int crop_y=frame_size.height-band_h;
        const float xf=static_cast<float>(band_w)/input_w_, yf=static_cast<float>(band_h)/input_h_;
        for (const Proposal &p : decoded)
        {
            float x0=p.roi.x*xf, y0=p.roi.y*yf+crop_y;
            float x1=(p.roi.x+p.roi.width)*xf, y1=(p.roi.y+p.roi.height)*yf+crop_y;
            proposals.push_back({p.confidence,cv::Rect2f(x0,y0,x1-x0,y1-y0)});
        }
        decode_ms_ = now_ms() - started;
        return;
    }

    const float *heatmap = out + 0 * plane;
    const float *offset_x = out + 1 * plane;
    const float *offset_y = out + 2 * plane;
    const float *scale_w = out + 3 * plane;
    const float *scale_h = out + 4 * plane;

    // Sigmoid is monotonic, so threshold and local-max NMS are equivalent in
    // logit space. This avoids evaluating exp() for all H*W cells; sigmoid is
    // needed only for the at-most max_proposals_ reported confidences.
    //
    // A cell survives if it equals the maximum of its 3x3 neighborhood
    // (clamped at the border). Equal logits retain the previous behavior: both
    // survive this stage and the existing full sort determines their order.
    peak_logits_.clear();
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const int idx = y * W + x;
            const float v = heatmap[idx];
            if (!(v >= heatmap_logit_thres_))
                continue;
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
                    if (heatmap[ny * W + nx] > v)
                    {
                        peak = false;
                        break;
                    }
                }
            }
            if (peak)
                peak_logits_.emplace_back(v, idx);
        }
    }

    std::sort(peak_logits_.begin(), peak_logits_.end(),
              [](const std::pair<float, int> &a, const std::pair<float, int> &b)
              { return a.first > b.first; });
    if (static_cast<int>(peak_logits_.size()) > max_proposals_)
        peak_logits_.resize(max_proposals_);

    // The network was fed the bottom-left 16:9 band, so map network coordinates
    // into that band's space and then offset back to full-frame coordinates.
    // For sources at or below the band size this reduces to identity offsets.
    const int band_w = std::min(frame_size.width, kCropW);
    const int band_h = std::min(frame_size.height, kCropH);
    const int crop_x = 0;
    const int crop_y = frame_size.height - band_h;
    const float x_factor = static_cast<float>(band_w) / input_w_;
    const float y_factor = static_cast<float>(band_h) / input_h_;

    proposals.reserve(peak_logits_.size());
    for (const auto &pk : peak_logits_)
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

        proposals.push_back({sigmoid(pk.first), cv::Rect2f(x0, y0, x1 - x0, y1 - y0)});
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

void TinyTagDet::detect_physical(uint64_t luma_paddr, cv::Size full_frame_size,
                                 std::vector<Proposal> &proposals)
{
    if (!input_->aligned)
        throw std::runtime_error("detect_physical requires an aligned-input cvimodel");
    if (luma_paddr == 0)
        throw std::runtime_error("detect_physical received a null physical address");

    const double started = now_ms();
    const CVI_RC ret = CVI_NN_SetTensorPhysicalAddr(input_, luma_paddr);
    if (ret != CVI_RC_SUCCESS)
        throw std::runtime_error("CVI_NN_SetTensorPhysicalAddr failed (err " +
                                 std::to_string(ret) + ")");
    physical_input_bound_ = true;
    preprocess_ms_ = now_ms() - started;
    inference();
    decode_proposals(full_frame_size, proposals);
}

void TinyTagDet::detect_compact_physical(uint64_t luma_paddr, cv::Size input_frame_size,
                                         size_t input_stride, size_t input_length,
                                         const uint8_t *validation_copy,
                                         int input_crop_y,
                                         cv::Size full_frame_size,
                                         std::vector<Proposal> &proposals)
{
    if (input_->aligned)
        throw std::runtime_error(
            "detect_compact_physical is only for a compact cvimodel; use detect_physical");
    if (luma_paddr == 0)
        throw std::runtime_error("detect_compact_physical received a null physical address");
    if (input_->fmt != CVI_FMT_UINT8 || input_->pixel_format != CVI_NN_PIXEL_GRAYSCALE)
        throw std::runtime_error(
            "compact physical input requires a fused uint8 GRAYSCALE model tensor");
    if (input_frame_size.width != input_w_ || input_crop_y < 0 ||
        input_crop_y + input_h_ > input_frame_size.height)
        throw std::runtime_error("compact physical crop is outside the VPSS frame");

    const size_t dense_size = static_cast<size_t>(input_w_) * input_h_;
    if (input_stride != static_cast<size_t>(input_w_) ||
        input_length < input_stride * static_cast<size_t>(input_frame_size.height) ||
        input_->mem_size != dense_size)
        throw std::runtime_error(
            "compact physical crop is not a dense model tensor plane");

    std::vector<float> copied_output;
    if (validation_copy != nullptr)
    {
        // This happens exactly once, before SetTensorPhysicalAddr releases the
        // model's original input allocation. It proves the physical binding
        // against the ordinary copied-input path using the same frame bytes.
        set_input(validation_copy, dense_size);
        inference();
        copy_output(copied_output);
    }

    const double started = now_ms();
    const uint64_t crop_paddr = luma_paddr +
                                static_cast<uint64_t>(input_crop_y) * input_stride;
    const CVI_RC ret = CVI_NN_SetTensorPhysicalAddr(input_, crop_paddr);
    if (ret != CVI_RC_SUCCESS)
        throw std::runtime_error("CVI_NN_SetTensorPhysicalAddr failed (err " +
                                 std::to_string(ret) + ")");
    physical_input_bound_ = true;
    preprocess_ms_ = now_ms() - started;
    inference();

    if (validation_copy != nullptr)
    {
        std::vector<float> physical_output;
        copy_output(physical_output);
        const bool exact = copied_output.size() == physical_output.size() &&
                           std::memcmp(copied_output.data(), physical_output.data(),
                                       copied_output.size() * sizeof(float)) == 0;
        float max_abs = 0.0f;
        if (copied_output.size() == physical_output.size())
        {
            for (size_t i = 0; i < copied_output.size(); ++i)
                max_abs = std::max(max_abs, std::abs(copied_output[i] - physical_output[i]));
        }
        fprintf(stderr,
                "[model-input] compact physical same-frame validation exact=%d "
                "max-abs=%.9g elements=%zu\n",
                exact ? 1 : 0, max_abs, physical_output.size());
        if (!exact)
            throw std::runtime_error(
                "compact physical input differs from the ordinary copied-input result");
    }
    decode_proposals(full_frame_size, proposals);
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
    decoder_profile_ = TagDecoderProfile{};

    crop_rects_.clear();

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

        // Widen to the alignment grid, then re-clamp. Expanding outward keeps
        // the whole proposal and only ever adds context.
        int ax0 = x0, ax1 = x1;
        if (crop_align_ > 1)
        {
            const int a = crop_align_;
            ax0 = (x0 / a) * a;
            ax1 = ((x1 + a - 1) / a) * a;
            if (ax0 < 0)
                ax0 = 0;
            if (ax1 > full_res_gray.cols)
                ax1 = full_res_gray.cols;
        }

        // Zero-copy view; the decoder respects .step so no clone is needed.
        const cv::Rect crop_rect(ax0, y0, ax1 - ax0, y1 - y0);
        const cv::Mat crop = full_res_gray(crop_rect);
        crop_rects_.push_back(crop_rect);
        ++crop_count_;

        const auto tags = decoder_->detect(crop);
        const TagDecoderProfile &profile = decoder_->last_profile();
        decoder_profile_.threshold_ms += profile.threshold_ms;
        decoder_profile_.contour_ms += profile.contour_ms;
        decoder_profile_.quad_ms += profile.quad_ms;
        decoder_profile_.decode_ms += profile.decode_ms;
        decoder_profile_.refine_ms += profile.refine_ms;
        decoder_profile_.pixels += profile.pixels;
        decoder_profile_.contours += profile.contours;
        decoder_profile_.candidates += profile.candidates;
        decoder_profile_.attempts += profile.attempts;
        decoder_profile_.markers += profile.markers;
        decoder_profile_.point_ms += profile.point_ms;
        decoder_profile_.point_refined += profile.point_refined;
        decoder_profile_.point_refine_failed += profile.point_refine_failed;
        decoder_profile_.point_fallback_tried += profile.point_fallback_tried;
        decoder_profile_.point_fallback_decoded += profile.point_fallback_decoded;
        decoder_profile_.point_samples += profile.point_samples;

        for (const auto &tag : tags)
        {
            TinyTagResult result{};
            result.id = tag.id;
            result.proposal_confidence = proposal.confidence;
            result.roi = proposal.roi;
            const cv::Point2f origin(static_cast<float>(crop_rect.x),
                                     static_cast<float>(crop_rect.y));
            result.center = tag.center + origin;
            for (int corner = 0; corner < 4; ++corner)
                result.corners[corner] = tag.corners[corner] + origin;
            result.has_ideal = tag.has_ideal;
            for (int corner = 0; corner < 4 && tag.has_ideal; ++corner)
                result.ideal_corners[corner] = tag.ideal_corners[corner];
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
