/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Buddy allocator implementation. Immediate-coalescing design; see
 * fragmentation.md §4.4 for the invariants. Every level's alloc and
 * split bitmaps fit in one uint64_t because the top-level arena is
 * capped at 256 KiB (64 level-0 blocks of 4 KiB).
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_buddy.h"
#include "v8m_debug.h"

static_assert(V8M_BUDDY_LEVELS ==
		  (V8M_BUDDY_MAX_SHIFT - V8M_BUDDY_MIN_SHIFT + 1),
	      "V8M_BUDDY_LEVELS must span min..max shifts");
static_assert(V8M_BUDDY_MAX_BLOCK / V8M_BUDDY_MIN_BLOCK <= 64,
	      "level-0 block count must fit a uint64_t bitmap");

static uint32_t size_to_level(size_t size)
{
	if (size <= V8M_BUDDY_MIN_BLOCK) {
		return 0;
	}
	size_t rounded = size - 1U;
	uint32_t msb =
	    63U - (uint32_t)__builtin_clzll((unsigned long long)rounded);
	uint32_t shift = msb + 1U;
	if (shift < V8M_BUDDY_MIN_SHIFT) {
		return 0;
	}
	return shift - V8M_BUDDY_MIN_SHIFT;
}

static size_t level_to_size(uint32_t level)
{
	return (size_t)1U << (V8M_BUDDY_MIN_SHIFT + level);
}

static uint32_t addr_to_index(const struct v8m_buddy *buddy, const void *ptr,
			      uint32_t level)
{
	uintptr_t offset = (uintptr_t)ptr - (uintptr_t)buddy->arena_base;
	return (uint32_t)(offset >> (V8M_BUDDY_MIN_SHIFT + level));
}

static struct v8m_buddy_node *index_to_node(const struct v8m_buddy *buddy,
					    uint32_t idx, uint32_t level)
{
	unsigned char *base =
	    buddy->arena_base + ((size_t)idx * level_to_size(level));
	/* Buddy blocks are at offsets that are multiples of at least
	 * V8M_BUDDY_MIN_BLOCK (4 KiB) from a V8M_BUDDY_MAX_BLOCK-aligned
	 * arena base, so the node's 8-byte alignment is honoured in
	 * practice. The void* intermediate is how C idiomatically
	 * re-expresses the alignment assertion; the tidy check that
	 * flags casting-through-void is aware of the common workaround
	 * and we suppress it locally. */
	/* NOLINTNEXTLINE(bugprone-casting-through-void) */
	return (struct v8m_buddy_node *)(void *)base;
}

static void list_push(struct v8m_buddy_node **head, struct v8m_buddy_node *node)
{
	node->prev = NULL;
	node->next = *head;
	if (*head != NULL) {
		(*head)->prev = node;
	}
	*head = node;
}

static struct v8m_buddy_node *list_pop(struct v8m_buddy_node **head)
{
	struct v8m_buddy_node *node = *head;
	if (node != NULL) {
		*head = node->next;
		if (*head != NULL) {
			(*head)->prev = NULL;
		}
	}
	return node;
}

static void list_remove(struct v8m_buddy_node **head,
			struct v8m_buddy_node *node)
{
	if (node->prev != NULL) {
		node->prev->next = node->next;
	} else {
		*head = node->next;
	}
	if (node->next != NULL) {
		node->next->prev = node->prev;
	}
}

void v8m_buddy_init(struct v8m_buddy *buddy, void *arena)
{
	buddy->arena_base = arena;
	buddy->arena_size = V8M_BUDDY_MAX_BLOCK;
	for (uint32_t i = 0; i < V8M_BUDDY_LEVELS; i++) {
		buddy->free_lists[i] = NULL;
		buddy->alloc_bitmap[i] = 0;
		buddy->split_bitmap[i] = 0;
	}
	list_push(&buddy->free_lists[V8M_BUDDY_LEVELS - 1U], arena);
	/* DEBUG-mode UAF detector: pre-poison the arena's body so the
	 * first-alloc verify sees the expected pattern. The first
	 * sizeof(struct v8m_buddy_node) bytes hold the top-level
	 * free-list head's prev/next and are excluded from the poison
	 * window. The helper is a no-op when V8M_OPT_DEBUG is 0. */
	v8m_debug_uaf_poison(
	    (unsigned char *)arena + sizeof(struct v8m_buddy_node),
	    V8M_BUDDY_MAX_BLOCK - sizeof(struct v8m_buddy_node));
}

