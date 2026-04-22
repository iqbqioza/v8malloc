/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Anchor reservation primitive (huge-pages.md §6.2). Standalone
 * tests because the page-heap integration that consumes this primitive
 * is a follow-on cycle. Each test allocates its own reservation,
 * exercises one slice of the API, and tears it down before
 * returning so subsequent tests start from a clean baseline.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "v8m_anchor_reservation.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_anchor_reservation: %s\n", msg);
	return 1;
}

/*
 * Use a small reservation cap (1 MiB) so the tests don't hold a
 * 256 MiB virtual range across the suite. Even at PROT_NONE the
 * kernel still tracks the VMA — keeping it small reduces test
 * environment pressure.
 */
enum {
	TEST_CAP_BYTES = 1U * 1024U * 1024U,
	TEST_CARVE_BYTES = 64U * 1024U,
	TEST_ALIGNMENT = 4096U,
};

/* Init must reserve the cap, zero counters, and survive a
 * matching destroy. */
static int check_init_destroy(void)
{
	struct v8m_anchor_reservation res;
	int init_rc = v8m_anchor_reservation_init(&res, TEST_CAP_BYTES);
	if (init_rc != 0) {
		return fail("init returned non-zero");
	}
	if (res.base == NULL) {
		v8m_anchor_reservation_destroy(&res);
		return fail("init left base NULL");
	}
	if (res.cap < TEST_CAP_BYTES) {
		v8m_anchor_reservation_destroy(&res);
		return fail("init recorded cap below the requested size");
	}
	if (res.bump_offset != 0U) {
		v8m_anchor_reservation_destroy(&res);
		return fail("init left bump_offset non-zero");
	}
	if (res.carve_calls != 0U || res.carve_failures != 0U ||
	    res.release_calls != 0U) {
		v8m_anchor_reservation_destroy(&res);
		return fail("init left counters non-zero");
	}
	if (v8m_anchor_reservation_remaining(&res) != res.cap) {
		v8m_anchor_reservation_destroy(&res);
		return fail("remaining() != cap on a fresh reservation");
	}
	v8m_anchor_reservation_destroy(&res);
	if (res.base != NULL) {
		return fail("destroy left base non-NULL");
	}
	/* Tolerate NULL + double-destroy. */
	v8m_anchor_reservation_destroy(NULL);
	v8m_anchor_reservation_destroy(&res);

	/* Init with cap = 0 picks up the default. */
	if (v8m_anchor_reservation_init(&res, 0U) != 0) {
		return fail("init(0) failed to use default cap");
	}
	if (res.cap < V8M_ANCHOR_DEFAULT_CAP_BYTES) {
		v8m_anchor_reservation_destroy(&res);
		return fail("init(0) did not use default cap");
	}
	v8m_anchor_reservation_destroy(&res);

	if (v8m_anchor_reservation_init(NULL, 0U) != -22 /* -EINVAL */) {
		return fail("init(NULL) did not return -EINVAL");
	}
	return 0;
}

/* Carve must return aligned, non-overlapping, writable slots and
 * advance the bump pointer. */
