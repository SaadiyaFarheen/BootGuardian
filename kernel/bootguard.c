// SPDX-License-Identifier: GPL-2.0
/*
 * bootguard.c - BootGuardian boot-health character driver
 * Tracks A/B slot, boot attempts, and a timer-based software watchdog.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/version.h>

#include "bootguard.h"

#define DRV "bootguard: "

static unsigned int max_attempts = 3;
module_param(max_attempts, uint, 0644);
MODULE_PARM_DESC(max_attempts, "Failed boots allowed before rollback");

static unsigned int timeout_sec = 10;
module_param(timeout_sec, uint, 0644);
MODULE_PARM_DESC(timeout_sec, "Watchdog heartbeat timeout in seconds");

static struct bg_status st;
static DEFINE_SPINLOCK(bg_lock);
static DECLARE_WAIT_QUEUE_HEAD(bg_wq);
static struct timer_list bg_timer;
static struct proc_dir_entry *bg_proc;

static void bg_arm_timer(void)
{
    mod_timer(&bg_timer, jiffies + msecs_to_jiffies(st.timeout_sec * 1000));
}

static void bg_timer_cb(struct timer_list *t)
{
    unsigned long flags;
    bool rolled = false;
    u32 slot, tmo;

    spin_lock_irqsave(&bg_lock, flags);
    st.watchdog_fires++;
    st.wd_expired = 1;
    if (!st.boot_confirmed && st.boot_attempts >= st.max_attempts) {
        st.active_slot ^= 1;
        st.rollback_pending = 1;
        st.boot_attempts = 0;
        rolled = true;
    }
    slot = st.active_slot;
    tmo = st.timeout_sec;
    spin_unlock_irqrestore(&bg_lock, flags);

    if (rolled)
        pr_warn(DRV "watchdog expired, attempts exhausted -> ROLLBACK to slot %c\n",
                slot ? 'B' : 'A');
    else
        pr_warn(DRV "watchdog expired (no heartbeat for %us)\n", tmo);

    wake_up_interruptible(&bg_wq);
}

static void bg_stop_timer(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
    timer_delete_sync(&bg_timer);
#else
    del_timer_sync(&bg_timer);
#endif
}

static int bg_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int bg_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t bg_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    char tmp[256];
    struct bg_status s;
    unsigned long flags;
    int n;

    spin_lock_irqsave(&bg_lock, flags);
    s = st;
    spin_unlock_irqrestore(&bg_lock, flags);

    n = scnprintf(tmp, sizeof(tmp),
        "slot=%c attempts=%u/%u confirmed=%u timeout=%us fires=%u hb=%u rollback=%u\n",
        s.active_slot ? 'B' : 'A', s.boot_attempts, s.max_attempts,
        s.boot_confirmed, s.timeout_sec, s.watchdog_fires, s.heartbeats,
        s.rollback_pending);

    return simple_read_from_buffer(buf, len, off, tmp, n);
}

static __poll_t bg_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;
    unsigned long flags;

    poll_wait(file, &bg_wq, wait);

    spin_lock_irqsave(&bg_lock, flags);
    if (st.wd_expired || st.rollback_pending)
        mask |= EPOLLIN | EPOLLRDNORM;
    spin_unlock_irqrestore(&bg_lock, flags);
    return mask;
}

static long bg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct bg_status snap;
    unsigned long flags;
    u32 val;

    if (_IOC_TYPE(cmd) != BG_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case BG_IOC_GET_STATUS:
        spin_lock_irqsave(&bg_lock, flags);
        snap = st;
        spin_unlock_irqrestore(&bg_lock, flags);
        if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
            return -EFAULT;
        return 0;

    case BG_IOC_HEARTBEAT:
        spin_lock_irqsave(&bg_lock, flags);
        st.heartbeats++;
        st.wd_expired = 0;
        bg_arm_timer();
        spin_unlock_irqrestore(&bg_lock, flags);
        return 0;

    case BG_IOC_BEGIN_BOOT:
        spin_lock_irqsave(&bg_lock, flags);
        st.boot_attempts++;
        st.boot_confirmed = 0;
        st.wd_expired = 0;
        bg_arm_timer();
        snap = st;
        spin_unlock_irqrestore(&bg_lock, flags);
        pr_info(DRV "boot attempt %u/%u on slot %c\n", snap.boot_attempts,
                snap.max_attempts, snap.active_slot ? 'B' : 'A');
        return 0;

    case BG_IOC_CONFIRM_BOOT:
        spin_lock_irqsave(&bg_lock, flags);
        st.boot_confirmed = 1;
        st.boot_attempts = 0;
        st.wd_expired = 0;
        bg_arm_timer();
        snap = st;
        spin_unlock_irqrestore(&bg_lock, flags);
        pr_info(DRV "boot confirmed good on slot %c\n", snap.active_slot ? 'B' : 'A');
        return 0;

    case BG_IOC_SET_SLOT:
        if (get_user(val, (u32 __user *)arg))
            return -EFAULT;
        if (val > BG_SLOT_B)
            return -EINVAL;
        spin_lock_irqsave(&bg_lock, flags);
        st.active_slot = val;
        spin_unlock_irqrestore(&bg_lock, flags);
        return 0;

    case BG_IOC_SET_ATTEMPTS:
        if (get_user(val, (u32 __user *)arg))
            return -EFAULT;
        spin_lock_irqsave(&bg_lock, flags);
        st.boot_attempts = val;
        spin_unlock_irqrestore(&bg_lock, flags);
        return 0;

    case BG_IOC_SET_TIMEOUT:
        if (get_user(val, (u32 __user *)arg))
            return -EFAULT;
        if (val < 1 || val > 3600)
            return -EINVAL;
        spin_lock_irqsave(&bg_lock, flags);
        st.timeout_sec = val;
        spin_unlock_irqrestore(&bg_lock, flags);
        return 0;

    case BG_IOC_ACK_EVENT:
        spin_lock_irqsave(&bg_lock, flags);
        st.wd_expired = 0;
        st.rollback_pending = 0;
        spin_unlock_irqrestore(&bg_lock, flags);
        return 0;

    case BG_IOC_RESET:
        bg_stop_timer();
        spin_lock_irqsave(&bg_lock, flags);
        memset(&st, 0, sizeof(st));
        st.timeout_sec = timeout_sec ? timeout_sec : 10;
        st.max_attempts = max_attempts ? max_attempts : 3;
        spin_unlock_irqrestore(&bg_lock, flags);
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations bg_fops = {
    .owner          = THIS_MODULE,
    .open           = bg_open,
    .release        = bg_release,
    .read           = bg_read,
    .poll           = bg_poll,
    .unlocked_ioctl = bg_ioctl,
    .compat_ioctl   = bg_ioctl,
    .llseek         = noop_llseek,
};

static struct miscdevice bg_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = BG_DEV_NAME,
    .fops  = &bg_fops,
    .mode  = 0660,
};

static int bg_proc_show(struct seq_file *m, void *v)
{
    struct bg_status s;
    unsigned long flags;

    spin_lock_irqsave(&bg_lock, flags);
    s = st;
    spin_unlock_irqrestore(&bg_lock, flags);

    seq_printf(m, "active_slot:      %c\n", s.active_slot ? 'B' : 'A');
    seq_printf(m, "boot_attempts:    %u / %u\n", s.boot_attempts, s.max_attempts);
    seq_printf(m, "boot_confirmed:   %u\n", s.boot_confirmed);
    seq_printf(m, "watchdog_timeout: %u s\n", s.timeout_sec);
    seq_printf(m, "watchdog_fires:   %u\n", s.watchdog_fires);
    seq_printf(m, "heartbeats:       %u\n", s.heartbeats);
    seq_printf(m, "rollback_pending: %u\n", s.rollback_pending);
    return 0;
}

static int __init bg_init(void)
{
    int ret;

    memset(&st, 0, sizeof(st));
    st.timeout_sec = timeout_sec ? timeout_sec : 10;
    st.max_attempts = max_attempts ? max_attempts : 3;
    timer_setup(&bg_timer, bg_timer_cb, 0);

    ret = misc_register(&bg_misc);
    if (ret) {
        pr_err(DRV "misc_register failed: %d\n", ret);
        return ret;
    }

    bg_proc = proc_create_single(BG_DEV_NAME, 0444, NULL, bg_proc_show);
    if (!bg_proc) {
        misc_deregister(&bg_misc);
        return -ENOMEM;
    }

    pr_info(DRV "loaded (max_attempts=%u, timeout=%us) -> /dev/%s\n",
            st.max_attempts, st.timeout_sec, BG_DEV_NAME);
    return 0;
}

static void __exit bg_exit(void)
{
    bg_stop_timer();
    proc_remove(bg_proc);
    misc_deregister(&bg_misc);
    pr_info(DRV "unloaded\n");
}

module_init(bg_init);
module_exit(bg_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Saadiya Farheen");
MODULE_DESCRIPTION("BootGuardian boot-health and watchdog driver");
MODULE_VERSION("0.1");
