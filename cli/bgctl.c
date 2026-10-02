/*
 * bgctl.c - BootGuardian control tool: A/B image updates with SHA-256 checks
 *
 * usage: bgctl [-d basedir] <command>
 *   init <image>      install image into both slots, boot slot A
 *   update <image>    write image to the INACTIVE slot, verify, switch to it
 *   verify [slot]     check slot image against its recorded SHA-256
 *   rollback          switch back to the other slot
 *   status            show slots, hashes and driver state
 *
 * Layout under basedir (default /var/lib/bootguard):
 *   state             "slot=N attempts=M"   (shared with bgd)
 *   slots/A.img A.meta B.img B.meta
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "../kernel/bootguard.h"

static char base[200] = "/var/lib/bootguard";

/* ---------------- SHA-256 ---------------- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha_ctx;

static const uint32_t K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_init(sha_ctx *c)
{
	static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
					0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
	memcpy(c->h, iv, sizeof(iv));
	c->len = 0;
	c->n = 0;
}

static void sha_block(sha_ctx *c, const uint8_t *p)
{
	uint32_t w[64], a, b, cc, d, e, f, g, h, s0, s1, t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
		       ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
	for (i = 16; i < 64; i++) {
		s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
		s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (i = 0; i < 64; i++) {
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
		h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_update(sha_ctx *c, const uint8_t *d, size_t len)
{
	c->len += len;
	while (len--) {
		c->buf[c->n++] = *d++;
		if (c->n == 64) {
			sha_block(c, c->buf);
			c->n = 0;
		}
	}
}

static void sha_final(sha_ctx *c, char hex[65])
{
	uint64_t bits = c->len * 8;
	uint8_t pad = 0x80, zero = 0, lenb[8];
	int i;

	sha_update(c, &pad, 1);
	while (c->n != 56)
		sha_update(c, &zero, 1);
	for (i = 0; i < 8; i++)
		lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha_update(c, lenb, 8);
	for (i = 0; i < 8; i++)
		snprintf(hex + 8 * i, 9, "%08x", c->h[i]);
}

static int sha_file(const char *path, char hex[65], long *size)
{
	uint8_t buf[4096];
	sha_ctx c;
	ssize_t n;
	long total = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	sha_init(&c);
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		sha_update(&c, buf, n);
		total += n;
	}
	close(fd);
	if (n < 0)
		return -1;
	sha_final(&c, hex);
	if (size)
		*size = total;
	return 0;
}

/* ---------------- helpers ---------------- */
static void path_state(char *o, size_t n) { snprintf(o, n, "%s/state", base); }
static void path_img(char *o, size_t n, int s) { snprintf(o, n, "%s/slots/%c.img", base, s ? 'B' : 'A'); }
static void path_meta(char *o, size_t n, int s) { snprintf(o, n, "%s/slots/%c.meta", base, s ? 'B' : 'A'); }

static void ensure_dirs(void)
{
	char d[300];

	mkdir(base, 0755);
	snprintf(d, sizeof(d), "%s/slots", base);
	mkdir(d, 0755);
}

/* write data atomically: tmp + fsync + rename */
static int write_atomic(const char *path, const void *data, size_t len)
{
	char tmp[400];
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	if (write(fd, data, len) != (ssize_t)len || fsync(fd) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return rename(tmp, path);
}

static int parse_slot(const char *s)
{
	if (!s) return -1;
	if (s[0] == 'A' || s[0] == 'a' || s[0] == '0') return 0;
	if (s[0] == 'B' || s[0] == 'b' || s[0] == '1') return 1;
	return -1;
}

static void read_state(unsigned *slot, unsigned *attempts)
{
	char p[300];
	FILE *f;

	*slot = 0; *attempts = 0;
	path_state(p, sizeof(p));
	f = fopen(p, "r");
	if (!f) return;
	if (fscanf(f, "slot=%u attempts=%u", slot, attempts) != 2) { *slot = 0; *attempts = 0; }
	fclose(f);
}

static int write_state(unsigned slot, unsigned attempts)
{
	char p[300], buf[64];
	int n = snprintf(buf, sizeof(buf), "slot=%u attempts=%u\n", slot, attempts);

	path_state(p, sizeof(p));
	return write_atomic(p, buf, n);
}

/* copy src -> slot image atomically, verify read-back hash, write meta */
static int install_image(const char *src, int slot)
{
	char img[300], meta[300], tmp[320], want[65], got[65], mbuf[200];
	uint8_t buf[4096];
	sha_ctx c;
	ssize_t n;
	long size = 0;
	int in, out;

	if (sha_file(src, want, &size) < 0) {
		fprintf(stderr, "cannot read %s: %s\n", src, strerror(errno));
		return -1;
	}
	path_img(img, sizeof(img), slot);
	path_meta(meta, sizeof(meta), slot);
	snprintf(tmp, sizeof(tmp), "%s.tmp", img);

	in = open(src, O_RDONLY);
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (in < 0 || out < 0) { perror("open"); return -1; }
	sha_init(&c);
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, n) != n) { perror("write"); return -1; }
		sha_update(&c, buf, n);
	}
	close(in);
	if (fsync(out) < 0) { perror("fsync"); return -1; }
	close(out);
	sha_final(&c, got);
	if (strcmp(got, want) != 0) {
		fprintf(stderr, "copy checksum mismatch, aborting\n");
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, img) < 0) { perror("rename"); return -1; }

	/* read back from disk and verify again */
	if (sha_file(img, got, NULL) < 0 || strcmp(got, want) != 0) {
		fprintf(stderr, "read-back verification FAILED for slot %c\n", slot ? 'B' : 'A');
		return -1;
	}
	n = snprintf(mbuf, sizeof(mbuf), "sha256=%s size=%ld time=%ld\n", want, size, (long)time(NULL));
	if (write_atomic(meta, mbuf, n) < 0) { perror("write meta"); return -1; }
	printf("slot %c: installed %ld bytes, sha256=%.16s... verified\n",
	       slot ? 'B' : 'A', size, want);
	return 0;
}

