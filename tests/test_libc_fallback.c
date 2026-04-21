/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Libc fallback tests. Confirm that:
 *   1. The library constructor resolves the libc free / malloc
 *      pointers via dlsym(RTLD_NEXT, ...).
 *   2. v8m_libc_malloc returns a usable, libc-owned pointer.
 *   3. v8m_libc_free safely releases it.
 *
 * A fourth check — running the public free() override on a
 * libc-allocated pointer to confirm the foreign-pointer branch
 * forwards to the captured libc free — is intentionally absent.
 * The dispatcher's foreign-vs-ours decision currently reads at the
 * pointer's page-aligned base, which can fault on a truly-foreign
 * pointer whose base sits in an unmapped page. Re-enabling that
 * route requires the page-heap region map (TODO.md open question
 * #1); until then, foreign frees are silently dropped, and the
 * helpers tested below stand ready for the cycle that wires them
 * back into the dispatcher behind a safe ownership check.
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
	return check_libc_malloc_free_roundtrip();
}
