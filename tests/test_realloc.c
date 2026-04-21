/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Exhaustive realloc semantics test. test_api covers the four
 * canonical shapes (NULL→malloc, grow-with-data, shrink, ptr+0);
 * this file exercises the cross-backend transitions that test_api
 * does not — slab→buddy, buddy→large, large→huge, and the reverse
 * shrink path back through every backend boundary. Every realloc
 * is preceded by stamping a deterministic byte pattern that
 * depends on both the index and the offset, and the post-realloc
 * bytes are verified up to `min(old_size, new_size)`. A regression
 * that drops a byte during the move surfaces as a precise mismatch
 * report rather than a vague segfault.
 *
 * The size ladder spans:
 *   - 8 B / 64 B          (slab Tiny)
 *   - 512 B / 4 KiB        (slab Small)
 *   - 16 KiB / 64 KiB      (buddy Medium)
 *   - 256 KiB              (buddy boundary)
 *   - 512 KiB / 1 MiB      (Large mmap)
 *   - 4 MiB                (Huge mmap)
 *
 * Realloc must never lose bytes that fit in both the old and the
 * new allocation. That contract is the entire test.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_realloc: %s\n", msg);
	return 1;
}

/* Pattern stamped at offset i for an allocation seeded with `seed`
 * — the seed cycles over each test so a stale buffer from a prior
 * realloc cannot accidentally match. */
static unsigned char pattern_byte(uint32_t seed, size_t offset)
{
	return (unsigned char)((seed * 0x9E3779B1U) ^
			       ((uint32_t)offset * 0x85EBCA77U));
}

static void stamp_range(unsigned char *buf, size_t start, size_t end,
			uint32_t seed)
{
	for (size_t i = start; i < end; i++) {
		buf[i] = pattern_byte(seed, i);
	}
}

