/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DUOS_NOMMU_LOG_H
#define DUOS_NOMMU_LOG_H
#define DUOS_LOG_ADDR 0x9fff0000ul
#define DUOS_LOG_BYTES 65536u
#define DUOS_LOG_MAGIC 0x4e4f4d4du
struct duos_log {
    volatile unsigned magic, version, written, heartbeat;
    volatile unsigned long ticks, trap_cause, trap_pc, trap_value;
    volatile unsigned stage, hart;
    unsigned char padding[8];
    volatile char text[DUOS_LOG_BYTES - 64];
};
_Static_assert(sizeof(struct duos_log) == DUOS_LOG_BYTES, "shared log ABI");
/* Match the vendor cache.c dcache.cpa a0 / sync.s encodings. DDR is not
 * assumed coherent across A53 and C906L. */
static inline void duos_clean_line(unsigned long address)
{
    register unsigned long line asm("a0") = address & ~63ul;
    asm volatile(".long 0x0295000b\n.long 0x0190000b" : "+r"(line) :: "memory");
}
static inline void duos_log_char(char c)
{
    volatile struct duos_log *log = (void *)DUOS_LOG_ADDR;
    unsigned count = log->written;
    log->text[count % sizeof(log->text)] = c;
    duos_clean_line((unsigned long)&log->text[count % sizeof(log->text)]);
    log->written = count + 1;
    duos_clean_line(DUOS_LOG_ADDR);
}
static inline void duos_log_string(const char *s)
{
    while (*s) duos_log_char(*s++);
}
#endif
