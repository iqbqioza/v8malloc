/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-page metadata header. Every 64 KiB-aligned slab, buddy, or
 * extent page begins with a v8m_page_meta (or v8m_tiny_page_meta for
 * Tiny slabs); given any pointer in a page, masking the low
 * V8M_PAGE_SHIFT bits recovers the metadata in O(1) with no table
 * lookup.
 *
 * V8M_MAGIC distinguishes v8malloc-managed pages from foreign
 * allocations that may reach our free() path; see architecture.md
 * §3.3 for the foreign-pointer fallback strategy.
 */

#ifndef V8M_PAGE_H
#define V8M_PAGE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "v8m_internal.h"

/*
 * The bytes 0x76 0x38 0x6D 0x61 0x6C 0x6C 0x6F 0x63 spell "v8malloc"
 * in ASCII; storing them in big-endian as a uint64_t gives us a
 * highly distinctive magic value whose collision probability with
 * a random foreign pointer's first eight bytes is ~2^-64.
 */
#define V8M_MAGIC UINT64_C(0x76386D616C6C6F63)

/*
 * Per-page metadata common to Small, Medium, and Large pages. Tiny
 * slabs use v8m_tiny_page_meta below, which extends this layout: the
 * leading fields share offsets exactly so generic code can introspect
 * either form via a v8m_page_meta pointer.
 */
struct v8m_page_meta {
	uint64_t magic;
	uint16_t size_class;
	uint16_t object_size;
	uint32_t capacity;
	atomic_uint used_count;
	uint64_t owner_thread;
	void *free_list_head;
	struct v8m_page_meta *next;
	/* Lifetime arena id (fragmentation.md §5.2). 0 = default
	 * arena, non-zero = the lifetime-class arena the dispatcher
	 * routed this page to (V8M_ARENA_EPHEMERAL / SHORT / LONG).
	 * The free path reads this to return the page to the right
	 * pool. Padding from the alignment of `next` covers the
	 * remaining 7 bytes. */
	uint8_t arena_id;
};

/*
 * Tiny slab metadata. Carries an in-page bitmap sized for headroom
 * up to 8192 slots; usable capacity is bounded by the per-page
 * metadata reservation (~7936 slots for the 8 B class). See
 * size-classes.md §3.
 */
struct v8m_tiny_page_meta {
	/* Common-prefix fields (offset-compatible with v8m_page_meta). */
	uint64_t magic;
	uint16_t size_class;
	uint16_t object_size;
	uint32_t capacity;
	atomic_uint used_count;
	uint64_t owner_thread;
	void *free_list_head; /* unused for Tiny; kept for layout parity */
	struct v8m_page_meta *next;
	uint8_t arena_id; /* same offset as v8m_page_meta.arena_id */
	/* Tiny-only fields. */
	uint64_t search_hint;
	uint64_t bitmap[128];
};

/*
 * Map any pointer to the v8m_page_meta header at the base of its
 * containing page. Branchless and inlinable; sits on every free()
 * path.
 *
 * The returned pointer is only meaningful if the page actually
 * belongs to v8malloc — callers must verify via v8m_page_meta_valid()
 * before treating any other field as trustworthy.
 */
static inline struct v8m_page_meta *v8m_ptr_to_meta(const void *ptr)
{
	uintptr_t base = (uintptr_t)ptr & V8M_PAGE_MASK;
	/* The int-to-ptr cast is intentional: the whole point of the
	 * page-alignment scheme is that the page base address IS the
	 * metadata header address. */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	return (struct v8m_page_meta *)base;
}

/*
 * Predicate: does this look like a v8malloc-managed page?
 *
 * Hot path; sits on every free(). NULL check first so that free(NULL)
 * (and stray pointers that mask to zero) never dereference.
 */
static inline bool v8m_page_meta_valid(const struct v8m_page_meta *meta)
{
	return meta != NULL && meta->magic == V8M_MAGIC;
}

#endif /* V8M_PAGE_H */
