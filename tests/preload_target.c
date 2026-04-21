/* SPDX-License-Identifier: Apache-2.0 */
/*
 * LD_PRELOAD target. Compiled as a standalone program with no
 * compile-time dependency on libv8malloc; the test runner injects
 * the library via LD_PRELOAD and verifies two things:
 *
 *   1. v8malloc's constructor ran — confirmed by resolving the
 *      public v8m_version symbol via dlsym(RTLD_DEFAULT, ...). If
 *      LD_PRELOAD didn't load us, the lookup returns NULL.
 *
 *   2. The standard malloc family routes through v8malloc — every
 *      malloc/realloc/free below hits our public override (the
 *      target was never linked against libc allocator hooks; the
 *      LD_PRELOAD path is the only way the symbols resolve to us).
 *
 * The target deliberately exercises every backend the dispatcher
 * routes to so that a regression in the symbol-resolution layer
 * for any one path surfaces immediately.
 */

/* dlfcn.h's RTLD_DEFAULT requires _GNU_SOURCE; the build sets it
 * via target_compile_definitions for the rest of the test suite,
 * but preload_target deliberately omits the global -D_GNU_SOURCE
 * to keep its compile flags as close as possible to a third-party
 * program LD_PRELOAD-ing libv8malloc. The reserved-identifier
 * lints are the cost of touching the feature-test macro directly. */
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 *             readability-identifier-naming) */
#define _GNU_SOURCE
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 *           readability-identifier-naming) */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg)
{
	(void)fprintf(stderr, "preload_target: %s\n", msg);
	return 1;
}

/* Sizes spanning slab Tiny / Small, buddy Medium, large mmap, and
 * huge mmap. Same set as test_threading so the two tests share a
 * single point of truth for backend coverage. */
static const size_t g_sizes[] = {
    16,
    256,
    4096,
    16384,
    65536,
    (size_t)256 * 1024,
    ((size_t)2 * 1024 * 1024) + 4096U,
};

int main(void)
{
	/* Step 1: confirm v8malloc was preloaded. dlsym on
	 * RTLD_DEFAULT walks every loaded object in search order; the
	 * lookup succeeds only when libv8malloc.so is in the process
	 * image. The cast routes through `union` to dodge the
	 * function-pointer-from-void* warning glibc declares on the
	 * dlsym return. */
	/* dlsym/dlerror are flagged MT-unsafe by clang-tidy; the
	 * single-threaded preload_target is exactly the case where
	 * the warning carries no signal. */
	/* NOLINTBEGIN(concurrency-mt-unsafe) */
	(void)dlerror();
	void *raw = dlsym(RTLD_DEFAULT, "v8m_version");
	if (raw == NULL) {
		const char *err = dlerror();
		(void)fprintf(stderr,
			      "preload_target: v8m_version not found via "
			      "dlsym (LD_PRELOAD missing?): %s\n",
			      err != NULL ? err : "(no error)");
		return 1;
	}
	/* NOLINTEND(concurrency-mt-unsafe) */
	union {
		void *ptr;
		const char *(*fn)(void);
	} cast;
	cast.ptr = raw;
	const char *version = cast.fn();
	if (version == NULL || version[0] == '\0') {
		return fail("v8m_version() returned empty string");
	}
	(void)fprintf(stderr, "preload_target: v8malloc version %s\n", version);

	/* Step 2: round-trip allocations across every backend. malloc
	 * resolves to v8m_malloc through the LD_PRELOAD chain; if it
	 * didn't, we'd be exercising libc malloc here and the test
	 * would silently pass without proving anything. The version
	 * check above is what makes that scenario detectable. */
	size_t count = sizeof(g_sizes) / sizeof(g_sizes[0]);
	for (size_t i = 0; i < count; i++) {
		size_t bytes = g_sizes[i];
		unsigned char *ptr = malloc(bytes);
		if (ptr == NULL) {
			return fail("malloc returned NULL");
		}
		(void)memset(ptr, (int)(i & 0xFFU), bytes);
		if (ptr[0] != (unsigned char)(i & 0xFFU) ||
		    ptr[bytes - 1U] != (unsigned char)(i & 0xFFU)) {
			free(ptr);
			return fail("memset readback mismatch");
		}
		size_t larger = bytes + 64U;
		unsigned char *grown = realloc(ptr, larger);
		if (grown == NULL) {
			free(ptr);
			return fail("realloc returned NULL");
		}
		if (grown[0] != (unsigned char)(i & 0xFFU)) {
			free(grown);
			return fail("realloc dropped the leading byte");
		}
		free(grown);
	}
	return 0;
}
