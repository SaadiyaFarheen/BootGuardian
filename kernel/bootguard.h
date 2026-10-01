/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BOOTGUARD_H
#define BOOTGUARD_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define BG_DEV_NAME   "bootguard"
#define BG_SLOT_A     0
#define BG_SLOT_B     1

struct bg_status {
    __u32 active_slot;
    __u32 boot_attempts;
    __u32 boot_confirmed;
    __u32 timeout_sec;
    __u32 max_attempts;
    __u32 watchdog_fires;
    __u32 heartbeats;
    __u32 rollback_pending;
    __u32 wd_expired;
};

#define BG_IOC_MAGIC 'B'
#define BG_IOC_GET_STATUS   _IOR(BG_IOC_MAGIC, 1, struct bg_status)
#define BG_IOC_HEARTBEAT    _IO(BG_IOC_MAGIC, 2)
#define BG_IOC_CONFIRM_BOOT _IO(BG_IOC_MAGIC, 3)
#define BG_IOC_SET_SLOT     _IOW(BG_IOC_MAGIC, 4, __u32)
#define BG_IOC_SET_TIMEOUT  _IOW(BG_IOC_MAGIC, 5, __u32)
#define BG_IOC_RESET        _IO(BG_IOC_MAGIC, 6)
#define BG_IOC_BEGIN_BOOT   _IO(BG_IOC_MAGIC, 7)
#define BG_IOC_ACK_EVENT    _IO(BG_IOC_MAGIC, 8)
#define BG_IOC_SET_ATTEMPTS _IOW(BG_IOC_MAGIC, 9, __u32)

#endif
