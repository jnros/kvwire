#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>

#include "kvwire.h"

const size_t g_sizes[] = {
	512,		/* per-token, per-layer */
	2048,
	8192,
	12288,		/* per-token, all layers */
	65536,
	262144,
	1u << 20,	/* per-layer K */
	2u << 20,	/* per-layer K+V */
	8u << 20,
	48u << 20,	/* whole cache */
};
const size_t n_sizes = sizeof(g_sizes) / sizeof(g_sizes[0]);

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double pct(double *v, int n, double p)
{
	int i = (int)(p * (n - 1) + 0.5);
	return v[i];
}

void run_lat(struct ctx *c, int is_client, int oob)
{
	double *samp = malloc(ITERS * sizeof(double));

	if (is_client)
		printf("size_bytes,lat_us_p50,lat_us_p99,lat_us_p999\n");

	for (size_t si = 0; si < n_sizes; si++) {
		size_t len = g_sizes[si];
		int iters = len >= (8u << 20) ? 200 : ITERS;
		sync_peer(oob);

		if (is_client) {
			for (int i = 0; i < WARMUP + iters; i++) {
				double t0 = now_us();
				post_write(c, len, 1, 1);
				poll_one(c);
				poll_one(c);
				post_recv(c);
				if (i >= WARMUP)
					samp[i - WARMUP] = (now_us() - t0) / 2.0;
			}
			qsort(samp, iters, sizeof(double), cmp_double);
			printf("%zu,%.2f,%.2f,%.2f\n", len,
			       pct(samp, iters, 0.50),
			       pct(samp, iters, 0.99),
			       pct(samp, iters, 0.999));
			fflush(stdout);
		} else {
			for (int i = 0; i < WARMUP + iters; i++) {
				poll_one(c);
				post_write(c, len, 1, 1);
				poll_one(c);
				post_recv(c);
			}
		}
	}
	free(samp);
}

void run_bw(struct ctx *c, int is_client, int oob)
{
	if (is_client)
		printf("size_bytes,bw_gbps,msg_per_s\n");

	for (size_t si = 0; si < n_sizes; si++) {
		size_t len = g_sizes[si];
		int iters = len >= (8u << 20) ? 200 : ITERS;
		sync_peer(oob);

		if (!is_client) continue;

		for (int i = 0; i < WARMUP; i++) {
			post_write(c, len, 0, 1);
			poll_one(c);
		}
		double t0 = now_us();
		for (int i = 0; i < iters; i++) {
			int sig = ((i + 1) % BW_DEPTH) == 0 || i == iters - 1;
			post_write(c, len, 0, sig);
			if (sig) poll_one(c);
		}
		double dt = now_us() - t0;
		double bytes = (double)len * iters;
		printf("%zu,%.3f,%.0f\n", len,
		       bytes * 8.0 / (dt * 1e3),
		       iters / (dt / 1e6));
		fflush(stdout);
	}
}

#define PIPE_LAYERS	24		/* 48 MiB cache / 2 MiB per layer */
#define PIPE_LEN	(2u << 20)	/* per-layer K+V */

static const double g_ratios[] = { 0.0, 0.25, 0.5, 1.0, 2.0, 4.0 };
static const size_t n_ratios = sizeof(g_ratios) / sizeof(g_ratios[0]);

/* stub for prefill's per-layer GPU work: sleep. swap for real
 * compute and the pipeline math is unchanged. */
static void prefill_compute(double us)
{
	long ns = (long)(us * 1e3);
	struct timespec ts = { .tv_sec = ns / 1000000000L,
			       .tv_nsec = ns % 1000000000L };
	if (ns > 0)
		nanosleep(&ts, NULL);
}

static double median_us(double *v, int n)
{
	qsort(v, n, sizeof(double), cmp_double);
	return v[n / 2];
}

static double measure_tx_us(struct ctx *c, int L, size_t len)
{
	double samp[PIPE_LAYERS];

	for (int i = 0; i < 8; i++) {		/* warm the path */
		post_write_at(c, 0, len, 1);
		poll_one(c);
	}
	for (int n = 0; n < L; n++) {
		double t0 = now_us();
		post_write_at(c, (n % 2) * len, len, 1);
		poll_one(c);
		samp[n] = now_us() - t0;
	}
	return median_us(samp, L);
}

/* compute-then-transfer, serialized: T = L*(Tc + Tx) */
static double run_seq(struct ctx *c, double tc_us, int L, size_t len)
{
	double t0 = now_us();
	for (int n = 0; n < L; n++) {
		prefill_compute(tc_us);
		post_write_at(c, (n % 2) * len, len, 1);
		poll_one(c);
	}
	return (now_us() - t0) / 1e3;
}

/* double-buffered: compute Ln while the NIC drains L(n-1) */
static double run_ovl(struct ctx *c, double tc_us, int L, size_t len)
{
	double t0 = now_us();
	prefill_compute(tc_us);
	post_write_at(c, 0, len, 1);
	for (int n = 1; n < L; n++) {
		prefill_compute(tc_us);
		poll_one(c);
		post_write_at(c, (n % 2) * len, len, 1);
	}
	poll_one(c);
	return (now_us() - t0) / 1e3;
}

void run_pipe(struct ctx *c, int is_client, int oob)
{
	int L = PIPE_LAYERS;
	size_t len = PIPE_LEN;

	sync_peer(oob);
	if (!is_client) {			/* passive target */
		for (size_t i = 0; i < n_ratios; i++)
			sync_peer(oob);
		return;
	}

	double tx = measure_tx_us(c, L, len);
	double bytes = (double)L * len;

	fprintf(stderr, "pipe: %d layers x %zu B, Tx=%.1f us/layer\n",
		L, len, tx);
	printf("compute_ratio,tc_us,tx_us,seq_ms,ovl_ms,"
	       "bw_seq_gbps,bw_ovl_gbps,speedup\n");

	for (size_t i = 0; i < n_ratios; i++) {
		double r = g_ratios[i];
		double tc = r * tx;
		sync_peer(oob);

		double seq_ms = run_seq(c, tc, L, len);
		double ovl_ms = run_ovl(c, tc, L, len);

		printf("%.2f,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
		       r, tc, tx, seq_ms, ovl_ms,
		       bytes * 8.0 / (seq_ms * 1e6),
		       bytes * 8.0 / (ovl_ms * 1e6),
		       seq_ms / ovl_ms);
		fflush(stdout);
	}
}

void load_kv(struct ctx *c, const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	size_t off = 0;

	if (!d) { fprintf(stderr, "no kv dir %s - using junk bytes\n", dir);
		  return; }
	while ((e = readdir(d)) && off < MAX_BUF) {
		if (!strstr(e->d_name, ".bin")) continue;
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		int fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		ssize_t n;
		char tmp[1 << 16];
		while (off < MAX_BUF && (n = read(fd, tmp, sizeof tmp)) > 0) {
			size_t cp = (off + n > MAX_BUF) ? MAX_BUF - off : (size_t)n;
			memcpy(c->buf + off, tmp, cp);
			off += cp;
		}
		close(fd);
	}
	closedir(d);
	fprintf(stderr, "loaded %zu bytes of real KV\n", off);
}
