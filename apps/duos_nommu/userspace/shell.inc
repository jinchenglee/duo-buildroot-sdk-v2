/* SPDX-License-Identifier: GPL-2.0-only */
static long console_fd;
static void output(const char *s)
{
    const char *end = s;
    while (*end) ++end;
    while (end != s) {
        long n = syscall6(64, console_fd, (long)s, end-s, 0, 0, 0);
        if (n <= 0) return;
        s += n;
    }
}
static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static void value(const char *label, ulong n)
{
    char b[128], *p = string(b, label);
    p = decimal(p, n); p = string(p, "\r\n"); *p = 0;
    output(b);
}
static void memory_test(void)
{
    long address = syscall6(222, 0, 4096, 3, 0x22, -1, 0);
    if ((ulong)address >= (ulong)-4095) { value("FAIL mmap errno=", -address); return; }
    volatile unsigned char *p = (void *)address;
    unsigned errors = 0;
    for (unsigned i = 0; i < 4096; ++i) p[i] = (i*37)&255;
    for (unsigned i = 0; i < 4096; ++i) if (p[i] != ((i*37)&255)) ++errors;
    long result = syscall6(215, address, 4096, 0, 0, 0, 0);
    if (errors) value("FAIL verification errors=", errors);
    else if (result) value("FAIL munmap errno=", -result);
    else output("OK: allocated, checked and released 4096 bytes\r\n");
}
static void command(char *line)
{
    char *args = line;
    while (*args && *args != ' ') ++args;
    if (*args) *args++ = 0;
    while (*args == ' ') ++args;
    if (!*line) return;
    if (equal(line, "help")) {
        output("Builtins: help, echo TEXT, uname, uptime, pid, memtest, sleep SECONDS\r\n"
               "Diagnostic shell: no external programs, pipes or job control.\r\n"
               "Ctrl-] detaches the A53 terminal; it does not stop this OS.\r\n");
    } else if (equal(line, "echo")) { output(args); output("\r\n"); }
    else if (equal(line, "pid")) value("PID=", syscall6(172,0,0,0,0,0,0));
    else if (equal(line, "memtest")) memory_test();
    else if (equal(line, "uptime")) {
        struct timespec time;
        long result = syscall6(113,1,(long)&time,0,0,0,0);
        if (result) value("FAIL clock_gettime errno=",-result);
        else value("Monotonic seconds=",time.sec);
    } else if (equal(line, "uname")) {
        char name[6][65];
        long result = syscall6(160,(long)name,0,0,0,0,0);
        if (result) value("FAIL uname errno=",-result);
        else { output(name[0]); output(" "); output(name[2]); output(" "); output(name[4]); output("\r\n"); }
    } else if (equal(line, "sleep")) {
        ulong n = 0; char *p = args;
        while (*p >= '0' && *p <= '9' && n <= 3600) n = n*10 + (*p++-'0');
        if (!*args || *p || n > 3600) { output("Usage: sleep 0..3600\r\n"); return; }
        struct timespec delay = {(long)n,0}, rest;
        long result;
        do {
            result = syscall6(101,(long)&delay,(long)&rest,0,0,0,0);
            if (result == -4) delay = rest;
        } while (result == -4);
        if (result) value("FAIL nanosleep errno=",-result);
    } else output("Unknown command; type help\r\n");
}
__attribute__((noreturn)) void probe(void)
{
    long log = syscall6(56,-100,(long)"/dev/kmsg",1,0,0,0);
    console_fd = syscall6(56,-100,(long)"/dev/duos-console",2,0,0,0);
    if (console_fd < 0) {
        report(log,"FAIL console open errno=",-console_fd);
        for (;;) { struct timespec delay = {1,0}; syscall6(101,(long)&delay,0,0,0,0,0); }
    }
    report(log,"diagnostic shell ready; pid=",syscall6(172,0,0,0,0,0,0));
    output("\r\nC906L Linux NOMMU diagnostic shell. Type help.\r\n");
    char line[128]; unsigned used = 0;
    output("c906l# ");
    for (;;) {
        char c;
        long result = syscall6(63,console_fd,(long)&c,1,0,0,0);
        if (result != 1) continue;
        if (c == '\r' || c == '\n') {
            output("\r\n"); line[used] = 0; command(line); used = 0; output("c906l# ");
        } else if (c == 3) { used = 0; output("^C\r\nc906l# "); }
        else if (c == 8 || c == 127) { if (used) { --used; output("\b \b"); } }
        else if (c >= 32 && c < 127 && used < sizeof(line)-1) {
            line[used++] = c; char echo[2] = {c,0}; output(echo);
        }
    }
}
