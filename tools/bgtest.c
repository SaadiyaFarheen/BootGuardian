/* bgtest.c - tiny CLI to exercise /dev/bootguard */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "../kernel/bootguard.h"

static void usage(const char *p)
{
    fprintf(stderr,
    "usage: %s <cmd> [arg]\n"
    "  status | begin | confirm | hb | ack | reset\n"
    "  slot <0|1> | timeout <sec> | attempts <n> | wait <ms>\n", p);
    exit(1);
}

static int do_ioctl(int fd, unsigned long req, void *arg)
{
    if (ioctl(fd, req, arg) < 0) {
        perror("ioctl");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int fd, rc = 0;
    __u32 v;

    if (argc < 2)
        usage(argv[0]);
    fd = open("/dev/" BG_DEV_NAME, O_RDWR);
    if (fd < 0) {
        perror("open /dev/" BG_DEV_NAME " (is the module loaded?)");
        return 1;
    }

    if (!strcmp(argv[1], "status")) {
        struct bg_status s;
        if (do_ioctl(fd, BG_IOC_GET_STATUS, &s))
            return 1;
        printf("slot=%c attempts=%u/%u confirmed=%u timeout=%us fires=%u hb=%u rollback=%u expired=%u\n",
               s.active_slot ? 'B' : 'A', s.boot_attempts, s.max_attempts,
               s.boot_confirmed, s.timeout_sec, s.watchdog_fires,
               s.heartbeats, s.rollback_pending, s.wd_expired);
    } else if (!strcmp(argv[1], "begin"))   rc = do_ioctl(fd, BG_IOC_BEGIN_BOOT, NULL);
    else if (!strcmp(argv[1], "confirm"))   rc = do_ioctl(fd, BG_IOC_CONFIRM_BOOT, NULL);
    else if (!strcmp(argv[1], "hb"))        rc = do_ioctl(fd, BG_IOC_HEARTBEAT, NULL);
    else if (!strcmp(argv[1], "ack"))       rc = do_ioctl(fd, BG_IOC_ACK_EVENT, NULL);
    else if (!strcmp(argv[1], "reset"))     rc = do_ioctl(fd, BG_IOC_RESET, NULL);
    else if (!strcmp(argv[1], "slot") && argc > 2)     { v = atoi(argv[2]); rc = do_ioctl(fd, BG_IOC_SET_SLOT, &v); }
    else if (!strcmp(argv[1], "timeout") && argc > 2)  { v = atoi(argv[2]); rc = do_ioctl(fd, BG_IOC_SET_TIMEOUT, &v); }
    else if (!strcmp(argv[1], "attempts") && argc > 2) { v = atoi(argv[2]); rc = do_ioctl(fd, BG_IOC_SET_ATTEMPTS, &v); }
    else if (!strcmp(argv[1], "wait") && argc > 2) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, atoi(argv[2]));
        printf(r > 0 ? "EVENT: watchdog/rollback signalled\n" : "no event (timeout)\n");
    } else usage(argv[0]);

    close(fd);
    return rc;
}
