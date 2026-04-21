/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Verify that the page-metadata machinery upholds the design
 * contract:
 *
 *   - v8m_ptr_to_meta(ptr) returns the page-aligned base for any
 *     pointer in the page, and rolls over at every page boundary.
 *   - v8m_page_meta_valid() distinguishes valid pages (magic present)
 *     from foreign pointers, including NULL.
 *   - The Tiny metadata extends v8m_page_meta with matching offsets,
 *     so generic code can read common fields through either type.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_internal.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_page_meta: %s\n", msg);
	return 1;
}

int main(void)
{
	/* Two consecutive 64 KiB-aligned pages so we can probe the
	 * intra-page and cross-page cases. Use the page heap (mmap)
	 * rather than libc's aligned_alloc — once v8m_api.c links the
	 * malloc/free hijacks, libc's aligned_alloc/free pair would
	 * route a fake page through v8m_dispatch_free, which would
	 * try to munmap heap memory after seeing the V8M_MAGIC. */
	void *raw = v8m_page_heap_alloc(2 * V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (raw == NULL) {
		return fail("page-heap alloc(2 pages) failed");
	}
	(void)memset(raw, 0xCC, 2 * V8M_PAGE_SIZE);

	struct v8m_page_meta *meta_a = (struct v8m_page_meta *)raw;
	struct v8m_page_meta *meta_b =
	    (struct v8m_page_meta *)((char *)raw + V8M_PAGE_SIZE);

	/* Page A is "valid"; page B is deliberately corrupted. */
	meta_a->magic = V8M_MAGIC;
	meta_a->size_class = 8;
	meta_a->object_size = 80;
	meta_a->capacity = 100;
	meta_b->magic = ~V8M_MAGIC;

	/* ptr_to_meta on each page base returns the base itself. */
	if (v8m_ptr_to_meta(meta_a) != meta_a) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("ptr_to_meta(base_a) != base_a");
	}
	if (v8m_ptr_to_meta(meta_b) != meta_b) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("ptr_to_meta(base_b) != base_b");
	}

	/* ptr_to_meta on every byte offset within page A maps back to A. */
	for (size_t off = 0; off < V8M_PAGE_SIZE; off++) {
		const void *probe = (const char *)meta_a + off;
		if (v8m_ptr_to_meta(probe) != meta_a) {
			v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
			return fail("ptr_to_meta drifted within page A");
		}
	}

	/* The byte immediately after page A belongs to page B. */
	if (v8m_ptr_to_meta((char *)meta_a + V8M_PAGE_SIZE) != meta_b) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("ptr_to_meta did not roll over at page boundary");
	}

	/* Magic verification: A is valid, B is not, NULL is not. */
	if (!v8m_page_meta_valid(meta_a)) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("valid page rejected by magic check");
	}
	if (v8m_page_meta_valid(meta_b)) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("corrupted page accepted by magic check");
	}
	/* cppcheck correctly constant-folds v8m_page_meta_valid(NULL) to
	 * false via inlining; the test exists to pin that behavior at
	 * runtime in case the implementation ever changes. */
	/* cppcheck-suppress knownConditionTrueFalse */
	if (v8m_page_meta_valid(NULL)) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("NULL accepted by magic check");
	}

	/* Tiny metadata layout parity: reading common fields through the
	 * base struct type must give the same values written through the
	 * Tiny struct type. */
	struct v8m_tiny_page_meta *tiny = (struct v8m_tiny_page_meta *)meta_b;
	tiny->magic = V8M_MAGIC;
	tiny->size_class = 0;
	tiny->object_size = 8;
	tiny->capacity = 7936;

	const struct v8m_page_meta *viewed = (const struct v8m_page_meta *)tiny;
	if (viewed->magic != V8M_MAGIC) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("tiny magic invisible via common-layout cast");
	}
	if (viewed->size_class != 0) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("tiny size_class invisible via common-layout cast");
	}
	if (viewed->object_size != 8) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail(
		    "tiny object_size invisible via common-layout cast");
	}
	if (viewed->capacity != 7936) {
		v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
		return fail("tiny capacity invisible via common-layout cast");
	}

	v8m_page_heap_free(raw, 2 * V8M_PAGE_SIZE);
	return 0;
}
