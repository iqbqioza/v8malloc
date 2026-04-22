/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Coverage for the thread-cache scaffolding (architecture.md §2.1
 * + thread-cache.md §2.1). v0 only ships the TLS slot, the lazy
 * initializer, and the pthread_key destructor; the cache itself
 * is not yet wired into malloc/free, so this test asserts the
 * plumbing rather than any allocator behaviour:
 *
 *   1. Same thread sees the same cache pointer across two calls.
 *   2. Two different threads see distinct cache pointers.
 *   3. v8m_thread_cache_peek returns NULL on a fresh thread that
 *      has not yet called get_or_create.
 *   4. The pthread_key destructor fires on thread exit — verified
 *      via the reclamation counter, which advances by exactly the
 *      number of threads we joined.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "v8m_remote_free.h" /* v8m_mpsc_push for the drain test */
#include "v8m_size_class.h" /* V8M_MEDIUM_FIRST_CLASS */
#include "v8m_thread_cache.h"
#include "v8malloc/v8malloc.h" /* v8m_purge */

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_thread_cache: %s\n", msg);
	return 1;
}

static int check_same_thread_returns_same_cache(void)
{
	const struct v8m_thread_cache *first = v8m_thread_cache_get_or_create();
	if (first == NULL) {
		return fail("get_or_create returned NULL on first call");
	}
	const struct v8m_thread_cache *second =
	    v8m_thread_cache_get_or_create();
	if (second != first) {
		return fail("get_or_create returned a different cache on the "
			    "second call from the same thread");
	}
	if (v8m_thread_cache_peek() != first) {
		return fail(
		    "peek diverged from get_or_create on the same thread");
	}
	return 0;
}

/* NOLINTBEGIN(misc-include-cleaner) — pthread.h IS included above;
 * the IWYU-style cleaner does not always recognize pthread_t /
 * pthread_barrier_t as coming from it. */
struct worker_arg {
	struct v8m_thread_cache *observed;
	struct v8m_thread_cache *peeked_before_create;
	/* Optional barrier — when non-NULL the worker waits on it
	 * AFTER recording the observation but BEFORE returning. The
	 * parent uses this to keep all workers alive simultaneously
	 * while it inspects each cache pointer; without it the OS
	 * is free to schedule the workers serially, in which case
	 * each worker's destructor frees its cache slot before the
	 * next worker's malloc fires and the slot gets reused. */
	pthread_barrier_t *gate;
};
/* NOLINTEND(misc-include-cleaner) */

static void *worker_observe(void *raw)
{
	struct worker_arg *arg = raw;
	arg->peeked_before_create = v8m_thread_cache_peek();
	arg->observed = v8m_thread_cache_get_or_create();
	if (arg->gate != NULL) {
		(void)pthread_barrier_wait(arg->gate);
	}
	return NULL;
}

static int check_distinct_threads_get_distinct_caches(void)
{
	const struct v8m_thread_cache *parent_cache =
	    v8m_thread_cache_get_or_create();
	if (parent_cache == NULL) {
		return fail("parent cache get failed");
	}

	/* Three-party barrier: parent + 2 workers. Workers wait on
	 * the barrier after recording observations so both caches
	 * stay live simultaneously; the parent reads observations
	 * after waiting, then joins. */
	pthread_barrier_t gate;
	if (pthread_barrier_init(&gate, NULL, 3) != 0) {
		return fail("pthread_barrier_init failed");
	}
	struct worker_arg arg_a = {.gate = &gate};
	struct worker_arg arg_b = {.gate = &gate};
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t thread_a;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t thread_b;
	if (pthread_create(&thread_a, NULL, worker_observe, &arg_a) != 0) {
		(void)pthread_barrier_destroy(&gate);
		return fail("pthread_create A failed");
	}
	if (pthread_create(&thread_b, NULL, worker_observe, &arg_b) != 0) {
		(void)pthread_barrier_wait(&gate);
		(void)pthread_join(thread_a, NULL);
		(void)pthread_barrier_destroy(&gate);
		return fail("pthread_create B failed");
	}
	(void)pthread_barrier_wait(&gate);

	int result = 0;
	if (arg_a.peeked_before_create != NULL) {
		result =
		    fail("worker A's peek-before-create returned non-NULL");
	} else if (arg_b.peeked_before_create != NULL) {
		result =
		    fail("worker B's peek-before-create returned non-NULL");
	} else if (arg_a.observed == NULL || arg_b.observed == NULL) {
		result = fail("worker get_or_create returned NULL");
	} else if (arg_a.observed == arg_b.observed) {
		result = fail("two workers shared the same cache pointer");
	} else if (arg_a.observed == parent_cache ||
		   arg_b.observed == parent_cache) {
		result = fail("worker shared parent's cache pointer");
	}

	(void)pthread_join(thread_a, NULL);
	(void)pthread_join(thread_b, NULL);
	(void)pthread_barrier_destroy(&gate);
	return result;
}