static int check_carve_basic(void)
{
	struct v8m_anchor_reservation res;
	if (v8m_anchor_reservation_init(&res, TEST_CAP_BYTES) != 0) {
		return fail("init failed for carve test");
	}
	int result = 0;
	void *first = v8m_anchor_reservation_carve(&res, TEST_CARVE_BYTES,
						   TEST_ALIGNMENT);
	if (first == NULL) {
		result = fail("first carve returned NULL");
		goto out;
	}
	if (((uintptr_t)first & (TEST_ALIGNMENT - 1U)) != 0U) {
		result = fail("first carve not aligned to requested boundary");
		goto out;
	}
	if (!v8m_anchor_reservation_owns(&res, first)) {
		result = fail("owns() did not recognise the carve");
		goto out;
	}
	/* Writable: a single byte write must not fault. */
	*(volatile char *)first = (char)0xCD;
	if (*(const volatile char *)first != (char)0xCD) {
		result = fail("write through carve did not stick");
		goto out;
	}

	void *second = v8m_anchor_reservation_carve(&res, TEST_CARVE_BYTES,
						    TEST_ALIGNMENT);
	if (second == NULL) {
		result = fail("second carve returned NULL");
		goto out;
	}
	if (second == first) {
		result = fail("two carves returned the same address");
		goto out;
	}
	uintptr_t gap = (uintptr_t)second - (uintptr_t)first;
	if (gap < TEST_CARVE_BYTES) {
		result = fail("second carve overlaps first");
		goto out;
	}
	if (res.carve_calls != 2U) {
		result = fail("carve_calls did not advance to 2");
		goto out;
	}
	if (v8m_anchor_reservation_remaining(&res) >= res.cap) {
		result = fail("remaining() did not decrease after carves");
		goto out;
	}
out:
	v8m_anchor_reservation_destroy(&res);
	return result;
}

/* A carve that exceeds the remaining cap must return NULL and bump
 * carve_failures, leaving the bump pointer unmoved. */
static int check_carve_exhaustion(void)
{
	struct v8m_anchor_reservation res;
	if (v8m_anchor_reservation_init(&res, TEST_CAP_BYTES) != 0) {
		return fail("init failed for exhaustion test");
	}
	int result = 0;
	/* Exhaust the reservation in cap-sized chunks. */
	if (v8m_anchor_reservation_carve(&res, res.cap, TEST_ALIGNMENT) ==
	    NULL) {
		result = fail(
		    "cap-sized carve returned NULL on a fresh reservation");
		goto out;
	}
	size_t bump_after_fill = res.bump_offset;
	const void *overflow = v8m_anchor_reservation_carve(
	    &res, TEST_CARVE_BYTES, TEST_ALIGNMENT);
	if (overflow != NULL) {
		result = fail("post-fill carve returned non-NULL");
		goto out;
	}
	if (res.carve_failures != 1U) {
		result = fail("carve_failures did not advance on overflow");
		goto out;
	}
	if (res.bump_offset != bump_after_fill) {
		result = fail("overflow carve mutated bump_offset");
		goto out;
	}
	if (v8m_anchor_reservation_remaining(&res) != 0U) {
		result = fail("remaining() did not report 0 after fill");
		goto out;
	}
out:
	v8m_anchor_reservation_destroy(&res);
	return result;
}

/* Release must madvise + mprotect(PROT_NONE) the slot AND not
 * recycle it (bump-only by design). The bump pointer stays put;
 * a follow-on carve gets a fresh slot past the bump. */
static int check_release_does_not_recycle(void)
{
	struct v8m_anchor_reservation res;
	if (v8m_anchor_reservation_init(&res, TEST_CAP_BYTES) != 0) {
		return fail("init failed for release test");
	}
	int result = 0;
	void *first = v8m_anchor_reservation_carve(&res, TEST_CARVE_BYTES,
						   TEST_ALIGNMENT);
	const void *second = v8m_anchor_reservation_carve(
	    &res, TEST_CARVE_BYTES, TEST_ALIGNMENT);
	if (first == NULL || second == NULL) {
		result = fail("setup carves returned NULL");
		goto out;
	}
	size_t bump_pre_release = res.bump_offset;
	if (!v8m_anchor_reservation_release(&res, first, TEST_CARVE_BYTES)) {
		result = fail("release of valid carve was rejected");
		goto out;
	}
	if (res.release_calls != 1U) {
		result = fail("release_calls did not advance");
		goto out;
	}
	if (res.bump_offset != bump_pre_release) {
		result = fail("release regressed the bump pointer");
		goto out;
	}
	const void *third = v8m_anchor_reservation_carve(&res, TEST_CARVE_BYTES,
							 TEST_ALIGNMENT);
	if (third == NULL) {
		result = fail("post-release carve returned NULL");
		goto out;
	}
	if (third == first) {
		result = fail("post-release carve recycled the released slot");
		goto out;
	}
out:
	v8m_anchor_reservation_destroy(&res);
	return result;
}

