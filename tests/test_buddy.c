/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Buddy allocator tests. Covers init state, single alloc/free round
 * trip (and the resulting full coalesce back to the top level),
 * exhaustion by repeated same-size allocation, size rounding up to
 * the next level, natural alignment of returned pointers, and the
 * invalid-input branches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "v8m_buddy.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_buddy: %s\n", msg);
	return 1;
}

enum {
	/* 256 KiB arena holds 32 blocks of 8 KiB — exhaustion test
	 * allocates that many. */
	EXHAUSTION_COUNT = 32
};

static void *alloc_arena(void)
{
	return aligned_alloc(V8M_BUDDY_MAX_BLOCK, V8M_BUDDY_MAX_BLOCK);
}

static uint32_t count_free(const struct v8m_buddy *buddy, uint32_t level)
{
	uint32_t count = 0;
	for (const struct v8m_buddy_node *node = buddy->free_lists[level];
	     node != NULL; node = node->next) {
		count++;
	}
	return count;
}

static bool is_fully_coalesced(const struct v8m_buddy *buddy)
{
	for (uint32_t i = 0; i < V8M_BUDDY_LEVELS - 1U; i++) {
		if (count_free(buddy, i) != 0U) {
			return false;
		}
	}
	return count_free(buddy, V8M_BUDDY_LEVELS - 1U) == 1U;
}

static int check_init_state(void)
{
	void *arena = alloc_arena();
	if (arena == NULL) {
		return fail("aligned_alloc failed (init)");
	}
	struct v8m_buddy buddy;
	v8m_buddy_init(&buddy, arena);

	if (!is_fully_coalesced(&buddy)) {
		free(arena);
		return fail("fresh buddy is not fully coalesced");
	}

	free(arena);
	return 0;
}

static int check_single_alloc_free(void)
{
	void *arena = alloc_arena();
	if (arena == NULL) {
		return fail("aligned_alloc failed (single)");
	}
	struct v8m_buddy buddy;
	v8m_buddy_init(&buddy, arena);

	void *ptr = v8m_buddy_alloc(&buddy, 8192);
	if (ptr == NULL) {
		free(arena);
		return fail("alloc(8 KiB) returned NULL on fresh arena");
	}
	if ((uintptr_t)ptr % 8192U != 0U) {
		free(arena);
		return fail("8 KiB alloc is not 8 KiB-aligned");
	}
	if ((uintptr_t)ptr < (uintptr_t)arena ||
	    (uintptr_t)ptr >= (uintptr_t)arena + V8M_BUDDY_MAX_BLOCK) {
		free(arena);
		return fail("alloc returned a pointer outside the arena");
	}

	v8m_buddy_free(&buddy, ptr, 8192);
	if (!is_fully_coalesced(&buddy)) {
		free(arena);
		return fail(
		    "buddy not fully coalesced after single alloc/free");
	}

	free(arena);
	return 0;
}

static int check_exhaustion(void)
{
	void *arena = alloc_arena();
	if (arena == NULL) {
		return fail("aligned_alloc failed (exhaustion)");
	}
	struct v8m_buddy buddy;
	v8m_buddy_init(&buddy, arena);

	uintptr_t ptrs[EXHAUSTION_COUNT];
	for (int i = 0; i < EXHAUSTION_COUNT; i++) {
		void *obj = v8m_buddy_alloc(&buddy, 8192);
		if (obj == NULL) {
			free(arena);
			return fail("alloc failed before arena exhaustion");
		}
		if ((uintptr_t)obj % 8192U != 0U) {
			free(arena);
			return fail("mid-sequence pointer not 8 KiB-aligned");
		}
		ptrs[i] = (uintptr_t)obj;
	}

	if (v8m_buddy_alloc(&buddy, 8192) != NULL) {
		free(arena);
		return fail("alloc succeeded past arena exhaustion");
	}

	/* All 32 issued pointers are pairwise distinct, separated by
	 * at least 8 KiB. */
	for (int i = 0; i < EXHAUSTION_COUNT; i++) {
		for (int j = i + 1; j < EXHAUSTION_COUNT; j++) {
			uintptr_t diff = (ptrs[i] > ptrs[j])
					     ? (ptrs[i] - ptrs[j])
					     : (ptrs[j] - ptrs[i]);
			if (diff < 8192U) {
				free(arena);
				return fail("two 8 KiB allocations overlap");
			}
		}
	}

	for (int i = 0; i < EXHAUSTION_COUNT; i++) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		void *obj = (void *)ptrs[i];
		v8m_buddy_free(&buddy, obj, 8192);
	}
	if (!is_fully_coalesced(&buddy)) {
		free(arena);
		return fail("arena not coalesced after full free");
	}

	free(arena);
	return 0;
}

