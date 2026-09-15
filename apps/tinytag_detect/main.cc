// Still-image TinyTag proposal detector for the Milk-V Duo S (SG2000 TPU).
//
// Loads a cvimodel built by tools/tinytag_cvimodel/compile_cvimodel.py, runs it
// over one image, and reports the decoded proposal ROIs. This is the neural
// half of the K230 two-stage detector; the traditional-CV AprilTag decode stage
// is intentionally not ported (see tinytag_det.h).

#include "tinytag_det.h"
#include "golden.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {

void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s <cvimodel> <image> [options]        detect on one image\n", argv0);
    printf("  %s <cvimodel> --selftest <bundle>      verify against golden references\n\n", argv0);
    printf("Options:\n");
    printf("  --thres <f>       heatmap threshold        (default 0.35)\n");
    printf("  --max <n>         max proposals            (default 8)\n");
    printf("  --expand <f>      ROI expansion factor     (default 1.5)\n");
    printf("  --iou <f>         ROI IoU suppression      (default 0.5, <=0 disables)\n");
    printf("  --out <path>      write annotated image    (default tinytag_det.jpg)\n");
    printf("  --repeat <n>      inference runs, for timing (default 1)\n");
    printf("  --warmup <n>      untimed runs first       (default 2)\n");
    printf("  --max-mae <f>     self-test error gate     (default 0.05)\n");
    printf("  --debug <0|1|2>   verbosity                (default 1)\n");
    printf("\nThe detection defaults match the K230 production operating point.\n");
}

struct Stats
{
    double min = 0.0, median = 0.0, mean = 0.0, max = 0.0;
};

Stats summarize(std::vector<double> samples)
{
    Stats stats;
    if (samples.empty())
        return stats;
    std::sort(samples.begin(), samples.end());
    stats.min = samples.front();
    stats.max = samples.back();
    stats.median = samples[samples.size() / 2];
    double total = 0.0;
    for (double value : samples)
        total += value;
    stats.mean = total / samples.size();
    return stats;
}

void print_stats(const char *label, const Stats &stats)
{
    printf("  %-12s min %7.3f  median %7.3f  mean %7.3f  max %7.3f\n",
           label, stats.min, stats.median, stats.mean, stats.max);
}

// Mean absolute difference, plus the largest single-element difference, which
// catches a localized corruption that an averaged figure would hide.
void compare(const std::vector<float> &a, const std::vector<float> &b,
             double &mae, double &max_abs)
{
    double total = 0.0;
    max_abs = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double diff = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        total += diff;
        if (diff > max_abs)
            max_abs = diff;
    }
    mae = a.empty() ? 0.0 : total / a.size();
}

// Grid position of the maximum of channel 0 (the heatmap).
void peak_cell(const std::vector<float> &tensor, int h, int w, int &py, int &px)
{
    size_t best = 0;
    for (size_t i = 1; i < static_cast<size_t>(h) * w; ++i)
        if (tensor[i] > tensor[best])
            best = i;
    py = static_cast<int>(best) / w;
    px = static_cast<int>(best) % w;
}

