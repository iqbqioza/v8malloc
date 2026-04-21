/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MB-05 fragmentation scenario (benchmarks.md §2.5). Measures
 * memory efficiency (RSS / logical usage ratio) under the
 * fragmentation-maximizing pattern the spec calls out:
 *
 *   1. Allocate N objects with random sizes
 *   2. Free 50 % at random
 *   3. Re-allocate N/2 with different sizes
 *   4. Repeat 2-3 K iterations
 *
 * After each iteration the bench samples:
 *   - RSS             from /proc/self/statm (second field, in pages)
 *   - logical_bytes   sum of `malloc_usable_size(ptr)` over every
 *                     live slot — the minimum memory a perfect
 *                     allocator would need to serve the same working
 *                     set
 *   - rss / logical   the Resident Set Ratio (lower is better; the
 *                     spec's pass criterion is ≤ 0.9× jemalloc)
 *
 * Internal and external fragmentation are harder to attribute
 * to individual allocations without an allocator-internal probe
 * (today we report the aggregate via the RSR column); that
 * expansion is follow-up work.
 *
 * Single-thread. The spec does not define thread count for MB-05;
 * single-thread is enough to surface the RSR trend and keeps the
 * run deterministic.
 *
 * Output is one row per iteration (plus the initial sample), for
 * a time-series plot:
 *
 *   iter  live_count  logical_bytes  rss_bytes  rsr
 *   0     10000       5_242_880      16_777_216 3.20
 *   1     10000       5_110_272      17_301_504 3.38
 *   ...
 *
 * Knobs (env vars):
 *   V8M_BENCH_LIVE_COUNT   initial N, default 10000 (spec wants 1M;
 *                          10000 keeps `make bench` runs under a
 *                          second on a typical dev box)
 *   V8M_BENCH_ITERS        iteration count, default 20 (spec wants
 *                          100; 20 is plenty to see the trend)
 *   V8M_BENCH_SEED         PRNG seed, default 0x5a5a
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <malloc.h> /* malloc_usable_size */

#include "v8malloc/v8malloc.h" /* v8m_get_stats */

/* Same distribution table as MB-04; duplicated here so each bench
 * is standalone. The > 64 KiB tail is capped at 128 KiB here (vs
 * 256 KiB in MB-04) so the working set fits in default CI-runner
 * RAM at 10 000 live objects. */
struct size_band {
	uint32_t cum_weight;
	uint32_t lo;
	uint32_t hi;
};

static const struct size_band g_bands[] = {
    {40, 8, 32},     {65, 33, 128},	{80, 129, 512},
    {90, 513, 4096}, {97, 4097, 65536}, {100, 65537, 131072},
};
static const size_t g_band_count = sizeof(g_bands) / sizeof(g_bands[0]);

static int env_int(const char *name, int fallback)
{
	/* getenv is single-threaded by spec; the bench is too. */
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	const char *value = getenv(name);
	if (value == NULL || *value == '\0') {
		return fallback;
	}
	long parsed = strtol(value, NULL, 10);
	if (parsed <= 0 || parsed > INT32_MAX) {
		return fallback;
	}
	return (int)parsed;
}

static uint64_t xorshift64(uint64_t *state)
{
	uint64_t value = *state;
	value ^= value << 13U;
	value ^= value >> 7U;
	value ^= value << 17U;
	*state = value;
	return value;
}

static size_t draw_size(uint64_t *rng)
{
	uint32_t roll = (uint32_t)(xorshift64(rng) % 100U);
	for (size_t i = 0; i < g_band_count; i++) {
		if (roll < g_bands[i].cum_weight) {
			uint32_t span = g_bands[i].hi - g_bands[i].lo + 1U;
			return g_bands[i].lo +
			       (uint32_t)(xorshift64(rng) % (uint64_t)span);
		}
	}
	return g_bands[g_band_count - 1].hi;
}

/* Parse the second field of /proc/self/statm — resident pages —
 * then multiply by page size. Returns 0 on any parse failure; the
 * bench keeps running so a single bad sample doesn't nuke the
 * time series. */
static size_t read_rss_bytes(void)
{
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	FILE *statm = fopen("/proc/self/statm", "r");
	if (statm == NULL) {
		return 0;
	}
	long total_pages = 0;
	long resident_pages = 0;
	/* fscanf does not surface conversion errors; that is fine
	 * here because /proc/self/statm is kernel-formatted and the
	 * fallback (return 0 on != 2 fields) handles the only realistic
	 * failure (race with /proc unmount on a sysfs tear-down test). */
	/* NOLINTNEXTLINE(cert-err34-c) */
	if (fscanf(statm, "%ld %ld", &total_pages, &resident_pages) != 2) {
		(void)fclose(statm);
		return 0;
	}
	(void)fclose(statm);
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0 || resident_pages <= 0) {
		return 0;
	}
	return (size_t)resident_pages * (size_t)page;
}

