// SPDX-License-Identifier: GPL-2.0-only
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/serial_core.h>
#include <linux/string.h>
#include <asm/csr.h>
#include "duos_nommu_log.h"

static void duos_console_write(struct console *con, const char *text, unsigned count)
{
    while (count--) duos_log_char(*text++);
}
static int __init duos_early_setup(struct earlycon_device *device, const char *options)
{
    device->con->write = duos_console_write;
    return 0;
}
EARLYCON_DECLARE(duos_shmlog, duos_early_setup);

#define MAILBOX 0x01900000ul
static bool duos_boot_reply(unsigned command)
{
    volatile u16 *lock = (void *)(MAILBOX + 0xc0 + 4*4);
    *lock = 0x100;
    asm volatile("fence iorw, iorw" ::: "memory");
    if (*lock != 0x100) return false;
    bool sent = false;
    for (unsigned i = 0; i < 8; ++i) {
        volatile u32 *slot = (void *)(MAILBOX + 0x400 + i*8);
        if (!(slot[0] & 0xffff0000u)) {
            slot[0] = (command & 0x8000u) | 6u | (0x52u << 8) | (1u << 24);
            slot[1] = 0x9e010000u;
            asm volatile("fence iorw, iorw" ::: "memory");
            *(volatile u32 *)(MAILBOX + 0x10) = 1u << i;
            *(volatile u32 *)(MAILBOX + 0x00) |= 1u << i;
            *(volatile u32 *)(MAILBOX + 0x60) = 1u << i;
            sent = true;
            break;
        }
    }
    asm volatile("fence iorw, iorw" ::: "memory");
    *lock = 0x100;
    return sent;
}
static int duos_mailbox(void *unused)
{
    for (;;) {
        unsigned pending = *(volatile u32 *)(MAILBOX + 0x38) & 0xff;
        for (unsigned i = 0; i < 8; ++i) if (pending & (1u << i)) {
            volatile u32 *slot = (void *)(MAILBOX + 0x400 + i*8);
            unsigned command = slot[0];
            *(volatile u32 *)(MAILBOX + 0x30) = 1u << i;
            *(volatile u32 *)(MAILBOX + 0x08) &= ~(1u << i);
            *(volatile u64 *)slot = 0;
            asm volatile("fence iorw, iorw" ::: "memory");
            if (((command >> 16) & 255) == 1 && (command & 255) == 6 &&
                ((command >> 8) & 127) == 0x51) {
                bool sent = false;
                /* Retry a contended mailbox without blocking the CPU. */
                for (unsigned retry = 0; retry < 20 && !sent; ++retry) {
                    sent = duos_boot_reply(command);
                    if (!sent) msleep(10);
                }
                pr_info("duos-nommu: main-core boot reply %s\n", sent ? "sent" : "FAILED");
            }
        }
        msleep(10);
    }
    return 0;
}

static int duos_heartbeat(void *unused)
{
    volatile struct duos_log *log = (void *)DUOS_LOG_ADDR;
    for (;;) {
        unsigned long page = __get_free_page(GFP_KERNEL);
        if (page) {
            memset((void *)page, 0x5a, PAGE_SIZE);
            if (*(volatile unsigned char *)(page + PAGE_SIZE - 1) != 0x5a)
                pr_err("duos-nommu: allocation verification failed\n");
            free_page(page);
        }
        ++log->heartbeat;
        log->ticks = csr_read(CSR_TIME);
        log->stage = 3;
        duos_clean_line(DUOS_LOG_ADDR);
        pr_info("duos-nommu: heartbeat=%u allocation=%s time=%llu\n",
                log->heartbeat, page ? "OK" : "FAILED", (unsigned long long)log->ticks);
        msleep(1000);
    }
    return 0;
}
static int __init duos_probe_init(void)
{
    struct task_struct *task;
    pr_info("duos-nommu: kernel-only scheduler probe; no userspace or devices\n");
    task = kthread_run(duos_mailbox, NULL, "duos-mailbox");
    if (IS_ERR(task)) return PTR_ERR(task);
    task = kthread_run(duos_heartbeat, NULL, "duos-heartbeat");
    return IS_ERR(task) ? PTR_ERR(task) : 0;
}
late_initcall(duos_probe_init);