static void stamp(unsigned char *buf, size_t bytes, uint32_t seed)
{
	stamp_range(buf, 0, bytes, seed);
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static int verify(const unsigned char *buf, size_t bytes, uint32_t seed,
		  const char *where)
{
	for (size_t i = 0; i < bytes; i++) {
		if (buf[i] != pattern_byte(seed, i)) {
			(void)fprintf(stderr,
				      "test_realloc: byte %zu mismatch in %s "
				      "(got 0x%02x, want 0x%02x)\n",
				      i, where, buf[i], pattern_byte(seed, i));
			return 1;
		}
	}
	return 0;
}

/* The size ladder is shared between the grow walk and the shrink
 * walk. Indices into this array also seed the pattern so a
 * regression report can name the ladder cell. */
static const size_t g_ladder[] = {
    8,
    64,
    512,
    4096,
    16384,
    65536,
    (size_t)256 * 1024,
    (size_t)512 * 1024,
    (size_t)1024 * 1024,
    (size_t)4 * 1024 * 1024,
};
static const size_t g_ladder_count = sizeof(g_ladder) / sizeof(g_ladder[0]);

/* Walk the ladder upward, reallocing one buffer through every cell.
 * Each step stamps the new tail (the previously unstamped bytes)
 * and verifies the old prefix survived — so by the end of the walk
 * the entire 4 MiB buffer holds a single coherent pattern that was
 * laid down across many realloc calls, each crossing a backend
 * boundary. */
static int check_grow_walk(void)
{
	const uint32_t seed = 0xA17B5CE3U;

	unsigned char *buf = malloc(g_ladder[0]);
	if (buf == NULL) {
		return fail("initial malloc failed");
	}
	stamp(buf, g_ladder[0], seed);
	size_t prev = g_ladder[0];

	for (size_t i = 1; i < g_ladder_count; i++) {
		size_t next = g_ladder[i];
		unsigned char *grown = realloc(buf, next);
		if (grown == NULL) {
			free(buf);
			return fail("realloc grow returned NULL");
		}
		buf = grown;
		if (verify(buf, prev, seed, "grow walk") != 0) {
			free(buf);
			return 1;
		}
		/* Stamp the tail [prev, next) so the next iteration can
		 * verify everything up to `next`. The pattern is keyed on
		 * the absolute offset, not the buffer-local offset, so the
		 * grown buffer holds one coherent run from index 0. */
		stamp_range(buf, prev, next, seed);
		prev = next;
	}

	/* Final pass: verify the entire buffer in one shot. */
	if (verify(buf, prev, seed, "grow walk final") != 0) {
		free(buf);
		return 1;
	}
	free(buf);
	return 0;
}

/* Reverse direction: start with the largest allocation, stamp it
 * fully, then shrink one ladder cell at a time. Each step verifies
 * that the surviving prefix is intact — shrink must never corrupt
 * the bytes it keeps. */
static int check_shrink_walk(void)
{
	const uint32_t seed = 0x12345678U;
	size_t initial = g_ladder[g_ladder_count - 1];

	unsigned char *buf = malloc(initial);
	if (buf == NULL) {
		return fail("initial large malloc failed");
	}
	stamp(buf, initial, seed);

	for (size_t i = g_ladder_count - 1; i > 0; i--) {
		size_t next = g_ladder[i - 1];
		unsigned char *shrunk = realloc(buf, next);
		if (shrunk == NULL) {
			free(buf);
			return fail("realloc shrink returned NULL");
		}
		buf = shrunk;
		if (verify(buf, next, seed, "shrink walk") != 0) {
			free(buf);
			return 1;
		}
	}
	free(buf);
	return 0;
}

/* In-place shrink within the same size class must be free of moves
 * AND preserve the data. We can't strictly observe the no-move via
 * pointer equality (the implementation is free to move), but we
 * can verify the bytes survive every small shrink within a single
 * class. */
static int check_intra_class_shrink(void)
{
	const uint32_t seed = 0xDEADBEEFU;
	const size_t start = 4096;
	unsigned char *buf = malloc(start);
	if (buf == NULL) {
		return fail("intra-class initial malloc failed");
	}
	stamp(buf, start, seed);

	/* Shrink in 256-byte steps down to 256. Slab class 31 covers
	 * exactly 4 KiB, but classes 26..31 span 1.5 KiB to 4 KiB —
	 * each step crosses class boundaries within the Small range
	 * and exercises the realloc shrink path repeatedly. */
	for (size_t target = start - 256; target >= 256; target -= 256) {
		unsigned char *shrunk = realloc(buf, target);
		if (shrunk == NULL) {
			free(buf);
			return fail("intra-class shrink returned NULL");
		}
		buf = shrunk;
		if (verify(buf, target, seed, "intra-class shrink") != 0) {
			free(buf);
			return 1;
		}
	}
	free(buf);
	return 0;
}

/* The four edge cases test_api also covers, but kept here so this
 * file runs as a standalone realloc contract. */
static int check_edge_cases(void)
{
	/* realloc(NULL, n) is malloc(n). */
	void *fresh = realloc(NULL, 128);
	if (fresh == NULL) {
		return fail("realloc(NULL, 128) returned NULL");
	}
	(void)memset(fresh, 0xAB, 128);

	/* realloc(ptr, 0) frees ptr and returns NULL per glibc — the
	 * tidy / cppcheck checks don't model that contract, so the
	 * NOLINT line tells them this is intentional. */
	/* cppcheck-suppress memleak */
	/* NOLINTNEXTLINE(clang-analyzer-optin.portability.UnixAPI) */
	void *zeroed = realloc(fresh, 0);
	if (zeroed != NULL) {
		free(zeroed);
		return fail("realloc(ptr, 0) did not return NULL");
	}

	/* realloc(NULL, 0) — implementation-defined per C11; v8malloc
	 * follows glibc and returns a unique non-NULL malloc(0)-style
	 * pointer or NULL. Either is acceptable; just ensure the
	 * pointer (if non-NULL) frees cleanly. */
	void *zero_seed = realloc(NULL, 0);
	free(zero_seed);

	/* cppcheck does not model realloc(ptr, 0) as freeing ptr, so it
	 * treats `fresh` as leaked at function exit. The glibc contract
	 * is well defined; suppress the false positive here. */
	/* cppcheck-suppress memleak */
	return 0;
}

/* Cross-backend round-trip: alloc small, grow to huge, shrink back
 * to small, verify the bytes that survived the round trip. The
 * shrink step's "surviving" range is the smallest size in the
 * sequence — so we stamp the small region first, grow without
 * extending the stamped range, then shrink, then verify. */
static int check_round_trip(void)
{
	const uint32_t seed = 0xCAFEF00DU;
	const size_t small = 96;
	const size_t huge = (size_t)4 * 1024 * 1024;

	unsigned char *buf = malloc(small);
	if (buf == NULL) {
		return fail("round-trip initial malloc failed");
	}
	stamp(buf, small, seed);

	unsigned char *grown = realloc(buf, huge);
	if (grown == NULL) {
		free(buf);
		return fail("round-trip grow returned NULL");
	}
	if (verify(grown, small, seed, "round-trip after grow") != 0) {
		free(grown);
		return 1;
	}

	unsigned char *shrunk = realloc(grown, small);
	if (shrunk == NULL) {
		free(grown);
		return fail("round-trip shrink returned NULL");
	}
	if (verify(shrunk, small, seed, "round-trip after shrink") != 0) {
		free(shrunk);
		return 1;
	}
	free(shrunk);
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_edge_cases();
	result |= check_grow_walk();
	result |= check_shrink_walk();
	result |= check_intra_class_shrink();
	result |= check_round_trip();
	if (result == 0) {
		(void)printf("test_realloc: OK\n");
	}
	return result;
}
