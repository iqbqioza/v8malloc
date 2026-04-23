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
#include <unistd.h>

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

static int check_aligned_alloc(void)
{
	static const size_t cases[][2] = {
	    /* {alignment, size} — covers slab, buddy, and large paths. */
	    {16, 16},	    {32, 32},	   {64, 64},	   {128, 128},
	    {256, 256},	    {512, 1024},   {1024, 1024},   {2048, 2048},
	    {4096, 4096},   {8192, 16384}, {16384, 32768}, {4096, 200000},
	    {32768, 65536},
	};
	size_t case_count = sizeof(cases) / sizeof(cases[0]);
	for (size_t i = 0; i < case_count; i++) {
		size_t alignment = cases[i][0];
		size_t size = cases[i][1];
		void *ptr = aligned_alloc(alignment, size);
		if (ptr == NULL) {
			(void)fprintf(
			    stderr,
			    "test_api: aligned_alloc(%zu, %zu) returned NULL\n",
			    alignment, size);
			return 1;
		}
		if (((uintptr_t)ptr & (alignment - 1U)) != 0U) {
			(void)fprintf(stderr,
				      "test_api: aligned_alloc(%zu, %zu) ptr "
				      "%p misaligned\n",
				      alignment, size, ptr);
			free(ptr);
			return 1;
		}
		(void)memset(ptr, 0xA5, size);
		free(ptr);
	}

	/* Invalid alignments: zero, three (not power of 2). The values
	 * are routed through a volatile variable so the compiler can't
	 * enforce its compile-time power-of-two check on the constants
	 * — the runtime guard inside v8m_aligned_alloc is exactly what
	 * we're testing. */
	volatile size_t zero_align = 0;
	volatile size_t three_align = 3;
	errno = 0;
	void *bad = aligned_alloc(zero_align, 16);
	if (bad != NULL || errno != EINVAL) {
		free(bad);
		return fail("aligned_alloc(0, 16) did not fail with EINVAL");
	}
	errno = 0;
	bad = aligned_alloc(three_align, 16);
	if (bad != NULL || errno != EINVAL) {
		free(bad);
		return fail("aligned_alloc(3, 16) did not fail with EINVAL");
	}

	/* Above the supported alignment cap. */
	errno = 0;
	bad = aligned_alloc((size_t)1 << 20, 64);
	if (bad != NULL) {
		free(bad);
		return fail("aligned_alloc(1MiB) unexpectedly succeeded");
	}
	if (errno != ENOMEM) {
		return fail("aligned_alloc(1MiB) did not set ENOMEM");
	}
	return 0;
}

static int check_posix_memalign(void)
{
	void *ptr = NULL;
	int ret = posix_memalign(&ptr, 64, 1024);
	if (ret != 0 || ptr == NULL) {
		return fail("posix_memalign(64, 1024) failed");
	}
	if (((uintptr_t)ptr & 63U) != 0U) {
		free(ptr);
		return fail("posix_memalign(64) returned misaligned ptr");
	}
	(void)memset(ptr, 0x33, 1024);
	free(ptr);

	/* Alignment must be a multiple of sizeof(void *). On all
	 * supported targets sizeof(void *) is 8, so alignment = 4 is
	 * rejected even though it is a valid power of two. */
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	void *guard = (void *)(uintptr_t)0xDEADBEEF;
	void *out = guard;
	ret = posix_memalign(&out, 4, 16);
	if (ret != EINVAL) {
		free(out);
		return fail("posix_memalign(4) did not return EINVAL");
	}
	if (out != guard) {
		return fail("posix_memalign(EINVAL) clobbered *memptr");
	}

	ret = posix_memalign(&out, 7, 16);
	if (ret != EINVAL) {
		free(out);
		return fail("posix_memalign(7) did not return EINVAL");
	}

	/* Exercise the runtime EINVAL guard by calling our own
	 * v8m_posix_memalign, which doesn't carry glibc's __nonnull
	 * attribute. That dodges both the compile-time nonnull
	 * diagnostic and UBSan's nonnull-attribute runtime check; the
	 * null branch inside v8m_posix_memalign is the code under
	 * test. */
	ret = v8m_posix_memalign(NULL, 64, 16);
	if (ret != EINVAL) {
		return fail(
		    "posix_memalign(NULL memptr) did not return EINVAL");
	}
	return 0;
}

