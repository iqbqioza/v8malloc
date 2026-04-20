/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Exhaustive round-trip test for the size-class machinery. For every
 * request in [1, V8M_LARGE_MAX_SIZE], v8m_size_class() must return
 * the smallest class whose byte size fits the request, and every
 * larger request must route to V8M_CLASS_HUGE.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "v8m_size_class.h"

static int fail_table(const char *msg)
{
	(void)fprintf(stderr, "test_size_class: %s\n", msg);
	return 1;
}

static int fail_round_trip(size_t req, uint32_t cls, const char *msg)
{
	(void)fprintf(stderr, "test_size_class: req=%zu class=%" PRIu32 " %s\n",
		      req, cls, msg);
	return 1;
}

int main(void)
{
	/* Table sanity: strictly monotonic, pinned boundary values. */
	for (uint32_t cls = 1; cls < V8M_NUM_SIZE_CLASSES; cls++) {
		if (v8m_class_to_size[cls] <= v8m_class_to_size[cls - 1]) {
			return fail_table("class_to_size not monotonic");
		}
	}
	if (v8m_class_to_size[0] != V8M_TINY_ALIGNMENT) {
		return fail_table("class 0 != 8");
	}
	if (v8m_class_to_size[7] != V8M_TINY_MAX_SIZE) {
		return fail_table("class 7 != 64");
	}
	if (v8m_class_to_size[V8M_SMALL_FIRST_CLASS] != 80) {
		return fail_table("class 8 != 80");
	}
	if (v8m_class_to_size[11] != 128) {
		return fail_table("class 11 != 128");
	}
	if (v8m_class_to_size[31] != V8M_SMALL_MAX_SIZE) {
		return fail_table("class 31 != 4096");
	}
	if (v8m_class_to_size[V8M_MEDIUM_FIRST_CLASS] != 8192) {
		return fail_table("class 32 != 8192");
	}
	if (v8m_class_to_size[V8M_LARGE_LAST_CLASS] != V8M_LARGE_MAX_SIZE) {
		return fail_table("largest class size drifted");
	}

	/* size == 0 maps to the smallest Tiny class (8B). */
	if (v8m_size_class(0) != 0) {
		return fail_table("malloc(0) did not map to class 0");
	}

	/* Round-trip: smallest-fit invariant across every size we serve
	 * from the class table. */
	for (size_t req = 1; req <= V8M_LARGE_MAX_SIZE; req++) {
		uint32_t cls = v8m_size_class(req);
		if (cls >= V8M_NUM_SIZE_CLASSES) {
			return fail_round_trip(
			    req, cls,
			    "class index out of range on non-Huge path");
		}
		if (v8m_class_to_size[cls] < req) {
			return fail_round_trip(req, cls,
					       "class byte size < request");
		}
		if (cls > 0 && v8m_class_to_size[cls - 1] >= req) {
			return fail_round_trip(
			    req, cls, "previous class already fits request");
		}
	}

	/* Huge path: anything strictly above V8M_LARGE_MAX_SIZE. */
	if (v8m_size_class((size_t)V8M_LARGE_MAX_SIZE + 1U) != V8M_CLASS_HUGE) {
		return fail_table(
		    "size just past Huge boundary not routed to Huge");
	}
	if (v8m_size_class(SIZE_MAX) != V8M_CLASS_HUGE) {
		return fail_table("SIZE_MAX not routed to Huge");
	}

	return 0;
}
