/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include "console.h"
static volatile sig_atomic_t stop;
static void interrupted(int sig) { (void)sig; stop = 1; }
int main(void)
{
    int lock = open("/tmp/duos-nommu-terminal.lock", O_CREAT|O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX|LOCK_NB)) { perror("terminal already attached"); return 1; }
    int fd = open("/dev/mem", O_RDWR|O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile struct duos_console *s = mmap(NULL, DUOS_CONSOLE_BYTES, PROT_READ|PROT_WRITE, MAP_SHARED, fd, DUOS_CONSOLE_ADDR);
    if (s == MAP_FAILED) { perror("mmap console"); return 1; }
    if (s->magic != DUOS_CONSOLE_MAGIC || s->version != 1) {
        fprintf(stderr, "No small-core console: magic=%08x version=%u\n", s->magic, s->version); return 1;
    }
    struct termios saved, raw;
    int tty = !tcgetattr(STDIN_FILENO, &saved);
    if (tty) { raw = saved; cfmakeraw(&raw); tcsetattr(STDIN_FILENO, TCSANOW, &raw); }
    signal(SIGINT, interrupted); signal(SIGTERM, interrupted); signal(SIGHUP, interrupted);
    fprintf(stderr, "Attached to C906L diagnostic shell; Ctrl-] detaches.\r\n");
    int result = 0;
    while (!stop) {
        unsigned consumed = s->output_read.value, write = s->output_write.value;
        __sync_synchronize();
        if (write - consumed > DUOS_OUTPUT_BYTES || s->magic != DUOS_CONSOLE_MAGIC) { result = 1; break; }
        while (consumed != write) {
            unsigned char c = s->output[consumed % DUOS_OUTPUT_BYTES];
            if (fputc(c, stdout) == EOF) { stop = 1; result = 1; break; }
            ++consumed;
        }
        fflush(stdout);
        __sync_synchronize(); s->output_read.value = consumed; __sync_synchronize();
        fd_set input; FD_ZERO(&input); FD_SET(0, &input);
        struct timeval delay = {0, 10000};
        if (select(1, &input, NULL, NULL, &delay) > 0) {
            unsigned char c;
            if (read(STDIN_FILENO, &c, 1) != 1) break;
            if (c == 29) break;
            unsigned w = s->input_write.value, r = s->input_read.value;
            __sync_synchronize();
            if (w - r >= DUOS_INPUT_BYTES) { fputc('\a', stderr); continue; }
            s->input[w % DUOS_INPUT_BYTES] = c;
            __sync_synchronize(); s->input_write.value = w+1; __sync_synchronize();
        }
    }
    if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    munmap((void *)s, DUOS_CONSOLE_BYTES); close(fd); close(lock);
    fprintf(stderr, "\r\nDetached.\n");
    return result;
}
