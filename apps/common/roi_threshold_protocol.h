#ifndef TINYTAG_ROI_THRESHOLD_PROTOCOL_H
#define TINYTAG_ROI_THRESHOLD_PROTOCOL_H
#include "roi_threshold.h"

/* Experimental IP_SYSTEM command below the stock SYS_CMD_INFO_LIMIT.
 * One-way notification; completion is published in a separate cache line.
 * Do not use the stock SEND_WAIT ioctl: its implementation has error/lifetime
 * issues. No extra mailbox IRQ owner or kernel ABI change is needed. */
#define TT_THRESHOLD_COMMAND 0x40u
#define TT_THRESHOLD_MAGIC 0x54544831u
#define TT_THRESHOLD_VERSION 1u
#define TT_THRESHOLD_HEADER_BYTES 128u
#define TT_THRESHOLD_OP_COMPUTE 1u
#define TT_THRESHOLD_OP_NOP 2u
#define TT_THRESHOLD_OK 0u
#define TT_THRESHOLD_BAD_JOB 1u

struct tt_threshold_request {
    uint32_t magic, version, sequence, operation;
    uint32_t width, height, kernel;
    int32_t threshold;
    uint32_t input_offset, output_offset, total_bytes;
    uint32_t reserved[5];
};
struct tt_threshold_completion {
    uint32_t magic, version, status, timer_hz;
    uint64_t compute_ticks, compute_cycles, service_ticks;
    uint32_t reserved[5];
    uint32_t sequence; /* published last, with release ordering */
};
struct tt_threshold_job {
    struct tt_threshold_request request;       /* Linux owns before submit */
    struct tt_threshold_completion completion; /* FreeRTOS owns while active */
};
typedef char tt_request_size_check[sizeof(struct tt_threshold_request) == 64 ? 1 : -1];
typedef char tt_completion_size_check[sizeof(struct tt_threshold_completion) == 64 ? 1 : -1];
typedef char tt_header_size_check[sizeof(struct tt_threshold_job) == 128 ? 1 : -1];
#endif