static size_t sum_logical_bytes(void **live, int live_count)
{
	size_t total = 0;
	for (int i = 0; i < live_count; i++) {
		if (live[i] != NULL) {
			total += malloc_usable_size(live[i]);
		}
	}
	return total;
}

static int count_live(void **live, int live_count)
{
	int count = 0;
	for (int i = 0; i < live_count; i++) {
		if (live[i] != NULL) {
			count++;
		}
	}
	return count;
}

/* Five-number report per iteration. `mapped_bytes` is the
 * allocator's own view (sum of live mmap regions from
 * v8m_get_stats.live_bytes); `frag_ratio` is mapped / logical —
 * the fraction of allocator reservation beyond the user's
 * asked-for bytes, combining internal + external fragmentation.
 * `rss_bytes` is the kernel's view from /proc/self/statm and
 * reflects only pages actually faulted in. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void report_iter(int iter, int live_count, size_t logical_bytes,
			size_t mapped_bytes, size_t rss_bytes)
{
	double frag_ratio =
	    (logical_bytes == 0U)
		? 0.0
		: ((double)mapped_bytes / (double)logical_bytes);
	(void)printf("%-5d %-11d %-14zu %-14zu %-12zu %-10.2f\n", iter,
		     live_count, logical_bytes, mapped_bytes, rss_bytes,
		     frag_ratio);
}

int main(void)
{
	int live_count = env_int("V8M_BENCH_LIVE_COUNT", 10000);
	int iters = env_int("V8M_BENCH_ITERS", 20);
	int seed_arg = env_int("V8M_BENCH_SEED", 0x5a5a);
	if (live_count < 1) {
		live_count = 1;
	}
	if (live_count > 1000000) {
		live_count = 1000000;
	}
	if (iters < 1) {
		iters = 1;
	}
	if (iters > 1000) {
		iters = 1000;
	}

	/* `live_count` is bounded to [1, 1_000_000] above; the
	 * analyzer can't see that bound flow through both branches. */
	/* NOLINTBEGIN(clang-analyzer-optin.taint.TaintedAlloc,
	 * bugprone-multi-level-implicit-pointer-conversion) */
	void **live = calloc((size_t)live_count, sizeof(*live));
	/* NOLINTEND(clang-analyzer-optin.taint.TaintedAlloc,
	 * bugprone-multi-level-implicit-pointer-conversion) */
	if (live == NULL) {
		(void)fprintf(stderr, "mb_05: live-table alloc failed\n");
		return 1;
	}

	uint64_t rng = (uint64_t)seed_arg;
	if (rng == 0U) {
		rng = 1U;
	}

	(void)printf("# MB-05 fragmentation scenario\n");
	(void)printf("# live_count=%d iters=%d seed=0x%x\n", live_count, iters,
		     seed_arg);
	(void)printf("%-5s %-11s %-14s %-14s %-12s %-10s\n", "iter",
		     "live_count", "logical_bytes", "mapped_bytes", "rss_bytes",
		     "frag_ratio");

	/* Step 1: initial full allocation. */
	for (int i = 0; i < live_count; i++) {
		size_t bytes = draw_size(&rng);
		live[i] = malloc(bytes);
		if (live[i] != NULL) {
			((volatile unsigned char *)live[i])[0] = 0xAB;
		}
	}
	{
		struct v8m_stats stats;
		v8m_get_stats(&stats);
		report_iter(0, count_live(live, live_count),
			    sum_logical_bytes(live, live_count),
			    (size_t)stats.live_bytes, read_rss_bytes());
	}

	/* Step 2-3 repeated: free half, re-allocate half. Always at
	 * different sizes (fresh draws) to drive fragmentation. */
	for (int iter = 1; iter <= iters; iter++) {
		/* Free ~50 % by flipping a coin per slot. */
		for (int i = 0; i < live_count; i++) {
			if ((xorshift64(&rng) & 1U) != 0U) {
				free(live[i]);
				live[i] = NULL;
			}
		}
		/* Re-allocate into exactly the freed slots with
		 * distribution-drawn sizes. */
		for (int i = 0; i < live_count; i++) {
			if (live[i] != NULL) {
				continue;
			}
			size_t bytes = draw_size(&rng);
			live[i] = malloc(bytes);
			if (live[i] != NULL) {
				((volatile unsigned char *)live[i])[0] =
				    (unsigned char)iter;
			}
		}
		struct v8m_stats stats;
		v8m_get_stats(&stats);
		report_iter(iter, count_live(live, live_count),
			    sum_logical_bytes(live, live_count),
			    (size_t)stats.live_bytes, read_rss_bytes());
	}

	for (int i = 0; i < live_count; i++) {
		free(live[i]);
	}
	/* NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion) */
	free(live);
	return 0;
}