void *v8m_buddy_alloc(struct v8m_buddy *buddy, size_t size)
{
	if (size == 0) {
		return NULL;
	}
	uint32_t target = size_to_level(size);
	if (target >= V8M_BUDDY_LEVELS) {
		return NULL;
	}

	uint32_t level = target;
	while (level < V8M_BUDDY_LEVELS && buddy->free_lists[level] == NULL) {
		level++;
	}
	if (level >= V8M_BUDDY_LEVELS) {
		return NULL;
	}

	struct v8m_buddy_node *node = list_pop(&buddy->free_lists[level]);

	/* Split down: at each iteration mark the current block split,
	 * descend to the lower level, and place the right child on
	 * that level's free list. The left child stays as `node`; its
	 * index at the new level is parent_idx * 2. */
	while (level > target) {
		uint32_t parent_idx = addr_to_index(buddy, node, level);
		buddy->split_bitmap[level] |= (uint64_t)1U << parent_idx;

		level--;
		struct v8m_buddy_node *right =
		    index_to_node(buddy, (parent_idx * 2U) + 1U, level);
		list_push(&buddy->free_lists[level], right);
	}

	uint32_t idx = addr_to_index(buddy, node, target);
	buddy->alloc_bitmap[target] |= (uint64_t)1U << idx;
	/* DEBUG-mode UAF detector: verify the body poison left by the
	 * prior free (or by the init-time pre-poison for the first
	 * alloc on this address range) is intact. The first
	 * sizeof(struct v8m_buddy_node) bytes were prev/next we just
	 * consumed and are not part of the verify window. No-op in
	 * release builds. */
	size_t target_size = level_to_size(target);
	if (target_size > sizeof(struct v8m_buddy_node)) {
		v8m_debug_uaf_verify(
		    (unsigned char *)node + sizeof(struct v8m_buddy_node),
		    target_size - sizeof(struct v8m_buddy_node), "buddy");
	}
	return node;
}

/*
 * Coalesce a freshly-freed block at (start_block, start_level)
 * with its buddy and propagate upward as long as buddies are free
 * and unsplit. The block is assumed to be ALREADY off every free
 * list (the caller is mid-free and has not yet pushed it). On
 * return the merged block is on the appropriate free list at the
 * highest level reached. Returns the level the block landed at —
 * useful for the on-demand coalesce sweep that wants to know if
 * any merge occurred.
 */
static uint32_t coalesce_upward(struct v8m_buddy *buddy,
				struct v8m_buddy_node *start_block,
				uint32_t start_level)
{
	struct v8m_buddy_node *current = start_block;
	uint32_t level = start_level;
	uint32_t idx = addr_to_index(buddy, current, level);

	while (level < V8M_BUDDY_LEVELS - 1U) {
		uint32_t buddy_idx = idx ^ 1U;
		uint64_t buddy_bit = (uint64_t)1U << buddy_idx;

		if ((buddy->alloc_bitmap[level] & buddy_bit) != 0U) {
			break;
		}
		if ((buddy->split_bitmap[level] & buddy_bit) != 0U) {
			break;
		}

		struct v8m_buddy_node *sibling =
		    index_to_node(buddy, buddy_idx, level);
		list_remove(&buddy->free_lists[level], sibling);

		if (buddy_idx < idx) {
			current = sibling;
		}
		level++;
		idx >>= 1;
		buddy->split_bitmap[level] &= ~((uint64_t)1U << idx);
	}
	/* DEBUG-mode UAF poison: stamp the entire merged block with the
	 * poison pattern before pushing back onto the free list. The
	 * subsequent list_push overwrites the first sizeof(struct
	 * v8m_buddy_node) bytes with prev/next, leaving the body
	 * uniformly poisoned. The full-block stamp also covers the
	 * sibling's old prev/next bytes that would otherwise create a
	 * non-poison gap inside the merged block. No-op in release
	 * builds. */
	v8m_debug_uaf_poison(current, level_to_size(level));
	list_push(&buddy->free_lists[level], current);
	return level;
}

