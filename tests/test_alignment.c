/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Exhaustive alignment sweep for the aligned-allocation family —
 * `aligned_alloc`, `posix_memalign`, `memalign`, `valloc`, and
 * `pvalloc`. test_api covers the basic happy paths and the EINVAL
 * error rails; this file complements it by walking every supported
 * power-of-two alignment from 16 (max_align_t) up to V8M_PAGE_SIZE
 * across a handful of representative request sizes that hit each
 * backend (slab / buddy / large mmap).
 *
 * The sweep is the contract: if the dispatcher accepts an
 * (alignment, size) pair, the returned pointer must satisfy
 * `((uintptr_t)p & (alignment - 1)) == 0`, must round-trip a
 * memset+free, and (for posix_memalign) must leave `*memptr`
 * unclobbered on failure. A future refactor of the buddy / large
 * routing should not be able to accidentally weaken the alignment
 * guarantee on any cell of this matrix.
 */

#include <errno.h>
#include <malloc.h> /* memalign / valloc / pvalloc — glibc extensions */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v8m_buddy.h"	  /* V8M_BUDDY_MAX_BLOCK */
#include "v8m_internal.h" /* V8M_PAGE_SIZE */

/* Dispatcher caps the alignment we can request:
 *   - alignment > V8M_BUDDY_MAX_BLOCK is rejected outright
 *   - for Large allocations (size > V8M_BUDDY_MAX_BLOCK) the
 *     large-aligned path additionally caps at V8M_PAGE_SIZE / 2 so
 *     the header offset still fits below the page base
 * The sweep skips combinations the dispatcher would reject; the
 * over-cap behaviour is exercised separately by
 * check_oversize_alignment_rejected. */
#define V8M_LARGE_ALIGN_CAP (V8M_PAGE_SIZE / 2U)

static bool combo_supported(size_t alignment, size_t size)
{
	if (alignment > V8M_BUDDY_MAX_BLOCK) {
		return false;
	}
	if (size > V8M_BUDDY_MAX_BLOCK && alignment > V8M_LARGE_ALIGN_CAP) {
		return false;
	}
	return true;
}

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_alignment: %s\n", msg);
	return 1;
}

static bool is_aligned(const void *ptr, size_t alignment)
{
	return ((uintptr_t)ptr & (uintptr_t)(alignment - 1U)) == 0U;
}

/* The (alignment, size) sweep. Sizes are picked so each backend is
 * touched: 64 B fits Tiny, 1 KiB fits Small, 8 KiB / 64 KiB fit
 * Buddy, and 512 KiB lands in Large. For every alignment the size
 * must be ≥ alignment for `aligned_alloc` (C11 contract); the loop
 * skips smaller pairs. */
static const size_t g_sweep_sizes[] = {
    64, 1024, 8192, 65536, (size_t)512 * 1024,
};
static const size_t g_sweep_size_count =
    sizeof(g_sweep_sizes) / sizeof(g_sweep_sizes[0]);

static int sweep_aligned_alloc(void)
{
	for (size_t align = 16; align <= V8M_BUDDY_MAX_BLOCK; align <<= 1) {
		for (size_t i = 0; i < g_sweep_size_count; i++) {
			size_t size = g_sweep_sizes[i];
			if (size < align) {
				continue;
			}
			if (!combo_supported(align, size)) {
				continue;
			}
			/* aligned_alloc requires size be a multiple of
			 * alignment per C11. Round up to the next multiple. */
			size_t rounded = (size + align - 1U) & ~(align - 1U);
			void *ptr = aligned_alloc(align, rounded);
			if (ptr == NULL) {
				(void)fprintf(
				    stderr,
				    "test_alignment: aligned_alloc(%zu, %zu) "
				    "returned NULL\n",
				    align, rounded);
				return 1;
			}
			if (!is_aligned(ptr, align)) {
				(void)fprintf(
				    stderr,
				    "test_alignment: aligned_alloc(%zu, %zu) "
				    "ptr %p misaligned\n",
				    align, rounded, ptr);
				free(ptr);
				return 1;
			}
			(void)memset(ptr, (int)(align & 0xFFU), rounded);
			free(ptr);
		}
	}
	return 0;
}

static int sweep_posix_memalign(void)
{
	for (size_t align = sizeof(void *); align <= V8M_BUDDY_MAX_BLOCK;
	     align <<= 1) {
		for (size_t i = 0; i < g_sweep_size_count; i++) {
			size_t size = g_sweep_sizes[i];
			if (!combo_supported(align, size)) {
				continue;
			}
			void *ptr = NULL;
			int ret = posix_memalign(&ptr, align, size);
			if (ret != 0) {
				(void)fprintf(
				    stderr,
				    "test_alignment: posix_memalign(%zu, %zu)"
				    " failed (ret=%d)\n",
				    align, size, ret);
				return 1;
			}
			if (!is_aligned(ptr, align)) {
				(void)fprintf(
				    stderr,
				    "test_alignment: posix_memalign(%zu, %zu)"
				    " ptr %p misaligned\n",
				    align, size, ptr);
				free(ptr);
				return 1;
			}
			(void)memset(ptr, (int)(align & 0xFFU), size);
			free(ptr);
		}
	}
	return 0;
}

