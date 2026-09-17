#ifndef TINYTAG_DET_H
#define TINYTAG_DET_H

#include "tag_crop_decoder.h"

#include <cviruntime.h>
#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

// One neural proposal, in full-frame pixel coordinates.
//
// This is the neural half of the K230 two-stage detector (see
// buildroot-overlay/package/ai_demo/tinytag_detect in the K230 SDK): the
// network proposes tag ROIs, and a traditional-CV AprilTag decoder reads the
// 36h11 ID out of each one at full resolution. Only the neural half is ported
// here -- the CV decode stage is a separate, much larger dependency, and the
// point of this application is to prove the SG2000 TPU path end to end.
struct Proposal
{
    float confidence; // sigmoid(heatmap) at the peak cell
    cv::Rect2f roi;   // full-frame pixel coords, after roi_expand + clamp
};

// One tag confirmed by the CV decoder, in full-frame pixel coordinates.
struct TinyTagResult
{
    int id;
    float proposal_confidence; // the neural proposal this came from
    cv::Rect2f roi;
    cv::Point2f center;
    cv::Point2f corners[4];
};

// TinyTag proposal detector on the cv181x/SG2000 TPU.
//
// The cvimodel is expected to have been built by
// tools/tinytag_cvimodel/compile_cvimodel.py, i.e. with --fuse_preprocess and
// --customization_format GRAYSCALE, so its input tensor is a raw uint8 luma
// plane and the /255 normalization happens on the TPU. The int8 (non-fused)
// case is handled too, so a model built without --fuse_preprocess still runs.
class TinyTagDet
{
public:
    // heatmap_thres: keep cells with sigmoid(heatmap) >= this. 0.35 is the K230
    //   production operating point; 0.20 is the training repo's frozen value and
    //   keeps more low-confidence proposals.
    // max_proposals: top-K cap on proposals per frame.
    // roi_expand: safety margin multiplied onto each decoded box.
    // roi_iou_thres: greedy box-level IoU suppression on the decoded ROIs;
    //   <= 0 disables it. Not part of the reference spec -- the spec's only NMS
    //   is the 3x3 max-pool over heatmap cells, which says nothing about how
    //   much the boxes those cells decode into overlap.
    TinyTagDet(const std::string &cvimodel_path, float heatmap_thres, int max_proposals,
               float roi_expand, float roi_iou_thres, int debug_mode = 0);
    ~TinyTagDet();

    TinyTagDet(const TinyTagDet &) = delete;
    TinyTagDet &operator=(const TinyTagDet &) = delete;

    // ori_img_gray must be CV_8UC1. Inputs larger than 1280x720 are first
    // cropped (zero-copy sub-Mat view) to the BOTTOM-LEFT 1280x720 band, so the
    // network sees an undistorted native 16:9 image rather than a non-uniform
    // stretch. Smaller inputs are used as-is: crop-only by design, no scale-up
    // and no letterbox pad. The band is then plain-resized into the input
    // tensor, matching the fixed scale factor the model was trained with.
    void pre_process(const cv::Mat &ori_img_gray);

    // Feed an already network-sized grayscale plane straight into the input
    // tensor, skipping the crop-and-resize. Used by the self-test, where the
    // golden bundle stores exactly the bytes the TPU is meant to see, so any
    // difference measured is the TPU's and not the resizer's.
    void set_input(const uint8_t *gray, size_t count);

    void inference();

    // Bind an aligned VPSS luma plane directly as the TPU input. The caller
    // retains ownership and must keep the physical buffer alive until this
    // synchronous call returns. Available only for an --aligned_input model.
    void detect_physical(uint64_t luma_paddr, cv::Size full_frame_size,
                         std::vector<Proposal> &proposals);

    // Experimental exception to the general aligned-input rule. A compact
    // model may be rebound directly only when the caller proves that the
    // external grayscale plane is byte-for-byte the model's dense tensor:
    // identical dimensions, stride == width, and sufficient plane length.
    // This keeps detect_physical()'s aligned-model safety contract intact.
    void detect_compact_physical(uint64_t luma_paddr, cv::Size input_frame_size,
                                 size_t input_stride, size_t input_length,
                                 const uint8_t *validation_copy,
                                 cv::Size full_frame_size,
                                 std::vector<Proposal> &proposals);
    bool uses_aligned_input() const { return input_->aligned; }

    // Copy the whole output tensor out as float, dequantizing if needed.
    void copy_output(std::vector<float> &out) const;

    // logit threshold -> 3x3 max-pool NMS -> top-K -> sigmoid only for the
    // surviving confidences -> center/size decode
    // -> roi_expand -> clamp -> optional IoU suppression. Proposals come back
    // in the coordinate space of the image passed to pre_process().
    void decode_proposals(cv::Size frame_size, std::vector<Proposal> &proposals);

    // Convenience: pre_process + inference + decode_proposals.
    void detect(const cv::Mat &ori_img_gray, std::vector<Proposal> &proposals);