static int check_destructor_fires_on_thread_exit(void)
{
	uint64_t before = v8m_thread_cache_destructor_calls();

	enum { WORKERS = 4 };
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t threads[WORKERS];
	struct worker_arg args[WORKERS] = {0};
	for (int i = 0; i < WORKERS; i++) {
		if (pthread_create(&threads[i], NULL, worker_observe,
				   &args[i]) != 0) {
			for (int j = 0; j < i; j++) {
				(void)pthread_join(threads[j], NULL);
			}
			return fail("pthread_create failed mid-batch");
		}
	}
	for (int i = 0; i < WORKERS; i++) {
		(void)pthread_join(threads[i], NULL);
	}

	uint64_t after = v8m_thread_cache_destructor_calls();
	if (after - before != (uint64_t)WORKERS) {
		(void)fprintf(stderr,
			      "test_thread_cache: destructor calls advanced "
			      "by %llu, expected %d\n",
			      (unsigned long long)(after - before), WORKERS);
		return 1;
	}
	return 0;
}

/*
 * Fast-path coverage: exercise the bin pop / push helpers
 * directly against a cache the test owns. The dispatcher-level
 * round-trip (malloc / free → TLC → slab pool) is exercised by
 * the rest of the suite; this test asserts the helper-level
 * invariants the dispatcher relies on.
 */
static int check_alloc_free_round_trip(void)
{
	struct v8m_thread_cache *cache = v8m_thread_cache_get_or_create();
	if (cache == NULL) {
		return fail("get_or_create returned NULL");
	}

	const uint32_t cls = 0; /* smallest Tiny class — 8-byte slots */

	/* Empty bin → alloc returns NULL. */
	if (v8m_thread_cache_alloc(cache, cls) != NULL) {
		return fail("empty-bin alloc did not return NULL");
	}

	/* Push three sentinel objects, pop them in LIFO order. The
	 * objects are local 8-byte buffers — the bin overwrites
	 * their first 8 bytes with the next-pointer link, which is
	 * exactly what the production path does on freed slots. */
	uint64_t slots[3];
	for (int i = 0; i < 3; i++) {
		slots[i] = 0;
		bool overflow = v8m_thread_cache_free(cache, cls, &slots[i]);
		if (overflow) {
			return fail("push under capacity reported overflow");
		}
	}

	for (int i = 2; i >= 0; i--) {
		const void *got = v8m_thread_cache_alloc(cache, cls);
		if (got != &slots[i]) {
			return fail("LIFO alloc returned the wrong slot");
		}
	}
	if (v8m_thread_cache_alloc(cache, cls) != NULL) {
		return fail("post-drain alloc did not return NULL");
	}
	return 0;
}

/*
 * Push to capacity and verify the next push reports overflow.
 * The objects are heap-allocated via malloc so the bin's writes
 * to their first 8 bytes do not stomp local stack state. We
 * do NOT free the objects here — the dispatcher's normal flow
 * has the slab pool take them via flush_half; the parent thread's
 * pthread_key destructor will drain at thread exit.
 */