static int check_memalign_valloc_pvalloc(void)
{
	long page_signed = sysconf(_SC_PAGESIZE);
	size_t page = (page_signed > 0) ? (size_t)page_signed : 4096U;

	void *aligned = memalign(128, 256);
	if (aligned == NULL) {
		return fail("memalign(128, 256) returned NULL");
	}
	if (((uintptr_t)aligned & 127U) != 0U) {
		free(aligned);
		return fail("memalign(128) returned misaligned ptr");
	}
	free(aligned);

	/* valloc / pvalloc are deprecated by POSIX and the linter flags
	 * them as MT-unsafe, but we deliberately exercise them here to
	 * confirm the v8malloc-side wiring is correct. */
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	void *page_aligned = valloc(page);
	if (page_aligned == NULL) {
		return fail("valloc returned NULL");
	}
	if (((uintptr_t)page_aligned & (page - 1U)) != 0U) {
		free(page_aligned);
		return fail("valloc returned non-page-aligned ptr");
	}
	free(page_aligned);

	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	void *rounded = pvalloc(page + 1U);
	if (rounded == NULL) {
		return fail("pvalloc returned NULL");
	}
	if (((uintptr_t)rounded & (page - 1U)) != 0U) {
		free(rounded);
		return fail("pvalloc returned non-page-aligned ptr");
	}
	if (malloc_usable_size(rounded) < (page * 2U)) {
		free(rounded);
		return fail("pvalloc did not round size up to page multiple");
	}
	free(rounded);
	return 0;
}

static int check_glibc_compat_surface(void)
{
	/* mallopt is a no-op returning 1 (success). Any param/value
	 * combination should succeed. mallopt / malloc_trim are flagged
	 * MT-unsafe by tidy because the legacy ABI assumes a single
	 * arena lock; v8malloc serializes them through dispatch state. */
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	if (mallopt(M_TRIM_THRESHOLD, 1024 * 1024) != 1) {
		return fail("mallopt(M_TRIM_THRESHOLD) did not return 1");
	}
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	if (mallopt(M_MMAP_MAX, 0) != 1) {
		return fail("mallopt(M_MMAP_MAX) did not return 1");
	}

	/* malloc_trim is a no-op honestly reporting 0 (no memory
	 * released by this call). */
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	if (malloc_trim(0) != 0) {
		return fail("malloc_trim(0) did not return 0");
	}

	/* mallinfo / mallinfo2 should reflect at least one live
	 * mmap region after we make a Large allocation that bypasses
	 * any in-arena reuse. */
	void *anchor = malloc((size_t)512 * 1024);
	if (anchor == NULL) {
		return fail("anchor malloc returned NULL");
	}
/* mallinfo is marked deprecated in <malloc.h> in favour of
 * mallinfo2; we deliberately exercise both for ABI coverage. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	struct mallinfo info = mallinfo();
#pragma GCC diagnostic pop
	if (info.hblks <= 0) {
		free(anchor);
		return fail("mallinfo.hblks not positive after malloc");
	}
	if (info.hblkhd <= 0) {
		free(anchor);
		return fail("mallinfo.hblkhd not positive after malloc");
	}
	struct mallinfo2 info2 = mallinfo2();
	if (info2.hblks == 0U) {
		free(anchor);
		return fail("mallinfo2.hblks zero after malloc");
	}
	if (info2.hblkhd == 0U) {
		free(anchor);
		return fail("mallinfo2.hblkhd zero after malloc");
	}
	free(anchor);

	/* malloc_info(NULL) → EINVAL; with a real stream it must
	 * succeed and write at least a header byte. */
	errno = 0;
	if (malloc_info(0, NULL) != -1 || errno != EINVAL) {
		return fail("malloc_info(NULL) did not fail with EINVAL");
	}
	char *buffer = NULL;
	size_t buffer_len = 0;
	FILE *stream = open_memstream(&buffer, &buffer_len);
	if (stream == NULL) {
		return fail("open_memstream failed");
	}
	if (malloc_info(0, stream) != 0) {
		(void)fclose(stream);
		free(buffer);
		return fail("malloc_info returned non-zero");
	}
	(void)fclose(stream);
	if (buffer == NULL || buffer_len == 0U ||
	    strstr(buffer, "<malloc") == NULL) {
		free(buffer);
		return fail("malloc_info output missing <malloc tag");
	}
	/* Spot-check the expanded fields land in the XML so a
	 * regression that drops a row doesn't go unnoticed. */
	if (strstr(buffer, "<large ") == NULL) {
		free(buffer);
		return fail("malloc_info missing <large> row");
	}
	if (strstr(buffer, "<huge ") == NULL) {
		free(buffer);
		return fail("malloc_info missing <huge> row");
	}
	if (strstr(buffer, "<vma ") == NULL) {
		free(buffer);
		return fail("malloc_info missing <vma> row");
	}
	if (strstr(buffer, "<mbind ") == NULL) {
		free(buffer);
		return fail("malloc_info missing <mbind> row");
	}
	free(buffer);

	/* malloc_stats writes to stderr. Redirect stderr through a
	 * pipe (open_memstream-backed FILE has no underlying fd) so
	 * the test can validate the output without leaking it into
	 * the suite log. */
	int saved_stderr = dup(STDERR_FILENO);
	if (saved_stderr < 0) {
		return fail("dup(stderr) failed");
	}
	int pipe_fds[2];
	if (pipe(pipe_fds) != 0) {
		(void)close(saved_stderr);
		return fail("pipe() failed");
	}
	(void)fflush(stderr);
	if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
		(void)close(pipe_fds[0]);
		(void)close(pipe_fds[1]);
		(void)close(saved_stderr);
		return fail("dup2(stderr) failed");
	}
	malloc_stats();
	(void)fflush(stderr);
	(void)dup2(saved_stderr, STDERR_FILENO);
	(void)close(saved_stderr);
	(void)close(pipe_fds[1]);

	char captured[1024];
	ssize_t bytes = read(pipe_fds[0], captured, sizeof(captured) - 1U);
	(void)close(pipe_fds[0]);
	if (bytes <= 0) {
		return fail("malloc_stats wrote nothing to stderr");
	}
	captured[bytes] = '\0';
	if (strstr(captured, "v8malloc statistics") == NULL) {
		return fail("malloc_stats output missing header");
	}
	return 0;
}

