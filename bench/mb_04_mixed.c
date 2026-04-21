/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MB-04 mixed-size workload benchmark (benchmarks.md §2.4). Drives
 * a bounded working set through alloc / free with sizes drawn from
 * the spec's six-band distribution:
 *
 *      8 –  32 B  : 40 %     (slab Tiny)
 *     33 – 128 B  : 25 %     (slab Tiny / Small)
 *    129 – 512 B  : 15 %     (slab Small)
 *    513 B – 4 KiB: 10 %     (slab Small)
 *    4 KiB – 64 KiB: 7 %     (buddy Medium)
 *      > 64 KiB    : 3 %     (buddy / Large)
 *
 * Each iteration picks a random slot in the live-object table,
 * frees whatever is there (if anything), and replaces it with a
 * fresh allocation whose size is drawn from the distribution. The
 * working set therefore stays at roughly `live_count` concurrent
 * allocations — the spec's "concurrently live objects" parameter.
 *
 * Single-thread by design. Multi-thread variants (the spec's
 * "threads: 1, 8, 64" row) land once the per-pool synchronization
 * gets finer-grained — the existing single-mutex slab/buddy pools
 * make multi-threaded throughput here uninteresting. Note: writing
 * MB-04 itself surfaced a slab-pool partials-list double-insertion
 * bug (was_full path inserting an already-current page) that was
 * fixed in `src/v8m_slab_pool.c` in the same cycle this bench
 * landed.
 *
 * Output is one header line plus a single result row, parseable
 * into a CSV-style ingest:
 *
 *   live_count  duration_ms  total_ops    ops_per_sec
 *   10000       1000         34_500_000   34_500_000
 *
 * Knobs (env vars):
 *   V8M_BENCH_DURATION_MS   timing budget, default 1000
 *   V8M_BENCH_WARMUP_MS     warmup before timing, default 100
 *   V8M_BENCH_LIVE_COUNT    concurrent live objects, default 10000
 *                           (spec asks for 100K / 1M / 10M; the
 *                           default keeps interactive `make bench`
 *                           runs under a second; bump up for the
 *                           full spec sweep)
 *   V8M_BENCH_SEED          PRNG seed, default 0x1234
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* IWYU pragma: keep — provides clock_gettime + CLOCK_MONOTONIC */

static uint64_t now_ns(void)
{
	struct timespec time;
	/* CLOCK_MONOTONIC ships under _GNU_SOURCE which the bench
	 * CMakeLists sets globally; IWYU loses that thread. */
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return ((uint64_t)time.tv_sec * 1000000000ULL) + (uint64_t)time.tv_nsec;
}

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

/* xorshift64 PRNG. Deterministic, single-cycle, plenty fast for
 * picking sizes / slots inside the alloc loop — `rand()` would
 * dominate the per-op cost and skew the bench. */
static uint64_t xorshift64(uint64_t *state)
{
	uint64_t value = *state;
	value ^= value << 13U;
	value ^= value >> 7U;
	value ^= value << 17U;
	*state = value;
	return value;
}

/* Distribution table mirroring the spec. Each row stores a
 * cumulative weight (out of 100) and the [lo, hi] size range it
 * picks uniformly from. Walked top-to-bottom; the matching row is
 * the one whose `cum_weight` first exceeds the random byte. */
struct size_band {
	uint32_t cum_weight;
	uint32_t lo;
	uint32_t hi;
};

static const struct size_band g_bands[] = {
    {40, 8, 32},     {65, 33, 128},	{80, 129, 512},
    {90, 513, 4096}, {97, 4097, 65536}, {100, 65537, 262144},
};
static const size_t g_band_count = sizeof(g_bands) / sizeof(g_bands[0]);

static size_t draw_size(uint64_t *rng)
{
	uint32_t roll = (uint32_t)(xorshift64(rng) % 100U);
	for (size_t i = 0; i < g_band_count; i++) {
		if (roll < g_bands[i].cum_weight) {
			uint32_t span = g_bands[i].hi - g_bands[i].lo + 1U;
			uint32_t bytes =
			    g_bands[i].lo +
			    (uint32_t)(xorshift64(rng) % (uint64_t)span);
			return bytes;
		}
	}
	/* Distribution sums to 100; the loop must hit. */
	return g_bands[g_band_count - 1].hi;
}

