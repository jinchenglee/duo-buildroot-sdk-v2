/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#define LOG_ADDR 0x9fff0000ul
#define LOG_BYTES 65536
struct log_header {
    uint32_t magic, version, written, heartbeat;
    uint64_t ticks, trap_cause, trap_pc, trap_value;
    uint32_t stage, hart;
    unsigned char padding[8];
    char text[LOG_BYTES - 64];
};
_Static_assert(sizeof(struct log_header) == LOG_BYTES, "log ABI");
int main(void)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile struct log_header *log = mmap(NULL, LOG_BYTES, PROT_READ, MAP_SHARED, fd, LOG_ADDR);
    if (log == MAP_FAILED) { perror("mmap reserved log"); close(fd); return 1; }
    if (log->magic != 0x4e4f4d4du || log->version != 1) {
        fprintf(stderr, "No NOMMU log header: magic=%08x version=%u\n", log->magic, log->version);
        munmap((void *)log, LOG_BYTES); close(fd); return 2;
    }
    fprintf(stderr, "stage=%u hart=%u heartbeat=%u ticks=%llu trap=%llx pc=%llx value=%llx\n",
            log->stage, log->hart, log->heartbeat, (unsigned long long)log->ticks,
            (unsigned long long)log->trap_cause, (unsigned long long)log->trap_pc,
            (unsigned long long)log->trap_value);
    uint32_t end = log->written;
    uint32_t count = end < sizeof(log->text) ? end : sizeof(log->text);
    for (uint32_t i = 0; i < count; ++i) putchar(log->text[(end-count+i) % sizeof(log->text)]);
    munmap((void *)log, LOG_BYTES); close(fd);
    return 0;
}