/* returns 0 ok, 1 bad, 2 unmanaged (nothing registered) */
static int verify_slot(int slot, int quiet)
{
	char img[300], meta[300], line[256], want[65] = "", got[65];
	FILE *f;
	char *p;
	int has_img;

	path_img(img, sizeof(img), slot);
	path_meta(meta, sizeof(meta), slot);
	has_img = access(img, F_OK) == 0;
	f = fopen(meta, "r");
	if (!f) {
		if (!has_img) {
			if (!quiet) printf("slot %c: no image registered (unmanaged)\n", slot ? 'B' : 'A');
			return 2;
		}
		if (!quiet) printf("slot %c: image present but NO metadata -> BAD\n", slot ? 'B' : 'A');
		return 1;
	}
	if (fgets(line, sizeof(line), f) && (p = strstr(line, "sha256=")))
		sscanf(p + 7, "%64s", want);
	fclose(f);
	if (sha_file(img, got, NULL) < 0) {
		if (!quiet) printf("slot %c: image missing -> BAD\n", slot ? 'B' : 'A');
		return 1;
	}
	if (strcmp(want, got) != 0) {
		if (!quiet) printf("slot %c: CHECKSUM MISMATCH (image corrupted) -> BAD\n", slot ? 'B' : 'A');
		return 1;
	}
	if (!quiet) printf("slot %c: checksum OK\n", slot ? 'B' : 'A');
	return 0;
}

static void driver_status(void)
{
	struct bg_status s;
	int fd = open("/dev/" BG_DEV_NAME, O_RDWR);

	if (fd < 0) { printf("driver: /dev/%s not available\n", BG_DEV_NAME); return; }
	if (ioctl(fd, BG_IOC_GET_STATUS, &s) == 0)
		printf("driver: slot=%c attempts=%u/%u confirmed=%u fires=%u hb=%u\n",
		       s.active_slot ? 'B' : 'A', s.boot_attempts, s.max_attempts,
		       s.boot_confirmed, s.watchdog_fires, s.heartbeats);
	close(fd);
}

static void usage(void)
{
	fprintf(stderr,
	"usage: bgctl [-d basedir] <command>\n"
	"  init <image> | update <image> | verify [slot] | rollback | status\n");
	exit(1);
}

int main(int argc, char **argv)
{
	unsigned slot, attempts;
	int i;

	if (argc > 2 && !strcmp(argv[1], "-d")) {
		snprintf(base, sizeof(base), "%s", argv[2]);
		argv += 2; argc -= 2;
	}
	if (argc < 2) usage();
	ensure_dirs();
	read_state(&slot, &attempts);

	if (!strcmp(argv[1], "init") && argc > 2) {
		if (install_image(argv[2], 0) || install_image(argv[2], 1)) return 1;
		write_state(0, 0);
		printf("initialised: both slots hold the image, active slot A\n");
	} else if (!strcmp(argv[1], "update") && argc > 2) {
		int target = slot ? 0 : 1;
		printf("active slot %c -> updating inactive slot %c\n", slot ? 'B' : 'A', target ? 'B' : 'A');
		if (install_image(argv[2], target)) {
			fprintf(stderr, "update FAILED, active slot untouched\n");
			return 1;
		}
		write_state(target, 0);
		printf("trial boot set to slot %c. Restart bgd (reboot) to boot it.\n", target ? 'B' : 'A');
	} else if (!strcmp(argv[1], "verify")) {
		int s = argc > 2 ? parse_slot(argv[2]) : (int)slot;
		if (s < 0) usage();
		return verify_slot(s, 0) == 1 ? 1 : 0;
	} else if (!strcmp(argv[1], "rollback")) {
		write_state(slot ? 0 : 1, 0);
		printf("manual rollback: active slot now %c. Restart bgd to boot it.\n", slot ? 'A' : 'B');
	} else if (!strcmp(argv[1], "status")) {
		printf("state file: active slot %c, attempts %u\n", slot ? 'B' : 'A', attempts);
		for (i = 0; i < 2; i++) verify_slot(i, 0);
		driver_status();
	} else usage();
	return 0;
}
