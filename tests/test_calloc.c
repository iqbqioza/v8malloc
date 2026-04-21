/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Exhaustive calloc / reallocarray test. test_api covers the basic
 * happy path + overflow rejection; this file expands to:
 *
 *  - zeroing across every backend (slab Tiny, slab Small, buddy
 *    Medium, Large mmap, Huge mmap)
 *  - the (nmemb, size) overflow guard at the boundary, not just at
 *    SIZE_MAX (off-by-one matters: nmemb * size must overflow even
 *    when one factor is small)
 *  - calloc(0, n) and calloc(n, 0) edge cases
 *  - reallocarray's overflow contract matches calloc's
 *  - reallocarray preserves bytes on grow / shrink
 *
 * Calloc bugs are usually one of two shapes: a missed zero (the
 * allocator handed back a slab slot that still held bytes from a
 * prior alloc), or a missed overflow (nmemb * size wrapped and the
 * caller got a smaller buffer than requested). Both are surfaced
 * here.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_calloc: %s\n", msg);
	return 1;
}

/* The (nmemb, size) sweep: one row per backend the allocator
 * routes through. Each row is chosen so nmemb * size hits the
 * intended class without crossing into the next. */
struct sweep_cell {
	size_t nmemb;
	size_t size;
	const char *label;
};

static const struct sweep_cell g_sweep[] = {
    {1, 8, "tiny class 0"},
    {8, 8, "tiny class 7 (8x8)"},
    {16, 32, "small class (16x32)"},
    {1, 4096, "small max (1x4K)"},
    {1, 16384, "buddy medium (16K)"},
    {1, 65536, "buddy medium (64K)"},
    {1, (size_t)256 * 1024, "buddy max (256K)"},
    {1, (size_t)512 * 1024, "large mmap (512K)"},
    {1, (size_t)4 * 1024 * 1024, "huge mmap (4M)"},
};
static const size_t g_sweep_count = sizeof(g_sweep) / sizeof(g_sweep[0]);

static int verify_zero(const unsigned char *buf, size_t bytes,
		       const char *label)
{
	for (size_t i = 0; i < bytes; i++) {
		if (buf[i] != 0U) {
			(void)fprintf(
			    stderr,
			    "test_calloc: %s — byte %zu was 0x%02x, want 0\n",
			    label, i, buf[i]);
			return 1;
		}
	}
	return 0;
}

static int sweep_calloc_zeroes(void)
{
	for (size_t i = 0; i < g_sweep_count; i++) {
		const struct sweep_cell *cell = &g_sweep[i];
		size_t bytes = cell->nmemb * cell->size;
		unsigned char *buf = calloc(cell->nmemb, cell->size);
		if (buf == NULL) {
			(void)fprintf(
			    stderr, "test_calloc: %s — calloc returned NULL\n",
			    cell->label);
			return 1;
		}
		if (verify_zero(buf, bytes, cell->label) != 0) {
			free(buf);
			return 1;
		}
		/* Stamp it dirty so the next reuse of this slab slot
		 * (if calloc backs onto the same slot) needs to zero it
		 * again — the next `sweep_calloc_zeroes` invocation
		 * (e.g. in a future fuzz integration) would catch a
		 * missed zero. */
		(void)memset(buf, 0xAB, bytes);
		free(buf);
	}
	return 0;
}

/* Re-run calloc on the same size and confirm zeros — the previous
 * sweep dirtied pages right before free, so a missed-zero bug
 * surfaces on the second pass. */
static int sweep_calloc_zeroes_after_dirty(void)
{
	for (size_t i = 0; i < g_sweep_count; i++) {
		const struct sweep_cell *cell = &g_sweep[i];
		size_t bytes = cell->nmemb * cell->size;
		unsigned char *buf = calloc(cell->nmemb, cell->size);
		if (buf == NULL) {
			return fail("post-dirty calloc returned NULL");
		}
		if (verify_zero(buf, bytes, cell->label) != 0) {
			free(buf);
			return 1;
		}
		free(buf);
	}
	return 0;
}

