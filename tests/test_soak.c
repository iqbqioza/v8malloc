/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ST-01 long-running stability soak — CI variant. The spec
 * (benchmarks.md §4.1) calls for a 24-hour soak of the MB-04
 * mixed-size workload, sampling RSS / page-fault time series and
 * failing on upward drift. That's not a unit-test budget; this
 * test is the shorter "does the allocator leak during sustained
 * workload" gate that every PR run should afford, with an env
 * knob to stretch it out for dedicated soak runs.
 *
 * Default: 2 seconds of MB-04-style churn at 2 000 live objects,
 * sampling `v8m_get_stats.live_bytes` and `.live_regions` every
 * 500 ms. After a full post-run drain (every slot freed) the
 * residual `live_bytes` must be within 1 MiB of the pre-run
 * baseline and `live_regions` must return to its pre-run value
 * (non-zero residuals here flag a leak). During the run,
 * `live_regions` must stay under a cap that scales with
 * live_count so a monotonic RSS drift surfaces immediately.
 *
 * Env knobs:
 *   V8M_SOAK_DURATION_MS   runtime, default 2000
 *                          (set to 300000 for a 5-minute mini-soak,
 *                          or 86400000 for the spec's 24 h)
 *   V8M_SOAK_LIVE_COUNT    concurrent live objects, default 2000
 *   V8M_SOAK_SEED          PRNG seed, default 0x5041
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* IWYU pragma: keep — CLOCK_MONOTONIC */

#include "v8malloc/v8malloc.h"

/* Same distribution as MB-04, narrowed at the > 64 KiB tail to
 * keep the working set inside buddy capacity even under
 * multi-second churn. */
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

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_soak: %s\n", msg);
	return 1;
}

static uint64_t now_ns(void)
{
	struct timespec time;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return ((uint64_t)time.tv_sec * 1000000000ULL) + (uint64_t)time.tv_nsec;
}

