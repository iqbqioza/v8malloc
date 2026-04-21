/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Libc fallback tests. Confirm that:
 *   1. The library constructor resolves the libc free / malloc
 *      pointers via dlsym(RTLD_NEXT, ...).
 *   2. v8m_libc_malloc returns a usable, libc-owned pointer.
 *   3. v8m_libc_free safely releases it.
 *   4. The dispatcher's foreign-pointer branch routes a libc-allocated
 *      pointer through the captured libc free when the public free()
 *      runs on it. This was deferred until the page-heap region map
 *      arrived; the v8m_page_heap_owns predicate now makes the
 *      foreign-vs-ours decision before any unsafe page-base read.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_libc_fallback.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_libc_fallback: %s\n", msg);
	return 1;
}

static int check_fallback_resolved(void)
{
	if (!v8m_libc_fallback_ready()) {
		return fail("v8m_libc_fallback_ready returned false");
	}
	return 0;
}

static int check_libc_malloc_free_roundtrip(void)
{
	void *ptr = v8m_libc_malloc(128);
	if (ptr == NULL) {
		return fail("v8m_libc_malloc(128) returned NULL");
	}
	(void)memset(ptr, 0x5A, 128);
	v8m_libc_free(ptr);
	return 0;
}

static int check_public_free_routes_foreign_to_libc(void)
{
	/* A libc-allocated pointer is not in any page-heap region the
	 * region map tracks, so v8m_page_heap_owns must return false
	 * and the dispatcher must forward to the captured libc free
	 * without reading at the pointer's page base. The covered
	 * sizes span libc's small-bin / large-bin split so a single
	 * coincidence in the heap layout cannot mask a regression. */
	static const size_t sizes[] = {32, 256, 4096, 65536};
	size_t count = sizeof(sizes) / sizeof(sizes[0]);
	for (size_t i = 0; i < count; i++) {
		void *ptr = v8m_libc_malloc(sizes[i]);
		if (ptr == NULL) {
			return fail("v8m_libc_malloc returned NULL");
		}
		(void)memset(ptr, 0xC3, sizes[i]);
		free(ptr); /* v8malloc-overridden free → foreign branch */
	}
	return 0;
}

int main(void)
{
	/* Touch the public allocation surface so the static linker
	 * pulls in v8m_api.o (and its constructor) — without this, a
	 * test that only references symbols from v8m_libc_fallback.o
	 * leaves the constructor on the cutting-room floor and the
	 * dlsym capture never runs. */
	free(malloc(1));

	int status = check_fallback_resolved();
	if (status != 0) {
		return status;
	}
	status = check_libc_malloc_free_roundtrip();
	if (status != 0) {
		return status;
	}
	return check_public_free_routes_foreign_to_libc();
}
