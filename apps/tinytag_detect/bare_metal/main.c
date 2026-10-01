/* Duo S ARM64 SD diagnostic firmware. No scheduler, interrupts or vendor
 * multimedia services. Reuse the exact FreeRTOS worker's compute/cache path. */
#include <stdint.h>
#include <stddef.h>
#define TT_THRESHOLD_BARE_METAL 1
#define configSYS_CLOCK_HZ 25000000u
#include "../../../freertos/cvitek/task/comm/src/riscv64/roi_threshold_worker.c"

void *memcpy(void *to, const void *from, size_t count)
{
    uint8_t *d = to;
    const uint8_t *s = from;
    while (count--) *d++ = *s++;
    return to;
}
void *memset(void *to, int value, size_t count)
{
    uint8_t *d = to;
    while (count--) *d++ = value;
    return to;
}
#define MB 0x01900000ul
#define REG(offset) (*(volatile uint32_t *)(MB + (offset)))
#define SLOT(index) ((volatile uint32_t *)(MB + 0x400 + (index) * 8))
/* Preserve the FSBL's 40-byte transfer structure for the Linux boot handshake.
 * Its checksum excludes the final status bytes. */
static uint8_t boot_info[40] __attribute__((aligned(64)));
static void reply_boot(uint32_t command)
{
    volatile uint16_t *lock = (void *)(MB + 0xc0 + 4 * 4);
    uint16_t token = 0x100; /* same RTOS owner encoding as vendor spinlock */
    uint64_t deadline = ticks() + configSYS_CLOCK_HZ / 5;
    do {
        *lock = token;
        __asm__ volatile("fence iorw, iorw" ::: "memory");
        if (*lock == token) {
            for (unsigned i = 0; i < 8; ++i) {
                volatile uint32_t *slot = SLOT(i);
                if (!(slot[0] & 0xffff0000u)) {
                    slot[0] = (command & 0x8000u) | 6u | (0x52u << 8) | (1u << 24);
                    slot[1] = (uintptr_t)boot_info;
                    __asm__ volatile("fence iorw, iorw" ::: "memory");
                    REG(0x10) = 1u << i; /* A53 receiver clear */
                    REG(0x00) |= 1u << i;
                    REG(0x60) = 1u << i;
                    break;
                }
            }
            __asm__ volatile("fence iorw, iorw" ::: "memory");
            *lock = token;
            return;
        }
    } while (ticks() < deadline);
}
int main(void)
{
    volatile uint32_t *initial = SLOT(0);
    for (unsigned i = 0; i < 10; ++i) {
        uint32_t word = initial[i];
        memcpy(boot_info + i * 4, &word, 4);
    }
    /* Accept only the ARM64 boot transfer structure; fail closed otherwise. */
    if (*(uint32_t *)boot_info != 0xa55aca53u ||
        *(uint32_t *)(boot_info + 4) != 36u)
        for (;;) __asm__ volatile("nop");
    unsigned checksum = 0;
    for (unsigned i = 0; i < 36; ++i) checksum += boot_info[i];
    if ((uint16_t)checksum != *(uint16_t *)(boot_info + 36))
        for (;;) __asm__ volatile("nop");
    boot_info[38] = 5; /* vendor type-1 running status for Linux handshake */
    flush_dcache_range((uintptr_t)boot_info, sizeof(boot_info));
    for (unsigned i = 0; i < 8; ++i) {
        SLOT(i)[0] = 0;
        SLOT(i)[1] = 0;
    }
    REG(0x30) = 0xff; /* clear C906L pending interrupts */
    REG(0x08) = 0;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    for (;;) {
        uint32_t pending = REG(0x38) & 0xff; /* poll masked mailbox status */
        for (unsigned i = 0; i < 8; ++i) if (pending & (1u << i)) {
            REG(0x30) = 1u << i;
            REG(0x08) &= ~(1u << i);
            __asm__ volatile("fence iorw, iorw" ::: "memory");
            volatile uint32_t *slot = SLOT(i);
            uint32_t command = slot[0], address = slot[1];
            /* Clear both words atomically, as in the vendor ISR, so Linux
             * cannot reuse a half-cleared slot. */
            *(volatile uint64_t *)slot = 0;
            __asm__ volatile("fence iorw, iorw" ::: "memory");
            if (((command >> 16) & 255) != 1 || (command & 255) != 6) continue;
            unsigned id = (command >> 8) & 127;
            if (id == TT_THRESHOLD_COMMAND) process(address);
            else if (id == 0x51) reply_boot(command);
            /* Other commands are consumed but unsupported. Benchmark only. */
        }
    }
}
