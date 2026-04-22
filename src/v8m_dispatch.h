/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Allocation dispatcher — the layer that routes a `size` request to
 * the correct backend (slab pool for Tiny + Small, buddy pool for
 * Medium, direct mmap for Large + Huge) and routes a `free` to the
 * pool that owns the pointer.
 *
 * Free dispatch goes:
 *   1. v8m_page_heap_owns. If false, the pointer never came from
 *      our mmap, so forward it to the captured libc free without
 *      reading through it. This guards step 2 against faults on
 *      truly-foreign pointers whose page-aligned base is unmapped.
 *   2. v8m_ptr_to_meta + magic check. If valid, the page belongs to
 *      either the slab pool (size_class < V8M_MEDIUM_FIRST_CLASS) or
 *      the Large/Huge direct path. Route accordingly.
 *   3. Otherwise the page-heap-owned pointer must belong to a buddy
 *      arena (buddy arenas don't stamp v8m_page_meta), so the
 *      buddy pool's range-check free is invoked unconditionally.
 *
 * This is the single-threaded baseline — every operation goes
 * through the slab/buddy pools' per-pool mutexes. The TLC + L2 core
 * cache will sit in front of this on the hot path in a future
 * cycle; the dispatcher then becomes the slow path / refill source.
 *
 * The dispatcher is intentionally not a singleton — each instance
 * carries its own pools so tests can run with isolated state. The
 * public API will hold one global instance behind its constructor.
 */

#ifndef V8M_DISPATCH_H
#define V8M_DISPATCH_H

#include <stddef.h>

#include "v8m_buddy_pool.h"
#include "v8m_slab_pool.h"

struct v8m_dispatch {
	struct v8m_slab_pool slab;
	struct v8m_buddy_pool buddy;
	/*
	 * Opt-in TLC routing. The thread cache is a per-thread
	 * singleton; routing two distinct dispatchers through the
	 * same TLC would mix slab pages from different pools and
	 * crash on free. The public API enables this on its single
	 * `g_dispatch`; isolated test fixtures (which create their
	 * own dispatcher) leave it false so alloc / free serve the
	 * local pools directly.
	 */
	bool use_tlc;
};

/*
 * Initialize a freshly-allocated dispatcher. Returns 0 on success,
 * an errno-style code on lock-init failure. On failure, no pools
 * remain initialized and v8m_dispatch_destroy must not be called.
 */
int v8m_dispatch_init(struct v8m_dispatch *dispatch);

/*
 * Tear down a dispatcher. Releases every page held by either pool
 * back to the page heap. Callers must ensure no allocations remain
 * live.
 */
void v8m_dispatch_destroy(struct v8m_dispatch *dispatch);

/*
 * Allocate `size` bytes. `size == 0` is treated like `size == 1`
 * (returns a unique pointer the caller can free) so that
 * malloc(0) — which POSIX permits to return either NULL or a
 * unique pointer — keeps the more useful behaviour. Returns NULL
 * only on backend failure (page-heap exhaustion, arena cap, etc.).
 */
void *v8m_dispatch_alloc(struct v8m_dispatch *dispatch, size_t size);

/*
 * Aligned variant. `alignment` must be a power of two; passing 0 or
 * a value <= the natural malloc alignment behaves identically to
 * v8m_dispatch_alloc. Routing:
 *   - Slab path when a size class exists whose object size is at
 *     least max(size, alignment) and is divisible by `alignment`.
 *   - Buddy path when the rounded-up effective size fits and the
 *     buddy level inherently satisfies the alignment.
 *   - Large path with a widened header offset for alignments
 *     between V8M_SLAB_HEADER_SIZE and V8M_PAGE_SIZE/2.
 *   - Returns NULL when alignment exceeds the largest supported
 *     value (V8M_BUDDY_MAX_BLOCK in v0).
 */
void *v8m_dispatch_alloc_aligned(struct v8m_dispatch *dispatch, size_t size,
				 size_t alignment);

/*
 * Free a previously-issued pointer. Tolerates NULL. Pointers that
 * neither pool owns are dropped silently in this cycle; the libc
 * fallback lands with the public API / init cycle.
 */
void v8m_dispatch_free(struct v8m_dispatch *dispatch, void *ptr);

/*
 * Bytes accessible through `ptr`, recovered from the owning
 * backend. Returns 0 for NULL or for pointers no backend owns.
 * For slabs the value is the size class's object size; for buddy
 * allocations the chosen power-of-two block size; for Large/Huge
 * the mmap_size minus the header reservation.
 */
size_t v8m_dispatch_usable_size(struct v8m_dispatch *dispatch, const void *ptr);

/*
 * pthread_atfork hooks. The parent process's library constructor
 * registers a triple that calls these via fixed wrappers; the
 * dispatcher exposes them so the public API layer doesn't need to
 * reach into pool internals.
 *
 * Lock acquisition order in `prefork` is slab → buddy; both
 * postfork handlers release in reverse (buddy → slab). Acquiring
 * before fork ensures the child sees a quiescent dispatcher state
 * even though only the calling thread survived the syscall, and
 * releasing afterwards lets both processes resume normal use.
 *
 * In the child, the locks acquired by the prefork hook are held
 * by the (only surviving) calling thread, so unlocking them is
 * safe — re-init via pthread_mutex_init is unnecessary.
 */
void v8m_dispatch_prefork(struct v8m_dispatch *dispatch);
void v8m_dispatch_postfork_parent(struct v8m_dispatch *dispatch);
void v8m_dispatch_postfork_child(struct v8m_dispatch *dispatch);

/*
 * Background-purge tick handler. Invoked by the bg purge thread once
 * per V8M_OPT_PURGE_INTERVAL. Today: ages drained buddy arenas one
 * tick and releases any that exceed the idle threshold. Future: TLC
 * bin shrink, per-NUMA empty-page sweep. Returns the number of
 * arenas released this pass — useful for bg-purge stats lines under
 * V8M_OPT_VERBOSE.
 */
size_t v8m_dispatch_bg_tick(struct v8m_dispatch *dispatch);

/*
 * Force-release every currently drained buddy arena (regardless of
 * idle-tick count). Returns the number of arenas released. Invoked
 * by `v8m_purge()` so an explicit caller gets immediate VMA + RSS
 * relief, and by the soft-limit refusal path so a workload that
 * sets a tight `v8m_set_soft_limit` cannot be blocked by
 * reclaimable bytes that the periodic sweep has not yet visited.
 */
size_t v8m_dispatch_purge_drained(struct v8m_dispatch *dispatch);

/*
 * Enable / disable TLC routing for this dispatcher. Default is
 * disabled — only the public-API singleton turns it on. Caller is
 * expected to set this once at init time, before any concurrent
 * alloc / free calls; the field is read on every alloc / free
 * fast-path entry but not synchronized.
 */
void v8m_dispatch_set_use_tlc(struct v8m_dispatch *dispatch, bool enabled);

#endif /* V8M_DISPATCH_H */
