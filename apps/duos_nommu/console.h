/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DUOS_CONSOLE_H
#define DUOS_CONSOLE_H
#define DUOS_CONSOLE_ADDR 0x9ffe0000ul
#define DUOS_CONSOLE_BYTES 65536
#define DUOS_CONSOLE_MAGIC 0x434f4e53u
#define DUOS_INPUT_BYTES 1024u
#define DUOS_OUTPUT_BYTES 32768u
/* Each index has one writer and its own cache line. Unsigned wrap is valid. */
struct duos_index { volatile unsigned value; unsigned char pad[60]; };
struct duos_console {
    volatile unsigned magic, version;
    unsigned char pad[56];
    struct duos_index input_write, input_read, output_write, output_read;
    volatile unsigned char input[DUOS_INPUT_BYTES];
    volatile unsigned char output[DUOS_OUTPUT_BYTES];
};
#endif
