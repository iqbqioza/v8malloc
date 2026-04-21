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
	return node;
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

	struct v8m_buddy_node *current = ptr;

	/* Coalesce while the buddy (idx ^ 1) is free and not split.
	 * The top level is skipped because its buddy is off the end
	 * of the arena — V8M_BUDDY_LEVELS - 1 blocks exist only as
	 * one whole-arena cell. */
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

	list_push(&buddy->free_lists[level], current);
}