static int check_overflow_signal(void)
{
	struct v8m_thread_cache *cache = v8m_thread_cache_get_or_create();
	if (cache == NULL) {
		return fail("get_or_create returned NULL");
	}

	const uint32_t cls = 1; /* Tiny class 1 — 16-byte slots */
	uint16_t cap = cache->bin_capacity[cls];
	if (cap < V8M_BIN_CAPACITY_MIN || cap > V8M_BIN_CAPACITY_MAX) {
		return fail("bin_capacity outside the documented clamp band");
	}

	/* Pre-load the bin to (capacity - 1) by recycling a single
	 * 16-byte slot — every push uses the same address, so the
	 * bin's `next` chain becomes self-referential after the
	 * second push. That breaks the "drain in LIFO" invariant
	 * but is fine for the overflow-signal test, which only
	 * checks the boolean return of the push that crosses the
	 * threshold. The test doesn't drain the bin afterward;
	 * the parent thread's exit destructor handles cleanup. */
	cache->bin_count[cls] = (uint16_t)(cap - 1U);
	uint64_t slot[2] = {0};
	bool overflow = v8m_thread_cache_free(cache, cls, slot);
	if (!overflow) {
		return fail("push at capacity did not signal overflow");
	}
	/* Restore the bin to a sane state so the parent's exit
	 * destructor's drain doesn't walk our self-referential
	 * chain. Pop the one slot we just pushed; bin_count goes
	 * back to where it started before this test. */
	(void)v8m_thread_cache_alloc(cache, cls);
	cache->bin_count[cls] = 0;
	cache->bin_heads[cls] = NULL;
	return 0;
}

/*
 * Drain coverage: simulate a remote producer pushing freed slots
 * to the cache's MPSC queue, then verify v8m_thread_cache_drain_remote
 * returns them and routes each to the appropriate local bin keyed
 * by the slot's page-meta size class. We use real Tiny-class
 * allocations as the queue payload so the page-meta lookup
 * succeeds — the bin push inside drain_remote walks
 * v8m_ptr_to_meta(slot)->size_class to pick the bin, so synthetic
 * stack addresses would skip the push (the drain tolerates
 * invalid meta defensively but does not bin them).
 */
static int check_drain_remote_routes_to_bin(void)
{
	struct v8m_thread_cache *cache = v8m_thread_cache_get_or_create();
	if (cache == NULL) {
		return fail("get_or_create returned NULL");
	}

	/* Allocate two real slabs to use as remote-queue payloads. */
	void *slot_a = malloc(8);
	void *slot_b = malloc(8);
	if (slot_a == NULL || slot_b == NULL) {
		free(slot_a);
		free(slot_b);
		return fail("malloc for drain test returned NULL");
	}

	/* Drain any state the previous tests left in the bins so we
	 * can assert the post-drain bin count cleanly. Hits the
	 * dispatcher purge path. */
	(void)v8m_purge();

	/* Push the slots onto the remote MPSC queue without going
	 * through the public free (which would land in the local
	 * bin instead). This is the white-box equivalent of "another
	 * thread freed these slots while routing remote-frees to us". */
	v8m_mpsc_push(&cache->remote, (struct v8m_mpsc_node *)slot_a);
	v8m_mpsc_push(&cache->remote, (struct v8m_mpsc_node *)slot_b);

	size_t drained = v8m_thread_cache_drain_remote(cache);
	if (drained != 2U) {
		return fail("drain_remote did not return 2 nodes");
	}
	/* Both slots are class 0 (8-byte). The drain pushes them
	 * onto bin_heads[0]; verify the bin holds 2 entries. */
	if (cache->bin_count[0] != 2U) {
		return fail("drained slots did not land in bin[0]");
	}

	/* Pop both back through the cache so the slabs return to
	 * canonical state when this test's main() destructor flushes. */
	(void)v8m_thread_cache_alloc(cache, 0);
	(void)v8m_thread_cache_alloc(cache, 0);

	/* Hand the slabs back to the slab pool via the normal free
	 * path so they don't leak. */
	free(slot_a);
	free(slot_b);
	return 0;
}

/*
 * Adaptive bin-capacity coverage. The gc_tick controller derives
 * capacity = EMA(demand) × 2, clamped to [MIN, MAX]. Driving the
 * inputs directly (via the per-class counter fields) keeps the
 * test fast and deterministic — exercising the natural wire
 * (V8M_TLC_GC_INTERVAL alloc/free pairs) would be slow and
 * sensitive to whatever else the suite has done to the cache.
 */
