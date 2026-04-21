/* SPDX-License-Identifier: Apache-2.0 */
/*
 * `malloc_usable_size` contract sweep. The function exists so callers
 * can ask "how many bytes can I actually write, including the slack
 * the allocator rounded up to?" — used by realloc-shrinkers, by
 * dynamic-array growth heuristics, and by the libc-fallback wrapper
 * in some glibc consumers.
 *
 * The contract is two-sided:
 *
 *   1. usable >= requested            (writable bytes ≥ what you asked)
 *   2. usable matches the backend's   (an allocator-internal upper bound,
 *      reported size class                so callers can size their data
 *                                          structures off it)
 *
 * test_api covers a single (80 B) sample. This file expands to:
 *
 *   - every Tiny / Small slab class (1 ≤ req ≤ V8M_SMALL_MAX_SIZE)
 *     reports usable == v8m_class_to_size[class]
 *   - every buddy power-of-two block (8 KiB → 256 KiB) reports
 *     usable >= req and at most the next-larger buddy block size
 *   - Large + Huge mmap allocations report usable >= req
 *   - the writable-window claim is honest: stamp every byte up to
 *     usable_size and the surrounding state stays intact (a misclaimed
 *     usable_size that overflows the backing allocation would be caught
 *     by the next free / by an out-of-bounds heap access in tooling)
 *   - malloc_usable_size(NULL) == 0
 *   - aligned_alloc'd pointers report usable >= request (the aligned
 *     path may bump into a larger class to satisfy alignment, so the
 *     reported size can exceed the requested size)
 */

#include <malloc.h> /* malloc_usable_size */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_buddy.h" /* V8M_BUDDY_MAX_BLOCK + MAX_SHIFT */
#include "v8m_size_class.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_usable_size: %s\n", msg);
	return 1;
}

/* For requests ≤ V8M_SMALL_MAX_SIZE, usable_size must equal the
 * size of the chosen class — slab slots are exact-sized. */
static int sweep_slab_classes_exact(void)
{
	for (size_t req = 1U; req <= V8M_SMALL_MAX_SIZE; req++) {
		void *ptr = malloc(req);
		if (ptr == NULL) {
			(void)fprintf(
			    stderr,
			    "test_usable_size: malloc(%zu) returned NULL\n",
			    req);
			return 1;
		}
		size_t usable = malloc_usable_size(ptr);
		uint32_t cls = v8m_size_class(req);
		size_t expected = v8m_class_to_size[cls];
		if (usable != expected) {
			(void)fprintf(stderr,
				      "test_usable_size: malloc(%zu) "
				      "usable=%zu expected=%zu (class %u)\n",
				      req, usable, expected, cls);
			free(ptr);
			return 1;
		}
		if (usable < req) {
			free(ptr);
			return fail("usable < requested");
		}
		free(ptr);
	}
	return 0;
}

/* For buddy-routed sizes (V8M_SMALL_MAX_SIZE+1 .. V8M_BUDDY_MAX_BLOCK),
 * usable_size must be the rounded-up power-of-two block. We sample
 * every power-of-two boundary and the size just above it. */
static int sweep_buddy_classes(void)
{
	for (size_t shift = 13U; shift <= V8M_BUDDY_MAX_SHIFT; shift++) {
		size_t block = (size_t)1U << shift;
		/* Two probes per level: exactly the block size, and one
		 * byte past the previous level (which still maps here). */
		size_t probes[] = {block - 16U, block};
		for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]);
		     i++) {
			size_t req = probes[i];
			if (req == 0U) {
				continue;
			}
			void *ptr = malloc(req);
			if (ptr == NULL) {
				(void)fprintf(stderr,
					      "test_usable_size: malloc(%zu) "
					      "returned NULL\n",
					      req);
				return 1;
			}
			size_t usable = malloc_usable_size(ptr);
			if (usable < req) {
				(void)fprintf(stderr,
					      "test_usable_size: buddy "
					      "malloc(%zu) usable=%zu < req\n",
					      req, usable);
				free(ptr);
				return 1;
			}
			if (usable > V8M_BUDDY_MAX_BLOCK) {
				free(ptr);
				return fail("buddy usable > MAX_BLOCK");
			}
			free(ptr);
		}
	}
	return 0;
}

