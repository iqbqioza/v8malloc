/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Huge-page slab carve (huge-pages.md §4.2). Standalone tests
 * because the per-NUMA HugePage pool that consumes this primitive
 * doesn't exist in v0 yet — the tests use a process-allocated
 * V8M_HUGE_PAGE_SIZE-aligned scratch buffer in lieu of an actual
 * mmap(MAP_HUGETLB) region (the bitmap allocator never dereferences
 * the buffer, so the alignment is the only thing that matters).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "v8m_arch.h" /* V8M_HUGE_PAGE_SIZE */
#include "v8m_huge_slab.h"
#include "v8m_internal.h" /* V8M_PAGE_SIZE */

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_huge_slab: %s\n", msg);
	return 1;
}

/*
 * Reserve a V8M_PAGE_SIZE-aligned scratch buffer for the bitmap
 * allocator's slot arithmetic. Goes through mmap directly to bypass
 * v8malloc's Large path (which offsets the user pointer past
 * V8M_SLAB_HEADER_SIZE and so does not return V8M_PAGE_SIZE-aligned
 * addresses) and over-allocates by one V8M_PAGE_SIZE so the trimmed
 * `base` is guaranteed aligned regardless of the kernel's natural
 * mmap alignment. The future pool will source its base from a real
 * mmap(MAP_HUGETLB) which is naturally huge-page aligned and skips
 * this trim. Tests never read or write through the buffer.
 */
struct huge_buf {
	void *raw; /* mmap return — for the matching munmap */
	size_t cap;
	void *base; /* V8M_PAGE_SIZE-aligned slice inside */
};

static int reserve_huge(struct huge_buf *out)
{
	size_t cap = V8M_HUGE_PAGE_SIZE + V8M_PAGE_SIZE;
	void *raw = mmap(NULL, cap, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED) {
		(void)fprintf(stderr, "test_huge_slab: mmap(%zu) failed\n",
			      cap);
		return -1;
	}
	uintptr_t aligned = ((uintptr_t)raw + (V8M_PAGE_SIZE - 1U)) &
			    ~(uintptr_t)(V8M_PAGE_SIZE - 1U);
	out->raw = raw;
	out->cap = cap;
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	out->base = (void *)aligned;
	return 0;
}

static void release_huge(const struct huge_buf *buf)
{
	if (buf == NULL || buf->raw == NULL) {
		return;
	}
	(void)munmap(buf->raw, buf->cap);
}

static int check_init_state(void)
{
	struct huge_buf buf;
	if (reserve_huge(&buf) != 0) {
		return 1;
	}
	struct v8m_huge_slab slab;
	v8m_huge_slab_init(&slab, buf.base, 7U, V8M_HUGE_SLABS_PER_HUGE);
	int result = 0;
	if (slab.base != buf.base) {
		result = fail("init did not record base");
	} else if (slab.numa_node != 7U) {
		result = fail("init did not record numa_node");
	} else if (slab.slabs_per_huge != V8M_HUGE_SLABS_PER_HUGE) {
		result = fail("init did not record slabs_per_huge");
	} else if (slab.bitmap != 0U) {
		result = fail("init left bitmap non-zero");
	} else if (slab.next != NULL) {
		result = fail("init left next non-NULL");
	} else if (!v8m_huge_slab_is_empty(&slab)) {
		result = fail("freshly-init'd descriptor not reported empty");
	} else if (v8m_huge_slab_is_full(&slab)) {
		result = fail("freshly-init'd descriptor reported full");
	} else if (v8m_huge_slab_live_count(&slab) != 0U) {
		result = fail("freshly-init'd live_count was not zero");
	}
	release_huge(&buf);

	/* NULL tolerance — must not crash. */
	v8m_huge_slab_init(NULL, NULL, 0, 0);
	return result;
}

/*
 * Allocate every slot, verify each is V8M_PAGE_SIZE-aligned, lives
 * inside the huge page, is unique, and that the next allocation
 * after fill returns NULL. Then free every slot in reverse and
 * confirm the descriptor is empty again.
 */
/* Fill every slot of the descriptor and stash the slot pointers in
 * `slots`. Returns 0 on success or a fail() result. Hoists the
 * inner per-slot validation out of the surrounding driver to keep
 * check_fill_and_drain inside clang-tidy's cognitive-complexity
 * threshold. */
static int fill_slots(struct v8m_huge_slab *slab, const struct huge_buf *buf,
		      void *slots[V8M_HUGE_SLABS_PER_HUGE])
{
	for (uint32_t i = 0; i < V8M_HUGE_SLABS_PER_HUGE; i++) {
		slots[i] = v8m_huge_slab_alloc(slab);
		if (slots[i] == NULL) {
			return fail("alloc returned NULL before fill");
		}
		if ((uintptr_t)slots[i] < (uintptr_t)buf->base ||
		    (uintptr_t)slots[i] >=
			(uintptr_t)buf->base + V8M_HUGE_PAGE_SIZE) {
			return fail("alloc returned out-of-range slot");
		}
		if (((uintptr_t)slots[i] & (V8M_PAGE_SIZE - 1U)) != 0U) {
			return fail("alloc returned non-page-aligned slot");
		}
		for (uint32_t j = 0; j < i; j++) {
			if (slots[j] == slots[i]) {
				return fail("alloc returned a duplicate slot");
			}
		}
	}
	return 0;
}