static int sweep_memalign(void)
{
	/* memalign is the legacy Solaris/SunOS API — the same
	 * power-of-two alignment requirement as aligned_alloc but no
	 * "size must be a multiple of alignment" rule. */
	for (size_t align = 16; align <= V8M_BUDDY_MAX_BLOCK; align <<= 1) {
		for (size_t i = 0; i < g_sweep_size_count; i++) {
			size_t size = g_sweep_sizes[i];
			if (!combo_supported(align, size)) {
				continue;
			}
			void *ptr = memalign(align, size);
			if (ptr == NULL) {
				(void)fprintf(
				    stderr,
				    "test_alignment: memalign(%zu, %zu) "
				    "returned NULL\n",
				    align, size);
				return 1;
			}
			if (!is_aligned(ptr, align)) {
				(void)fprintf(
				    stderr,
				    "test_alignment: memalign(%zu, %zu) ptr "
				    "%p misaligned\n",
				    align, size, ptr);
				free(ptr);
				return 1;
			}
			(void)memset(ptr, (int)(align & 0xFFU), size);
			free(ptr);
		}
	}
	return 0;
}

static int check_valloc_pvalloc(void)
{
	long page_long = sysconf(_SC_PAGESIZE);
	if (page_long <= 0) {
		return fail("sysconf(_SC_PAGESIZE) failed");
	}
	size_t page = (size_t)page_long;

	for (size_t i = 0; i < g_sweep_size_count; i++) {
		size_t size = g_sweep_sizes[i];
		/* valloc is documented as thread-unsafe by glibc; the test
		 * is single-threaded so the warning adds no signal. */
		/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
		void *ptr = valloc(size);
		if (ptr == NULL) {
			(void)fprintf(stderr,
				      "test_alignment: valloc(%zu) returned "
				      "NULL\n",
				      size);
			return 1;
		}
		if (!is_aligned(ptr, page)) {
			(void)fprintf(stderr,
				      "test_alignment: valloc(%zu) ptr %p not "
				      "page-aligned\n",
				      size, ptr);
			free(ptr);
			return 1;
		}
		(void)memset(ptr, 0x5A, size);
		free(ptr);
	}

	/* pvalloc rounds the request up to the page size. A sub-page
	 * request must still come back page-aligned and writable. */
	void *small = pvalloc(1);
	if (small == NULL) {
		return fail("pvalloc(1) returned NULL");
	}
	if (!is_aligned(small, page)) {
		free(small);
		return fail("pvalloc(1) ptr not page-aligned");
	}
	(void)memset(small, 0x5B, page);
	free(small);

	return 0;
}

static int check_posix_memalign_failure_preserves_memptr(void)
{
	/* On failure, posix_memalign must not write to *memptr — any
	 * non-NULL sentinel survives every reject path. Using a static
	 * variable's own address gives us a valid pointer value without
	 * the integer-to-pointer cast the performance lint objects to. */
	static int sentinel_storage;
	void *const sentinel = &sentinel_storage;
	void *out = sentinel;

	int ret = posix_memalign(&out, 0, 64);
	if (ret != EINVAL) {
		return fail("posix_memalign(0) did not return EINVAL");
	}
	if (out != sentinel) {
		return fail("posix_memalign(EINVAL) clobbered *memptr");
	}

	/* `out` is still the sentinel (the previous block returned on
	 * any other state); each subsequent call re-tests that the
	 * EINVAL path leaves *memptr unchanged. */
	ret = posix_memalign(&out, 3 * sizeof(void *), 64);
	if (ret != EINVAL) {
		return fail("posix_memalign(non-pow2) did not return EINVAL");
	}
	if (out != sentinel) {
		return fail("posix_memalign(non-pow2) clobbered *memptr");
	}

	ret = posix_memalign(&out, sizeof(void *) / 2, 64);
	if (ret != EINVAL) {
		return fail("posix_memalign(<voidp) did not return EINVAL");
	}
	if (out != sentinel) {
		return fail("posix_memalign(<voidp) clobbered *memptr");
	}

	return 0;
}

static int check_oversize_alignment_rejected(void)
{
	/* Anything past the supported cap returns NULL with errno set.
	 * V8M_PAGE_SIZE is 64 KiB; doubling that overshoots the
	 * Large/Huge guard rail (page_size / 2 for Large, max buddy
	 * block for everything else). */
	errno = 0;
	void *ptr = aligned_alloc(V8M_PAGE_SIZE * 64U, V8M_PAGE_SIZE * 64U);
	if (ptr != NULL) {
		free(ptr);
		return fail("aligned_alloc(huge align) unexpectedly succeeded");
	}
	if (errno != EINVAL && errno != ENOMEM) {
		return fail("aligned_alloc(huge align) did not set errno");
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= sweep_aligned_alloc();
	result |= sweep_posix_memalign();
	result |= sweep_memalign();
	result |= check_valloc_pvalloc();
	result |= check_posix_memalign_failure_preserves_memptr();
	result |= check_oversize_alignment_rejected();
	if (result == 0) {
		(void)printf("test_alignment: OK\n");
	}
	return result;
}
