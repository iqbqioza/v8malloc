/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bump-pointer primitive — implementation. See the matching header
 * for the design notes.
 */

#include "v8m_bump.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *v8m_bump_alloc(struct v8m_bump *bump, size_t request)
{
	if (__builtin_expect(
		bump == NULL || bump->buffer == NULL || bump->align == 0U, 0)) {
		return NULL;
	}
	/* Reject any size that cannot fit in the buffer before the round-
	 * up, so a near-SIZE_MAX request cannot wrap to a small `aligned`
	 * and silently hand the caller a tiny slot for a huge promise. */
	if (__builtin_expect(request > bump->size, 0)) {
		return NULL;
	}
	size_t aligned =
	    (request + (bump->align - 1U)) & ~(size_t)(bump->align - 1U);
	if (__builtin_expect(aligned == 0U, 0)) {
		aligned = bump->align;
	}

	size_t off = atomic_fetch_add_explicit(&bump->offset, aligned,
					       memory_order_relaxed);
	if (__builtin_expect(off > bump->size || aligned > bump->size - off,
			     0)) {
		return NULL;
	}
	return &bump->buffer[off];
}

bool v8m_bump_owns(const struct v8m_bump *bump, const void *ptr)
{
	if (__builtin_expect(bump == NULL || bump->buffer == NULL, 0)) {
		return false;
	}
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t start = (uintptr_t)bump->buffer;
	return addr >= start && addr < start + bump->size;
}

size_t v8m_bump_remaining(const struct v8m_bump *bump, const void *ptr)
{
	if (!v8m_bump_owns(bump, ptr)) {
		return 0U;
	}
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t end = (uintptr_t)bump->buffer + bump->size;
	return (size_t)(end - addr);
}
