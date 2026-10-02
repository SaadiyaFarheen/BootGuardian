/*
 * bghealth.c - BootGuardian health check (called by bgd)
 *
 * exit 0 = healthy, 1 = unhealthy. bgd sets BG_SLOT (0 = A, 1 = B).
 *   1) fault simulation: /tmp/bg_break_A makes slot A unhealthy
 *   2) integrity: the active slot's image must match its recorded SHA-256
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
	const char *slot = getenv("BG_SLOT");
	char dir[256], cmd[512], *slash;
	int rc;

	(void)argc;
	if (!slot)
		slot = "0";

	if (!strcmp(slot, "0") && access("/tmp/bg_break_A", F_OK) == 0)
		return 1;

	/* locate cli/bgctl relative to this binary: <root>/tools/bghealth */
	snprintf(dir, sizeof(dir), "%s", argv[0]);
	slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	else
		snprintf(dir, sizeof(dir), ".");
	snprintf(cmd, sizeof(cmd), "%s/../cli/bgctl verify %s >/dev/null 2>&1", dir, slot);

	rc = system(cmd);
	return (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0) ? 0 : 1;
}
