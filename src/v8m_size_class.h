/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Size-class machinery. Maps allocation request sizes to a compact
 * 0..40 class index used by every layer of the allocator. See
 * .claude/docs/size-classes.md for the design. Requests above
 * V8M_LARGE_MAX_SIZE return V8M_CLASS_HUGE and bypass the size-class
 * table entirely (routed through the mmap-direct Huge path).
 */

#ifndef V8M_SIZE_CLASS_H
#define V8M_SIZE_CLASS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* Tiny: 8B-spaced buckets in [1, 64], classes 0..7. */
	V8M_TINY_MAX_SIZE = 64,
	V8M_TINY_ALIGNMENT = 8,
	V8M_TINY_ROUND_UP_MASK = V8M_TINY_ALIGNMENT - 1,
	V8M_TINY_SHIFT = 3,

	/* Small: 4 subclasses per power-of-two group, classes 8..31. */
	V8M_SMALL_MAX_SIZE = 4096,
	V8M_SMALL_FIRST_CLASS = 8,
	V8M_SMALL_MSB_BASE = 6, /* 2^6 = 64, the top of Tiny */
	V8M_SMALL_SUBCLASS_BITS = 2,
	V8M_SMALL_SUBCLASS_MASK = (1 << V8M_SMALL_SUBCLASS_BITS) - 1,

	/* Medium: one class per power-of-two, classes 32..37. Large
	 * (classes 38..40) extends the same formula. */
	V8M_MEDIUM_FIRST_CLASS = 32,
	V8M_MEDIUM_MSB_BASE = 12, /* 2^12 = 4096, the top of Small */

	V8M_LARGE_LAST_CLASS = 40,
	V8M_LARGE_MAX_SIZE = 2 * 1024 * 1024, /* 2 MiB */

	V8M_NUM_SIZE_CLASSES = 41
};

/*
 * Sentinel returned by v8m_size_class() when the request size exceeds
 * V8M_LARGE_MAX_SIZE. Callers must branch on this and route to the
 * mmap-direct Huge path before indexing v8m_class_to_size[].
 */
#define V8M_CLASS_HUGE UINT32_MAX

/*
 * Exact byte size of each size class — the v0 baseline table.
 * Direct indexing is supported but does NOT honour the hot-reload
 * override below; consumers that need the live size should call
 * `v8m_size_class_size(cls)` instead. Must not be indexed with
 * V8M_CLASS_HUGE.
 */
extern const uint32_t v8m_class_to_size[V8M_NUM_SIZE_CLASSES];

/*
 * Active class-size table — initialized to point at
 * `v8m_class_to_size` and swappable at runtime via
 * `v8m_size_class_install_table`. Allocations after a swap format
 * new slab pages with the new sizes; live pages retain the size
 * baked into their `meta->object_size` at init time so existing
 * allocations stay safe (the hot-reload is per-spec a forward
 * transition only — old pages "leave as-is").
 */
extern _Atomic(const uint32_t *) v8m_active_class_table;

/*
 * Resolve the live byte size for `cls`. Reads the active table
 * with relaxed memory order — stale reads are safe because each
 * slab page records its own object_size at init time, so a swap
 * never reaches across already-allocated pages.
 */
static inline uint32_t v8m_size_class_size(uint32_t cls)
{
	const uint32_t *table =
	    atomic_load_explicit(&v8m_active_class_table, memory_order_relaxed);
	return table[cls];
}

/*
 * Hot-reload entry point. Atomically swaps the active table to
 * `new_table`. The pointer must remain valid for the lifetime of
 * the process — the size class machinery does not copy the table.
 * Pass `NULL` to revert to the v0 baseline. Returns the previously
 * active table so callers can chain.
 */
const uint32_t *v8m_size_class_install_table(const uint32_t *new_table);

/*
 * Map a request size to the smallest size class whose byte size is
 * >= the request. Hot path; branchless on the Small subpath.
 */
static inline uint32_t v8m_size_class(size_t req_size)
{
	/* Tiny: 8B-spaced buckets. Coerce size==0 to 1 so the unsigned
	 * arithmetic below never underflows into UINT32_MAX. */
	if (req_size <= V8M_TINY_MAX_SIZE) {
		size_t coerced = req_size + (size_t)(req_size == 0U);
		return (uint32_t)(((coerced + V8M_TINY_ROUND_UP_MASK) >>
				   V8M_TINY_SHIFT) -
				  1U);
	}

	/* Medium / Large / Huge: one class per power-of-two. */
	if (req_size > V8M_SMALL_MAX_SIZE) {
		size_t adjusted = req_size - 1U;
		uint32_t msb = (uint32_t)(63 - __builtin_clzl(adjusted));
		uint32_t cls =
		    V8M_MEDIUM_FIRST_CLASS + (msb - V8M_MEDIUM_MSB_BASE);
		if (cls > V8M_LARGE_LAST_CLASS) {
			return V8M_CLASS_HUGE;
		}
		return cls;
	}

	/* Small: 4 subclasses per power-of-two group. (req_size - 1)
	 * ensures boundary values (80, 96, 128, ...) land in the
	 * smaller class. */
	size_t adjusted = req_size - 1U;
	uint32_t msb = (uint32_t)(63 - __builtin_clzl(adjusted));
	uint32_t group = msb - V8M_SMALL_MSB_BASE;
	uint32_t sub =
	    (uint32_t)((adjusted >> (msb - V8M_SMALL_SUBCLASS_BITS)) &
		       V8M_SMALL_SUBCLASS_MASK);
	return V8M_SMALL_FIRST_CLASS + (group << V8M_SMALL_SUBCLASS_BITS) + sub;
}

#endif /* V8M_SIZE_CLASS_H */