static int check_size_rounding(void)
{
	void *arena = alloc_arena();
	if (arena == NULL) {
		return fail("aligned_alloc failed (rounding)");
	}
	struct v8m_buddy buddy;
	v8m_buddy_init(&buddy, arena);

	/* Non-power-of-two size rounds up to the next level. */
	void *rounded_up = v8m_buddy_alloc(&buddy, 5000);
	if (rounded_up == NULL) {
		free(arena);
		return fail("alloc(5000) returned NULL");
	}
	if ((uintptr_t)rounded_up % 8192U != 0U) {
		free(arena);
		return fail("alloc(5000) pointer not 8 KiB-aligned");
	}

	/* Minimum block is 4 KiB. */
	void *min_block = v8m_buddy_alloc(&buddy, V8M_BUDDY_MIN_BLOCK);
	if (min_block == NULL) {
		free(arena);
		return fail("alloc(4 KiB) returned NULL");
	}
	if ((uintptr_t)min_block % V8M_BUDDY_MIN_BLOCK != 0U) {
		free(arena);
		return fail("alloc(4 KiB) pointer not 4 KiB-aligned");
	}

	/* Max block is 256 KiB — allocate what we can (after the two
	 * small allocations above, at most one level-5 block is left). */
	v8m_buddy_free(&buddy, rounded_up, 5000);
	v8m_buddy_free(&buddy, min_block, V8M_BUDDY_MIN_BLOCK);
	void *max_block = v8m_buddy_alloc(&buddy, V8M_BUDDY_MAX_BLOCK);
	if (max_block == NULL) {
		free(arena);
		return fail("alloc(256 KiB) returned NULL after drain");
	}
	if ((uintptr_t)max_block % V8M_BUDDY_MAX_BLOCK != 0U) {
		free(arena);
		return fail("alloc(256 KiB) pointer not 256 KiB-aligned");
	}
	v8m_buddy_free(&buddy, max_block, V8M_BUDDY_MAX_BLOCK);

	if (!is_fully_coalesced(&buddy)) {
		free(arena);
		return fail("arena not coalesced after rounding test");
	}

	free(arena);
	return 0;
}

static int check_invalid_inputs(void)
{
	void *arena = alloc_arena();
	if (arena == NULL) {
		return fail("aligned_alloc failed (invalid)");
	}
	struct v8m_buddy buddy;
	v8m_buddy_init(&buddy, arena);

	if (v8m_buddy_alloc(&buddy, 0) != NULL) {
		free(arena);
		return fail("alloc(0) did not return NULL");
	}
	if (v8m_buddy_alloc(&buddy, V8M_BUDDY_MAX_BLOCK + 1U) != NULL) {
		free(arena);
		return fail("alloc above max block size did not return NULL");
	}
	/* NULL free is tolerated. */
	v8m_buddy_free(&buddy, NULL, 8192);

	free(arena);
	return 0;
}

int main(void)
{
	int status = check_init_state();
	if (status != 0) {
		return status;
	}
	status = check_single_alloc_free();
	if (status != 0) {
		return status;
	}
	status = check_exhaustion();
	if (status != 0) {
		return status;
	}
	status = check_size_rounding();
	if (status != 0) {
		return status;
	}
	return check_invalid_inputs();
}
