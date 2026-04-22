/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MPSC remote-free coverage. Two scenarios:
 *
 * 1. MPSC primitive stress (`check_single_thread` /
 *    `check_multi_thread`). N producers each push K tagged nodes,
 *    one consumer drains; verifies that every pushed node is
 *    observed exactly once, no loss, no duplication, and that
 *    draining an empty queue returns NULL. Covers the lock-free
 *    queue itself in isolation.
 *
 * 2. End-to-end dispatch integration
 *    (`check_alloc_remote_realloc`, TODO P0 row "Slow path chain"
 *    + [Test] row "test_remote_free.c"). The spec scenario is "A
 *    allocates, B frees, A re-allocates same class → MPSC drain
 *    works". v0 keeps slab pages pool-owned so the dispatcher's
 *    free path always lands in the freeing thread's local bin
 *    (the producer-side routing piece lights up with the
 *    thread-owned-slab refactor); the integration test simulates
 *    that routing by having thread B push A's slot directly onto
 *    A's `cache->remote` MPSC queue, then verifies A's next
 *    allocation drains the queue on the TLC slow path and
 *    returns the same pointer.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "v8m_remote_free.h"
#include "v8m_thread_cache.h"
#include "v8malloc/v8malloc.h" /* v8m_purge_thread */

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_remote_free: %s\n", msg);
	return 1;
}

enum {
	PRODUCERS = 8,
	NODES_PER_PRODUCER = 1024,
	TOTAL_NODES = PRODUCERS * NODES_PER_PRODUCER
};

enum consumer_status {
	CONSUMER_OK = 0,
	CONSUMER_BAD_TAG = 1,
	CONSUMER_DUPLICATE = 2
};

struct tagged_node {
	struct v8m_mpsc_node mpsc;
	int producer_id;
	int sequence;
};

static struct tagged_node g_nodes[TOTAL_NODES];
static struct v8m_mpsc_queue g_queue;
static bool g_received[TOTAL_NODES];
static atomic_int g_consumer_status = CONSUMER_OK;

struct producer_arg {
	int producer_id;
};

static void *producer(void *arg)
{
	const struct producer_arg *parg = arg;
	int base = parg->producer_id * NODES_PER_PRODUCER;
	for (int seq = 0; seq < NODES_PER_PRODUCER; seq++) {
		struct tagged_node *node = &g_nodes[base + seq];
		node->producer_id = parg->producer_id;
		node->sequence = seq;
		v8m_mpsc_push(&g_queue, &node->mpsc);
	}
	return NULL;
}

static void *consumer(void *arg)
{
	(void)arg;
	int consumed = 0;
	while (consumed < TOTAL_NODES) {
		struct v8m_mpsc_node *list = v8m_mpsc_drain(&g_queue);
		while (list != NULL) {
			const struct tagged_node *node =
			    (const struct tagged_node *)list;
			int idx = (node->producer_id * NODES_PER_PRODUCER) +
				  node->sequence;
			if (idx < 0 || idx >= TOTAL_NODES) {
				atomic_store(&g_consumer_status,
					     CONSUMER_BAD_TAG);
				return NULL;
			}
			if (g_received[idx]) {
				atomic_store(&g_consumer_status,
					     CONSUMER_DUPLICATE);
				return NULL;
			}
			g_received[idx] = true;
			consumed++;
			list = atomic_load_explicit(&list->next,
						    memory_order_relaxed);
		}
	}
	return NULL;
}

static int check_single_thread(void)
{
	v8m_mpsc_init(&g_queue);

	if (v8m_mpsc_drain(&g_queue) != NULL) {
		return fail("drain of empty queue returned non-NULL");
	}

	/* Single-thread test only checks pointer round-trip; the tag
	 * fields are irrelevant. */
	struct tagged_node only = {0};
	v8m_mpsc_push(&g_queue, &only.mpsc);

	struct v8m_mpsc_node *drained = v8m_mpsc_drain(&g_queue);
	if (drained != &only.mpsc) {
		return fail("single push/drain did not round-trip");
	}
	if (atomic_load(&drained->next) != NULL) {
		return fail("single node's next is not NULL after drain");
	}
	if (v8m_mpsc_drain(&g_queue) != NULL) {
		return fail("drain after consumption returned non-NULL");
	}
	return 0;
}

