/*
 * bgfault.c - BootGuardian fault injector and resilience tester
 *
 * Injects real failures (corrupt images, broken boots, runtime hangs, both
 * slots bad, interrupted update), runs bgd against each one, and checks the
 * system recovered the way it should. Writes docs/resilience-report.txt.
 *
 * usage (as root, from the project root):  tools/bgfault [scenario]
 *   scenarios: bad-update broken-boot runtime-hang both-bad interrupted-update
 *   no argument = run all
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include "../kernel/bootguard.h"

#define BASE     "/var/lib/bootguard"
#define BGCTL    "./cli/bgctl"
#define BGD      "./daemon/bgd"
#define HEALTH   "./tools/bghealth"
#define IMG1     "/tmp/bgfault_v1.img"
#define IMG2     "/tmp/bgfault_v2.img"
#define BREAK_A  "/tmp/bg_break_A"
#define TIMEOUT  50

struct scenario {
	const char *name;
	const char *desc;
	const char *expect;     /* string that must appear in bgd's log */
};

static const struct scenario scenarios[] = {
	{ "bad-update",        "update to a corrupted image in slot B",
	  "BOOT CONFIRMED on slot A" },
	{ "broken-boot",       "slot A boots but its service never becomes healthy",
	  "BOOT CONFIRMED on slot B" },
	{ "runtime-hang",      "slot A boots fine, then hangs after confirmation",
	  "BOOT CONFIRMED on slot B" },
	{ "both-bad",          "both slots corrupted",
	  "RESCUE MODE" },
	{ "interrupted-update","power cut mid-update leaves a partial image",
	  "BOOT CONFIRMED on slot A" },
};
#define NSCEN (sizeof(scenarios) / sizeof(scenarios[0]))

static int sh(const char *cmd)
{
	int rc = system(cmd);
	return rc == -1 ? -1 : WEXITSTATUS(rc);
}

static int make_image(const char *path, size_t size)
{
	unsigned char buf[4096];
	int in = open("/dev/urandom", O_RDONLY);
	int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t left = size;

	if (in < 0 || out < 0)
		return -1;
	while (left) {
		size_t n = left > sizeof(buf) ? sizeof(buf) : left;
		if (read(in, buf, n) != (ssize_t)n || write(out, buf, n) != (ssize_t)n)
			return -1;
		left -= n;
	}
	close(in);
	close(out);
	return 0;
}

static int flip_byte(const char *path)
{
	unsigned char b;
	int fd = open(path, O_RDWR);

	if (fd < 0 || pread(fd, &b, 1, 10) != 1)
		return -1;
	b ^= 0xFF;
	if (pwrite(fd, &b, 1, 10) != 1)
		return -1;
	close(fd);
	return 0;
}

static int file_contains(const char *path, const char *needle)
{
	static char buf[1 << 16];
	size_t n;
	FILE *f = fopen(path, "r");

	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return strstr(buf, needle) != NULL;
}

/* clean slate: stop timer + zero driver state, wipe slots/state, clear faults */
static int reset_env(void)
{
	int fd = open("/dev/" BG_DEV_NAME, O_RDWR);

	if (fd < 0) {
		perror("open /dev/" BG_DEV_NAME " (module loaded? running as root?)");
		return -1;
	}
	ioctl(fd, BG_IOC_RESET);
	close(fd);
	sh("rm -rf " BASE);
	unlink(BREAK_A);
	return 0;
}

static pid_t start_bgd(const char *logfile)
{
	pid_t pid = fork();

	if (pid == 0) {
		int fd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		execl(BGD, "bgd", "-c", HEALTH, "-i", "1", (char *)NULL);
		_exit(127);
	}
	return pid;
}

static void stop_bgd(pid_t pid)
{
	int st;

	if (waitpid(pid, &st, WNOHANG) == 0) {
		kill(pid, SIGTERM);
		waitpid(pid, &st, 0);
	}
}

