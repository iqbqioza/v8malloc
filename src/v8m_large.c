/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Large/Huge direct-mmap allocation path. One v8m_page_heap_alloc()
 * per request; the region's V8M_SLAB_HEADER_SIZE-aligned header
 * carries the page-metadata plus the mmap size so free() can unmap
 * the exact bytes allocated.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_internal.h"
#include "v8m_large.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_size_class.h"

/* Common-prefix offsets must match v8m_page_meta exactly, so generic
 * reverse-lookup code (v8m_ptr_to_meta + magic check) works against
 * Large/Huge pages without a tag check. */
static_assert(offsetof(struct v8m_large_page_meta, magic) ==
		  offsetof(struct v8m_page_meta, magic),
	      "Large meta: magic offset diverges from common layout");
static_assert(offsetof(struct v8m_large_page_meta, size_class) ==
		  offsetof(struct v8m_page_meta, size_class),
	      "Large meta: size_class offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, object_size) ==
		  offsetof(struct v8m_page_meta, object_size),
	      "Large meta: object_size offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, capacity) ==
		  offsetof(struct v8m_page_meta, capacity),
	      "Large meta: capacity offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, used_count) ==
		  offsetof(struct v8m_page_meta, used_count),
	      "Large meta: used_count offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, owner_thread) ==
		  offsetof(struct v8m_page_meta, owner_thread),
	      "Large meta: owner_thread offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, free_list_head) ==
		  offsetof(struct v8m_page_meta, free_list_head),
	      "Large meta: free_list_head offset diverges");
static_assert(offsetof(struct v8m_large_page_meta, next) ==
		  offsetof(struct v8m_page_meta, next),
	      "Large meta: next offset diverges");

static_assert(sizeof(struct v8m_large_page_meta) <= V8M_SLAB_HEADER_SIZE,
	      "Large meta exceeds V8M_SLAB_HEADER_SIZE");

/*
 * Sentinel stored in size_class for Huge allocations (> 2 MiB), so a
 * walker that wants to distinguish Huge from Large can check against
 * V8M_LARGE_HUGE_TAG. Large allocations (classes 38..40) keep their
 * actual class value in the field.
 */
#define V8M_LARGE_HUGE_TAG UINT16_MAX

/*
 * Per-class lifetime counters. Updated on every alloc / free; read
 * by v8m_large_get_stats and the public v8m_huge_stats reporter.
 * All relaxed because they're diagnostic — the surrounding
 * page-heap counters carry the load-bearing happens-before edges.
 */
static _Atomic uint64_t v8m_large_alloc_count;
static _Atomic uint64_t v8m_large_free_count;
static _Atomic uint64_t v8m_large_bytes_in_use;
static _Atomic uint64_t v8m_huge_alloc_count;
static _Atomic uint64_t v8m_huge_free_count;
static _Atomic uint64_t v8m_huge_bytes_in_use;

/*
 * Round `value` up to the next multiple of `multiple`. `multiple`
 * must be a power of two; the caller checks that.
 */
static size_t round_up_pow2(size_t value, size_t multiple)
{
	return (value + multiple - 1U) & ~(multiple - 1U);
}