/* Release rejects pointers that lie outside the reservation and
 * pointers whose extent runs past the reservation's end. */
static int check_release_rejects_foreign(void)
{
	struct v8m_anchor_reservation res;
	if (v8m_anchor_reservation_init(&res, TEST_CAP_BYTES) != 0) {
		return fail("init failed for foreign-release test");
	}
	int result = 0;
	int local = 0;
	if (v8m_anchor_reservation_release(&res, &local, sizeof(local))) {
		result = fail("release accepted a stack-local pointer");
		goto out;
	}
	void *carved = v8m_anchor_reservation_carve(&res, TEST_CARVE_BYTES,
						    TEST_ALIGNMENT);
	if (carved == NULL) {
		result = fail("carve for extent test returned NULL");
		goto out;
	}
	/* Length runs past the reservation end. */
	if (v8m_anchor_reservation_release(&res, carved, res.cap + 1U)) {
		result = fail("release accepted out-of-extent length");
		goto out;
	}
	if (v8m_anchor_reservation_release(&res, NULL, TEST_CARVE_BYTES)) {
		result = fail("release accepted NULL pointer");
		goto out;
	}
	if (v8m_anchor_reservation_release(&res, carved, 0U)) {
		result = fail("release accepted zero bytes");
		goto out;
	}
out:
	v8m_anchor_reservation_destroy(&res);
	return result;
}

/* owns() returns false on NULL, on pointers before base, and on
 * pointers past the reservation end. */
static int check_owns_predicate(void)
{
	struct v8m_anchor_reservation res;
	if (v8m_anchor_reservation_init(&res, TEST_CAP_BYTES) != 0) {
		return fail("init failed for owns test");
	}
	int result = 0;
	if (v8m_anchor_reservation_owns(&res, NULL)) {
		result = fail("owns(NULL) returned true");
		goto out;
	}
	if (v8m_anchor_reservation_owns(NULL, res.base)) {
		result = fail("owns on NULL res returned true");
		goto out;
	}
	const char *first_byte = (const char *)res.base;
	const char *last_byte = first_byte + res.cap - 1U;
	const char *one_past = first_byte + res.cap;
	if (!v8m_anchor_reservation_owns(&res, first_byte)) {
		result = fail("owns(first byte) returned false");
		goto out;
	}
	if (!v8m_anchor_reservation_owns(&res, last_byte)) {
		result = fail("owns(last byte) returned false");
		goto out;
	}
	if (v8m_anchor_reservation_owns(&res, one_past)) {
		result = fail("owns(one past end) returned true");
		goto out;
	}
out:
	v8m_anchor_reservation_destroy(&res);
	return result;
}

/* Non-power-of-two alignment is rejected with NULL and a failure
 * counter bump. */
static int check_carve_rejects_bad_alignment(void)
{
	struct v8m_anchor_reservation res;
	if (v8m_anchor_reservation_init(&res, TEST_CAP_BYTES) != 0) {
		return fail("init failed for bad-alignment test");
	}
	const void *got =
	    v8m_anchor_reservation_carve(&res, TEST_CARVE_BYTES, 3U);
	int result = 0;
	if (got != NULL) {
		result = fail("carve accepted non-power-of-two alignment");
	}
	v8m_anchor_reservation_destroy(&res);
	return result;
}

int main(void)
{
	int result = 0;
	result |= check_init_destroy();
	result |= check_carve_basic();
	result |= check_carve_exhaustion();
	result |= check_release_does_not_recycle();
	result |= check_release_rejects_foreign();
	result |= check_owns_predicate();
	result |= check_carve_rejects_bad_alignment();
	if (result == 0) {
		(void)printf("test_anchor_reservation: OK\n");
	}
	return result;
}