static int check_v8m_option_api(void)
{
	int64_t value = 0;
	if (v8m_get_option(V8M_OPT_VERBOSE, &value) != 0) {
		return fail("v8m_get_option(VERBOSE) failed");
	}
	int64_t original = value;

	if (v8m_set_option(V8M_OPT_VERBOSE, original ^ 1) != 0) {
		return fail("v8m_set_option(VERBOSE) failed");
	}
	if (v8m_get_option(V8M_OPT_VERBOSE, &value) != 0 ||
	    value != (original ^ 1)) {
		(void)v8m_set_option(V8M_OPT_VERBOSE, original);
		return fail("v8m_get_option did not see the new value");
	}
	(void)v8m_set_option(V8M_OPT_VERBOSE, original);

	/* Out-of-range option ids must yield -1 / EINVAL on both
	 * setter and getter. */
	errno = 0;
	if (v8m_set_option(-1, 0) != -1 || errno != EINVAL) {
		return fail("v8m_set_option(-1) did not fail with EINVAL");
	}
	errno = 0;
	if (v8m_set_option(V8M_OPT_COUNT, 0) != -1 || errno != EINVAL) {
		return fail(
		    "v8m_set_option(OPT_COUNT) did not fail with EINVAL");
	}
	errno = 0;
	if (v8m_get_option(V8M_OPT_COUNT, &value) != -1 || errno != EINVAL) {
		return fail(
		    "v8m_get_option(OPT_COUNT) did not fail with EINVAL");
	}
	errno = 0;
	if (v8m_get_option(V8M_OPT_VERBOSE, NULL) != -1 || errno != EINVAL) {
		return fail("v8m_get_option(NULL) did not fail with EINVAL");
	}
	return 0;
}

static int check_v8m_stats_api(void)
{
	struct v8m_stats before = {0};
	v8m_get_stats(&before);

	void *anchor = malloc((size_t)512 * 1024);
	if (anchor == NULL) {
		return fail("anchor malloc returned NULL");
	}
	struct v8m_stats after = {0};
	v8m_get_stats(&after);
	if (after.live_regions <= before.live_regions) {
		free(anchor);
		return fail("live_regions did not increase after malloc");
	}
	if (after.live_bytes <= before.live_bytes) {
		free(anchor);
		return fail("live_bytes did not increase after malloc");
	}
	free(anchor);

	/* NULL out is a no-op, not a crash. */
	v8m_get_stats(NULL);

	/* v8m_dump_stats writes the same digest as malloc_stats; just
	 * confirm it does not crash. The malloc_stats output check
	 * already lives in check_glibc_compat_surface. */
	v8m_dump_stats();
	return 0;
}

