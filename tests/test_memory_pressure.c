/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Memory-pressure stress test (benchmarks.md §4 ST-02). The full
 * spec calls for operating with ≥ 90% of physical RAM in use,
 * which isn't feasible inside a CI container — instead we use
 * v8m_set_soft_limit to simulate the same boundary condition:
 * the allocator refuses requests that would push live bytes past
 * the cap, malloc returns NULL with errno = ENOMEM, and the
 * program recovers gracefully once we free other allocations.
 *
 * Phases:
 *
 *   1. Build a working set so live_bytes is meaningfully above
 *      zero. This is the "the system already has memory in use"
 *      precondition.
 *   2. Plant a soft limit at the current live_bytes (zero
 *      headroom) and confirm the next sizeable allocation fails
 *      with errno = ENOMEM.
 *   3. Free part of the working set and confirm the same
 *      allocation now succeeds — the post-OOM recovery contract.
 *   4. Re-tighten the limit, install an OOM handler that frees
 *      another part of the set on demand, and confirm the
 *      allocator's retry path (one retry on non-zero handler
 *      return) lets the request succeed.
 *   5. Drop the soft limit and free the rest. live_bytes after
 *      teardown matches the pre-test baseline within a small
 *      tolerance — anything larger would imply we leaked.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8malloc/v8malloc.h"

enum {
	WORKING_SET_COUNT = 8,
	/* Each slot uses the Large direct-mmap path (> 256 KiB → its
	 * own page-heap region). Buddy-pool sizes wouldn't work
	 * here: freeing one buddy block doesn't drop live_bytes
	 * because the surrounding 256 KiB arena stays mapped until
	 * the whole arena drains. */
	WORKING_SET_SIZE = 512 * 1024,
	REQUEST_SIZE = 1024 * 1024,
	/* Phase-4 retry request needs to fit within the headroom one
	 * freed Large block leaves (≤ 512 KiB). 256 KiB is well
	 * under that. */
	HANDLER_RETRY_REQUEST = 256 * 1024,
	/* Live-bytes drift between the start of the test and the
	 * post-cleanup snapshot must stay below this cap. The slab /
	 * buddy pools may retain a few empty pages internally on
	 * teardown, hence non-zero. */
	MAX_RESIDUAL_BYTES = 1024 * 1024,
};

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_memory_pressure: %s\n", msg);
	return 1;
}

/* OOM handler shared between phase 4 and main; releases one
 * working-set element per invocation and asks for a retry. */
static void *g_oom_release_slot;

static int oom_release_one(size_t size)
{
	(void)size;
	if (g_oom_release_slot == NULL) {
		return 0;
	}
	void *to_free = g_oom_release_slot;
	g_oom_release_slot = NULL;
	free(to_free);
	return 1; /* retry */
}

int main(void)
{
	struct v8m_stats baseline = {0};
	v8m_get_stats(&baseline);

	/* Phase 1: build the working set. */
	void *working_set[WORKING_SET_COUNT] = {NULL};
	for (int i = 0; i < WORKING_SET_COUNT; i++) {
		working_set[i] = malloc(WORKING_SET_SIZE);
		if (working_set[i] == NULL) {
			for (int j = 0; j < i; j++) {
				free(working_set[j]);
			}
			return fail("working-set alloc failed");
		}
		(void)memset(working_set[i], i & 0xFF, WORKING_SET_SIZE);
	}

	struct v8m_stats stats = {0};
	v8m_get_stats(&stats);
	uint64_t pressure_limit = stats.live_bytes;

	/* Phase 2: hit the limit. */
	v8m_set_soft_limit(pressure_limit);
	errno = 0;
	void *blocked = malloc(REQUEST_SIZE);
	if (blocked != NULL) {
		v8m_set_soft_limit(0);
		free(blocked);
		for (int i = 0; i < WORKING_SET_COUNT; i++) {
			free(working_set[i]);
		}
		return fail("alloc unexpectedly succeeded under tight limit");
	}
	if (errno != ENOMEM) {
		v8m_set_soft_limit(0);
		for (int i = 0; i < WORKING_SET_COUNT; i++) {
			free(working_set[i]);
		}
		return fail("blocked alloc did not set errno = ENOMEM");
	}

	/* Phase 3: free half the working set, retry — should succeed. */
	for (int i = 0; i < WORKING_SET_COUNT / 2; i++) {
		free(working_set[i]);
		working_set[i] = NULL;
	}
	void *recovered = malloc(REQUEST_SIZE);
	if (recovered == NULL) {
		v8m_set_soft_limit(0);
		for (int i = WORKING_SET_COUNT / 2; i < WORKING_SET_COUNT;
		     i++) {
			free(working_set[i]);
		}
		return fail("recovery alloc failed after freeing half the set");
	}
	(void)memset(recovered, 0xA5, REQUEST_SIZE);
	free(recovered);

	/* Phase 4: OOM handler installed; tighten the limit again,
	 * the handler frees a slot and the retry succeeds. */
	v8m_oom_handler_t prev_handler = v8m_set_oom_handler(oom_release_one);
	if (prev_handler != NULL) {
		v8m_set_oom_handler(prev_handler);
		v8m_set_soft_limit(0);
		for (int i = WORKING_SET_COUNT / 2; i < WORKING_SET_COUNT;
		     i++) {
			free(working_set[i]);
		}
		return fail("OOM handler was non-NULL on entry");
	}

	struct v8m_stats now = {0};
	v8m_get_stats(&now);
	v8m_set_soft_limit(now.live_bytes);

	/* Pick a working-set slot to release on demand. */
	int donor = WORKING_SET_COUNT - 1;
	g_oom_release_slot = working_set[donor];
	working_set[donor] = NULL;

	void *handled = malloc(HANDLER_RETRY_REQUEST);
	if (handled == NULL) {
		v8m_set_oom_handler(NULL);
		v8m_set_soft_limit(0);
		for (int i = WORKING_SET_COUNT / 2; i < WORKING_SET_COUNT;
		     i++) {
			free(working_set[i]);
		}
		return fail("OOM handler retry path failed");
	}
	free(handled);

	/* Phase 5: cleanup + leak check. */
	v8m_set_oom_handler(NULL);
	v8m_set_soft_limit(0);
	for (int i = WORKING_SET_COUNT / 2; i < WORKING_SET_COUNT; i++) {
		free(working_set[i]);
	}
	/* Drain the Large/Huge recycle cache so the residual check
	 * below sees a true post-free baseline. The cache (see
	 * v8m_large.c::large_cache_take) keeps freed Large regions
	 * mapped for follow-on alloc reuse — that's a deliberate
	 * speed/RSS trade-off for production, but for this leak-check
	 * test we want the bytes truly returned. */
	(void)v8m_purge();

	struct v8m_stats end = {0};
	v8m_get_stats(&end);
	if (end.live_bytes > baseline.live_bytes &&
	    end.live_bytes - baseline.live_bytes > MAX_RESIDUAL_BYTES) {
		(void)fprintf(
		    stderr,
		    "test_memory_pressure: live_bytes residual %llu exceeds "
		    "cap %llu (baseline=%llu end=%llu)\n",
		    (unsigned long long)(end.live_bytes - baseline.live_bytes),
		    (unsigned long long)MAX_RESIDUAL_BYTES,
		    (unsigned long long)baseline.live_bytes,
		    (unsigned long long)end.live_bytes);
		return 1;
	}
	return 0;
}
