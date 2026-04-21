/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Libc fallback tests. Confirm that:
 *   1. The library constructor resolves the libc free / malloc
 *      pointers via dlsym(RTLD_NEXT, ...).
 *   2. v8m_libc_malloc returns a usable, libc-owned pointer.
 *   3. v8m_libc_free safely releases it.
 *   4. The dispatcher's foreign-pointer branch routes a libc-allocated
 *      pointer through the fallback when the public free() runs on it.
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
	/* A libc-allocated pointer has none of v8malloc's metadata. The
	 * public free() must therefore reach the foreign-pointer branch
	 * and forward to the captured libc free; if it instead tried to
	 * unmap or treat the memory as v8malloc-owned, the process would
	 * crash. We allocate a few sizes that span libc's small-bin /
	 * large-bin split to make the test less coincidence-sensitive. */
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