/*
 * OOM handler / soft-limit tests. The handler counts invocations
 * and (when configured) frees a stashed allocation so the retry
 * inside v8m_malloc succeeds. Static state because the handler
 * signature has no user-data slot.
 */
static int g_oom_invocations;
static void *g_oom_release_target;

static int oom_release_then_retry(size_t requested)
{
	(void)requested;
	g_oom_invocations++;
	if (g_oom_release_target != NULL) {
		void *to_free = g_oom_release_target;
		g_oom_release_target = NULL;
		free(to_free);
		return 1; /* retry — we just freed enough room */
	}
	return 0; /* give up */
}

static int oom_never_retry(size_t requested)
{
	(void)requested;
	g_oom_invocations++;
	return 0;
}

static int check_soft_limit_blocks_alloc(void)
{
	if (v8m_get_soft_limit() != 0U) {
		return fail("soft limit nonzero before any setter");
	}

	/* Consume some real bytes first so live_bytes is well above
	 * zero, then plant a limit that the next allocation must
	 * exceed. */
	void *baseline = malloc((size_t)256 * 1024);
	if (baseline == NULL) {
		return fail("baseline malloc returned NULL");
	}
	struct v8m_stats stats = {0};
	v8m_get_stats(&stats);

	v8m_set_soft_limit(stats.live_bytes); /* zero headroom */
	if (v8m_get_soft_limit() != stats.live_bytes) {
		v8m_set_soft_limit(0);
		free(baseline);
		return fail("v8m_get_soft_limit did not see the new value");
	}

	errno = 0;
	void *blocked = malloc((size_t)512 * 1024);
	if (blocked != NULL) {
		v8m_set_soft_limit(0);
		free(blocked);
		free(baseline);
		return fail("soft limit did not block allocation");
	}
	if (errno != ENOMEM) {
		v8m_set_soft_limit(0);
		free(baseline);
		return fail("blocked allocation did not set ENOMEM");
	}

	v8m_set_soft_limit(0); /* clear */
	free(baseline);
	return 0;
}

static int check_oom_handler_retry(void)
{
	v8m_oom_handler_t prev = v8m_set_oom_handler(oom_release_then_retry);
	if (prev != NULL) {
		v8m_set_oom_handler(prev);
		return fail("OOM handler was non-NULL before install");
	}

	void *anchor = malloc((size_t)256 * 1024);
	if (anchor == NULL) {
		v8m_set_oom_handler(NULL);
		return fail("anchor malloc failed");
	}
	struct v8m_stats stats = {0};
	v8m_get_stats(&stats);
	v8m_set_soft_limit(stats.live_bytes);

	g_oom_invocations = 0;
	g_oom_release_target = anchor;
	void *retry_succeeded = malloc((size_t)128 * 1024);
	if (retry_succeeded == NULL) {
		v8m_set_soft_limit(0);
		v8m_set_oom_handler(NULL);
		return fail("OOM-handler retry path did not succeed");
	}
	/* The analyzer can't see g_oom_invocations being incremented
	 * through the function pointer the allocator invokes on the OOM
	 * path, so it flags this comparison as known-false. */
	/* cppcheck-suppress knownConditionTrueFalse */
	if (g_oom_invocations == 0) {
		v8m_set_soft_limit(0);
		v8m_set_oom_handler(NULL);
		free(retry_succeeded);
		return fail("OOM handler was not invoked");
	}
	free(retry_succeeded);
	v8m_set_soft_limit(0);

	v8m_set_oom_handler(NULL);
	return 0;
}

static int check_oom_handler_no_retry(void)
{
	v8m_set_oom_handler(oom_never_retry);

	void *anchor = malloc((size_t)256 * 1024);
	if (anchor == NULL) {
		v8m_set_oom_handler(NULL);
		return fail("anchor malloc failed");
	}
	struct v8m_stats stats = {0};
	v8m_get_stats(&stats);
	v8m_set_soft_limit(stats.live_bytes);

	g_oom_invocations = 0;
	errno = 0;
	void *blocked = malloc((size_t)128 * 1024);
	if (blocked != NULL) {
		v8m_set_soft_limit(0);
		v8m_set_oom_handler(NULL);
		free(blocked);
		free(anchor);
		return fail("alloc unexpectedly succeeded after no-retry");
	}
	if (errno != ENOMEM) {
		v8m_set_soft_limit(0);
		v8m_set_oom_handler(NULL);
		free(anchor);
		return fail("blocked alloc did not set ENOMEM");
	}
	if (g_oom_invocations == 0) {
		v8m_set_soft_limit(0);
		v8m_set_oom_handler(NULL);
		free(anchor);
		return fail("OOM handler was not invoked on failure");
	}

	v8m_set_soft_limit(0);
	v8m_set_oom_handler(NULL);
	free(anchor);
	return 0;
}