/* returns 1 = pass, 0 = fail */
static int run_scenario(const struct scenario *sc, double *secs)
{
	char log[128], cmd[256];
	int ok = 0, injected = 0, i;
	time_t t0 = time(NULL);
	pid_t pid;

	snprintf(log, sizeof(log), "/tmp/bgfault_%s.log", sc->name);
	if (reset_env() < 0)
		return 0;

	snprintf(cmd, sizeof(cmd), BGCTL " init " IMG1 " >/dev/null");
	if (sh(cmd)) { fprintf(stderr, "init failed\n"); return 0; }

	if (!strcmp(sc->name, "bad-update")) {
		sh(BGCTL " update " IMG2 " >/dev/null");
		flip_byte(BASE "/slots/B.img");
	} else if (!strcmp(sc->name, "broken-boot")) {
		sh("touch " BREAK_A);
	} else if (!strcmp(sc->name, "both-bad")) {
		flip_byte(BASE "/slots/A.img");
		flip_byte(BASE "/slots/B.img");
	} else if (!strcmp(sc->name, "interrupted-update")) {
		/* partial image left behind by a power cut during an update */
		make_image(BASE "/slots/B.img.tmp", 12345);
		if (sh(BGCTL " verify A >/dev/null")) {
			fprintf(stderr, "active slot damaged by interrupted update!\n");
			return 0;
		}
	}

	pid = start_bgd(log);
	for (i = 0; i < TIMEOUT; i++) {
		sleep(1);
		if (!strcmp(sc->name, "runtime-hang") && !injected &&
		    file_contains(log, "BOOT CONFIRMED on slot A")) {
			sleep(2);
			sh("touch " BREAK_A);   /* service dies after a good boot */
			injected = 1;
		}
		if (file_contains(log, sc->expect)) {
			ok = 1;
			break;
		}
	}
	stop_bgd(pid);
	unlink(BREAK_A);
	*secs = difftime(time(NULL), t0);
	return ok;
}

int main(int argc, char **argv)
{
	int passed = 0, ran = 0, fail_count;
	size_t i;
	FILE *rep;
	time_t now = time(NULL);

	if (geteuid() != 0) {
		fprintf(stderr, "run as root: sudo tools/bgfault\n");
		return 1;
	}
	if (access(BGD, X_OK) || access(BGCTL, X_OK) || access(HEALTH, X_OK)) {
		fprintf(stderr, "run from the project root after 'make' (needs %s, %s, %s)\n",
			BGD, BGCTL, HEALTH);
		return 1;
	}
	make_image(IMG1, 100000);
	make_image(IMG2, 50000);

	mkdir("docs", 0755);
	rep = fopen("docs/resilience-report.txt", "w");
	printf("\n BootGuardian resilience test\n ============================\n");
	if (rep)
		fprintf(rep, "BootGuardian resilience report - %s\n", ctime(&now));

	for (i = 0; i < NSCEN; i++) {
		double secs = 0;
		int ok;

		if (argc > 1 && strcmp(argv[1], scenarios[i].name))
			continue;
		printf("\n[%zu] %s: %s\n    expect: \"%s\" ...\n", i + 1,
		       scenarios[i].name, scenarios[i].desc, scenarios[i].expect);
		fflush(stdout);
		ok = run_scenario(&scenarios[i], &secs);
		ran++;
		passed += ok;
		printf("    %s (%.0fs)  log: /tmp/bgfault_%s.log\n",
		       ok ? "PASS" : "FAIL", secs, scenarios[i].name);
		if (rep)
			fprintf(rep, "%-20s %-5s %3.0fs  %s\n", scenarios[i].name,
				ok ? "PASS" : "FAIL", secs, scenarios[i].desc);
	}

	fail_count = ran - passed;
	printf("\n RESULT: %d/%d scenarios recovered correctly\n\n", passed, ran);
	if (rep) {
		fprintf(rep, "\nRESULT: %d/%d scenarios recovered correctly\n", passed, ran);
		fclose(rep);
		printf(" report saved to docs/resilience-report.txt\n");
	}
	reset_env();
	return fail_count ? 1 : 0;
}
