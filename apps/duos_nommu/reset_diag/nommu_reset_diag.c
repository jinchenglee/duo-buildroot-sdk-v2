// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic only: never initiates or bypasses shutdown/reset. */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/syscore_ops.h>
#include <linux/timer.h>
static void __iomem *uart;
static struct task_struct *reboot_task;
static struct timer_list timer;
static unsigned samples;
static int saved_loglevel;
static void dump_blocked_tasks(void)
{
    struct task_struct *group, *task, *blocked[64];
    unsigned count = 0, i;
    bool truncated = false;
    /* Hold task references, then print outside the RCU read section. */
    rcu_read_lock();
    for_each_process_thread(group, task) {
        if (!(READ_ONCE(task->state) & TASK_UNINTERRUPTIBLE)) continue;
        if (count == ARRAY_SIZE(blocked)) { truncated = true; continue; }
        get_task_struct(task);
        blocked[count++] = task;
    }
    rcu_read_unlock();
    pr_info("nommu-reset: blocked-task snapshot count=%u truncated=%u\n", count, truncated);
    for (i = 0; i < count; ++i) {
        sched_show_task(blocked[i]);
        put_task_struct(blocked[i]);
    }
}
static void raw(const char *text)
{
    /* UART0 already configured by the main OS. Bound every hardware wait. */
    while (*text) {
        unsigned spin = 100000;
        while (!(readl(uart + 0x14) & 0x20) && --spin) cpu_relax();
        if (!spin) return;
        writel(*text++, uart);
    }
}
static void trace_timer(struct timer_list *unused)
{
    raw("\r\n[nommu-reset] still waiting; shutdown task stack follows\r\n");
    if (reboot_task) sched_show_task(reboot_task);
    if (!samples) dump_blocked_tasks();
    if (++samples < 10) mod_timer(&timer, jiffies + 2*HZ);
}
static int begin(struct notifier_block *nb, unsigned long event, void *data)
{
    if (event != SYS_RESTART) return NOTIFY_DONE;
    /* Production boots can suppress ordinary printk stack messages. */
    console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
    if (!reboot_task) { reboot_task = current; get_task_struct(reboot_task); }
    samples = 0;
    raw("\r\n[nommu-reset] reboot notifiers begin\r\n");
    mod_timer(&timer, jiffies + 2*HZ);
    return NOTIFY_DONE;
}
static int end(struct notifier_block *nb, unsigned long event, void *data)
{
    if (event == SYS_RESTART)
        raw("\r\n[nommu-reset] reboot notifiers completed; device shutdown next\r\n");
    return NOTIFY_DONE;
}
static void syscore(void)
{
    raw("\r\n[nommu-reset] device shutdown completed; syscore shutdown reached\r\n");
}
static int restart(struct notifier_block *nb, unsigned long event, void *data)
{
    raw("\r\n[nommu-reset] restart handler chain reached\r\n");
    return NOTIFY_DONE;
}
static struct notifier_block first = { .notifier_call = begin, .priority = INT_MAX };
static struct notifier_block last = { .notifier_call = end, .priority = INT_MIN };
static struct notifier_block reset = { .notifier_call = restart, .priority = 255 };
static struct syscore_ops core = { .shutdown = syscore };
static int __init start(void)
{
    int err;
    uart = ioremap(0x04140000, 0x100);
    if (!uart) return -ENOMEM;
    timer_setup(&timer, trace_timer, 0);
    saved_loglevel = console_loglevel;
    err = register_reboot_notifier(&first);
    if (err) goto unmap;
    err = register_reboot_notifier(&last);
    if (err) goto undo_first;
    err = register_restart_handler(&reset);
    if (err) goto undo_last;
    register_syscore_ops(&core);
    console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
    pr_info("nommu-reset: diagnostic armed; no reset behavior changed\n");
    dump_blocked_tasks();
    console_loglevel = saved_loglevel;
    return 0;
undo_last: unregister_reboot_notifier(&last);
undo_first: unregister_reboot_notifier(&first);
unmap: iounmap(uart); return err;
}
static void __exit stop(void)
{
    unregister_syscore_ops(&core);
    unregister_restart_handler(&reset);
    unregister_reboot_notifier(&last);
    unregister_reboot_notifier(&first);
    del_timer_sync(&timer);
    console_loglevel = saved_loglevel;
    if (reboot_task) put_task_struct(reboot_task);
    iounmap(uart);
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Duo S NOMMU warm-reset stage and blocked-task diagnostic");
