/* SPDX-License-Identifier: Apache-2.0 */
/*
 * L2 per-core cache (v8m_core_cache) coverage. The L2 is a
 * lock-free Treiber stack with tagged-pointer ABA protection;
 * v0 ships the primitive but does not yet wire it into the
 * TLC overflow / refill paths. This test exercises the
 * primitive directly:
 *
 *   1. Single-thread round-trip (push N, pop N, LIFO order).
 *   2. Empty-stack pop returns NULL.
 *   3. Multi-thread concurrent push + pop on the same stack —
 *      total objects pushed equals total popped, no losses,
 *      no duplicates.
 *
 * The wiring into the TLC is the spec'd `push_batch / pop_batch
 * to amortize CAS` cycle (TODO P1 row 63), which lands later.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_core_cache.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_core_cache: %s\n", msg);
	return 1;
}

static int check_single_thread_round_trip(void)
{
	struct v8m_core_cache *cache = v8m_core_cache_for_cpu(0);
	if (cache == NULL) {
		return fail("for_cpu(0) returned NULL");
	}
	const uint32_t cls = 5;

	/* Drain anything other tests might have left for cpu 0. */
	while (v8m_core_cache_pop(cache, cls) != NULL) {
	}

	if (v8m_core_cache_pop(cache, cls) != NULL) {
		return fail("empty-stack pop did not return NULL");
	}

	enum { COUNT = 16 };
	uint64_t slots[COUNT];
	for (int i = 0; i < COUNT; i++) {
		slots[i] = (uint64_t)i;
		if (!v8m_core_cache_push(cache, cls, &slots[i])) {
			return fail("push returned false under capacity");
		}
	}
	for (int i = COUNT - 1; i >= 0; i--) {
		const void *got = v8m_core_cache_pop(cache, cls);
		if (got != &slots[i]) {
			return fail("LIFO pop returned the wrong slot");
		}
	}
	if (v8m_core_cache_pop(cache, cls) != NULL) {
		return fail("post-drain pop did not return NULL");
	}
	return 0;
}

static int check_out_of_range_class(void)
{
	struct v8m_core_cache *cache = v8m_core_cache_for_cpu(0);
	uint64_t slot = 0;
	if (v8m_core_cache_push(cache, 0xFFFFU, &slot)) {
		return fail("push accepted out-of-range class");
	}
	if (v8m_core_cache_pop(cache, 0xFFFFU) != NULL) {
		return fail("pop accepted out-of-range class");
	}
	if (v8m_core_cache_push(NULL, 0, &slot)) {
		return fail("push accepted NULL cache");
	}
	if (v8m_core_cache_pop(NULL, 0) != NULL) {
		return fail("pop accepted NULL cache");
	}
	if (v8m_core_cache_push(cache, 0, NULL)) {
		return fail("push accepted NULL node");
	}
	return 0;
}

/* Conservation stress: N producer threads push K nodes each, M
 * consumer threads pop until exhaustion. Total pushed must equal
 * total popped; every pushed pointer must show up exactly once
 * on the consumer side. */
enum {
	STRESS_PRODUCERS = 4,
	STRESS_CONSUMERS = 4,
	STRESS_PER_PRODUCER = 1024,
	STRESS_TOTAL = STRESS_PRODUCERS * STRESS_PER_PRODUCER,
};

static struct v8m_core_cache *g_stress_cache;
static const uint32_t g_stress_cls = 7;
static uint64_t g_stress_storage[STRESS_TOTAL];
static atomic_int g_consumed_count;
static atomic_int g_consumed_marks[STRESS_TOTAL];

struct stress_arg {
	int producer_id;
};

static void *stress_producer(void *raw)
{
	const struct stress_arg *arg = raw;
	int base = arg->producer_id * STRESS_PER_PRODUCER;
	for (int i = 0; i < STRESS_PER_PRODUCER; i++) {
		(void)v8m_core_cache_push(g_stress_cache, g_stress_cls,
					  &g_stress_storage[base + i]);
	}
	return NULL;
}

static void *stress_consumer(void *raw)
{
	(void)raw;
	for (;;) {
		void *node = v8m_core_cache_pop(g_stress_cache, g_stress_cls);
		if (node == NULL) {
			/* Empty for now — but producers may still be
			 * pushing. Spin briefly and retry until the
			 * total consumed reaches STRESS_TOTAL. */
			if (atomic_load_explicit(&g_consumed_count,
						 memory_order_acquire) >=
			    STRESS_TOTAL) {
				return NULL;
			}
			continue;
		}
		size_t idx = (uint64_t *)node - g_stress_storage;
		if (idx >= STRESS_TOTAL) {
			(void)fprintf(stderr,
				      "test_core_cache: consumer popped a "
				      "pointer outside the stress storage\n");
			(void)atomic_fetch_add_explicit(&g_consumed_count, 1,
							memory_order_release);
			continue;
		}
		(void)atomic_fetch_add_explicit(&g_consumed_marks[idx], 1,
						memory_order_relaxed);
		(void)atomic_fetch_add_explicit(&g_consumed_count, 1,
						memory_order_release);
	}
}