    // Stage two: crop each proposal out of the full-resolution frame and run
    // the CV tag decoder over it. `full_res_gray` must be CV_8UC1 and the same
    // size as the image passed to pre_process(). Crops are zero-copy sub-Mat
    // views -- the decoder reads .step as a stride, so nothing is cloned.
    //
    // Only meaningful with a decoder installed; without one this is a no-op.
    void post_process(const cv::Mat &full_res_gray,
                      const std::vector<Proposal> &proposals,
                      std::vector<TinyTagResult> &results);

    void set_decoder(std::shared_ptr<TagCropDecoder> decoder) { decoder_ = std::move(decoder); }

    // Snap each crop's horizontal extent out to a multiple of `align` pixels.
    // One luma pixel is one byte, so align=4 starts every row of the crop on a
    // 4-byte boundary and makes its width a whole number of 32-bit words --
    // friendlier for the decoder's row scans. Only x is adjusted: each row
    // already begins at a multiple of the Mat's stride, so aligning y changes
    // no byte address. Expanding never loses image data, and the extra margin
    // is harmless to the decoder (it needs a quiet zone anyway). 0 or 1
    // disables. Default 4.
    void set_crop_align(int align) { crop_align_ = align > 1 ? align : 1; }
    int crop_align() const { return crop_align_; }

    // The aligned rectangles actually handed to the decoder on the last
    // post_process() call, for overlay/diagnostics.
    const std::vector<cv::Rect> &last_crop_rects() const { return crop_rects_; }
    bool has_decoder() const { return decoder_ != nullptr; }

    static void draw_proposals(cv::Mat &bgr, const std::vector<Proposal> &proposals);
    static void draw_detections(cv::Mat &bgr, const std::vector<TinyTagResult> &results);

    cv::Size input_size() const { return cv::Size(input_w_, input_h_); }
    int output_channels() const { return output_c_; }
    cv::Size output_size() const { return cv::Size(output_w_, output_h_); }
    size_t output_count() const
    {
        return static_cast<size_t>(output_c_) * output_h_ * output_w_;
    }
    double last_preprocess_ms() const { return preprocess_ms_; }
    double last_inference_ms() const { return inference_ms_; }
    double last_decode_ms() const { return decode_ms_; }
    double last_crop_decode_ms() const { return crop_decode_ms_; }
    size_t last_crop_count() const { return crop_count_; }
    const TagDecoderProfile &last_decoder_profile() const { return decoder_profile_; }

private:
    static float rect_iou(const cv::Rect2f &a, const cv::Rect2f &b);

    // Writes one network-sized grayscale plane into the input tensor, applying
    // whatever conversion the tensor's format needs.
    void store_input_plane(const uint8_t *src, size_t src_stride);

    CVI_MODEL_HANDLE model_ = nullptr;
    CVI_TENSOR *input_tensors_ = nullptr;
    CVI_TENSOR *output_tensors_ = nullptr;
    int32_t input_num_ = 0;
    int32_t output_num_ = 0;
    CVI_TENSOR *input_ = nullptr;
    CVI_TENSOR *output_ = nullptr;

    int input_w_ = 0;
    int input_h_ = 0;
    int output_c_ = 0;
    int output_h_ = 0;
    int output_w_ = 0;

    float heatmap_logit_thres_;
    int max_proposals_;
    float roi_expand_;
    float roi_iou_thres_;
    int debug_mode_;

    // Scratch reused across frames so a steady-state loop does no allocation.
    cv::Mat resized_;
    std::vector<std::pair<float, int>> peak_logits_;
    std::vector<float> dequantized_;

    double preprocess_ms_ = 0.0;
    double inference_ms_ = 0.0;
    double decode_ms_ = 0.0;
    double crop_decode_ms_ = 0.0;
    size_t crop_count_ = 0;
    int crop_align_ = 4;
    std::vector<cv::Rect> crop_rects_;
    TagDecoderProfile decoder_profile_;
    bool physical_input_bound_ = false;

    std::shared_ptr<TagCropDecoder> decoder_;

    // Head geometry. Keep in sync with tools/tinytag_cvimodel/prepare_calibration.py.
    static constexpr int kStride = 8;
    static constexpr int kCropW = 1280;
    static constexpr int kCropH = 720;
    static constexpr float kScaleClampLo = -4.0f;
    static constexpr float kScaleClampHi = 6.0f;
    // Channels 0-4 are the trained head (heatmap, offset_x, offset_y, scale_w,
    // scale_h); 5-20 are dormant corner/visibility outputs, deliberately unread.
    static constexpr int kTrainedChannels = 5;
    // Two proposals overlapping one physical tag decode to the same id; treat
    // hits closer than this as the same tag. Not a value from the training
    // repo -- a reasonable default, retune against real footage.
    static constexpr float kDedupeDistPx = 20.0f;
};

#endif // TINYTAG_DET_H
