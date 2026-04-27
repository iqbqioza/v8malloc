/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Anchor reservation — implementation. See the matching header for
 * the design notes and v0 status.
 */

#include "v8m_anchor_reservation.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t round_up_pow2(size_t value, size_t alignment)
{
	if (alignment <= 1U) {
		return value;
	}
	return (value + (alignment - 1U)) & ~(alignment - 1U);
}

static size_t os_page_size(void)
{
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0) {
		return 4096U; /* defensive — every supported OS reports a
			       * positive page size, but the fallback keeps
			       * the carve path arithmetic well-defined. */
	}
	return (size_t)page;
}

int v8m_anchor_reservation_init(struct v8m_anchor_reservation *res, size_t cap)
{
	if (res == NULL) {
		return -EINVAL;
	}
	(void)memset(res, 0, sizeof(*res));
	if (cap == 0U) {
		cap = V8M_ANCHOR_DEFAULT_CAP_BYTES;
	}
	cap = round_up_pow2(cap, os_page_size());

	/* PROT_NONE + MAP_NORESERVE: reserve the virtual range without
	 * committing physical memory or swap. The reservation only
	 * gains a backing footprint when carve() flips its
	 * sub-ranges to PROT_READ | PROT_WRITE and the caller
	 * faults pages in. */
	void *base = mmap(NULL, cap, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED) {
		return -ENOMEM;
	}
	int mutex_rc = pthread_mutex_init(&res->lock, NULL);
	if (mutex_rc != 0) {
		(void)munmap(base, cap);
		return -mutex_rc;
	}
	res->base = base;
	res->cap = cap;
	return 0;
}

void v8m_anchor_reservation_destroy(struct v8m_anchor_reservation *res)
{
	if (res == NULL || res->base == NULL) {
		return;
	}
	(void)munmap(res->base, res->cap);
	(void)pthread_mutex_destroy(&res->lock);
	(void)memset(res, 0, sizeof(*res));
}

/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
void *v8m_anchor_reservation_carve(struct v8m_anchor_reservation *res,
				   size_t bytes, size_t alignment)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
{
	if (__builtin_expect(res == NULL || res->base == NULL || bytes == 0U,
			     0)) {
		return NULL;
	}
	if (__builtin_expect(alignment == 0U, 0)) {
		alignment = 1U;
	}
	/* Reject non-power-of-two alignment so the round-up below
	 * stays well-defined. */
	if (__builtin_expect((alignment & (alignment - 1U)) != 0U, 0)) {
		return NULL;
	}

	(void)pthread_mutex_lock(&res->lock);
	/* Align the absolute target address (base + bump_offset) up to
	 * the requested alignment, not just the bump_offset itself —
	 * base may only be OS-page aligned, so carving with alignment >
	 * OS_PAGE_SIZE needs the extra rounding to land on the right
	 * boundary. The skipped padding stays inside the anchor and is
	 * never reclaimed (bump-only). */
	uintptr_t target = (uintptr_t)res->base + res->bump_offset;
	uintptr_t aligned_target =
	    (target + (alignment - 1U)) & ~(uintptr_t)(alignment - 1U);
	if (__builtin_expect(aligned_target < target, 0)) {
		res->carve_failures++;
		(void)pthread_mutex_unlock(&res->lock);
		return NULL;
	}
	size_t aligned_offset = aligned_target - (uintptr_t)res->base;
	if (__builtin_expect(aligned_offset > res->cap ||
				 bytes > res->cap - aligned_offset,
			     0)) {
		res->carve_failures++;
		(void)pthread_mutex_unlock(&res->lock);
		return NULL;
	}
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	void *carved = (void *)aligned_target;
	if (__builtin_expect(
		mprotect(carved, bytes, PROT_READ | PROT_WRITE) != 0, 0)) {
		res->carve_failures++;
		(void)pthread_mutex_unlock(&res->lock);
		return NULL;
	}
	res->bump_offset = aligned_offset + bytes;
	res->carve_calls++;
	(void)pthread_mutex_unlock(&res->lock);
	return carved;
}

bool v8m_anchor_reservation_release(struct v8m_anchor_reservation *res,
				    void *ptr, size_t bytes)
{
	if (__builtin_expect(res == NULL || res->base == NULL || ptr == NULL ||
				 bytes == 0U,
			     0)) {
		return false;
	}
	uintptr_t base = (uintptr_t)res->base;
	uintptr_t addr = (uintptr_t)ptr;
	if (__builtin_expect(addr < base || addr - base >= res->cap ||
				 bytes > res->cap - (addr - base),
			     0)) {
		return false;
	}
	/* MADV_DONTNEED returns physical pages to the OS while leaving
	 * the virtual mapping in place; the subsequent
	 * mprotect(PROT_NONE) makes any access SIGSEGV — catches
	 * use-after-free attempts that escape the bump-only design. */
	(void)madvise(ptr, bytes, MADV_DONTNEED);
	(void)mprotect(ptr, bytes, PROT_NONE);
	(void)pthread_mutex_lock(&res->lock);
	res->release_calls++;
	(void)pthread_mutex_unlock(&res->lock);
	return true;
}

bool v8m_anchor_reservation_owns(const struct v8m_anchor_reservation *res,
				 const void *ptr)
{
	if (__builtin_expect(res == NULL || res->base == NULL || ptr == NULL,
			     0)) {
		return false;
	}
	uintptr_t base = (uintptr_t)res->base;
	uintptr_t addr = (uintptr_t)ptr;
	return addr >= base && (addr - base) < res->cap;
}

size_t
v8m_anchor_reservation_remaining(const struct v8m_anchor_reservation *res)
{
	if (res == NULL || res->base == NULL) {
		return 0U;
	}
	if (res->bump_offset >= res->cap) {
		return 0U;
	}
	return res->cap - res->bump_offset;
}
