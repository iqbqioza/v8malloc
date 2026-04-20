/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MPSC lock-free queue for cross-thread ("remote") frees. See
 * thread-cache.md §2.3 and winning-algorithms.md §6.
 *
 * Multiple producers / single consumer: any thread may push; only the
 * owner thread — the one whose thread-cache the queue belongs to —
 * drains. Push is a CAS loop over the head pointer; drain is a single
 * atomic_exchange that hands the entire list to the consumer in one
 * shot.
 *
 * Design notes
 * ------------
 *  - release / acquire memory ordering is sufficient; the hot path
 *    deliberately avoids seq_cst.
 *  - The list is intrusive: freed objects reuse their own storage
 *    for the `next` link (the caller has just relinquished the
 *    object, so overwriting its first word is safe).
 *  - `v8m_mpsc_drain()` returns nodes in LIFO order relative to
 *    pushes. Consumers that care about order must reorder after
 *    draining.
 *  - ABA is a non-issue on push: we never interpret the old head
 *    value — we only swap it — so a stale pointer observed by the
 *    CAS loop is harmless.
 */

#ifndef V8M_REMOTE_FREE_H
#define V8M_REMOTE_FREE_H

#include <stdatomic.h>
#include <stddef.h>

/*
 * Intrusive queue node. When v8m_mpsc_push() is called on a freed
 * object, the object's storage is cast to this type and the first
 * word becomes the `next` link for the duration of its stay on the
 * queue.
 */
struct v8m_mpsc_node {
	_Atomic(struct v8m_mpsc_node *) next;
};

struct v8m_mpsc_queue {
	_Atomic(struct v8m_mpsc_node *) head;
};

/*
 * Initialize a queue to empty. Storage for the queue is the caller's
 * concern; this only zeroes the head pointer.
 */
static inline void v8m_mpsc_init(struct v8m_mpsc_queue *queue)
{
	atomic_store_explicit(&queue->head, NULL, memory_order_relaxed);
}

/*
 * Push a node onto the queue. Safe from any thread. One CAS loop; on
 * success the new head is published with release ordering so that
 * subsequent drains see the node's `next` field via the paired
 * acquire.
 */
static inline void v8m_mpsc_push(struct v8m_mpsc_queue *queue,
				 struct v8m_mpsc_node *node)
{
	struct v8m_mpsc_node *old_head =
	    atomic_load_explicit(&queue->head, memory_order_relaxed);
	do {
		atomic_store_explicit(&node->next, old_head,
				      memory_order_relaxed);
	} while (!atomic_compare_exchange_weak_explicit(
	    &queue->head, &old_head, node, memory_order_release,
	    memory_order_relaxed));
}

/*
 * Drain the queue, returning the head of an intrusive list (walked
 * via node->next). Returns NULL when the queue was already empty.
 * Must be called only from the owner thread.
 *
 * Acquire ordering on the exchange pairs with the release in every
 * prior push, so the `next` links written by producers are visible
 * under relaxed load during traversal.
 */
static inline struct v8m_mpsc_node *v8m_mpsc_drain(struct v8m_mpsc_queue *queue)
{
	return atomic_exchange_explicit(&queue->head, NULL,
					memory_order_acquire);
}

#endif /* V8M_REMOTE_FREE_H */