static int check_concurrent_push_pop(void)
{
	g_stress_cache = v8m_core_cache_for_cpu(1);
	if (g_stress_cache == NULL) {
		return fail("for_cpu(1) returned NULL");
	}
	while (v8m_core_cache_pop(g_stress_cache, g_stress_cls) != NULL) {
	}
	atomic_store_explicit(&g_consumed_count, 0, memory_order_relaxed);
	for (int i = 0; i < STRESS_TOTAL; i++) {
		atomic_store_explicit(&g_consumed_marks[i], 0,
				      memory_order_relaxed);
	}

	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t producers[STRESS_PRODUCERS];
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t consumers[STRESS_CONSUMERS];
	struct stress_arg producer_args[STRESS_PRODUCERS];
	for (int i = 0; i < STRESS_PRODUCERS; i++) {
		producer_args[i].producer_id = i;
		if (pthread_create(&producers[i], NULL, stress_producer,
				   &producer_args[i]) != 0) {
			return fail("producer pthread_create failed");
		}
	}
	for (int i = 0; i < STRESS_CONSUMERS; i++) {
		if (pthread_create(&consumers[i], NULL, stress_consumer,
				   NULL) != 0) {
			return fail("consumer pthread_create failed");
		}
	}
	for (int i = 0; i < STRESS_PRODUCERS; i++) {
		(void)pthread_join(producers[i], NULL);
	}
	for (int i = 0; i < STRESS_CONSUMERS; i++) {
		(void)pthread_join(consumers[i], NULL);
	}

	int total =
	    atomic_load_explicit(&g_consumed_count, memory_order_acquire);
	if (total != STRESS_TOTAL) {
		(void)fprintf(stderr,
			      "test_core_cache: consumed %d, expected %d\n",
			      total, STRESS_TOTAL);
		return 1;
	}
	for (int i = 0; i < STRESS_TOTAL; i++) {
		int marks = atomic_load_explicit(&g_consumed_marks[i],
						 memory_order_relaxed);
		if (marks != 1) {
			(void)fprintf(stderr,
				      "test_core_cache: slot %d consumed "
				      "%d times, expected 1\n",
				      i, marks);
			return 1;
		}
	}
	return 0;
}

/*
 * Batch op coverage. push_batch single-CAS pushes a chain;
 * pop_batch returns up to N nodes assembled into a forward
 * chain. Both are pure data manipulation given the test owns
 * the stack — no concurrency in this scenario, just the
 * push/pop bookkeeping.
 */
static int check_batch_round_trip(void)
{
	struct v8m_core_cache *cache = v8m_core_cache_for_cpu(2);
	if (cache == NULL) {
		return fail("for_cpu(2) returned NULL");
	}
	const uint32_t cls = 9;
	while (v8m_core_cache_pop(cache, cls) != NULL) {
	}

	enum { COUNT = 32 };
	uint64_t slots[COUNT];

	/* Pre-link COUNT slots into a forward chain, head=slot[0]. */
	for (int i = 0; i < COUNT - 1; i++) {
		void *next = &slots[i + 1];
		(void)memcpy(&slots[i], (const void *)&next, sizeof(next));
	}
	void *terminator = NULL;
	(void)memcpy(&slots[COUNT - 1], (const void *)&terminator,
		     sizeof(terminator));

	if (!v8m_core_cache_push_batch(cache, cls, &slots[0],
				       &slots[COUNT - 1])) {
		return fail("push_batch returned false");
	}

	void *out_head = NULL;
	void *out_tail = NULL;
	size_t got =
	    v8m_core_cache_pop_batch(cache, cls, COUNT, &out_head, &out_tail);
	if (got != COUNT) {
		(void)fprintf(
		    stderr, "test_core_cache: pop_batch got %zu, expected %d\n",
		    got, COUNT);
		return 1;
	}
	if (out_head != &slots[0]) {
		return fail("pop_batch out_head != pushed head");
	}
	/* Walk the chain and verify each node appears exactly once. */
	int seen[COUNT] = {0};
	void *node = out_head;
	for (int i = 0; i < COUNT; i++) {
		size_t idx = (uint64_t *)node - slots;
		if (idx >= COUNT) {
			return fail("popped chain points outside slot array");
		}
		seen[idx]++;
		void *next = NULL;
		(void)memcpy((void *)&next, node, sizeof(next));
		node = next;
	}
	if (node != NULL) {
		return fail("chain not terminated after COUNT walks");
	}
	for (int i = 0; i < COUNT; i++) {
		if (seen[i] != 1) {
			return fail("a slot did not appear exactly once in "
				    "the popped chain");
		}
	}

	/* Empty stack — pop_batch returns 0 with NULL outs. */
	got = v8m_core_cache_pop_batch(cache, cls, COUNT, &out_head, &out_tail);
	if (got != 0U || out_head != NULL || out_tail != NULL) {
		return fail("empty pop_batch did not return zero/NULL");
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_single_thread_round_trip();
	result |= check_out_of_range_class();
	result |= check_concurrent_push_pop();
	result |= check_batch_round_trip();
	if (result == 0) {
		(void)printf("test_core_cache: OK\n");
	}
	return result;
}
