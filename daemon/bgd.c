/*
 * bgd.c - BootGuardian userspace daemon
 *
 * - restores persisted slot/attempt state into the kernel driver at start
 * - runs a health check periodically; healthy -> heartbeat, else withhold it
 * - confirms the boot after N consecutive healthy checks
 * - reacts to watchdog / rollback events delivered through poll()
 * - persists state atomically (write tmp + fsync + rename)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include "../kernel/bootguard.h"

static volatile sig_atomic_t running = 1;
static const char *state_path = "/var/lib/bootguard/state";
static const char *health_cmd = "true";
static unsigned interval = 1;
static unsigned confirm_after = 2;

static void on_signal(int sig) { (void)sig; running = 0; }

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...)
{
	char ts[16];
	time_t now = time(NULL);
	va_list ap;

	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
	printf("[bgd %s] ", ts);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static void ensure_dir(const char *path)
{
	char dir[256];
	char *slash;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
}

static void load_state(unsigned *slot, unsigned *attempts)
{
	FILE *f = fopen(state_path, "r");

	*slot = 0;
	*attempts = 0;
	if (!f)
		return;
	if (fscanf(f, "slot=%u attempts=%u", slot, attempts) != 2) {
		*slot = 0;
		*attempts = 0;
	}
	fclose(f);
}

/* Atomic save: a power cut leaves either the old file or the new one. */
static int save_state(unsigned slot, unsigned attempts)
{
	char tmp[300], buf[64];
	int fd, n;

	ensure_dir(state_path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", state_path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror("open state tmp");
		return -1;
	}
	n = snprintf(buf, sizeof(buf), "slot=%u attempts=%u\n", slot, attempts);
	if (write(fd, buf, n) != n || fsync(fd) < 0) {
		perror("write state");
		close(fd);
		return -1;
	}
	close(fd);
	if (rename(tmp, state_path) < 0) {
		perror("rename state");
		return -1;
	}
	return 0;
}

static int get_status(int fd, struct bg_status *s)
{
	if (ioctl(fd, BG_IOC_GET_STATUS, s) < 0) {
		perror("GET_STATUS");
		return -1;
	}
	return 0;
}

static void persist_from_driver(int fd)
{
	struct bg_status s;

	if (get_status(fd, &s) == 0)
		save_state(s.active_slot, s.boot_attempts);
}

/* One "boot": count the attempt and arm the kernel watchdog. */
static void boot_sequence(int fd)
{
	struct bg_status s;

	if (ioctl(fd, BG_IOC_BEGIN_BOOT) < 0) {
		perror("BEGIN_BOOT");
		return;
	}
	if (get_status(fd, &s) == 0) {
		logmsg("BOOT slot %c, attempt %u/%u", s.active_slot ? 'B' : 'A',
		       s.boot_attempts, s.max_attempts);
		save_state(s.active_slot, s.boot_attempts);
	}
}

static int run_health(unsigned slot)
{
	char val[8];
	int rc;

	snprintf(val, sizeof(val), "%u", slot);
	setenv("BG_SLOT", val, 1);
	rc = system(health_cmd);
	return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static void handle_event(int fd)
{
	struct bg_status s;

	if (get_status(fd, &s) < 0)
		return;
	if (s.rollback_pending)
		logmsg("!! ROLLBACK: attempts exhausted, switching to slot %c",
		       s.active_slot ? 'B' : 'A');
	else
		logmsg("!! WATCHDOG fired: no healthy heartbeat");

	ioctl(fd, BG_IOC_ACK_EVENT);
	logmsg("simulating reboot...");
	boot_sequence(fd);
}

static void usage(const char *p)
{
	fprintf(stderr, "usage: %s [-s statefile] [-c healthcmd] [-i interval_sec] [-n confirm_after]\n", p);
	exit(1);
}

int main(int argc, char **argv)
{
	unsigned slot, attempts, good = 0;
	int fd, opt, confirmed = 0;

	while ((opt = getopt(argc, argv, "s:c:i:n:h")) != -1) {
		switch (opt) {
		case 's': state_path = optarg; break;
		case 'c': health_cmd = optarg; break;
		case 'i': interval = atoi(optarg); break;
		case 'n': confirm_after = atoi(optarg); break;
		default: usage(argv[0]);
		}
	}
	if (!interval || !confirm_after)
		usage(argv[0]);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	fd = open("/dev/" BG_DEV_NAME, O_RDWR);
	if (fd < 0) {
		perror("open /dev/" BG_DEV_NAME " (module loaded? run as root?)");
		return 1;
	}

	load_state(&slot, &attempts);
	logmsg("restoring state: slot %c, attempts %u", slot ? 'B' : 'A', attempts);
	ioctl(fd, BG_IOC_SET_SLOT, &slot);
	ioctl(fd, BG_IOC_SET_ATTEMPTS, &attempts);
	boot_sequence(fd);

	while (running) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		struct bg_status s;
		int r = poll(&p, 1, interval * 1000);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (r > 0 && (p.revents & POLLIN)) {
			handle_event(fd);
			good = 0;
			confirmed = 0;
			continue;
		}

		if (get_status(fd, &s) < 0)
			break;

		if (run_health(s.active_slot)) {
			ioctl(fd, BG_IOC_HEARTBEAT);
			if (!confirmed) {
				good++;
				logmsg("health OK (%u/%u) on slot %c", good, confirm_after,
				       s.active_slot ? 'B' : 'A');
				if (good >= confirm_after) {
					ioctl(fd, BG_IOC_CONFIRM_BOOT);
					confirmed = 1;
					persist_from_driver(fd);
					logmsg("** BOOT CONFIRMED on slot %c **",
					       s.active_slot ? 'B' : 'A');
				}
			}
		} else {
			good = 0;
			logmsg("health check FAILED on slot %c, withholding heartbeat",
			       s.active_slot ? 'B' : 'A');
		}
	}

	logmsg("shutting down");
	close(fd);
	return 0;
}
