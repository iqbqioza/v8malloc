/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Public allocation API tests. The library's constructor runs before
 * main(), so by the time these tests execute the dispatcher is up
 * and every malloc() call in the process — including those inside
 * libc — is being served by v8malloc. The tests exercise both the
 * standard names (malloc, free, …) and the v8m_-prefixed variants.
 */

#include <errno.h>
#include <malloc.h> /* malloc_usable_size — glibc extension */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v8malloc/v8malloc.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_api: %s\n", msg);
	return 1;
}

static int check_basic_malloc_free(void)
{
	void *small = malloc(64);
	if (small == NULL) {
		return fail("malloc(64) returned NULL");
	}
	(void)memset(small, 0xAB, 64);
	free(small);

	void *medium = malloc(8192);
	if (medium == NULL) {
		return fail("malloc(8K) returned NULL");
	}
	(void)memset(medium, 0xCD, 8192);
	free(medium);

	void *large = malloc((size_t)512 * 1024);
	if (large == NULL) {
		return fail("malloc(512K) returned NULL");
	}
	(void)memset(large, 0xEF, (size_t)512 * 1024);
	free(large);

	/* malloc(0) yields a unique non-NULL pointer per our policy.
	 * The analyzer's portability check would prefer a non-zero
	 * size; the whole point of this test is the zero size. */
	/* NOLINTNEXTLINE(clang-analyzer-optin.portability.UnixAPI) */
	void *zero = malloc(0);
	if (zero == NULL) {
		return fail("malloc(0) returned NULL");
	}
	free(zero);

	free(NULL); /* must not crash */
	return 0;
}

static int check_calloc_zeroes_and_overflows(void)
{
	unsigned char *buf = calloc(128, 1);
	if (buf == NULL) {
		return fail("calloc(128, 1) returned NULL");
	}
	for (int i = 0; i < 128; i++) {
		if (buf[i] != 0U) {
			free(buf);
			return fail("calloc memory not zero-initialized");
		}
	}
	free(buf);

	/* nmemb * size overflow must yield NULL with ENOMEM. */
	errno = 0;
	void *bad = calloc(SIZE_MAX, SIZE_MAX);
	if (bad != NULL) {
		free(bad);
		return fail("calloc(SIZE_MAX, SIZE_MAX) returned non-NULL");
	}
	if (errno != ENOMEM) {
		return fail("calloc overflow did not set errno = ENOMEM");
	}
	return 0;
}

static int check_realloc_grow_shrink(void)
{
	/* realloc(NULL, n) == malloc(n). */
	void *seed = realloc(NULL, 32);
	if (seed == NULL) {
		return fail("realloc(NULL, 32) returned NULL");
	}
	(void)memset(seed, 0x77, 32);

	/* Grow: data is preserved across the boundary. */
	unsigned char *grown = realloc(seed, 256);
	if (grown == NULL) {
		free(seed);
		return fail("realloc(grow) returned NULL");
	}
	for (int i = 0; i < 32; i++) {
		if (grown[i] != 0x77U) {
			free(grown);
			return fail("realloc(grow) lost original bytes");
		}
	}

	/* Shrink: in-place when the existing block fits. */
	void *shrunk = realloc(grown, 16);
	if (shrunk == NULL) {
		free(grown);
		return fail("realloc(shrink) returned NULL");
	}

	/* realloc(ptr, 0) is equivalent to free(ptr) and returns NULL. */
	void *freed = realloc(shrunk, 0);
	if (freed != NULL) {
		free(freed);
		return fail("realloc(ptr, 0) did not return NULL");
	}
	return 0;
}

static int check_reallocarray_overflows(void)
{
	errno = 0;
	void *bad = reallocarray(NULL, SIZE_MAX, SIZE_MAX);
	if (bad != NULL) {
		free(bad);
		return fail(
		    "reallocarray(SIZE_MAX, SIZE_MAX) returned non-NULL");
	}
	if (errno != ENOMEM) {
		return fail("reallocarray overflow did not set ENOMEM");
	}

	void *good = reallocarray(NULL, 16, 32);
	if (good == NULL) {
		return fail("reallocarray(16, 32) returned NULL");
	}
	free(good);
	return 0;
}