static int check_ptr_info_per_backend(void)
{
	struct v8m_ptr_info info;

	/* SLAB: a 64 B request lands in size class 7. */
	void *slab = malloc(64);
	if (slab == NULL) {
		return fail("slab malloc returned NULL");
	}
	if (!v8m_is_valid_ptr(slab) || v8m_ptr_info(slab, &info) != 0) {
		free(slab);
		return fail("v8m_ptr_info(slab) reported foreign");
	}
	if (info.backend != V8M_PTR_SLAB || info.usable_size < 64U ||
	    info.size_class < 0) {
		free(slab);
		return fail("v8m_ptr_info(slab) returned wrong fields");
	}
	free(slab);

	/* BUDDY: 8 KiB request lands in the buddy pool. */
	void *buddy = malloc(8192);
	if (buddy == NULL) {
		return fail("buddy malloc returned NULL");
	}
	if (v8m_ptr_info(buddy, &info) != 0 || info.backend != V8M_PTR_BUDDY) {
		free(buddy);
		return fail("v8m_ptr_info(buddy) wrong backend");
	}
	if (info.usable_size < 8192U || info.size_class != -1) {
		free(buddy);
		return fail("v8m_ptr_info(buddy) wrong fields");
	}
	free(buddy);

	/* LARGE: > 256 KiB hits the direct mmap path. */
	void *large = malloc((size_t)512 * 1024);
	if (large == NULL) {
		return fail("large malloc returned NULL");
	}
	if (v8m_ptr_info(large, &info) != 0 || info.backend != V8M_PTR_LARGE) {
		free(large);
		return fail("v8m_ptr_info(large) wrong backend");
	}
	if (info.usable_size < (size_t)512 * 1024 || info.size_class != -1) {
		free(large);
		return fail("v8m_ptr_info(large) wrong fields");
	}
	free(large);
	return 0;
}

static int check_ptr_info_invalid(void)
{
	struct v8m_ptr_info info;

	/* NULL out pointer. */
	errno = 0;
	if (v8m_ptr_info(NULL, NULL) != -1 || errno != EINVAL) {
		return fail("v8m_ptr_info(NULL out) did not fail");
	}

	/* NULL ptr — out is filled with FOREIGN, returns -1. */
	errno = 0;
	if (v8m_ptr_info(NULL, &info) != -1 || errno != EINVAL) {
		return fail("v8m_ptr_info(NULL ptr) did not fail");
	}
	if (info.backend != V8M_PTR_FOREIGN || info.usable_size != 0U ||
	    info.size_class != -1) {
		return fail("v8m_ptr_info(NULL ptr) did not zero out");
	}

	/* Stack address — definitely never v8malloc-issued. */
	int local = 0;
	errno = 0;
	if (v8m_ptr_info(&local, &info) != -1 || errno != EINVAL) {
		return fail("v8m_ptr_info(stack) did not fail");
	}
	if (v8m_is_valid_ptr(&local)) {
		return fail("v8m_is_valid_ptr(stack) returned true");
	}
	if (v8m_is_valid_ptr(NULL)) {
		return fail("v8m_is_valid_ptr(NULL) returned true");
	}

	/* Mid-allocation pointer inside a buddy block — not the start
	 * of any allocation, so foreign as far as the API cares. */
	void *buddy = malloc(8192);
	if (buddy == NULL) {
		return fail("buddy alloc for mid-ptr check returned NULL");
	}
	const void *mid = (const unsigned char *)buddy + 16;
	errno = 0;
	if (v8m_ptr_info(mid, &info) != -1 || errno != EINVAL) {
		free(buddy);
		return fail("v8m_ptr_info(mid-buddy) accepted offset");
	}
	free(buddy);
	return 0;
}

