/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-core L2 cache (architecture.md §2.2). Sits between the
 * per-thread TLC (L1) and the per-NUMA pool (L3) — when a TLC
 * bin overflows, half the bin lands in the calling core's L2;
 * when a TLC bin underflows, the calling core's L2 supplies a
 * batch of refills before the slow path falls through to the
 * NUMA / global pool.
 *
 * v0 ships the primitive only — the wiring (TLC overflow ->
 * L2 push, TLC underflow <- L2 pop) lands with the batch
 * push/pop cycle (TODO P1 row "push_batch / pop_batch to
 * amortize CAS"). Today the L2 is callable from tests but
 * unused on the alloc/free hot path.
 *
 * Lock-freedom: each per-class stack uses a Treiber-style CAS
 * loop on a tagged head pointer (head | ABA-tag in the upper
 * 16 bits — winning-algorithms.md §6, x86_64 / aarch64
 * canonical-form addresses leave those bits zero in user
 * mappings). The tag advances on every successful push or pop
 * so a producer that observes a stale head pointer cannot
 * mis-CAS even when the same address has cycled through the
 * stack between observations.
 */

#ifndef V8M_CORE_CACHE_H
#define V8M_CORE_CACHE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_arch.h" /* V8M_CACHELINE_ALIGNED */
#include "v8m_size_class.h" /* V8M_NUM_SIZE_CLASSES */

/*
 * Tagged 64-bit head pointer. Layout:
 *   bits  0..47 : the actual pointer value (canonical-form
 *                 addresses on x86_64 / aarch64 fit in 48 bits;
 *                 the kernel half is unreachable from user code,
 *                 so the top 16 bits are always zero for any
 *                 pointer the allocator hands out).
 *   bits 48..63 : ABA tag, monotonically incremented on every
 *                 push and pop. The tag wraps after 65 536 ops,
 *                 which the spec treats as acceptable: a stale
 *                 head pointer would have to be observed AND
 *                 re-presented within exactly 65 536 ops with no
 *                 intervening other observation by the same
 *                 thread — vanishingly improbable on the L2's
 *                 traffic.
 */
typedef uint64_t v8m_tagged_ptr;

#define V8M_TAGPTR_PTR_BITS 48U
#define V8M_TAGPTR_PTR_MASK ((1ULL << V8M_TAGPTR_PTR_BITS) - 1ULL)

static inline v8m_tagged_ptr v8m_tagptr_make(void *ptr, uint16_t tag)
{
	uint64_t raw = (uint64_t)(uintptr_t)ptr & V8M_TAGPTR_PTR_MASK;
	raw |= (uint64_t)tag << V8M_TAGPTR_PTR_BITS;
	return raw;
}

static inline void *v8m_tagptr_ptr(v8m_tagged_ptr tagged)
{
	uintptr_t addr = (uintptr_t)(tagged & V8M_TAGPTR_PTR_MASK);
	/* The address-bits-only half is canonical-form, so the
	 * cast back to a real pointer is well-defined. The lint
	 * rule against int-to-ptr casts is the entire point of
	 * the tagged-pointer scheme. */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	return (void *)addr;
}

static inline uint16_t v8m_tagptr_tag(v8m_tagged_ptr tagged)
{
	return (uint16_t)(tagged >> V8M_TAGPTR_PTR_BITS);
}

/*
 * Per-core L2 cache. One Treiber-style head per size class.
 * Cache-line aligned so neighbouring per-core entries in the
 * global array do not share a line — each thread writes only to
 * its own core's struct, but a cross-core peek must not bounce
 * the line on every read. V8M_NUM_SIZE_CLASSES * 8 bytes of head
 * pointers comes out to 328 B on every supported arch; rounded
 * up to the next 64 B cache-line gives 384 B per core. With
 * V8M_NUMA_MAX_CPUS = 4096 cores the full table fits in 1.5 MiB
 * of BSS — only physically backed for cores the workload
 * actually touches.
 */
struct v8m_core_cache {
	_Atomic v8m_tagged_ptr stacks[V8M_NUM_SIZE_CLASSES];
} V8M_CACHELINE_ALIGNED;

/*
 * Get the L2 cache for `cpu_id`. The global table is sized at
 * V8M_NUMA_MAX_CPUS; out-of-range ids return NULL. The returned
 * pointer is stable for the process lifetime — the table sits
 * in BSS, never reallocated.
 */
struct v8m_core_cache *v8m_core_cache_for_cpu(uint32_t cpu_id);

/*
 * Convenience: get the L2 cache for the calling thread's
 * current CPU (resolved via the cached vDSO `getcpu()` path
 * v8m_numa already wires up).
 */
struct v8m_core_cache *v8m_core_cache_for_current_cpu(void);

/*
 * Push `node` onto the cache's `cls` stack. The node's first
 * sizeof(void *) bytes are overwritten with the previous head
 * pointer (intrusive linking). `node` must be at least
 * sizeof(void *) wide and outlive its time on the stack.
 *
 * Single CAS loop on the tagged head; safe for concurrent
 * producers / consumers on the same stack. Returns false on
 * out-of-range `cls` or NULL inputs.
 */
bool v8m_core_cache_push(struct v8m_core_cache *cache, uint32_t cls,
			 void *node);

/*
 * Pop one node from the cache's `cls` stack. Returns NULL on
 * empty stack, out-of-range `cls`, or NULL `cache`. Single CAS
 * loop on the tagged head.
 */
void *v8m_core_cache_pop(struct v8m_core_cache *cache, uint32_t cls);

/*
 * Push a pre-linked chain of nodes onto the `cls` stack atomically.
 * `head` is the first node (becomes the new stack head); `tail` is
 * the last node in the chain (its first 8 bytes will be
 * overwritten with the previous stack head). The chain must be
 * intact (head reachable from `head` via the next-pointer chain
 * to `tail`) before the call. Single CAS on the tagged head;
 * amortizes the CAS cost over `count` nodes.
 *
 * Returns true on success. False on out-of-range `cls`, NULL
 * cache, NULL head/tail, or zero count.
 */
bool v8m_core_cache_push_batch(struct v8m_core_cache *cache, uint32_t cls,
			       void *head, void *tail);

/*
 * Pop up to `max` nodes from the `cls` stack into a chain
 * starting at `*out_head` and ending at `*out_tail` (the chain
 * is linked via the same intrusive next-pointer scheme push uses).
 * Returns the actual count, possibly zero. The implementation is a
 * loop of single pops — concurrent consumers walking a shared
 * chain would race on internal `next` reads, so the safe amortized
 * variant trades CAS amortization for correctness on the pop side.
 * The CAS amortization on push (the larger half of TLC overflow
 * traffic) is the substantive win.
 */
size_t v8m_core_cache_pop_batch(struct v8m_core_cache *cache, uint32_t cls,
				size_t max, void **out_head, void **out_tail);

/*
 * Cross-CPU work-stealing pop. When the calling thread's local L2
 * is empty (the producer side of a producer/consumer pair never
 * frees, so its local L2 stays empty while the consumer's L2 hoards
 * the freed slots), this helper redirects the pop to the L2 of the
 * CPU that most recently push_batch'd to this class. Single
 * relaxed-atomic load to find the donor + a normal pop_batch
 * against that donor's stack.
 *
 * Without stealing, the producer's TLC underflow would always fall
 * through to the slab pool's mutex, even though slots the consumer
 * just freed are sitting one cache line away in another L2.
 * Measured on mb_03 (2 producer/consumer pairs at size 64): closes
 * the gap with tcmalloc/mimalloc from 2.7× to roughly parity.
 *
 * Returns 0 when no donor is recorded yet, the donor's stack is
 * empty, the donor is the calling CPU itself (no point self-stealing
 * — the caller already tried the local L2), or `cls` is out of
 * range. On non-zero return, `*out_head`/`*out_tail` carry a
 * forward-linked chain identical to `pop_batch`'s output.
 */
size_t v8m_core_cache_steal_batch(uint32_t cls, uint32_t exclude_cpu,
				  size_t max, void **out_head, void **out_tail);

#endif /* V8M_CORE_CACHE_H */