static int check_adaptive_capacity_grows_with_demand(void)
{
	struct v8m_thread_cache *cache = v8m_thread_cache_get_or_create();
	if (cache == NULL) {
		return fail("get_or_create returned NULL");
	}

	const uint32_t hot = 0; /* hammer this class */
	const uint32_t cold = 1; /* leave alone — should decay */

	/* Reset everything the prior tests in this file accumulated
	 * so the EMA starts from a known zero and we can predict the
	 * post-tick capacity. */
	for (uint32_t cls = 0; cls < V8M_MEDIUM_FIRST_CLASS; cls++) {
		cache->alloc_count_per_class[cls] = 0;
		cache->free_count_per_class[cls] = 0;
		cache->ema_demand[cls] = 0;
		cache->bin_capacity[cls] = V8M_BIN_CAPACITY_DEFAULT;
	}

	/* Stage hot demand of 200 (allocs) - 0 (frees). EMA after one
	 * tick = (3*0 + 200)/4 = 50. Capacity = 50*2 = 100, in band. */
	cache->alloc_count_per_class[hot] = 200;
	cache->free_count_per_class[hot] = 0;

	uint32_t pre_gen = cache->gc_generation;
	v8m_thread_cache_gc_tick(cache);
	if (cache->gc_generation != pre_gen + 1U) {
		return fail("gc_tick did not advance gc_generation");
	}
	if (cache->ema_demand[hot] != 50U) {
		return fail("hot-class EMA did not advance to 50");
	}
	if (cache->bin_capacity[hot] != 100U) {
		return fail("hot-class capacity did not become EMA*2 = 100");
	}
	if (cache->bin_capacity[cold] != V8M_BIN_CAPACITY_MIN) {
		return fail("cold class did not decay to MIN");
	}
	if (cache->alloc_count_per_class[hot] != 0U) {
		return fail("gc_tick did not reset alloc counter");
	}

	/* Stage another 200-alloc tick and verify capacity continues
	 * to grow (EMA(0.25, 200) over two ticks ≈ 87, capacity ≈ 174). */
	cache->alloc_count_per_class[hot] = 200;
	v8m_thread_cache_gc_tick(cache);
	if (cache->ema_demand[hot] <= 50U) {
		return fail("EMA did not grow on second hot tick");
	}
	if (cache->bin_capacity[hot] !=
	    (uint16_t)(cache->ema_demand[hot] * 2U)) {
		return fail("capacity != EMA * 2 after second tick");
	}

	/* Sustained idle ticks should decay capacity back toward MIN. */
	for (int i = 0; i < 64; i++) {
		v8m_thread_cache_gc_tick(cache);
	}
	if (cache->bin_capacity[hot] != V8M_BIN_CAPACITY_MIN) {
		return fail(
		    "capacity did not decay to MIN under sustained idle");
	}
	return 0;
}

/*
 * The countdown trigger fires gc_tick after exactly
 * V8M_TLC_GC_INTERVAL alloc / free operations on the cache.
 * Verifies the wire — without this the controller never runs in
 * production.
 */
static int check_gc_countdown_fires(void)
{
	struct v8m_thread_cache *cache = v8m_thread_cache_get_or_create();
	if (cache == NULL) {
		return fail("get_or_create returned NULL");
	}

	uint32_t pre_gen = cache->gc_generation;
	uint32_t pre_countdown = cache->gc_countdown;
	if (pre_countdown == 0U || pre_countdown > V8M_TLC_GC_INTERVAL) {
		return fail("gc_countdown out of range");
	}

	/* Push then pop a single slot enough times to consume the
	 * countdown. Each free + alloc decrements by 2, so we drive
	 * the countdown to zero by issuing pre_countdown/2 pairs
	 * (rounded up). The class doesn't matter for the trigger
	 * itself; pick class 0 (Tiny 8 B) so each push/pop affects
	 * the same bin. */
	const uint32_t cls = 0;
	uint64_t slot[2] = {0};
	uint32_t pairs = (pre_countdown + 1U) / 2U;
	for (uint32_t i = 0; i < pairs; i++) {
		(void)v8m_thread_cache_free(cache, cls, slot);
		(void)v8m_thread_cache_alloc(cache, cls);
	}
	if (cache->gc_generation == pre_gen) {
		return fail("gc_tick did not fire after pre_countdown ops");
	}
	if (cache->gc_countdown == 0U ||
	    cache->gc_countdown > V8M_TLC_GC_INTERVAL) {
		return fail("gc_countdown not reset after fire");
	}
	return 0;
}