/*
 * Shared backend used by v8m_large_alloc / v8m_large_alloc_aligned.
 * `alignment` is a power of two, < V8M_PAGE_SIZE, or 0 for the
 * default V8M_SLAB_HEADER_SIZE offset.
 */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void *large_alloc_with_offset(size_t size, size_t header_offset,
				     uint64_t owner_thread)
{
	/* Overflow guard: need room for the header + size, then round
	 * up to a page multiple. */
	if (size > SIZE_MAX - header_offset - V8M_PAGE_SIZE) {
		return NULL;
	}

	size_t total = header_offset + size;
	size_t mmap_size = round_up_pow2(total, V8M_PAGE_SIZE);

	void *region = v8m_page_heap_alloc(mmap_size, V8M_PAGE_SIZE);
	if (region == NULL) {
		return NULL;
	}

	struct v8m_large_page_meta *meta = region;
	uint32_t cls = v8m_size_class(size);
	uint16_t size_class_tag = (cls == V8M_CLASS_HUGE)
				      ? (uint16_t)V8M_LARGE_HUGE_TAG
				      : (uint16_t)cls;

	meta->magic = V8M_MAGIC;
	meta->size_class = size_class_tag;
	meta->object_size = 0;
	meta->capacity = 1;
	atomic_store_explicit(&meta->used_count, 1U, memory_order_relaxed);
	meta->owner_thread = owner_thread;
	meta->free_list_head = NULL;
	meta->next = NULL;
	meta->mmap_size = mmap_size;

	if (cls == V8M_CLASS_HUGE) {
		atomic_fetch_add_explicit(&v8m_huge_alloc_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&v8m_huge_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&v8m_large_alloc_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&v8m_large_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	}

	return (unsigned char *)region + header_offset;
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void *v8m_large_alloc(size_t size, uint64_t owner_thread)
{
	if (size == 0) {
		return NULL;
	}
	return large_alloc_with_offset(size, V8M_SLAB_HEADER_SIZE,
				       owner_thread);
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void *v8m_large_alloc_aligned(size_t size, size_t alignment,
			      uint64_t owner_thread)
{
	if (size == 0) {
		return NULL;
	}
	if (alignment == 0 || alignment <= V8M_SLAB_HEADER_SIZE) {
		/* Default header offset already satisfies alignment <=
		 * V8M_SLAB_HEADER_SIZE (which is a power of two). */
		return large_alloc_with_offset(size, V8M_SLAB_HEADER_SIZE,
					       owner_thread);
	}
	/* The header sits at region offset 0; the user pointer is at
	 * offset header_offset. v8m_ptr_to_meta masks away the low
	 * V8M_PAGE_SHIFT bits, so the meta is recoverable iff
	 * header_offset < V8M_PAGE_SIZE. */
	if (alignment >= V8M_PAGE_SIZE) {
		return NULL;
	}
	/* alignment is a power of two > V8M_SLAB_HEADER_SIZE, so the
	 * smallest multiple of alignment that admits the header is
	 * `alignment` itself. */
	return large_alloc_with_offset(size, alignment, owner_thread);
}

void v8m_large_free(const void *obj)
{
	if (obj == NULL) {
		return;
	}
	/* Recover the page base via the shared ptr-to-meta helper so the
	 * int-to-ptr cast stays behind v8m_ptr_to_meta and this module
	 * doesn't need its own NOLINT. ptr_to_meta returns a non-const
	 * pointer, so writing to the header fields below is well-formed
	 * even though the caller's object pointer is const-qualified. */
	struct v8m_page_meta *common = v8m_ptr_to_meta(obj);
	struct v8m_large_page_meta *meta = (struct v8m_large_page_meta *)common;
	size_t mmap_size = meta->mmap_size;
	bool was_huge = meta->size_class == (uint16_t)V8M_LARGE_HUGE_TAG;

	atomic_store_explicit(&meta->used_count, 0U, memory_order_relaxed);
	meta->magic = 0U;

	if (was_huge) {
		atomic_fetch_add_explicit(&v8m_huge_free_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_sub_explicit(&v8m_huge_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&v8m_large_free_count, 1U,
					  memory_order_relaxed);
		atomic_fetch_sub_explicit(&v8m_large_bytes_in_use, mmap_size,
					  memory_order_relaxed);
	}

	v8m_page_heap_free(common, mmap_size);
}

size_t v8m_large_usable_size(const void *obj)
{
	if (obj == NULL) {
		return 0;
	}
	const struct v8m_page_meta *common = v8m_ptr_to_meta(obj);
	const struct v8m_large_page_meta *meta =
	    (const struct v8m_large_page_meta *)common;
	/* The user pointer sits at offset header_offset from the page
	 * base; recover that offset from the pointer's low bits. For the
	 * default path header_offset == V8M_SLAB_HEADER_SIZE, for the
	 * aligned variant it equals the requested alignment. Both fit in
	 * the page (< V8M_PAGE_SIZE), so the low-bits trick is exact. */
	uintptr_t header_offset = (uintptr_t)obj & (V8M_PAGE_SIZE - 1U);
	return meta->mmap_size - header_offset;
}

void v8m_large_get_stats(struct v8m_large_stats *out)
{
	if (out == NULL) {
		return;
	}
	out->large_alloc_count =
	    atomic_load_explicit(&v8m_large_alloc_count, memory_order_relaxed);
	out->large_free_count =
	    atomic_load_explicit(&v8m_large_free_count, memory_order_relaxed);
	out->large_bytes_in_use =
	    atomic_load_explicit(&v8m_large_bytes_in_use, memory_order_relaxed);
	out->huge_alloc_count =
	    atomic_load_explicit(&v8m_huge_alloc_count, memory_order_relaxed);
	out->huge_free_count =
	    atomic_load_explicit(&v8m_huge_free_count, memory_order_relaxed);
	out->huge_bytes_in_use =
	    atomic_load_explicit(&v8m_huge_bytes_in_use, memory_order_relaxed);
}
