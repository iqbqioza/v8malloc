/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Huge-page slab carve — implementation. See the matching header
 * for the design notes and the v0 status.
 */

#include "v8m_huge_slab.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "v8m_arch.h"
#include "v8m_internal.h" /* V8M_PAGE_SIZE — surfaces through the header */

/* Mask of all bits the bitmap considers valid for this descriptor.
 * Used by the full-detect predicate and to reject out-of-range
 * slot indices on the free path. */
V8M_PURE static uint32_t valid_mask(const struct v8m_huge_slab *slab)
{
	uint8_t count = slab->slabs_per_huge;
	if (count == 0U) {
		return 0U;
	}
	if (count >= 32U) {
		return UINT32_MAX;
	}
	return (1U << count) - 1U;
}

/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
void v8m_huge_slab_init(struct v8m_huge_slab *slab, void *base,
			uint32_t numa_node, uint8_t slabs_per_huge)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
{
	if (slab == NULL) {
		return;
	}
	(void)memset(slab, 0, sizeof(*slab));
	slab->base = base;
	slab->numa_node = numa_node;
	if (slabs_per_huge == 0U) {
		slabs_per_huge = V8M_HUGE_SLABS_PER_HUGE;
	}
	if (slabs_per_huge > V8M_HUGE_SLABS_PER_HUGE) {
		slabs_per_huge = V8M_HUGE_SLABS_PER_HUGE;
	}
	slab->slabs_per_huge = slabs_per_huge;
}

void *v8m_huge_slab_alloc(struct v8m_huge_slab *slab)
{
	if (slab == NULL || slab->base == NULL) {
		return NULL;
	}
	uint32_t full = valid_mask(slab);
	if (__builtin_expect((slab->bitmap & full) == full, 0)) {
		return NULL; /* every slot in use */
	}
	/* First free slot — `~bitmap & full` clears already-used bits
	 * AND tail bits past slabs_per_huge so __builtin_ctz finds the
	 * lowest valid free slot deterministically. */
	uint32_t free_mask = ~slab->bitmap & full;
	uint32_t idx = (uint32_t)__builtin_ctz(free_mask);
	slab->bitmap |= (1U << idx);
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	return (void *)((uintptr_t)slab->base + ((size_t)idx * V8M_PAGE_SIZE));
}

bool v8m_huge_slab_free(struct v8m_huge_slab *slab, const void *slot)
{
	if (slab == NULL || slab->base == NULL || slot == NULL) {
		return false;
	}
	uintptr_t base = (uintptr_t)slab->base;
	uintptr_t addr = (uintptr_t)slot;
	if (addr < base) {
		return false;
	}
	uintptr_t offset = addr - base;
	if ((offset & (V8M_PAGE_SIZE - 1U)) != 0U) {
		return false; /* not slot-aligned */
	}
	size_t idx = offset / V8M_PAGE_SIZE;
	if (idx >= (size_t)slab->slabs_per_huge) {
		return false; /* past the live slot range */
	}
	uint32_t mask = 1U << idx;
	if ((slab->bitmap & mask) == 0U) {
		return false; /* double-free */
	}
	slab->bitmap &= ~mask;
	return true;
}

bool v8m_huge_slab_is_full(const struct v8m_huge_slab *slab)
{
	if (slab == NULL) {
		return false;
	}
	uint32_t full = valid_mask(slab);
	return full != 0U && (slab->bitmap & full) == full;
}

bool v8m_huge_slab_is_empty(const struct v8m_huge_slab *slab)
{
	if (slab == NULL) {
		return true; /* matches "no live slots" by vacuous truth */
	}
	return (slab->bitmap & valid_mask(slab)) == 0U;
}

uint32_t v8m_huge_slab_live_count(const struct v8m_huge_slab *slab)
{
	if (slab == NULL) {
		return 0U;
	}
	return (uint32_t)__builtin_popcount(slab->bitmap & valid_mask(slab));
}
