// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "duos_nommu_log.h"
#include "duos_nommu_console.h"
static struct duos_console *shared = (void *)DUOS_CONSOLE_ADDR;
static DEFINE_MUTEX(input_lock);
static DEFINE_MUTEX(output_lock);
static void invalidate(unsigned long address)
{
    register unsigned long line asm("a0") = address & ~63ul;
    asm volatile(".long 0x02a5000b\n.long 0x0190000b" : "+r"(line) :: "memory");
}
static ssize_t console_read(struct file *file, char __user *buf, size_t count, loff_t *off)
{
    ssize_t done = 0;
    if (!count) return 0;
    if (mutex_lock_interruptible(&input_lock)) return -ERESTARTSYS;
    while (!done) {
        invalidate((unsigned long)&shared->input_write);
        unsigned read = shared->input_read.value, write = shared->input_write.value;
        if (write - read > DUOS_INPUT_BYTES) { done = -EIO; break; }
        while (read != write && done < count) {
            volatile unsigned char *p = &shared->input[read % DUOS_INPUT_BYTES];
            invalidate((unsigned long)p);
            unsigned char c = *p;
            if (put_user(c, buf + done)) { if (!done) done = -EFAULT; goto out; }
            ++read; ++done;
            shared->input_read.value = read;
            duos_clean_line((unsigned long)&shared->input_read);
        }
        if (!done) {
            if (file->f_flags & O_NONBLOCK) { done = -EAGAIN; break; }
            if (msleep_interruptible(5)) { done = -ERESTARTSYS; break; }
        }
    }
out:
    mutex_unlock(&input_lock);
    return done;
}
static ssize_t console_write(struct file *file, const char __user *buf, size_t count, loff_t *off)
{
    ssize_t done = 0;
    if (mutex_lock_interruptible(&output_lock)) return -ERESTARTSYS;
    while (done < count) {
        invalidate((unsigned long)&shared->output_read);
        unsigned write = shared->output_write.value, read = shared->output_read.value;
        if (write - read > DUOS_OUTPUT_BYTES) { if (!done) done = -EIO; break; }
        if (write - read == DUOS_OUTPUT_BYTES) {
            if (file->f_flags & O_NONBLOCK) { if (!done) done = -EAGAIN; break; }
            if (msleep_interruptible(5)) { if (!done) done = -ERESTARTSYS; break; }
            continue;
        }
        unsigned char c;
        if (get_user(c, buf + done)) { if (!done) done = -EFAULT; break; }
        volatile unsigned char *p = &shared->output[write % DUOS_OUTPUT_BYTES];
        *p = c;
        duos_clean_line((unsigned long)p);
        shared->output_write.value = write + 1;
        duos_clean_line((unsigned long)&shared->output_write);
        ++done;
    }
    mutex_unlock(&output_lock);
    return done;
}
static const struct file_operations console_ops = {
    .owner = THIS_MODULE, .read = console_read, .write = console_write,
    .open = nonseekable_open,
};
static struct miscdevice console_device = {
    .minor = 250, .name = "duos-console", .fops = &console_ops,
};
static int __init console_init(void)
{
    memset(shared, 0, sizeof(*shared));
    shared->version = 1;
    for (unsigned long p = (unsigned long)shared; p < (unsigned long)shared + sizeof(*shared); p += 64)
        duos_clean_line(p);
    /* Publish magic after initialization. Only C906 initializes the rings. */
    shared->magic = DUOS_CONSOLE_MAGIC;
    duos_clean_line((unsigned long)shared);
    return misc_register(&console_device);
}
device_initcall(console_init);