static int check_multi_thread(void)
{
	v8m_mpsc_init(&g_queue);
	atomic_store(&g_consumer_status, CONSUMER_OK);
	for (int i = 0; i < TOTAL_NODES; i++) {
		g_received[i] = false;
	}

	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t consumer_thread;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t producer_threads[PRODUCERS];
	struct producer_arg args[PRODUCERS];

	if (pthread_create(&consumer_thread, NULL, consumer, NULL) != 0) {
		return fail("consumer pthread_create failed");
	}
	for (int i = 0; i < PRODUCERS; i++) {
		args[i].producer_id = i;
		if (pthread_create(&producer_threads[i], NULL, producer,
				   &args[i]) != 0) {
			return fail("producer pthread_create failed");
		}
	}
	for (int i = 0; i < PRODUCERS; i++) {
		(void)pthread_join(producer_threads[i], NULL);
	}
	(void)pthread_join(consumer_thread, NULL);

	int status = atomic_load(&g_consumer_status);
	if (status == CONSUMER_BAD_TAG) {
		return fail("consumer saw a node with an out-of-range tag");
	}
	if (status == CONSUMER_DUPLICATE) {
		return fail("consumer saw the same node twice");
	}
	for (int i = 0; i < TOTAL_NODES; i++) {
		if (!g_received[i]) {
			return fail("at least one pushed node never observed");
		}
	}
	if (v8m_mpsc_drain(&g_queue) != NULL) {
		return fail("queue not empty after consumer exits");
	}
	return 0;
}

/*
 * End-to-end dispatch integration. Owner thread allocates a
 * Tiny slot through the public malloc, exposes its TLC pointer,
 * waits for the producer to push the slot onto the TLC's MPSC
 * remote queue, drains its local cache via v8m_purge_thread (so
 * the next alloc forces the TLC slow path), and asserts that
 * the realloc returns the same pointer. Producer thread does
 * the cross-thread MPSC push to simulate the dispatcher's
 * future owner-thread free routing.
 */
struct shared_state {
	struct v8m_thread_cache *owner_cache;
	void *owner_slot;
	void *owner_realloc;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_barrier_t step;
};

static void *owner_thread(void *raw)
{
	struct shared_state *state = raw;
	state->owner_slot = malloc(8);
	state->owner_cache = v8m_thread_cache_peek();
	(void)pthread_barrier_wait(&state->step);

	(void)pthread_barrier_wait(&state->step);

	/* Drain TLC bin + local-CPU L2 to slab pool so the next
	 * alloc cannot satisfy from those layers and falls into
	 * the slow path where v8m_thread_cache_drain_remote
	 * fires. The MPSC remote queue is intentionally NOT
	 * touched by purge — that is the surface this test is
	 * exercising. */
	(void)v8m_purge_thread();

	state->owner_realloc = malloc(8);
	(void)pthread_barrier_wait(&state->step);
	return NULL;
}

static void *producer_thread(void *raw)
{
	struct shared_state *state = raw;
	(void)pthread_barrier_wait(&state->step);

	/* Push the owner's slot onto the owner's remote MPSC
	 * queue. Same call shape the dispatcher will make on a
	 * cross-thread free once thread-owned slab pages land. */
	v8m_mpsc_push(&state->owner_cache->remote,
		      (struct v8m_mpsc_node *)state->owner_slot);
	(void)pthread_barrier_wait(&state->step);

	(void)pthread_barrier_wait(&state->step);
	return NULL;
}

static int check_alloc_remote_realloc(void)
{
	struct shared_state state = {0};
	if (pthread_barrier_init(&state.step, NULL, 2) != 0) {
		return fail("pthread_barrier_init failed");
	}
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t owner_tid;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t producer_tid;
	if (pthread_create(&owner_tid, NULL, owner_thread, &state) != 0) {
		(void)pthread_barrier_destroy(&state.step);
		return fail("owner pthread_create failed");
	}
	if (pthread_create(&producer_tid, NULL, producer_thread, &state) != 0) {
		(void)pthread_join(owner_tid, NULL);
		(void)pthread_barrier_destroy(&state.step);
		return fail("producer pthread_create failed");
	}
	(void)pthread_join(owner_tid, NULL);
	(void)pthread_join(producer_tid, NULL);
	(void)pthread_barrier_destroy(&state.step);

	if (state.owner_slot == NULL) {
		return fail("owner's first malloc returned NULL");
	}
	if (state.owner_cache == NULL) {
		return fail("owner_cache was not captured");
	}
	if (state.owner_realloc == NULL) {
		return fail("owner's realloc returned NULL");
	}
	if (state.owner_realloc != state.owner_slot) {
		(void)fprintf(stderr,
			      "test_remote_free: drain did not return the "
			      "MPSC-pushed slot (slot=%p realloc=%p)\n",
			      state.owner_slot, state.owner_realloc);
		return 1;
	}

	/* Free the realloc once for cleanup. The original slot
	 * was effectively given back to the owner by the
	 * simulated remote free; freeing the realloc once
	 * releases it. */
	free(state.owner_realloc);
	return 0;
}

int main(void)
{
	int status = check_single_thread();
	if (status != 0) {
		return status;
	}
	status = check_multi_thread();
	if (status != 0) {
		return status;
	}
	return check_alloc_remote_realloc();
}
