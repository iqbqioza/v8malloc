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
	return check_v8m_namespace();
}