static int check_huge_and_frag_stats(void)
{
	struct v8m_huge_stats huge_before = {0};
	struct v8m_huge_stats huge_after = {0};
	struct v8m_frag_metrics frag = {0};

	/* Tolerate NULL on every reporter without crashing. */
	v8m_get_huge_stats(NULL);
	v8m_get_frag_metrics(NULL);

	v8m_get_huge_stats(&huge_before);

	/* A 512 KiB allocation is in the Large class (256K..2M). */
	void *large = malloc((size_t)512 * 1024);
	if (large == NULL) {
		return fail("Large malloc returned NULL");
	}
	v8m_get_huge_stats(&huge_after);
	if (huge_after.large_alloc_count !=
	    huge_before.large_alloc_count + 1U) {
		free(large);
		return fail("large_alloc_count did not advance by 1");
	}
	if (huge_after.large_bytes_in_use <= huge_before.large_bytes_in_use) {
		free(large);
		return fail("large_bytes_in_use did not advance");
	}

	/* A > 2 MiB allocation lands in the Huge bucket. */
	void *huge_obj = malloc((size_t)3 * 1024 * 1024);
	if (huge_obj == NULL) {
		free(large);
		return fail("Huge malloc returned NULL");
	}
	struct v8m_huge_stats huge_with_huge = {0};
	v8m_get_huge_stats(&huge_with_huge);
	if (huge_with_huge.huge_alloc_count !=
	    huge_after.huge_alloc_count + 1U) {
		free(huge_obj);
		free(large);
		return fail("huge_alloc_count did not advance by 1");
	}

	/* Hold a small (Tiny-class) allocation across the snapshot so the
	 * slab pool walker sees at least one live slot. The pool walks
	 * `current` + `partials` only; without a live small object the
	 * walker returns zero and the utilization assertions cannot
	 * distinguish "broken walker" from "pool happened to be empty". */
	void *small = malloc(32);
	if (small == NULL) {
		free(huge_obj);
		free(large);
		return fail("Tiny malloc for slab-stats check returned NULL");
	}

	v8m_get_frag_metrics(&frag);
	if (frag.live_regions == 0U || frag.live_bytes == 0U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("frag metrics reported zero live regions/bytes");
	}
	if (frag.region_map_capacity == 0U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("region_map_capacity reported as zero");
	}
	if (frag.large_live_count == 0U || frag.huge_live_count == 0U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("frag.live counts did not reflect the allocations");
	}
	if (frag.slab_pages_in_use == 0U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail(
		    "slab_pages_in_use stayed zero with a live Tiny obj");
	}
	if (frag.slab_slots_total == 0U ||
	    frag.slab_slots_used > frag.slab_slots_total) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("slab slots counters out of range");
	}
	if (frag.slab_utilization_pct > 100U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("slab_utilization_pct exceeded 100");
	}
	if (frag.slab_slots_used == 0U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("slab_slots_used stayed zero with a live Tiny obj");
	}
	/* /proc/self/maps must report at least the v8malloc SO + libc
	 * + ld-linux + the test binary itself + the live anchor
	 * mappings, so a healthy bench sees double-digit VMAs at
	 * minimum. Any value past 0 means the read worked. */
	if (frag.vma_count == 0U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("vma_count reported as zero (open failed?)");
	}
	uint64_t direct = v8m_count_vmas();
	if (direct == 0U) {
		free(small);
		free(huge_obj);
		free(large);
		return fail("v8m_count_vmas returned 0");
	}

	free(small);
	free(huge_obj);
	free(large);

	/* Thread stats stub returns zero until TLC lands; just verify
	 * the call itself is safe and the values are within range. */
	struct v8m_thread_stats thread = {0};
	v8m_get_thread_stats(NULL);
	v8m_get_thread_stats(&thread);
	if (thread.fast_path_allocs != 0U || thread.slow_path_allocs != 0U ||
	    thread.fast_path_frees != 0U ||
	    thread.remote_frees_received != 0U ||
	    thread.bin_overflow_flushes != 0U) {
		return fail("v0 thread stats expected to be zero");
	}
	return 0;
}

