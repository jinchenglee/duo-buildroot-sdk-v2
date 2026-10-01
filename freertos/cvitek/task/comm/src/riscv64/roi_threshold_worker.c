#include <stdint.h>
#include <string.h>
#ifndef TT_THRESHOLD_BARE_METAL
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#endif
#include "arch_helpers.h"
#ifndef TT_THRESHOLD_BARE_METAL
#include "arch_cpu.h"
#endif
#include "cvi_board_memmap.h"
#include "roi_threshold_protocol.h"

#ifndef TT_THRESHOLD_BARE_METAL
static QueueHandle_t threshold_queue;
#endif
static uint32_t columns[TT_THRESHOLD_MAX_WIDTH];

static uint64_t ticks(void)
{
    uint64_t value;
    __asm__ volatile("rdtime %0" : "=r"(value) :: "memory");
    return value;
}
static uint64_t cycles(void)
{
    uint64_t value;
    __asm__ volatile("rdcycle %0" : "=r"(value) :: "memory");
    return value;
}
static int in_ion(uint32_t address, uint32_t bytes)
{
    return address >= CVIMMAP_ION_ADDR &&
           (uint64_t)address + bytes <= (uint64_t)CVIMMAP_ION_ADDR + CVIMMAP_ION_SIZE;
}
static void process(uint32_t address)
{
    struct tt_threshold_request request;
    struct tt_threshold_job *job = (void *)(uintptr_t)address;
    struct tt_threshold_completion *result;
    uint64_t service_start = ticks(), begin, cycle_begin;
    uint32_t pixels, status = TT_THRESHOLD_BAD_JOB;
    if ((address & 63u) || !in_ion(address, TT_THRESHOLD_HEADER_BYTES))
        return;
    inv_dcache_range(address, 64);
    memcpy(&request, &job->request, sizeof(request));
    if (request.magic != TT_THRESHOLD_MAGIC || request.version != TT_THRESHOLD_VERSION ||
        !request.sequence || request.total_bytes < TT_THRESHOLD_HEADER_BYTES ||
        !in_ion(address, request.total_bytes))
        return;
    result = &job->completion;
    memset(result, 0, sizeof(*result));
    result->magic = TT_THRESHOLD_MAGIC;
    result->version = TT_THRESHOLD_VERSION;
    result->timer_hz = configSYS_CLOCK_HZ;
#ifdef TT_THRESHOLD_BARE_METAL
    result->reserved[0] = TT_THRESHOLD_BARE_METAL_MAGIC;
    /* Read back cache/prefetch controls for diagnosis, outside kernel timing. */
    __asm__ volatile("csrr %0, mhcr" : "=r"(result->reserved[1]));
    __asm__ volatile("csrr %0, mhint" : "=r"(result->reserved[2]));
#endif
    pixels = request.width * request.height;
    if (request.operation == TT_THRESHOLD_OP_NOP) {
        status = TT_THRESHOLD_OK;
    } else if (request.operation == TT_THRESHOLD_OP_COMPUTE &&
               request.width && request.width <= TT_THRESHOLD_MAX_WIDTH &&
               request.height && request.height <= TT_THRESHOLD_MAX_HEIGHT &&
               request.kernel && (request.kernel & 1u) &&
               request.kernel <= TT_THRESHOLD_MAX_KERNEL &&
               request.input_offset >= TT_THRESHOLD_HEADER_BYTES &&
               !(request.input_offset & 63u) && !(request.output_offset & 63u) &&
               (uint64_t)request.input_offset + pixels <= request.output_offset &&
               (uint64_t)request.output_offset + pixels <= request.total_bytes) {
        uint8_t *input = (void *)(uintptr_t)(address + request.input_offset);
        uint8_t *output = (void *)(uintptr_t)(address + request.output_offset);
        inv_dcache_range((uintptr_t)input, pixels);
        /* Discard previous output cache contents before taking ownership. */
        inv_dcache_range((uintptr_t)output, pixels);
        begin = ticks();
        cycle_begin = cycles();
        status = tt_roi_threshold(input, request.width, output, request.width,
                                  request.width, request.height, request.kernel,
                                  request.threshold, columns) ? TT_THRESHOLD_BAD_JOB : TT_THRESHOLD_OK;
        result->compute_cycles = cycles() - cycle_begin;
        result->compute_ticks = ticks() - begin;
        flush_dcache_range((uintptr_t)output, pixels);
    }
    result->status = status;
    result->service_ticks = ticks() - service_start;
    __atomic_store_n(&result->sequence, request.sequence, __ATOMIC_RELEASE);
    flush_dcache_range((uintptr_t)result, sizeof(*result));
}
#ifndef TT_THRESHOLD_BARE_METAL
static void worker(void *unused)
{
    uint32_t address;
    (void)unused;
    for (;;) {
        if (xQueueReceive(threshold_queue, &address, portMAX_DELAY) == pdTRUE)
            process(address);
    }
}
/* The command task only dispatches. Long image work never runs in its high
 * priority context or in the mailbox ISR. One outstanding job per client. */
int roi_threshold_worker_init(void)
{
    threshold_queue = xQueueCreate(2, sizeof(uint32_t));
    if (!threshold_queue) return -1;
    if (xTaskCreate(worker, "roi-threshold", configMINIMAL_STACK_SIZE,
                    NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        vQueueDelete(threshold_queue);
        threshold_queue = NULL;
        return -1;
    }
    return 0;
}
int roi_threshold_worker_submit(uint32_t address)
{
    return threshold_queue && xQueueSend(threshold_queue, &address, 0) == pdTRUE ? 0 : -1;
}
#endif
