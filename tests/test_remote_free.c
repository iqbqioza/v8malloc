/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MPSC lock-free queue stress test. N producers each push K tagged
 * nodes; one consumer continuously drains. After the consumer has
 * accounted for every expected node it exits. The test verifies that
 * every pushed node is observed exactly once — no loss, no
 * duplication — and that draining an empty queue returns NULL.
 */

#include <pthread.h> /* IWYU pragma: keep */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "v8m_remote_free.h"

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

int main(void)
{
	int status = check_single_thread();
	if (status != 0) {
		return status;
	}
	return check_multi_thread();
}
