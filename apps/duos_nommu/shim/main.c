/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include "../log.h"
#define SLOT(index) ((volatile uint32_t *)(0x01900400ul + (index) * 8))
int main(void)
{
    volatile struct duos_log *log = (void *)DUOS_LOG_ADDR;
    volatile uint32_t *words = (void *)log;
    for (unsigned i = 0; i < 16; ++i) words[i] = 0;
    log->magic = DUOS_LOG_MAGIC;
    log->version = 1;
    log->stage = 1;
    asm volatile("csrr %0, mhartid" : "=r"(log->hart));
    duos_clean_line(DUOS_LOG_ADDR);
    duos_log_string("duos-nommu shim: M-mode entry, preserving ARM64 boot transfer\n");
    volatile uint8_t *info = (void *)0x9e010000ul;
    for (unsigned i = 0; i < 10; ++i)
        ((volatile uint32_t *)info)[i] = SLOT(0)[i];
    unsigned sum = 0;
    for (unsigned i = 0; i < 36; ++i) sum += info[i];
    if (*(volatile uint32_t *)info != 0xa55aca53u ||
        *(volatile uint32_t *)(info + 4) != 36 ||
        (uint16_t)sum != *(volatile uint16_t *)(info + 36)) {
        duos_log_string("STOP: boot transfer magic/length/checksum mismatch\n");
        for (;;) asm volatile("wfi");
    }
    info[38] = 5;
    duos_clean_line((unsigned long)info);
    for (unsigned i = 0; i < 8; ++i) { SLOT(i)[0] = 0; SLOT(i)[1] = 0; }
    *(volatile uint32_t *)0x01900030ul = 0xff;
    *(volatile uint32_t *)0x01900008ul = 0;
    log->stage = 2;
    duos_clean_line(DUOS_LOG_ADDR);
    duos_log_string("duos-nommu shim: entering Linux Image at 0x9e200000\n");
    unsigned long hart;
    asm volatile("csrr %0, mhartid\nfence iorw, iorw\nfence.i" : "=r"(hart) :: "memory");
    ((void (*)(unsigned long, unsigned long))0x9e200000ul)(hart, 0x9e020000ul);
    for (;;) asm volatile("wfi");
}