int run_selftest(TinyTagDet &detector, const std::string &bundle_path,
                 int repeat, int warmup, double max_mae)
{
    const GoldenBundle bundle = load_golden(bundle_path);
    printf("\nself-test: %zu frame(s) from %s\n", bundle.frames.size(), bundle_path.c_str());
    printf("input %ux%u, output [%u,%u,%u]\n\n",
           bundle.in_w, bundle.in_h, bundle.out_c, bundle.out_h, bundle.out_w);

    if (static_cast<uint32_t>(detector.input_size().width) != bundle.in_w ||
        static_cast<uint32_t>(detector.input_size().height) != bundle.in_h)
    {
        printf("FAIL: model input is %dx%d but the bundle holds %ux%u frames\n",
               detector.input_size().width, detector.input_size().height,
               bundle.in_w, bundle.in_h);
        return 1;
    }
    if (detector.output_count() != bundle.output_count())
    {
        printf("FAIL: model output has %zu elements, bundle has %zu\n",
               detector.output_count(), bundle.output_count());
        return 1;
    }

    std::vector<double> inference_samples;
    std::vector<float> actual;
    double worst_fp32_mae = 0.0;
    double worst_sim_mae = 0.0;
    double worst_sim_max = 0.0;
    int peak_matches = 0;

    printf("%-28s %10s %10s %10s %6s\n",
           "frame", "vs-fp32", "vs-sim", "sim-maxdiff", "peak");
    for (const GoldenFrame &frame : bundle.frames)
    {
        detector.set_input(frame.input.data(), frame.input.size());

        for (int i = 0; i < warmup; ++i)
            detector.inference();
        for (int i = 0; i < repeat; ++i)
        {
            detector.inference();
            inference_samples.push_back(detector.last_inference_ms());
        }
        detector.copy_output(actual);

        double fp32_mae = 0.0, fp32_max = 0.0;
        double sim_mae = 0.0, sim_max = 0.0;
        compare(actual, frame.fp32, fp32_mae, fp32_max);
        compare(actual, frame.sim8, sim_mae, sim_max);

        int ref_y = 0, ref_x = 0, act_y = 0, act_x = 0;
        peak_cell(frame.fp32, bundle.out_h, bundle.out_w, ref_y, ref_x);
        peak_cell(actual, bundle.out_h, bundle.out_w, act_y, act_x);
        const bool peak_ok = std::abs(ref_y - act_y) <= 2 && std::abs(ref_x - act_x) <= 2;
        if (peak_ok)
            ++peak_matches;

        worst_fp32_mae = std::max(worst_fp32_mae, fp32_mae);
        worst_sim_mae = std::max(worst_sim_mae, sim_mae);
        worst_sim_max = std::max(worst_sim_max, sim_max);

        printf("%-28s %10.5f %10.6f %10.6f %6s\n",
               frame.name.c_str(), fp32_mae, sim_mae, sim_max, peak_ok ? "ok" : "MOVED");
    }

    const size_t frames = bundle.frames.size();
    printf("\n---- accuracy ----\n");
    printf("hardware-INT8 vs FP32 : worst frame MAE %.5f  (gate <= %.5f)\n",
           worst_fp32_mae, max_mae);
    printf("heatmap peak match    : %d/%zu within 2 cells\n", peak_matches, frames);
    printf("hardware vs simulator : worst frame MAE %.6f, worst element %.6f\n",
           worst_sim_mae, worst_sim_max);

    printf("\n---- inference timing (TPU only, %zu runs) ----\n", inference_samples.size());
    const Stats stats = summarize(inference_samples);
    print_stats("ms", stats);
    if (stats.median > 0.0)
        printf("  %-12s %.1f inferences/s at the median\n", "throughput", 1000.0 / stats.median);

    bool failed = false;
    if (worst_fp32_mae > max_mae)
    {
        printf("\nFAIL: quantization error %.5f exceeds the %.5f gate\n",
               worst_fp32_mae, max_mae);
        failed = true;
    }
    if (peak_matches < static_cast<int>(frames))
    {
        printf("\nFAIL: %zu frame(s) moved their heatmap peak by more than 2 cells\n",
               frames - peak_matches);
        failed = true;
    }
    // The simulator and the real TPU execute the same instruction stream, so
    // they should agree to the bit. Any real divergence means the model was
    // signed off against something the hardware does not reproduce.
    if (worst_sim_max > 1e-3)
    {
        printf("\nFAIL: hardware diverges from the simulator by up to %.6f. The model was "
               "validated against the simulator, so that validation does not hold here.\n",
               worst_sim_max);
        failed = true;
    }

    printf("\n%s\n", failed ? "SELF-TEST FAILED" : "SELF-TEST PASSED");
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        usage(argv[0]);
        return 1;
    }

    const std::string cvimodel_path = argv[1];

    // Either "<cvimodel> <image>" or "<cvimodel> --selftest <bundle>".
    std::string image_path;
    std::string selftest_path;
    int first_option = 3;
    if (std::string(argv[2]) == "--selftest")
    {
        if (argc < 4)
        {
            printf("--selftest needs a bundle path\n\n");
            usage(argv[0]);
            return 1;
        }
        selftest_path = argv[3];
        first_option = 4;
    }
    else
    {
        image_path = argv[2];
    }

    float heatmap_thres = 0.35f;
    int max_proposals = 8;
    float roi_expand = 1.5f;
    float roi_iou_thres = 0.5f;
    std::string output_path = "tinytag_det.jpg";
    int repeat = 1;
    int warmup = 2;
    double max_mae = 0.05;
    int debug_mode = 1;

    for (int i = first_option; i < argc; ++i)
    {
        const std::string flag = argv[i];
        const bool has_value = (i + 1 < argc);
        if (flag == "--thres" && has_value)       heatmap_thres = std::atof(argv[++i]);
        else if (flag == "--max" && has_value)    max_proposals = std::atoi(argv[++i]);
        else if (flag == "--expand" && has_value) roi_expand = std::atof(argv[++i]);
        else if (flag == "--iou" && has_value)    roi_iou_thres = std::atof(argv[++i]);
        else if (flag == "--out" && has_value)    output_path = argv[++i];
        else if (flag == "--repeat" && has_value) repeat = std::atoi(argv[++i]);
        else if (flag == "--warmup" && has_value) warmup = std::atoi(argv[++i]);
        else if (flag == "--max-mae" && has_value) max_mae = std::atof(argv[++i]);
        else if (flag == "--debug" && has_value)  debug_mode = std::atoi(argv[++i]);
        else
        {
            printf("Unknown or incomplete option: %s\n\n", flag.c_str());
            usage(argv[0]);
            return 1;
        }
    }
    if (repeat < 1)
        repeat = 1;
    if (warmup < 0)
        warmup = 0;

    try
    {
        if (!selftest_path.empty())
        {
            TinyTagDet detector(cvimodel_path, heatmap_thres, max_proposals, roi_expand,
                                roi_iou_thres, debug_mode);
            return run_selftest(detector, selftest_path, repeat, warmup, max_mae);
        }

        const cv::Mat gray = cv::imread(image_path, cv::IMREAD_GRAYSCALE);
        if (gray.empty())
        {
            printf("Could not open or decode image: %s\n", image_path.c_str());
            return 1;
        }
        if (debug_mode > 0)
            printf("image  : %s (%dx%d)\n", image_path.c_str(), gray.cols, gray.rows);

        TinyTagDet detector(cvimodel_path, heatmap_thres, max_proposals, roi_expand,
                            roi_iou_thres, debug_mode);

        std::vector<Proposal> proposals;
        // The first inference pays one-off setup the steady state does not, so
        // warm up before measuring; otherwise a --repeat 1 figure overstates the
        // per-frame cost a real application would see.
        for (int i = 0; i < warmup; ++i)
            detector.detect(gray, proposals);

        std::vector<double> preprocess_samples, inference_samples, decode_samples, total_samples;
        for (int i = 0; i < repeat; ++i)
        {
            detector.detect(gray, proposals);
            preprocess_samples.push_back(detector.last_preprocess_ms());
            inference_samples.push_back(detector.last_inference_ms());
            decode_samples.push_back(detector.last_decode_ms());
            total_samples.push_back(detector.last_preprocess_ms() +
                                    detector.last_inference_ms() +
                                    detector.last_decode_ms());
        }

        printf("\n%zu proposal(s) at threshold %.2f\n", proposals.size(), heatmap_thres);
        for (size_t i = 0; i < proposals.size(); ++i)
        {
            const Proposal &p = proposals[i];
            printf("  [%zu] conf %.3f  roi x=%.1f y=%.1f w=%.1f h=%.1f\n",
                   i, p.confidence, p.roi.x, p.roi.y, p.roi.width, p.roi.height);
        }

        if (debug_mode > 0)
        {
            printf("\n---- timing, ms over %d run(s) after %d warmup ----\n", repeat, warmup);
            print_stats("pre_process", summarize(preprocess_samples));
            print_stats("inference", summarize(inference_samples));
            print_stats("decode", summarize(decode_samples));
            print_stats("total", summarize(total_samples));

            const Stats inference_stats = summarize(inference_samples);
            const Stats total_stats = summarize(total_samples);
            if (inference_stats.median > 0.0)
                printf("\n  TPU inference    : %.1f/s at the median\n",
                       1000.0 / inference_stats.median);
            if (total_stats.median > 0.0)
                printf("  this pipeline    : %.1f frames/s at the median\n",
                       1000.0 / total_stats.median);
            printf("  NOTE: a full two-stage detector adds a per-ROI CV decode stage,\n");
            printf("        which dominated the K230 budget (~10.7 of ~15.25 ms/frame).\n");
        }

        if (!output_path.empty())
        {
            cv::Mat annotated;
            cv::cvtColor(gray, annotated, cv::COLOR_GRAY2BGR);
            TinyTagDet::draw_proposals(annotated, proposals);
            if (cv::imwrite(output_path, annotated))
                printf("\nwrote %s\n", output_path.c_str());
            else
                printf("\nfailed to write %s\n", output_path.c_str());
        }
    }
    catch (const std::exception &error)
    {
        printf("Error: %s\n", error.what());
        return 1;
    }

    return 0;
}