void v8m_buddy_free(struct v8m_buddy *buddy, void *ptr, size_t size)
{
	if (ptr == NULL) {
		return;
	}
	uint32_t level = size_to_level(size);
	if (level >= V8M_BUDDY_LEVELS) {
		return;
	}

	uint32_t idx = addr_to_index(buddy, ptr, level);
	buddy->alloc_bitmap[level] &= ~((uint64_t)1U << idx);

	(void)coalesce_upward(buddy, ptr, level);
}

void v8m_buddy_free_no_coalesce(struct v8m_buddy *buddy, void *ptr, size_t size)
{
	if (ptr == NULL) {
		return;
	}
	uint32_t level = size_to_level(size);
	if (level >= V8M_BUDDY_LEVELS) {
		return;
	}
	uint32_t idx = addr_to_index(buddy, ptr, level);
	buddy->alloc_bitmap[level] &= ~((uint64_t)1U << idx);
	/* Poison the freed block before installing it on the free list;
	 * list_push overwrites the first sizeof(struct v8m_buddy_node)
	 * bytes with prev/next. No-op in release builds. */
	v8m_debug_uaf_poison(ptr, level_to_size(level));
	list_push(&buddy->free_lists[level], ptr);
}

size_t v8m_buddy_coalesce_all(struct v8m_buddy *buddy)
{
	size_t merges = 0;
	/* Walk levels from low to high — every merge promotes a
	 * block to a higher level, so a single bottom-up pass
	 * fully drains all coalescable pairs. */
	for (uint32_t level = 0; level < V8M_BUDDY_LEVELS - 1U; level++) {
		struct v8m_buddy_node *node = buddy->free_lists[level];
		while (node != NULL) {
			struct v8m_buddy_node *next = node->next;
			uint32_t idx = addr_to_index(buddy, node, level);
			uint32_t buddy_idx = idx ^ 1U;
			uint64_t buddy_bit = (uint64_t)1U << buddy_idx;

			if ((buddy->alloc_bitmap[level] & buddy_bit) == 0U &&
			    (buddy->split_bitmap[level] & buddy_bit) == 0U) {
				/* Buddy is free + not split — merge.
				 * Pull `node` off this level (it'll be
				 * re-pushed as part of the merged block
				 * at a higher level by coalesce_upward). */
				list_remove(&buddy->free_lists[level], node);
				(void)coalesce_upward(buddy, node, level);
				merges++;
				/* `next` may have been the sibling we
				 * just merged away — restart the walk
				 * at the (possibly new) head. */
				node = buddy->free_lists[level];
			} else {
				node = next;
			}
		}
	}
	return merges;
}

size_t v8m_buddy_block_size(const struct v8m_buddy *buddy, const void *ptr)
{
	if (ptr == NULL) {
		return 0;
	}
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t base = (uintptr_t)buddy->arena_base;
	if (addr < base || addr >= base + buddy->arena_size) {
		return 0;
	}
	uintptr_t offset = addr - base;
	for (uint32_t level = 0; level < V8M_BUDDY_LEVELS; level++) {
		uint32_t idx =
		    (uint32_t)(offset >> (V8M_BUDDY_MIN_SHIFT + level));
		if ((buddy->alloc_bitmap[level] & ((uint64_t)1U << idx)) !=
		    0U) {
			return level_to_size(level);
		}
	}
	return 0;
}

bool v8m_buddy_is_empty(const struct v8m_buddy *buddy)
{
	for (uint32_t i = 0; i < V8M_BUDDY_LEVELS; i++) {
		if (buddy->alloc_bitmap[i] != 0U) {
			return false;
		}
	}
	return true;
}