static int check_malloc_usable_size(void)
{
	void *ptr = malloc(80);
	if (ptr == NULL) {
		return fail("malloc(80) returned NULL");
	}
	size_t usable = malloc_usable_size(ptr);
	if (usable < 80U) {
		free(ptr);
		return fail("malloc_usable_size < requested");
	}
	free(ptr);

	if (malloc_usable_size(NULL) != 0) {
		return fail("malloc_usable_size(NULL) != 0");
	}
	return 0;
}

static int check_v8m_namespace(void)
{
	/* The v8m_-prefixed variants share an implementation with the
	 * standard names; smoke-test that they round-trip. */
	void *ptr = v8m_malloc(200);
	if (ptr == NULL) {
		return fail("v8m_malloc returned NULL");
	}
	(void)memset(ptr, 0x11, 200);
	v8m_free(ptr);

	unsigned char *cleared = v8m_calloc(50, 4);
	if (cleared == NULL) {
		return fail("v8m_calloc returned NULL");
	}
	for (int i = 0; i < 200; i++) {
		if (cleared[i] != 0U) {
			v8m_free(cleared);
			return fail("v8m_calloc memory not zero-initialized");
		}
	}
	v8m_free(cleared);

	void *grown = v8m_realloc(NULL, 64);
	if (grown == NULL) {
		return fail("v8m_realloc(NULL, 64) returned NULL");
	}
	void *grown2 = v8m_realloc(grown, 128);
	if (grown2 == NULL) {
		v8m_free(grown);
		return fail("v8m_realloc(grow) returned NULL");
	}
	if (v8m_malloc_usable_size(grown2) < 128U) {
		v8m_free(grown2);
		return fail("v8m_malloc_usable_size < requested after realloc");
	}
	v8m_free(grown2);
	return 0;
}

static int check_aligned_alloc(void)
{
	static const size_t cases[][2] = {
	    /* {alignment, size} — covers slab, buddy, and large paths. */
	    {16, 16},	    {32, 32},	   {64, 64},	   {128, 128},
	    {256, 256},	    {512, 1024},   {1024, 1024},   {2048, 2048},
	    {4096, 4096},   {8192, 16384}, {16384, 32768}, {4096, 200000},
	    {32768, 65536},
	};
	size_t case_count = sizeof(cases) / sizeof(cases[0]);
	for (size_t i = 0; i < case_count; i++) {
		size_t alignment = cases[i][0];
		size_t size = cases[i][1];
		void *ptr = aligned_alloc(alignment, size);
		if (ptr == NULL) {
			(void)fprintf(
			    stderr,
			    "test_api: aligned_alloc(%zu, %zu) returned NULL\n",
			    alignment, size);
			return 1;
		}
		if (((uintptr_t)ptr & (alignment - 1U)) != 0U) {
			(void)fprintf(stderr,
				      "test_api: aligned_alloc(%zu, %zu) ptr "
				      "%p misaligned\n",
				      alignment, size, ptr);
			free(ptr);
			return 1;
		}
		(void)memset(ptr, 0xA5, size);
		free(ptr);
	}

	/* Invalid alignments: zero, three (not power of 2). The values
	 * are routed through a volatile variable so the compiler can't
	 * enforce its compile-time power-of-two check on the constants
	 * — the runtime guard inside v8m_aligned_alloc is exactly what
	 * we're testing. */
	volatile size_t zero_align = 0;
	volatile size_t three_align = 3;
	errno = 0;
	void *bad = aligned_alloc(zero_align, 16);
	if (bad != NULL || errno != EINVAL) {
		free(bad);
		return fail("aligned_alloc(0, 16) did not fail with EINVAL");
	}
	errno = 0;
	bad = aligned_alloc(three_align, 16);
	if (bad != NULL || errno != EINVAL) {
		free(bad);
		return fail("aligned_alloc(3, 16) did not fail with EINVAL");
	}

	/* Above the supported alignment cap. */
	errno = 0;
	bad = aligned_alloc((size_t)1 << 20, 64);
	if (bad != NULL) {
		free(bad);
		return fail("aligned_alloc(1MiB) unexpectedly succeeded");
	}
	if (errno != ENOMEM) {
		return fail("aligned_alloc(1MiB) did not set ENOMEM");
	}
	return 0;
}