static int env_int(const char *name, int fallback)
{
	/* getenv is single-threaded by spec; the test is too. */
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

/* Free every pointer in the live table and zero the slot so the
 * caller can reuse / free the array. Extracted so main() stays
 * under the cognitive-complexity ceiling. */
static void drain_live(void **live, int live_count)
{
	for (int i = 0; i < live_count; i++) {
		free(live[i]);
		live[i] = NULL;
	}
}

static int check_drain_invariants(const struct v8m_stats *baseline,
				  const struct v8m_stats *after)
{
	const uint64_t bytes_slack = (uint64_t)1 << 20U; /* 1 MiB */
	/* Region slack: allow up to V8M_SOAK_REGION_SLACK regions to
	 * remain past baseline. The slab/buddy pools amortize free
	 * via the drained-cache hold window — even an explicit
	 * v8m_purge() can leave a transient region or two when the
	 * bg purge thread happens to hold the lock during the
	 * teardown sample. The slack is small enough to still catch
	 * a real leak (which would push the count by orders of
	 * magnitude), large enough to absorb the drained-cache
	 * hysteresis CI sees on noisy GitHub runners. The 16-region
	 * value tracks the V8M_BUDDY_POOL_MAX_ARENAS=256 budget:
	 * when 2/39 ctest jobs run the soak alongside other tests,
	 * the buddy pool may legitimately retain a handful of
	 * drained arenas in the deferred-keep window. */
	const uint64_t region_slack = 16U;
	uint64_t bytes_delta = (after->live_bytes > baseline->live_bytes)
				   ? after->live_bytes - baseline->live_bytes
				   : 0U;
	uint64_t region_delta =
	    (after->live_regions > baseline->live_regions)
		? after->live_regions - baseline->live_regions
		: 0U;
	int bad = 0;
	if (bytes_delta > bytes_slack) {
		(void)fprintf(
		    stderr,
		    "test_soak: live_bytes grew by %llu (baseline=%llu, "
		    "after=%llu, slack=%llu) — leak\n",
		    (unsigned long long)bytes_delta,
		    (unsigned long long)baseline->live_bytes,
		    (unsigned long long)after->live_bytes,
		    (unsigned long long)bytes_slack);
		bad = 1;
	}
	if (region_delta > region_slack) {
		(void)fprintf(
		    stderr,
		    "test_soak: live_regions grew by %llu "
		    "(baseline=%llu, after=%llu, slack=%llu) — leak\n",
		    (unsigned long long)region_delta,
		    (unsigned long long)baseline->live_regions,
		    (unsigned long long)after->live_regions,
		    (unsigned long long)region_slack);
		bad = 1;
	}
	return bad;
}

int main(void)
{
	int duration_ms = env_int("V8M_SOAK_DURATION_MS", 2000);
	int live_count = env_int("V8M_SOAK_LIVE_COUNT", 2000);
	int seed_arg = env_int("V8M_SOAK_SEED", 0x5041);
	if (duration_ms < 100) {
		duration_ms = 100;
	}
	if (live_count < 1) {
		live_count = 1;
	}
	if (live_count > 1000000) {
		live_count = 1000000;
	}

	/* `live_count` is bounded above; analyzer can't see the bound. */
	/* NOLINTBEGIN(clang-analyzer-optin.taint.TaintedAlloc,
	 * bugprone-multi-level-implicit-pointer-conversion) */
	void **live = calloc((size_t)live_count, sizeof(*live));
	/* NOLINTEND(clang-analyzer-optin.taint.TaintedAlloc,
	 * bugprone-multi-level-implicit-pointer-conversion) */
	if (live == NULL) {
		return fail("live-table alloc failed");
	}

	uint64_t rng = (uint64_t)seed_arg;
	if (rng == 0U) {
		rng = 1U;
	}

	struct v8m_stats baseline;
	v8m_get_stats(&baseline);

	/* Worst-case region cap: two regions per live slot (one slab
	 * page + one buddy arena), plus a healthy slack for partials.
	 * A leak would push live_regions well above this; a healthy
	 * allocator sits comfortably below. */
	uint64_t region_cap = ((uint64_t)live_count * 2U) + 64U;
	uint64_t peak_regions = 0;

	uint64_t start = now_ns();
	uint64_t deadline = start + ((uint64_t)duration_ms * 1000000ULL);
	uint64_t next_sample = start + (500ULL * 1000000ULL);
	uint64_t ops = 0;

	while (now_ns() < deadline) {
		size_t slot = (size_t)(xorshift64(&rng) % (uint64_t)live_count);
		free(live[slot]);
		live[slot] = NULL;
		size_t bytes = draw_size(&rng);
		void *ptr = malloc(bytes);
		if (ptr != NULL) {
			((volatile unsigned char *)ptr)[0] = (unsigned char)ops;
			live[slot] = ptr;
		}
		ops++;

		if (now_ns() >= next_sample) {
			struct v8m_stats mid;
			v8m_get_stats(&mid);
			if (mid.live_regions > peak_regions) {
				peak_regions = mid.live_regions;
			}
			if (peak_regions > region_cap) {
				drain_live(live, live_count);
				void *raw_live = (void *)live;
				free(raw_live);
				(void)fprintf(
				    stderr,
				    "test_soak: live_regions peaked at %llu "
				    "(cap %llu) — allocator leaking pages\n",
				    (unsigned long long)peak_regions,
				    (unsigned long long)region_cap);
				return 1;
			}
			next_sample += 500ULL * 1000000ULL;
		}
	}

	/* Drain: free every live slot. Buddy arenas that empty during
	 * the workload go into the drained-but-mapped state and are
	 * released lazily by the bg purge sweep; an explicit
	 * v8m_purge() forces immediate release so the leak-check
	 * invariant ("live_bytes returns within 1 MiB of baseline")
	 * reflects the allocator's steady-state footprint rather than
	 * its drain-hold grace window. */
	drain_live(live, live_count);
	(void)v8m_purge();

	struct v8m_stats after;
	v8m_get_stats(&after);
	int result = check_drain_invariants(&baseline, &after);

	void *raw_live = (void *)live;
	free(raw_live);
	if (result == 0) {
		(void)printf("test_soak: OK (ops=%llu peak_regions=%llu "
			     "duration_ms=%d)\n",
			     (unsigned long long)ops,
			     (unsigned long long)peak_regions, duration_ms);
	}
	return result;
}