static int check_fill_and_drain(void)
{
	struct huge_buf buf;
	if (reserve_huge(&buf) != 0) {
		return 1;
	}
	struct v8m_huge_slab slab;
	v8m_huge_slab_init(&slab, buf.base, 0U, V8M_HUGE_SLABS_PER_HUGE);

	void *slots[V8M_HUGE_SLABS_PER_HUGE] = {NULL};
	int result = fill_slots(&slab, &buf, slots);
	if (result != 0) {
		goto out;
	}
	if (!v8m_huge_slab_is_full(&slab)) {
		result = fail("descriptor not reported full after fill");
		goto out;
	}
	if (v8m_huge_slab_alloc(&slab) != NULL) {
		result = fail("alloc returned non-NULL after fill");
		goto out;
	}
	if (v8m_huge_slab_live_count(&slab) != V8M_HUGE_SLABS_PER_HUGE) {
		result = fail("live_count disagreed with fill count");
		goto out;
	}

	/* Drain in reverse so the bitmap-bottom path exercises both
	 * "free a high slot first" and "the resulting bitmap reuses the
	 * lowest free slot on the next alloc". */
	for (int i = (int)V8M_HUGE_SLABS_PER_HUGE - 1; i >= 0; i--) {
		if (!v8m_huge_slab_free(&slab, slots[i])) {
			result =
			    fail("free rejected a previously-allocated slot");
			goto out;
		}
	}
	if (!v8m_huge_slab_is_empty(&slab)) {
		result = fail("descriptor not reported empty after drain");
		goto out;
	}
	if (v8m_huge_slab_live_count(&slab) != 0U) {
		result = fail("live_count non-zero after drain");
		goto out;
	}
out:
	release_huge(&buf);
	return result;
}

/* The free path must reject non-slot pointers without modifying the
 * bitmap — bad alignment, out-of-range, double-free, foreign region.
 */
static int check_free_rejects_bad_pointers(void)
{
	struct huge_buf buf;
	if (reserve_huge(&buf) != 0) {
		return 1;
	}
	struct v8m_huge_slab slab;
	v8m_huge_slab_init(&slab, buf.base, 0U, V8M_HUGE_SLABS_PER_HUGE);
	void *slot = v8m_huge_slab_alloc(&slab);
	int result = 0;
	uint32_t baseline = slab.bitmap;

	/* Misaligned: 1 byte past slot. */
	if (v8m_huge_slab_free(&slab, (const char *)slot + 1)) {
		result = fail("free accepted a misaligned pointer");
		goto out;
	}
	/* Out of range: past the huge page. */
	const char *past = (const char *)buf.base + V8M_HUGE_PAGE_SIZE;
	if (v8m_huge_slab_free(&slab, past)) {
		result = fail("free accepted an out-of-range pointer");
		goto out;
	}
	/* Foreign region: a stack address. */
	int local = 0;
	if (v8m_huge_slab_free(&slab, &local)) {
		result = fail("free accepted a foreign pointer");
		goto out;
	}
	if (slab.bitmap != baseline) {
		result = fail("rejected free still mutated bitmap");
		goto out;
	}

	/* Successful release, then a double-free of the same slot. */
	if (!v8m_huge_slab_free(&slab, slot)) {
		result = fail("first free of valid slot rejected");
		goto out;
	}
	if (v8m_huge_slab_free(&slab, slot)) {
		result = fail("double-free was not rejected");
		goto out;
	}
out:
	release_huge(&buf);
	return result;
}

/* slabs_per_huge clamping: zero coerces to the default, oversize
 * clamps to V8M_HUGE_SLABS_PER_HUGE; partial counts (e.g. 4) limit
 * the bitmap so the 5th alloc fails. */
static int check_partial_slabs_per_huge(void)
{
	struct huge_buf buf;
	if (reserve_huge(&buf) != 0) {
		return 1;
	}
	struct v8m_huge_slab slab;

	v8m_huge_slab_init(&slab, buf.base, 0U, 0U);
	if (slab.slabs_per_huge != V8M_HUGE_SLABS_PER_HUGE) {
		release_huge(&buf);
		return fail("zero slabs_per_huge did not coerce to default");
	}
	v8m_huge_slab_init(&slab, buf.base, 0U, 99U);
	if (slab.slabs_per_huge != V8M_HUGE_SLABS_PER_HUGE) {
		release_huge(&buf);
		return fail("oversize slabs_per_huge did not clamp");
	}

	v8m_huge_slab_init(&slab, buf.base, 0U, 4U);
	for (int i = 0; i < 4; i++) {
		if (v8m_huge_slab_alloc(&slab) == NULL) {
			release_huge(&buf);
			return fail(
			    "alloc returned NULL before partial limit reached");
		}
	}
	if (v8m_huge_slab_alloc(&slab) != NULL) {
		release_huge(&buf);
		return fail(
		    "alloc returned non-NULL past the partial slab limit");
	}
	if (!v8m_huge_slab_is_full(&slab)) {
		release_huge(&buf);
		return fail("partial-limit descriptor did not report full");
	}
	release_huge(&buf);
	return 0;
}

/* NULL/empty descriptor edge cases. */
static int check_null_tolerance(void)
{
	if (v8m_huge_slab_alloc(NULL) != NULL) {
		return fail("alloc(NULL) did not return NULL");
	}
	if (v8m_huge_slab_free(NULL, NULL)) {
		return fail("free(NULL, NULL) returned true");
	}
	if (v8m_huge_slab_is_full(NULL)) {
		return fail("is_full(NULL) returned true");
	}
	if (!v8m_huge_slab_is_empty(NULL)) {
		return fail("is_empty(NULL) did not return true");
	}
	if (v8m_huge_slab_live_count(NULL) != 0U) {
		return fail("live_count(NULL) was non-zero");
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_init_state();
	result |= check_fill_and_drain();
	result |= check_free_rejects_bad_pointers();
	result |= check_partial_slabs_per_huge();
	result |= check_null_tolerance();
	if (result == 0) {
		(void)printf("test_huge_slab: OK\n");
	}
	return result;
}