int main(void)
{
	int duration_ms = env_int("V8M_BENCH_DURATION_MS", 1000);
	int warmup_ms = env_int("V8M_BENCH_WARMUP_MS", 100);
	int live_count = env_int("V8M_BENCH_LIVE_COUNT", 10000);
	int seed_arg = env_int("V8M_BENCH_SEED", 0x1234);
	if (live_count < 1) {
		live_count = 1;
	}
	if (live_count > 10000000) {
		live_count = 10000000;
	}

	/* `live_count` is bounded to [1, 10000000] above; the analyzer
	 * can't see the bound flow through both branches. */
	/* NOLINTNEXTLINE(clang-analyzer-optin.taint.TaintedAlloc,bugprone-multi-level-implicit-pointer-conversion)
	 */
	void **live = calloc((size_t)live_count, sizeof(*live));
	if (live == NULL) {
		(void)fprintf(stderr,
			      "mb_04: failed to allocate live-object table\n");
		return 1;
	}

	uint64_t rng = (uint64_t)seed_arg;
	if (rng == 0U) {
		rng = 1U; /* xorshift64 deadlocks on zero */
	}

	(void)printf("# MB-04 mixed-size workload\n");
	(void)printf("# duration_ms=%d warmup_ms=%d live_count=%d seed=0x%x\n",
		     duration_ms, warmup_ms, live_count, seed_arg);
	(void)printf("%-12s %-13s %-13s %-13s %-13s\n", "live_count",
		     "duration_ms", "total_ops", "ops_per_sec", "alloc_fails");

	/* Warmup: the same loop, untimed, so the slab/buddy/large
	 * pools fill out and the live-object table holds steady
	 * state before timing starts. NULL returns are tolerated —
	 * the > 64 KiB tail of the spec distribution can momentarily
	 * exhaust the buddy pool's 16 MiB capacity at high
	 * `live_count` values; skipping the slot keeps the loop
	 * running so timing starts with a representative working
	 * set. */
	uint64_t warmup_end = now_ns() + ((uint64_t)warmup_ms * 1000000ULL);
	uint64_t alloc_failures = 0;
	while (now_ns() < warmup_end) {
		size_t slot = (size_t)(xorshift64(&rng) % (uint64_t)live_count);
		free(live[slot]);
		live[slot] = NULL;
		size_t bytes = draw_size(&rng);
		void *ptr = malloc(bytes);
		if (ptr == NULL) {
			alloc_failures++;
			continue;
		}
		((volatile unsigned char *)ptr)[0] = 0xAB;
		live[slot] = ptr;
	}

	/* Timed loop. NULL returns count toward `alloc_failures` (a
	 * footnote in the report) so the throughput number reflects
	 * only successful ops. */
	uint64_t deadline = now_ns() + ((uint64_t)duration_ms * 1000000ULL);
	uint64_t start = now_ns();
	uint64_t ops = 0;
	while (now_ns() < deadline) {
		size_t slot = (size_t)(xorshift64(&rng) % (uint64_t)live_count);
		free(live[slot]);
		live[slot] = NULL;
		size_t bytes = draw_size(&rng);
		void *ptr = malloc(bytes);
		if (ptr == NULL) {
			alloc_failures++;
			continue;
		}
		((volatile unsigned char *)ptr)[0] = (unsigned char)ops;
		live[slot] = ptr;
		ops++;
	}
	uint64_t elapsed_ns = now_ns() - start;
	uint64_t ops_per_sec =
	    (elapsed_ns == 0U) ? 0U : (ops * 1000000000ULL) / elapsed_ns;

	(void)printf("%-12d %-13d %-13llu %-13llu %-13llu\n", live_count,
		     duration_ms, (unsigned long long)ops,
		     (unsigned long long)ops_per_sec,
		     (unsigned long long)alloc_failures);

	/* Drain the live-object table so a leak-checking sanitizer
	 * does not flag the residual allocations. */
	for (int i = 0; i < live_count; i++) {
		free(live[i]);
	}
	/* NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion) */
	free(live);
	return 0;
}
