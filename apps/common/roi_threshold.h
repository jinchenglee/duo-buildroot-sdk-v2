/* Plain-C counterpart of ArUco Nano's ROI boxFilter/subtract/threshold stage.
 * No allocation, OS, OpenCV or floating-point dependency. */
#ifndef TINYTAG_ROI_THRESHOLD_H
#define TINYTAG_ROI_THRESHOLD_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* OpenCV 3.2 uses SHIFT=16; OpenCV 4.12 uses SHIFT=23 for the
 * uint16 accumulator normalizer. Keep the production/A53 reference default.
 * A separately compiled experiment may select its matching library precision. */
#ifndef TT_THRESHOLD_FIXED_SHIFT
#define TT_THRESHOLD_FIXED_SHIFT 16u
#endif
#if TT_THRESHOLD_FIXED_SHIFT != 16 && TT_THRESHOLD_FIXED_SHIFT != 23
#error "Supported OpenCV fixed-point shifts are 16 and 23"
#endif

#define TT_THRESHOLD_MAX_WIDTH 1280u
#define TT_THRESHOLD_MAX_HEIGHT 800u
#define TT_THRESHOLD_MAX_KERNEL 63u

/* Inputs/outputs must not overlap. Columns has at least width uint32_t items.
 * Borders replicate the ROI itself (OpenCV BORDER_REPLICATE|BORDER_ISOLATED).
 * Mean follows the selected OpenCV uint8 normalizer (fixed reciprocal for area<=256),
 * subtraction saturates at zero, comparison is >. See the experiment guide. */
static inline int tt_roi_threshold(const uint8_t *__restrict src, uint32_t src_stride,
                                  uint8_t *__restrict dst, uint32_t dst_stride,
                                  uint32_t width, uint32_t height,
                                  uint32_t kernel, int32_t threshold,
                                  uint32_t *__restrict columns)
{
    uint32_t x, y, i, radius_x, radius_y, area, divisor_scale = 0, divisor_delta = 0;
    uint32_t cutoff[256];
    const uint32_t reciprocal = 1u << TT_THRESHOLD_FIXED_SHIFT;
    if (!src || !dst || !columns || !width || !height ||
        width > TT_THRESHOLD_MAX_WIDTH || height > TT_THRESHOLD_MAX_HEIGHT ||
        src_stride < width || dst_stride < width || !kernel ||
        !(kernel & 1u) || kernel > TT_THRESHOLD_MAX_KERNEL)
        return -1;
    if (threshold < 0 || threshold >= 255 || kernel == 1u) {
        for (y = 0; y < height; ++y)
            memset(dst + (size_t)y * dst_stride, threshold < 0 ? 255 : 0, width);
        return 0;
    }
    /* OpenCV collapses the corresponding kernel dimension for a one-row or
     * one-column isolated ROI, which also changes its normalization path. */
    radius_x = width == 1 ? 0 : kernel / 2u;
    radius_y = height == 1 ? 0 : kernel / 2u;
    area = (radius_x * 2u + 1u) * (radius_y * 2u + 1u);
    if (area > 1u && area <= 256u) {
        divisor_scale = reciprocal / area;
        divisor_delta = area / 2u;
        if ((reciprocal % area) * 2u < area)
            ++divisor_delta;
        else
            ++divisor_scale;
    }
    /* Invert the exact OpenCV comparison once for each possible input byte.
     * floor((sum+delta)*scale/reciprocal) > input+threshold iff sum >= cutoff[input].
     * This removes normalization, saturation and division from the pixel loop. */
    for (i = 0; i < 256u; ++i) {
        uint32_t target = i + (uint32_t)threshold + 1u;
        cutoff[i] = divisor_scale ?
            (target * reciprocal + divisor_scale - 1u) / divisor_scale - divisor_delta :
            target * area - area / 2u;
    }
    for (x = 0; x < width; ++x) {
        uint32_t sum = (radius_y + 1u) * src[x];
        for (i = 1; i <= radius_y; ++i) {
            uint32_t row = i < height ? i : height - 1u;
            sum += src[(size_t)row * src_stride + x];
        }
        columns[x] = sum;
    }
    for (y = 0; y < height; ++y) {
        uint32_t sum = (radius_x + 1u) * columns[0];
        const uint8_t *in = src + (size_t)y * src_stride;
        uint8_t *out = dst + (size_t)y * dst_stride;
        for (i = 1; i <= radius_x; ++i)
            sum += columns[i < width ? i : width - 1u];
        if (width > 2u * radius_x) {
            /* Replicated borders are separate from the common interior: no
             * clamp/edge branches or normalization choice per interior pixel. */
            for (x = 0; x < radius_x; ++x) {
                out[x] = sum >= cutoff[in[x]] ? 255 : 0;
                sum += columns[x + radius_x + 1u] - columns[0];
            }
            /* Load independent window deltas ahead of the running sum. Four
             * adjacent outputs expose independent byte/table loads instead
             * of paying one loop-control dependency for each pixel. */
            for (; x + radius_x + 4u < width; x += 4u) {
                uint32_t d0 = columns[x + radius_x + 1u] - columns[x - radius_x];
                uint32_t d1 = columns[x + radius_x + 2u] - columns[x - radius_x + 1u];
                uint32_t d2 = columns[x + radius_x + 3u] - columns[x - radius_x + 2u];
                uint32_t d3 = columns[x + radius_x + 4u] - columns[x - radius_x + 3u];
                uint32_t s1 = sum + d0, s2 = s1 + d1, s3 = s2 + d2;
                out[x] = sum >= cutoff[in[x]] ? 255 : 0;
                out[x + 1u] = s1 >= cutoff[in[x + 1u]] ? 255 : 0;
                out[x + 2u] = s2 >= cutoff[in[x + 2u]] ? 255 : 0;
                out[x + 3u] = s3 >= cutoff[in[x + 3u]] ? 255 : 0;
                sum = s3 + d3;
            }
            for (; x + radius_x + 1u < width; ++x) {
                out[x] = sum >= cutoff[in[x]] ? 255 : 0;
                sum += columns[x + radius_x + 1u] - columns[x - radius_x];
            }
            for (; x + 1u < width; ++x) {
                out[x] = sum >= cutoff[in[x]] ? 255 : 0;
                sum += columns[width - 1u] - columns[x - radius_x];
            }
            out[x] = sum >= cutoff[in[x]] ? 255 : 0;
        } else {
            for (x = 0; x < width; ++x) {
                out[x] = sum >= cutoff[in[x]] ? 255 : 0;
                if (x + 1u < width) {
                    uint32_t leave = x >= radius_x ? x - radius_x : 0;
                    uint32_t enter = x + radius_x + 1u;
                    if (enter >= width) enter = width - 1u;
                    sum = sum - columns[leave] + columns[enter];
                }
            }
        }
        if (y + 1u < height) {
            uint32_t leave = y >= radius_y ? y - radius_y : 0;
            uint32_t enter = y + radius_y + 1u;
            const uint8_t *old_row, *new_row;
            if (enter >= height) enter = height - 1u;
            old_row = src + (size_t)leave * src_stride;
            new_row = src + (size_t)enter * src_stride;
            for (x = 0; x < width; ++x)
                columns[x] = columns[x] - old_row[x] + new_row[x];
        }
    }
    return 0;
}
#endif
