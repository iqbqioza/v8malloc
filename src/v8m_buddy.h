/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Buddy allocator for Medium-class allocations (size classes 32..37,
 * 8 KiB through 256 KiB). Each v8m_buddy instance manages exactly
 * one 256 KiB arena handed in at init time; higher layers (the NUMA
 * pool, in a later cycle) maintain the list of arenas as demand
 * grows.
 *
 * Design: fragmentation.md §4.4.
 *
 *   - Seven levels covering 4 KiB .. 256 KiB (V8M_BUDDY_LEVELS).
 *   - Per-level doubly-linked free list whose next/prev links live
 *     inside the free block's own storage, so the allocator adds
 *     zero auxiliary memory beyond the v8m_buddy struct.
 *   - Per-level alloc_bitmap (bit set => that block is currently
 *     handed out) and split_bitmap (bit set => that block has been
 *     split into two level-below children). A block on the free
 *     list has both bits clear.
 *   - Each bitmap is one uint64_t; level 0 has at most 64 blocks
 *     (256 KiB / 4 KiB), every other level has fewer.
 *   - Coalescing is immediate on free (`idx ^ 1` to find the
 *     buddy, merge if both children are free and not split).
 *     Deferred / epoch-GC coalescing is a later optimization cycle.
 */

#ifndef V8M_BUDDY_H
#define V8M_BUDDY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define V8M_BUDDY_MIN_SHIFT 12
#define V8M_BUDDY_MAX_SHIFT 18
#define V8M_BUDDY_LEVELS 7
#define V8M_BUDDY_MIN_BLOCK (1U << V8M_BUDDY_MIN_SHIFT)
#define V8M_BUDDY_MAX_BLOCK (1U << V8M_BUDDY_MAX_SHIFT)

/*
 * Intrusive doubly-linked list node. Laid inside the first
 * sizeof(v8m_buddy_node) bytes of a free block; allocated blocks
 * overwrite that storage.
 */
struct v8m_buddy_node {
	struct v8m_buddy_node *next;
	struct v8m_buddy_node *prev;
};

struct v8m_buddy {
	unsigned char *arena_base;
	size_t arena_size;
	struct v8m_buddy_node *free_lists[V8M_BUDDY_LEVELS];
	uint64_t alloc_bitmap[V8M_BUDDY_LEVELS];
	uint64_t split_bitmap[V8M_BUDDY_LEVELS];
};

/*
 * Initialize `buddy` to manage one V8M_BUDDY_MAX_BLOCK-sized arena
 * at `arena`. `arena` must be V8M_BUDDY_MAX_BLOCK-aligned; callers
 * typically obtain it from v8m_page_heap_alloc(256 KiB, 256 KiB).
 */
void v8m_buddy_init(struct v8m_buddy *buddy, void *arena);

/*
 * Allocate a block of at least `size` bytes. The returned pointer is
 * aligned to the chosen level's block size (size rounded up to the
 * next power of two, lower-bounded by V8M_BUDDY_MIN_BLOCK). Returns
 * NULL when `size` exceeds V8M_BUDDY_MAX_BLOCK or the arena has no
 * free block at or above the required level.
 */
void *v8m_buddy_alloc(struct v8m_buddy *buddy, size_t size);

/*
 * Return a block previously issued by v8m_buddy_alloc(). `size` must
 * match the value passed to alloc (or any value that rounds to the
 * same buddy level). Tolerates ptr == NULL.
 */
void v8m_buddy_free(struct v8m_buddy *buddy, void *ptr, size_t size);

#endif /* V8M_BUDDY_H */
