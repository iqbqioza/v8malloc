/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Dispatcher implementation. Allocation routes by size class to the
 * slab pool, buddy pool, or direct mmap path. Free uses the
 * v8m_page_meta magic check to tell slab/large pages apart from
 * buddy and foreign pointers; buddy pages have no meta header so
 * the buddy pool's range-check ownership covers them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_buddy.h"
#include "v8m_buddy_pool.h"
#include "v8m_dispatch.h"
#include "v8m_large.h"
#include "v8m_page.h"
#include "v8m_size_class.h"
#include "v8m_slab_pool.h"

int v8m_dispatch_init(struct v8m_dispatch *dispatch)
{
	int ret = v8m_slab_pool_init(&dispatch->slab);
	if (ret != 0) {
		return ret;
	}
	ret = v8m_buddy_pool_init(&dispatch->buddy);
	if (ret != 0) {
		v8m_slab_pool_destroy(&dispatch->slab);
		return ret;
	}
	return 0;
}

void v8m_dispatch_destroy(struct v8m_dispatch *dispatch)
{
	v8m_buddy_pool_destroy(&dispatch->buddy);
	v8m_slab_pool_destroy(&dispatch->slab);
}

void *v8m_dispatch_alloc(struct v8m_dispatch *dispatch, size_t size)
{
	/* malloc(0) — POSIX permits NULL or a unique pointer; return a
	 * unique pointer so that downstream code that frees the result
	 * doesn't need a special-case branch. */
	if (size == 0) {
		size = 1;
	}

	uint32_t cls = v8m_size_class(size);
	if (cls < V8M_MEDIUM_FIRST_CLASS) {
		return v8m_slab_pool_alloc(&dispatch->slab, cls, 0);
	}
	if (size <= V8M_BUDDY_MAX_BLOCK) {
		return v8m_buddy_pool_alloc(&dispatch->buddy, size);
	}
	return v8m_large_alloc(size, 0);
}

void v8m_dispatch_free(struct v8m_dispatch *dispatch, void *ptr)
{
	if (ptr == NULL) {
		return;
	}

	struct v8m_page_meta *meta = v8m_ptr_to_meta(ptr);
	if (v8m_page_meta_valid(meta)) {
		if (meta->size_class < V8M_MEDIUM_FIRST_CLASS) {
			(void)v8m_slab_pool_free(&dispatch->slab, meta, ptr);
		} else {
			/* size_class is a Large class (38..40) or the
			 * UINT16_MAX Huge sentinel; v8m_large_free
			 * handles both via the same mmap_size header. */
			v8m_large_free(ptr);
		}
		return;
	}

	/* No magic at the page base — could be a buddy allocation
	 * (buddy arenas don't stamp v8m_page_meta) or a foreign
	 * pointer. Try the buddy pool's range check. */
	if (v8m_buddy_pool_free(&dispatch->buddy, ptr)) {
		return;
	}

	/* Foreign pointer — silently dropped for v0. The libc
	 * fallback (dlsym(RTLD_NEXT, "free")) lands with the public
	 * API / init cycle. */
}