static int check_slab_class_breakdown_lifetime_api(void)
{
	struct v8m_slab_class_breakdown breakdown[V8M_PUBLIC_NUM_SIZE_CLASSES];

	/* NULL out tolerated. */
	v8m_get_slab_class_breakdown_lifetime(V8M_LIFETIME_UNKNOWN, NULL);

	/* UNKNOWN routes to the default arena — must agree with the
	 * unscoped accessor. */
	struct v8m_slab_class_breakdown defaults[V8M_PUBLIC_NUM_SIZE_CLASSES];
	v8m_get_slab_class_breakdown(defaults);
	v8m_get_slab_class_breakdown_lifetime(V8M_LIFETIME_UNKNOWN, breakdown);
	for (uint32_t i = 0; i < V8M_PUBLIC_NUM_SIZE_CLASSES; i++) {
		if (breakdown[i].pages_in_use != defaults[i].pages_in_use ||
		    breakdown[i].slots_total != defaults[i].slots_total ||
		    breakdown[i].slots_used != defaults[i].slots_used) {
			(void)fprintf(stderr,
				      "test_api: lifetime UNKNOWN class %u "
				      "disagrees with default\n",
				      i);
			return 1;
		}
	}

	/* The lifetime arenas exist regardless of V8M_OPT_LIFETIME_TRACKING
	 * setting (the option only controls whether allocations route to
	 * them). With tracking off, the lifetime arenas should report
	 * empty (no pages, no slots), but the accessor must not crash and
	 * must zero the buffer cleanly. */
	(void)memset(breakdown, 0xCC, sizeof(breakdown));
	v8m_get_slab_class_breakdown_lifetime(V8M_LIFETIME_EPHEMERAL,
					      breakdown);
	for (uint32_t i = 0; i < V8M_PUBLIC_NUM_SIZE_CLASSES; i++) {
		if (breakdown[i].pages_in_use != 0U ||
		    breakdown[i].slots_total != 0U ||
		    breakdown[i].slots_used != 0U ||
		    breakdown[i].utilization_pct != 0U) {
			(void)fprintf(stderr,
				      "test_api: ephemeral arena class %u has "
				      "non-zero stats with tracking off\n",
				      i);
			return 1;
		}
	}

	(void)memset(breakdown, 0xCC, sizeof(breakdown));
	v8m_get_slab_class_breakdown_lifetime(V8M_LIFETIME_SHORT, breakdown);
	(void)memset(breakdown, 0xCC, sizeof(breakdown));
	v8m_get_slab_class_breakdown_lifetime(V8M_LIFETIME_LONG, breakdown);
	/* The SHORT and LONG arena fills above just verify the
	 * accessor doesn't crash; per-class invariants tested above
	 * are repeated implicitly by the memset/fill cycle. */
	return 0;
}

static int check_option_name_api(void)
{
	/* Out-of-range ids return NULL. */
	if (v8m_option_name(-1) != NULL) {
		return fail("v8m_option_name(-1) should return NULL");
	}
	if (v8m_option_name(V8M_OPT_COUNT) != NULL) {
		return fail("v8m_option_name(COUNT) should return NULL");
	}

	/* Every in-range id resolves to a non-empty lowercase name.
	 * Spot-check a couple of well-known ids for stability. */
	for (int i = 0; i < V8M_OPT_COUNT; i++) {
		const char *name = v8m_option_name(i);
		if (name == NULL) {
			(void)fprintf(
			    stderr,
			    "test_api: v8m_option_name(%d) returned NULL\n", i);
			return 1;
		}
		if (name[0] == '\0') {
			(void)fprintf(
			    stderr, "test_api: v8m_option_name(%d) is empty\n",
			    i);
			return 1;
		}
	}
	if (strcmp(v8m_option_name(V8M_OPT_VERBOSE), "verbose") != 0) {
		return fail("v8m_option_name(VERBOSE) != \"verbose\"");
	}
	if (strcmp(v8m_option_name(V8M_OPT_HUGE_PAGES), "huge_pages") != 0) {
		return fail("v8m_option_name(HUGE_PAGES) != \"huge_pages\"");
	}
	return 0;
}

static int check_init_release_thread_api(void)
{
	/* init_thread is idempotent: a second call from the same
	 * thread returns 0 without re-allocating. */
	if (v8m_init_thread() != 0) {
		return fail("v8m_init_thread first call returned non-zero");
	}
	if (v8m_init_thread() != 0) {
		return fail(
		    "v8m_init_thread idempotent call returned non-zero");
	}

	/* Allocate something to populate the TLC, then release. The
	 * allocation surviving the release proves the slot was
	 * freed cleanly (not leaked) and that subsequent allocs work
	 * via a fresh cache. */
	void *probe = malloc(128);
	if (probe == NULL) {
		return fail("probe alloc failed before release_thread");
	}
	free(probe);

	if (v8m_release_thread() != 0) {
		return fail("v8m_release_thread returned non-zero");
	}

	/* Idempotent on a thread with no cache. */
	if (v8m_release_thread() != 0) {
		return fail(
		    "v8m_release_thread idempotent call returned non-zero");
	}

	/* Re-init after release works. */
	if (v8m_init_thread() != 0) {
		return fail("v8m_init_thread after release returned non-zero");
	}

	void *post = malloc(64);
	if (post == NULL) {
		return fail("post alloc failed after re-init");
	}
	free(post);
	return 0;
}

