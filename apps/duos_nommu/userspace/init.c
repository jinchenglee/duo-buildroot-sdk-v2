/* SPDX-License-Identifier: GPL-2.0-only */
/* Libc-free RV64 Linux syscall probe. No FPU, vector or dynamic linker. */
typedef unsigned long ulong;
struct timespec { long sec, nsec; };
static long syscall6(long nr, long x0, long x1, long x2, long x3, long x4, long x5)
{
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a2 asm("a2") = x2;
    register long a3 asm("a3") = x3;
    register long a4 asm("a4") = x4;
    register long a5 asm("a5") = x5;
    register long a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3),
                 "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
}
static char *decimal(char *p, ulong n)
{
    char digits[24]; unsigned count = 0;
    do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
    while (count) *p++ = digits[--count];
    return p;
}
static char *string(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}
static void report(long fd, const char *text, ulong n)
{
    char buf[192], *p = string(buf, "<6>duos-user: ");
    p = string(p, text); p = decimal(p, n); *p++ = '\n';
    long remaining = p - buf; p = buf;
    while (remaining > 0) {
        long written = syscall6(64, fd, (long)p, remaining, 0, 0, 0);
        if (written <= 0) break;
        p += written; remaining -= written;
    }
}
#ifndef DUOS_SHELL
__attribute__((noreturn)) void probe(void)
{
    long fd = syscall6(56, -100, (long)"/dev/kmsg", 1, 0, 0, 0);
    report(fd, "entered user mode; pid=", syscall6(172, 0, 0, 0, 0, 0, 0));
    ulong iterations = 0;
    for (;;) {
        unsigned allocation_ok = 0;
        long address = syscall6(222, 0, 4096, 3, 0x22, -1, 0);
        if ((ulong)address >= (ulong)-4095) {
            report(fd, "FAIL mmap errno=", -address);
        } else {
            volatile unsigned char *page = (void *)address;
            for (unsigned i = 0; i < 4096; ++i) page[i] = (i * 37 + iterations) & 255;
            unsigned errors = 0;
            for (unsigned i = 0; i < 4096; ++i)
                if (page[i] != ((i * 37 + iterations) & 255)) ++errors;
            if (errors) report(fd, "FAIL page verification errors=", errors);
            long result = syscall6(215, address, 4096, 0, 0, 0, 0);
            if (result) report(fd, "FAIL munmap errno=", -result);
            allocation_ok = !errors && !result;
        }
        struct timespec now = {0, 0}, delay = {1, 0}, rest;
        long result = syscall6(113, 1, (long)&now, 0, 0, 0, 0);
        if (result) report(fd, "FAIL clock_gettime errno=", -result);
        report(fd, "iteration=", ++iterations);
        if (allocation_ok) report(fd, "allocation cycle OK; count=", iterations);
        report(fd, "monotonic seconds=", now.sec);
        do {
            result = syscall6(101, (long)&delay, (long)&rest, 0, 0, 0, 0);
            if (result == -4) delay = rest;
        } while (result == -4);
        if (result) report(fd, "FAIL nanosleep errno=", -result);
    }
}
#else
#include "shell.inc"
#endif
asm(".pushsection .text.start,\"ax\"\n.global _start\n_start:\n"
    "andi sp, sp, -16\ncall probe\n1: j 1b\n"
    ".popsection\n.pushsection .note.GNU-stack,\"\",@progbits\n.popsection\n");