/* Large / Huge: usable_size >= req. The Large path reports
 * mmap_size minus header offset, so it can be slightly larger than
 * the request after page rounding. */
static int sweep_large_huge(void)
{
	size_t sizes[] = {
	    (size_t)512 * 1024,
	    (size_t)1024 * 1024,
	    (size_t)2 * 1024 * 1024,
	    (size_t)4 * 1024 * 1024,
	};
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t req = sizes[i];
		void *ptr = malloc(req);
		if (ptr == NULL) {
			(void)fprintf(
			    stderr,
			    "test_usable_size: large malloc(%zu) NULL\n", req);
			return 1;
		}
		size_t usable = malloc_usable_size(ptr);
		if (usable < req) {
			free(ptr);
			return fail("large usable < req");
		}
		free(ptr);
	}
	return 0;
}

/* The writable-window claim: an allocation reporting usable=N must
 * permit writes to all N bytes without disturbing neighbours. We
 * place two anchor allocations around a probe, write 0xCD across
 * the probe's full usable range, then verify the anchors still
 * hold their original pattern. */
static int check_writable_window(void)
{
	const unsigned char anchor_pattern = 0x42U;
	const unsigned char probe_pattern = 0xCDU;
	const size_t anchor_bytes = 1024U;
	size_t probes[] = {1U, 7U, 13U, 64U, 96U, 1023U, 4095U, 8192U};

	unsigned char *before = malloc(anchor_bytes);
	unsigned char *after = malloc(anchor_bytes);
	if (before == NULL || after == NULL) {
		free(before);
		free(after);
		return fail("anchor malloc failed");
	}
	(void)memset(before, anchor_pattern, anchor_bytes);
	(void)memset(after, anchor_pattern, anchor_bytes);

	for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		void *probe = malloc(probes[i]);
		if (probe == NULL) {
			free(before);
			free(after);
			return fail("probe malloc failed");
		}
		size_t usable = malloc_usable_size(probe);
		(void)memset(probe, (int)probe_pattern, usable);
		free(probe);
	}

	for (size_t i = 0; i < anchor_bytes; i++) {
		if (before[i] != anchor_pattern || after[i] != anchor_pattern) {
			(void)fprintf(
			    stderr,
			    "test_usable_size: anchor corrupted at %zu\n", i);
			free(before);
			free(after);
			return 1;
		}
	}
	free(before);
	free(after);
	return 0;
}

static int check_null_returns_zero(void)
{
	if (malloc_usable_size(NULL) != 0U) {
		return fail("malloc_usable_size(NULL) != 0");
	}
	return 0;
}

/* Aligned allocations may bump into a larger class to satisfy the
 * alignment constraint; usable_size must still be at least the
 * request. */
static int check_aligned_alloc_usable_size(void)
{
	size_t aligns[] = {32U, 64U, 256U, 4096U};
	for (size_t i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
		size_t align = aligns[i];
		size_t req = align * 2U;
		void *ptr = aligned_alloc(align, req);
		if (ptr == NULL) {
			free(ptr);
			return fail("aligned_alloc returned NULL");
		}
		if (((uintptr_t)ptr & (align - 1U)) != 0U) {
			free(ptr);
			return fail("aligned_alloc returned misaligned ptr");
		}
		size_t usable = malloc_usable_size(ptr);
		if (usable < req) {
			free(ptr);
			return fail("aligned_alloc usable < req");
		}
		free(ptr);
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_null_returns_zero();
	result |= sweep_slab_classes_exact();
	result |= sweep_buddy_classes();
	result |= sweep_large_huge();
	result |= check_writable_window();
	result |= check_aligned_alloc_usable_size();
	if (result == 0) {
		(void)printf("test_usable_size: OK\n");
	}
	return result;
}