static int check_overflow_boundary(void)
{
	/* The classical SIZE_MAX × SIZE_MAX overflow is the easy
	 * case test_api covers. The harder cases are when one factor
	 * is small but the product still wraps — those test the
	 * implementation's check is "does multiplication overflow"
	 * rather than "are both factors large". */
	errno = 0;
	void *bad = calloc(SIZE_MAX / 2U + 2U, 2U);
	if (bad != NULL) {
		free(bad);
		return fail("calloc((SIZE_MAX/2)+2, 2) unexpectedly succeeded");
	}
	if (errno != ENOMEM) {
		return fail("calloc overflow did not set errno = ENOMEM");
	}

	/* Another shape: large nmemb × medium size. */
	errno = 0;
	bad = calloc(SIZE_MAX / 8U + 1U, 8U);
	if (bad != NULL) {
		free(bad);
		return fail("calloc((SIZE_MAX/8)+1, 8) unexpectedly succeeded");
	}
	if (errno != ENOMEM) {
		return fail("calloc overflow #2 did not set errno = ENOMEM");
	}

	/* Just under the boundary should succeed (assuming
	 * sufficient memory; the test runs in a CI container with
	 * limited RAM, so we cap the success-path check at a small
	 * size). */
	void *good = calloc(64U, 16U);
	if (good == NULL) {
		return fail("calloc(64, 16) returned NULL");
	}
	if (verify_zero(good, (size_t)64U * 16U, "boundary-good") != 0) {
		free(good);
		return 1;
	}
	free(good);
	return 0;
}

static int check_zero_factor_edges(void)
{
	/* calloc(0, n) and calloc(n, 0) are explicitly defined to be
	 * acceptable but implementation-defined. v8malloc follows
	 * glibc and returns a unique non-NULL malloc(0)-style
	 * pointer (or NULL, depending on the runtime). Either is
	 * acceptable; we just confirm the path doesn't crash and a
	 * non-NULL return is freeable. */
	/* The allocations here are intentionally not exercised — the
	 * test is that calloc(0, n)/calloc(n, 0)/calloc(0, 0) do not
	 * crash and that the returned pointer (if non-NULL) is
	 * freeable. cppcheck flags the "unused" allocations; suppress
	 * since the test target is the lifecycle, not the data. */
	/* cppcheck-suppress unusedAllocatedMemory */
	/* NOLINTNEXTLINE(clang-analyzer-optin.portability.UnixAPI) */
	void *zero_n = calloc(0U, 64U);
	free(zero_n);
	/* cppcheck-suppress unusedAllocatedMemory */
	/* NOLINTNEXTLINE(clang-analyzer-optin.portability.UnixAPI) */
	void *n_zero = calloc(64U, 0U);
	free(n_zero);
	/* cppcheck-suppress unusedAllocatedMemory */
	/* NOLINTNEXTLINE(clang-analyzer-optin.portability.UnixAPI) */
	void *zero_zero = calloc(0U, 0U);
	free(zero_zero);
	return 0;
}

static int check_reallocarray_overflow(void)
{
	errno = 0;
	void *bad = reallocarray(NULL, SIZE_MAX / 2U + 2U, 2U);
	if (bad != NULL) {
		free(bad);
		return fail("reallocarray overflow unexpectedly succeeded");
	}
	if (errno != ENOMEM) {
		return fail("reallocarray overflow did not set errno = ENOMEM");
	}
	return 0;
}

static int check_reallocarray_grow_shrink(void)
{
	/* Seed with reallocarray, stamp, then grow + shrink and
	 * verify the prefix survives. */
	const uint32_t pattern_hi = 0x42U;
	const size_t initial_n = 64U;
	const size_t element = 16U;

	unsigned char *buf = reallocarray(NULL, initial_n, element);
	if (buf == NULL) {
		return fail("reallocarray seed returned NULL");
	}
	(void)memset(buf, (int)pattern_hi, initial_n * element);

	unsigned char *grown = reallocarray(buf, initial_n * 2U, element);
	if (grown == NULL) {
		free(buf);
		return fail("reallocarray grow returned NULL");
	}
	for (size_t i = 0; i < initial_n * element; i++) {
		if (grown[i] != pattern_hi) {
			free(grown);
			return fail("reallocarray grow lost bytes");
		}
	}

	unsigned char *shrunk = reallocarray(grown, initial_n / 2U, element);
	if (shrunk == NULL) {
		free(grown);
		return fail("reallocarray shrink returned NULL");
	}
	for (size_t i = 0; i < (initial_n / 2U) * element; i++) {
		if (shrunk[i] != pattern_hi) {
			free(shrunk);
			return fail("reallocarray shrink lost bytes");
		}
	}
	free(shrunk);
	return 0;
}

int main(void)
{
	int result = 0;
	result |= sweep_calloc_zeroes();
	result |= sweep_calloc_zeroes_after_dirty();
	result |= check_overflow_boundary();
	result |= check_zero_factor_edges();
	result |= check_reallocarray_overflow();
	result |= check_reallocarray_grow_shrink();
	if (result == 0) {
		(void)printf("test_calloc: OK\n");
	}
	return result;
}
