/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Allocation dispatcher — the layer that routes a `size` request to
 * the correct backend (slab pool for Tiny + Small, buddy pool for
 * Medium, direct mmap for Large + Huge) and routes a `free` to the
 * pool that owns the pointer.
 *
 * Free dispatch goes:
 *   1. v8m_ptr_to_meta + magic check. If valid, the page belongs to
 *      either the slab pool (size_class < V8M_MEDIUM_FIRST_CLASS) or
 *      the Large/Huge direct path. Route accordingly.
 *   2. Otherwise try the buddy pool's range-check ownership; on
 *      success it owned and freed.
 *   3. Otherwise the pointer is foreign — for v0 it is silently
 *      dropped. The init/fini cycle adds the libc fallback via
 *      dlsym(RTLD_NEXT, "free").
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
 * Free a previously-issued pointer. Tolerates NULL. Pointers that
 * neither pool owns are dropped silently in this cycle; the libc
 * fallback lands with the public API / init cycle.
 */
void v8m_dispatch_free(struct v8m_dispatch *dispatch, void *ptr);

#endif /* V8M_DISPATCH_H */
