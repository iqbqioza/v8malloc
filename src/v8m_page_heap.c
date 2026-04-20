/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Page heap implementation. A thin wrapper over mmap/munmap/madvise
 * that handles arbitrary power-of-two alignment by over-allocating
 * and trimming, and keeps lifetime statistics for diagnostic and
 * tuning purposes.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "v8m_internal.h"
#include "v8m_page_heap.h"

static _Atomic uint64_t v8m_mmap_calls = 0;
static _Atomic uint64_t v8m_munmap_calls = 0;
static _Atomic uint64_t v8m_advise_calls = 0;
static _Atomic uint64_t v8m_bytes_mapped = 0;
static _Atomic uint64_t v8m_bytes_unmapped = 0;

static bool is_power_of_two(size_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

static void record_mmap(size_t bytes)
{
	atomic_fetch_add_explicit(&v8m_mmap_calls, 1U, memory_order_relaxed);
	atomic_fetch_add_explicit(&v8m_bytes_mapped, bytes,
				  memory_order_relaxed);
}

static void record_munmap(size_t bytes)
{
	atomic_fetch_add_explicit(&v8m_munmap_calls, 1U, memory_order_relaxed);
	atomic_fetch_add_explicit(&v8m_bytes_unmapped, bytes,
				  memory_order_relaxed);
}

void *v8m_page_heap_alloc(size_t bytes, size_t alignment)
{
	if (bytes == 0 || alignment < V8M_PAGE_SIZE ||
	    !is_power_of_two(alignment)) {
		return NULL;
	}
	/* Over-allocate by `alignment` so we can slide up to the next
	 * aligned boundary and trim whatever lies outside. Guard against
	 * size_t overflow in the addition. */
	if (bytes > SIZE_MAX - alignment) {
		return NULL;
	}
	size_t request = bytes + alignment;

	void *raw = mmap(NULL, request, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED) {
		return NULL;
	}
	record_mmap(request);

	uintptr_t raw_addr = (uintptr_t)raw;
	uintptr_t aligned =
	    (raw_addr + alignment - 1U) & ~(uintptr_t)(alignment - 1U);
	size_t pre = (size_t)(aligned - raw_addr);
	size_t post = request - pre - bytes;

	if (pre > 0) {
		(void)munmap(raw, pre);
		record_munmap(pre);
	}
	if (post > 0) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		(void)munmap((void *)(aligned + bytes), post);
		record_munmap(post);
	}

	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	return (void *)aligned;
}

void v8m_page_heap_free(void *ptr, size_t bytes)
{
	if (ptr == NULL || bytes == 0) {
		return;
	}
	(void)munmap(ptr, bytes);
	record_munmap(bytes);
}

void v8m_page_heap_advise_dont_need(void *ptr, size_t bytes)
{
	if (ptr == NULL || bytes == 0) {
		return;
	}
	(void)madvise(ptr, bytes, MADV_DONTNEED);
	atomic_fetch_add_explicit(&v8m_advise_calls, 1U, memory_order_relaxed);
}

void v8m_page_heap_get_stats(struct v8m_page_heap_stats *out)
{
	if (out == NULL) {
		return;
	}
	out->mmap_calls =
	    atomic_load_explicit(&v8m_mmap_calls, memory_order_relaxed);
	out->munmap_calls =
	    atomic_load_explicit(&v8m_munmap_calls, memory_order_relaxed);
	out->advise_calls =
	    atomic_load_explicit(&v8m_advise_calls, memory_order_relaxed);
	out->bytes_mapped =
	    atomic_load_explicit(&v8m_bytes_mapped, memory_order_relaxed);
	out->bytes_unmapped =
	    atomic_load_explicit(&v8m_bytes_unmapped, memory_order_relaxed);
}