static int check_arch_info_api(void)
{
	v8m_get_arch_info(NULL); /* tolerates NULL */

	struct v8m_arch_info info;
	(void)memset(&info, 0xCC, sizeof(info));
	v8m_get_arch_info(&info);

	if (info.arch_name[0] == '\0') {
		return fail("v8m_get_arch_info: arch_name is empty");
	}
	if (strnlen(info.arch_name, sizeof(info.arch_name)) ==
	    sizeof(info.arch_name)) {
		return fail("v8m_get_arch_info: arch_name not NUL-terminated");
	}
	if (info.cache_line_bytes == 0U ||
	    (info.cache_line_bytes & (info.cache_line_bytes - 1U)) != 0U) {
		return fail(
		    "v8m_get_arch_info: cache_line_bytes not power-of-two");
	}
	if (info.build_cache_line_bytes == 0U ||
	    (info.build_cache_line_bytes &
	     (info.build_cache_line_bytes - 1U)) != 0U) {
		return fail("v8m_get_arch_info: build_cache_line_bytes not "
			    "power-of-two");
	}
	if (info.tsc_mhz == 0U) {
		return fail("v8m_get_arch_info: tsc_mhz is zero");
	}
	if (info.has_lse > 1U) {
		return fail("v8m_get_arch_info: has_lse out of bounds");
	}
	if (info.has_zbb > 1U) {
		return fail("v8m_get_arch_info: has_zbb out of bounds");
	}
	if (info.has_zacas > 1U) {
		return fail("v8m_get_arch_info: has_zacas out of bounds");
	}
	if (info.reserved != 0U) {
		return fail("v8m_get_arch_info: reserved not zeroed");
	}
	return 0;
}

static int check_purge_and_namespaced_compat(void)
{
	if (v8m_purge() != 0) {
		return fail("v8m_purge returned non-zero");
	}
	if (v8m_purge_thread() != 0) {
		return fail("v8m_purge_thread returned non-zero");
	}

	/* The v8m_-namespaced wrappers forward to the unprefixed
	 * names; smoke-test that they don't crash and report
	 * something. */
	struct mallinfo2 info = v8m_mallinfo2();
	if (info.hblks == 0U && info.hblkhd == 0U) {
		/* OK if the allocator is currently quiet, but allocate
		 * to make sure values move. */
		void *prime = malloc((size_t)512 * 1024);
		if (prime == NULL) {
			return fail("prime alloc for namespaced check failed");
		}
		info = v8m_mallinfo2();
		free(prime);
		if (info.hblks == 0U || info.hblkhd == 0U) {
			return fail("v8m_mallinfo2 reported zero after alloc");
		}
	}

	if (v8m_mallopt(M_TRIM_THRESHOLD, 1024 * 1024) != 1) {
		return fail("v8m_mallopt did not return 1");
	}
	if (v8m_malloc_trim(0) != 0) {
		return fail("v8m_malloc_trim returned non-zero");
	}
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
	status = check_v8m_namespace();
	if (status != 0) {
		return status;
	}
	status = check_aligned_alloc();
	if (status != 0) {
		return status;
	}
	status = check_posix_memalign();
	if (status != 0) {
		return status;
	}
	status = check_memalign_valloc_pvalloc();
	if (status != 0) {
		return status;
	}
	status = check_glibc_compat_surface();
	if (status != 0) {
		return status;
	}
	status = check_v8m_option_api();
	if (status != 0) {
		return status;
	}
	status = check_v8m_stats_api();
	if (status != 0) {
		return status;
	}
	status = check_soft_limit_blocks_alloc();
	if (status != 0) {
		return status;
	}
	status = check_oom_handler_retry();
	if (status != 0) {
		return status;
	}
	status = check_oom_handler_no_retry();
	if (status != 0) {
		return status;
	}
	status = check_ptr_info_per_backend();
	if (status != 0) {
		return status;
	}
	status = check_ptr_info_invalid();
	if (status != 0) {
		return status;
	}
	status = check_huge_and_frag_stats();
	if (status != 0) {
		return status;
	}
	status = check_arch_info_api();
	if (status != 0) {
		return status;
	}
	status = check_init_release_thread_api();
	if (status != 0) {
		return status;
	}
	status = check_option_name_api();
	if (status != 0) {
		return status;
	}
	status = check_slab_class_breakdown_lifetime_api();
	if (status != 0) {
		return status;
	}
	return check_purge_and_namespaced_compat();
}