static int check_posix_memalign(void)
{
	void *ptr = NULL;
	int ret = posix_memalign(&ptr, 64, 1024);
	if (ret != 0 || ptr == NULL) {
		return fail("posix_memalign(64, 1024) failed");
	}
	if (((uintptr_t)ptr & 63U) != 0U) {
		free(ptr);
		return fail("posix_memalign(64) returned misaligned ptr");
	}
	(void)memset(ptr, 0x33, 1024);
	free(ptr);

	/* Alignment must be a multiple of sizeof(void *). On all
	 * supported targets sizeof(void *) is 8, so alignment = 4 is
	 * rejected even though it is a valid power of two. */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	void *guard = (void *)(uintptr_t)0xDEADBEEF;
	void *out = guard;
	ret = posix_memalign(&out, 4, 16);
	if (ret != EINVAL) {
		free(out);
		return fail("posix_memalign(4) did not return EINVAL");
	}
	if (out != guard) {
		return fail("posix_memalign(EINVAL) clobbered *memptr");
	}

	ret = posix_memalign(&out, 7, 16);
	if (ret != EINVAL) {
		free(out);
		return fail("posix_memalign(7) did not return EINVAL");
	}

	/* Same volatile-trick as in check_aligned_alloc: dodge the
	 * compiler's nonnull diagnostic on a literal NULL so the runtime
	 * EINVAL guard gets exercised. */
	void **null_memptr = NULL;
	void **volatile sink = null_memptr;
	/* NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker) */
	ret = posix_memalign(sink, 64, 16);
	if (ret != EINVAL) {
		return fail(
		    "posix_memalign(NULL memptr) did not return EINVAL");
	}
	return 0;
}

static int check_memalign_valloc_pvalloc(void)
{
	long page_signed = sysconf(_SC_PAGESIZE);
	size_t page = (page_signed > 0) ? (size_t)page_signed : 4096U;

	void *aligned = memalign(128, 256);
	if (aligned == NULL) {
		return fail("memalign(128, 256) returned NULL");
	}
	if (((uintptr_t)aligned & 127U) != 0U) {
		free(aligned);
		return fail("memalign(128) returned misaligned ptr");
	}
	free(aligned);

	/* valloc / pvalloc are deprecated by POSIX and the linter flags
	 * them as MT-unsafe, but we deliberately exercise them here to
	 * confirm the v8malloc-side wiring is correct. */
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	void *page_aligned = valloc(page);
	if (page_aligned == NULL) {
		return fail("valloc returned NULL");
	}
	if (((uintptr_t)page_aligned & (page - 1U)) != 0U) {
		free(page_aligned);
		return fail("valloc returned non-page-aligned ptr");
	}
	free(page_aligned);

	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	void *rounded = pvalloc(page + 1U);
	if (rounded == NULL) {
		return fail("pvalloc returned NULL");
	}
	if (((uintptr_t)rounded & (page - 1U)) != 0U) {
		free(rounded);
		return fail("pvalloc returned non-page-aligned ptr");
	}
	if (malloc_usable_size(rounded) < (page * 2U)) {
		free(rounded);
		return fail("pvalloc did not round size up to page multiple");
	}
	free(rounded);
	return 0;
}

int main(void)
{
	int status = check_basic_malloc_free();
	if (status != 0) {
		return status;
	}
	status = check_calloc_zeroes_and_overflows();
	if (status != 0) {
		return status;
	}
	status = check_realloc_grow_shrink();
	if (status != 0) {
		return status;
	}
	status = check_reallocarray_overflows();
	if (status != 0) {
		return status;
	}
	status = check_malloc_usable_size();
	if (status != 0) {
		return status;
	}
	status = check_v8m_namespace();
	if (status != 0) {
		return status;
	}
	status = check_aligned_alloc();
	if (status != 0) {
		return status;
	}
	status = check_posix_memalign();
	if (status != 0) {
		return status;
	}
	return check_memalign_valloc_pvalloc();
}
