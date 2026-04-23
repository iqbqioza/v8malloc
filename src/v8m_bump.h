/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Atomic bump-pointer primitive shared by the bootstrap allocator
 * (v8m_bootstrap.c) and the async-signal-safe emergency allocator
 * (v8m_signal_safe.c). Both modules need the same shape — a
 * fixed-size BSS buffer, an atomic offset, range-check ownership,
 * 16-byte alignment of returned pointers — but differ in their
 * out-of-budget behaviour: bootstrap aborts (process-fatal is
 * correct before the real allocator exists) and signal-safe
 * returns NULL (handler must check). Hoisting the shared mechanics
 * here keeps the two callers from drifting (a previous size-overflow
 * fix had to land in two places because the same code was copied).
 */

#ifndef V8M_BUMP_H
#define V8M_BUMP_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

struct v8m_bump {
	unsigned char *buffer;
	size_t size;
	size_t align;
	_Atomic size_t offset;
};

/*
 * Allocate `request` bytes. Returns NULL on out-of-budget OR on a
 * size that cannot fit in the buffer (rejected up front so a near-
 * SIZE_MAX request cannot wrap to a small `aligned` value and hand
 * the caller a tiny slot for a huge promise). Pointers are aligned
 * to `bump->align`. The bump pointer never rewinds — once exhausted,
 * subsequent allocations also return NULL.
 */
void *v8m_bump_alloc(struct v8m_bump *bump, size_t request);

/*
 * True iff `ptr` lies in the bump buffer's address range. Lock-free.
 */
bool v8m_bump_owns(const struct v8m_bump *bump, const void *ptr);

/*
 * Bytes from `ptr` to the end of the buffer — safe upper bound on
 * what realloc may read when copying out of a bump allocation
 * (per-allocation sizes are not tracked). Returns 0 if `ptr` is
 * outside the buffer.
 */
size_t v8m_bump_remaining(const struct v8m_bump *bump, const void *ptr);

#endif /* V8M_BUMP_H */