/*
 * Predictive prefetch table state — the hash + lookup + update
 * paths. The prefetch helper itself only issues a hint, so we
 * cannot directly observe its effect; this test asserts the
 * surrounding bookkeeping (the lookup is consistent for the same
 * PC; updates land at the indexed slot; out-of-range classes are
 * rejected).
 */
static int check_predict_table_round_trip(void)
{
	struct v8m_thread_cache *cache = v8m_thread_cache_get_or_create();
	if (cache == NULL) {
		return fail("get_or_create returned NULL");
	}

	/* Two synthetic call-site PCs whose mid bits land in
	 * different 1024-entry table slots (the index uses
	 * (pc >> 4) & (size - 1) — pick values whose low 14 bits
	 * differ in the [4..14) range so the hash diverges). */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	const void *pc_a = (const void *)(uintptr_t)0xCAFE0100U;
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	const void *pc_b = (const void *)(uintptr_t)0xBEEF0200U;
	uintptr_t idx_a =
	    ((uintptr_t)pc_a >> 4) & (V8M_PREDICT_TABLE_SIZE - 1U);
	uintptr_t idx_b =
	    ((uintptr_t)pc_b >> 4) & (V8M_PREDICT_TABLE_SIZE - 1U);
	/* The constants make this comparison constant-foldable;
	 * cppcheck rightly notes "always false". The check stays as
	 * documentation that future tweaks to the synthetic PCs
	 * cannot silently collapse the test into a no-op. */
	/* cppcheck-suppress knownConditionTrueFalse */
	if (idx_a == idx_b) {
		return fail("synthetic PCs collided in the predict table");
	}

	/* Wipe the table so prior tests' updates don't leak in. */
	for (size_t i = 0; i < V8M_PREDICT_TABLE_SIZE; i++) {
		cache->predict_table[i] = V8M_PREDICT_NONE;
	}

	/* Update + read-back per PC. Use class 5 for A, class 12 for B. */
	v8m_thread_cache_predict_update(cache, pc_a, 5U);
	v8m_thread_cache_predict_update(cache, pc_b, 12U);
	if (cache->predict_table[idx_a] != 5U) {
		return fail("predict_update did not record class for PC A");
	}
	if (cache->predict_table[idx_b] != 12U) {
		return fail("predict_update did not record class for PC B");
	}

	/* Out-of-range class is rejected; prior value stays. */
	v8m_thread_cache_predict_update(cache, pc_a, V8M_MEDIUM_FIRST_CLASS);
	if (cache->predict_table[idx_a] != 5U) {
		return fail("out-of-range class clobbered the slot");
	}
	v8m_thread_cache_predict_update(cache, pc_a, 99U); /* well past end */
	if (cache->predict_table[idx_a] != 5U) {
		return fail("99 clobbered the slot");
	}

	/* Prefetch is a hint — call it on both known and unknown PCs
	 * just to make sure neither path crashes. There is no
	 * observable post-state. */
	v8m_thread_cache_predict_prefetch(cache, pc_a);
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	const void *unknown_pc = (const void *)(uintptr_t)0xBADD00DBU;
	v8m_thread_cache_predict_prefetch(cache, unknown_pc);
	v8m_thread_cache_predict_prefetch(NULL, pc_a); /* null cache */
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_same_thread_returns_same_cache();
	result |= check_distinct_threads_get_distinct_caches();
	result |= check_destructor_fires_on_thread_exit();
	result |= check_alloc_free_round_trip();
	result |= check_overflow_signal();
	result |= check_drain_remote_routes_to_bin();
	result |= check_adaptive_capacity_grows_with_demand();
	result |= check_gc_countdown_fires();
	result |= check_predict_table_round_trip();
	if (result == 0) {
		(void)printf("test_thread_cache: OK\n");
	}
	return result;
}
