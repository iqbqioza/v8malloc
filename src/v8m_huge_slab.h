/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Huge-page slab carve (huge-pages.md §4.2). A `struct v8m_huge_slab`
 * describes one huge page (V8M_HUGE_PAGE_SIZE bytes — 2 MiB on every
 * Tier 1/2 arch, 1 MiB on s390x) sliced into V8M_PAGE_SIZE-sized
 * (64 KiB) slab pages. The 32-bit bitmap tracks per-slot occupancy
 * (bit set = in use); on every supported arch the slab count divides
 * the bitmap exactly (32 slots on default arches, 16 slots on s390x).
 *
 * **Why**: a single huge page maps to a single TLB entry, so as long
 * as allocations land within the same huge page the hardware never
 * incurs a TLB miss between adjacent slabs. The future per-NUMA
 * HugePage pool (TODO P0 row 88) instances one of these per
 * MAP_HUGETLB allocation it has on hand and routes 64 KiB slab-page
 * requests through the bitmap allocator below; once a slab empties
 * (`v8m_huge_slab_is_empty()` becomes true), the pool returns the
 * underlying huge page to the OS as a single munmap.
 *
 * **v0 status**: this is the standalone primitive. The per-NUMA
 * HugePage pool (the consumer of this struct) is the multi-cycle
 * piece that lands separately. Shipping the primitive on its own
 * now means the future pool cycle can wire up against a tested,
 * frozen API rather than implementing the carve and the pool in
 * one go.
 */

#ifndef V8M_HUGE_SLAB_H
#define V8M_HUGE_SLAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_arch.h" /* V8M_HUGE_PAGE_SIZE */
#include "v8m_internal.h" /* V8M_PAGE_SIZE */

/*
 * Slabs per huge page. Per-arch by construction: V8M_HUGE_PAGE_SIZE
 * is 2 MiB on x86_64 / aarch64 / riscv64 / ppc64le / loongarch64 and
 * 1 MiB on s390x; V8M_PAGE_SIZE is fixed at 64 KiB. The division
 * yields 32 (default) or 16 (s390x), exactly matching the spec
 * table at huge-pages.md §4.2.
 */
#define V8M_HUGE_SLABS_PER_HUGE ((uint8_t)(V8M_HUGE_PAGE_SIZE / V8M_PAGE_SIZE))

/*
 * Cap the bitmap at 32 slots so a single uint32_t covers it on
 * every supported arch. The static_assert below makes the build
 * fail loudly if a future arch widens V8M_HUGE_PAGE_SIZE past the
 * point this representation can serve.
 */
_Static_assert(V8M_HUGE_SLABS_PER_HUGE <= 32U,
	       "huge slab bitmap must fit in 32 bits");
_Static_assert(V8M_HUGE_SLABS_PER_HUGE >= 1U,
	       "huge slab must hold at least one slab page");

/*
 * Per-huge-page descriptor. The pool owns one of these per huge page
 * it has carved; it is meant to be embedded in the pool's per-node
 * arena structure (so `next` links peer huge pages on the same
 * partial / full / empty list). Holding it here in its own struct
 * keeps the bitmap-allocator surface clean.
 *
 * Concurrency: the descriptor itself is single-threaded — the
 * caller (the future pool) is expected to hold its per-node lock
 * across alloc/free. The struct does not embed a lock on purpose.
 */
struct v8m_huge_slab {
	void *base; /* base address of the huge page (HUGE_PAGE-aligned) */
	uint32_t bitmap; /* bit i set ⇔ slot i is in use */
	uint32_t numa_node; /* owning NUMA node (informational) */
	uint8_t slabs_per_huge; /* count of valid bits in `bitmap` */
	struct v8m_huge_slab *next; /* list link for the owning pool */
};

/*
 * Initialize a huge_slab descriptor. `base` must be
 * V8M_HUGE_PAGE_SIZE-aligned (typically the return of an
 * mmap(MAP_HUGETLB) the pool issued). `slabs_per_huge` is normally
 * V8M_HUGE_SLABS_PER_HUGE — exposed as an explicit parameter so a
 * future cycle that wants to carve a partial huge page (e.g. for a
 * region the pool reclaimed mid-life) can install a smaller count.
 * `slabs_per_huge` is clamped to [1, V8M_HUGE_SLABS_PER_HUGE]; the
 * bitmap is zero-cleared and `next` is NULLed so the descriptor is
 * safe to chain into a pool list immediately. Tolerates NULL.
 */
/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
void v8m_huge_slab_init(struct v8m_huge_slab *slab, void *base,
			uint32_t numa_node, uint8_t slabs_per_huge);
/* NOLINTEND(bugprone-easily-swappable-parameters) */

/*
 * Carve one V8M_PAGE_SIZE-aligned slab page out of `hs`. Returns the
 * slot pointer or NULL when every slot is already in use. The bitmap
 * is updated under the assumption that the caller holds the pool's
 * per-node lock. Single-threaded.
 */
void *v8m_huge_slab_alloc(struct v8m_huge_slab *slab);

/*
 * Release `slot` back to the bitmap. `slot` MUST have been issued by
 * a previous `v8m_huge_slab_alloc(hs)` call — pointers from a
 * different huge_slab or arbitrary addresses inside the huge page
 * are silently ignored (the bitmap is not modified). Returns true on
 * a successful release, false on any rejection; callers can ignore
 * the return value when they trust the pointer's provenance.
 */
bool v8m_huge_slab_free(struct v8m_huge_slab *slab, const void *slot);

/* Helpers: every slot in use → full; no slots in use → empty. */
bool v8m_huge_slab_is_full(const struct v8m_huge_slab *slab);
bool v8m_huge_slab_is_empty(const struct v8m_huge_slab *slab);

/*
 * Count of live (in-use) slots — population count of the bitmap,
 * useful for the pool's per-node "least loaded huge page" picker.
 * Returns 0 on a NULL descriptor.
 */
uint32_t v8m_huge_slab_live_count(const struct v8m_huge_slab *slab);

#endif /* V8M_HUGE_SLAB_H */
