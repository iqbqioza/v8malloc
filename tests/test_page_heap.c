/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Page heap tests. Verify alignment, input validation, read/write
 * access, distinct addresses across allocations, and that statistics
 * increment in step with mmap/munmap/madvise calls.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "v8m_config.h"
#include "v8m_internal.h"
#include "v8m_page_heap.h"
#include "v8m_thp.h"
#include "v8malloc/v8malloc.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_page_heap: %s\n", msg);
	return 1;
}

enum {
	BIG_ALIGNMENT = 2 * 1024 * 1024, /* 2 MiB (HugePage-sized) */
	MANY_REGIONS = 16,
	HUGEPAGE_BYTES = 2 * 1024 * 1024,
};

static bool is_aligned(const void *ptr, size_t alignment)
{
	return ((uintptr_t)ptr & (alignment - 1U)) == 0;
}

static int check_basic_alignment(void)
{
	void *ptr = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (ptr == NULL) {
		return fail("alloc(64K, 64K) returned NULL");
	}
	if (!is_aligned(ptr, V8M_PAGE_SIZE)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("alloc(64K, 64K) not V8M_PAGE_SIZE-aligned");
	}
	/* Must be readable and writable over the full requested range. */
	(void)memset(ptr, 0xAA, V8M_PAGE_SIZE);
	if (((const unsigned char *)ptr)[0] != 0xAAU ||
	    ((const unsigned char *)ptr)[V8M_PAGE_SIZE - 1U] != 0xAAU) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("allocated region not readable/writable");
	}
	v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
	return 0;
}

static int check_big_alignment(void)
{
	void *ptr = v8m_page_heap_alloc(BIG_ALIGNMENT, BIG_ALIGNMENT);
	if (ptr == NULL) {
		return fail("alloc(2M, 2M) returned NULL");
	}
	if (!is_aligned(ptr, BIG_ALIGNMENT)) {
		v8m_page_heap_free(ptr, BIG_ALIGNMENT);
		return fail("alloc(2M, 2M) not 2 MiB-aligned");
	}
	v8m_page_heap_free(ptr, BIG_ALIGNMENT);
	return 0;
}

static int check_invalid_arguments(void)
{
	if (v8m_page_heap_alloc(0, V8M_PAGE_SIZE) != NULL) {
		return fail("alloc(0, _) did not return NULL");
	}
	if (v8m_page_heap_alloc(V8M_PAGE_SIZE, 4096) != NULL) {
		return fail("alloc with alignment < V8M_PAGE_SIZE succeeded");
	}
	/* 3 * V8M_PAGE_SIZE is not a power of two. */
	if (v8m_page_heap_alloc(V8M_PAGE_SIZE, 3U * V8M_PAGE_SIZE) != NULL) {
		return fail("alloc with non-power-of-two alignment succeeded");
	}
	if (v8m_page_heap_alloc(SIZE_MAX, V8M_PAGE_SIZE) != NULL) {
		return fail("alloc with overflowing bytes+alignment succeeded");
	}
	return 0;
}

static int check_distinct_addresses(void)
{
	void *ptrs[MANY_REGIONS] = {NULL};
	for (int i = 0; i < MANY_REGIONS; i++) {
		ptrs[i] = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
		if (ptrs[i] == NULL) {
			for (int j = 0; j < i; j++) {
				v8m_page_heap_free(ptrs[j], V8M_PAGE_SIZE);
			}
			return fail("alloc returned NULL mid-sequence");
		}
		if (!is_aligned(ptrs[i], V8M_PAGE_SIZE)) {
			for (int j = 0; j <= i; j++) {
				v8m_page_heap_free(ptrs[j], V8M_PAGE_SIZE);
			}
			return fail("mid-sequence allocation not aligned");
		}
	}
	for (int i = 0; i < MANY_REGIONS; i++) {
		for (int j = i + 1; j < MANY_REGIONS; j++) {
			if (ptrs[i] == ptrs[j]) {
				for (int k = 0; k < MANY_REGIONS; k++) {
					v8m_page_heap_free(ptrs[k],
							   V8M_PAGE_SIZE);
				}
				return fail("two allocations returned the same "
					    "address");
			}
		}
	}
	for (int i = 0; i < MANY_REGIONS; i++) {
		v8m_page_heap_free(ptrs[i], V8M_PAGE_SIZE);
	}
	return 0;
}

static int check_stats_advance(void)
{
	struct v8m_page_heap_stats before = {0};
	struct v8m_page_heap_stats after = {0};

	v8m_page_heap_get_stats(&before);
	void *ptr = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (ptr == NULL) {
		return fail("alloc returned NULL during stats check");
	}
	v8m_page_heap_get_stats(&after);
	if (after.mmap_calls <= before.mmap_calls) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("mmap_calls did not increase");
	}
	if (after.bytes_mapped <= before.bytes_mapped) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("bytes_mapped did not increase");
	}

	v8m_page_heap_advise_dont_need(ptr, V8M_PAGE_SIZE);
	struct v8m_page_heap_stats after_advise = {0};
	v8m_page_heap_get_stats(&after_advise);
	if (after_advise.advise_calls <= after.advise_calls) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("advise_calls did not increase");
	}

	v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
	struct v8m_page_heap_stats after_free = {0};
	v8m_page_heap_get_stats(&after_free);
	if (after_free.munmap_calls <= after_advise.munmap_calls) {
		return fail("munmap_calls did not increase on free");
	}
	if (after_free.bytes_unmapped <= after_advise.bytes_unmapped) {
		return fail("bytes_unmapped did not increase on free");
	}
	return 0;
}

static int check_advise_and_free_tolerate_null(void)
{
	/* Tolerant of NULL / zero bytes — must not crash. */
	v8m_page_heap_free(NULL, V8M_PAGE_SIZE);
	v8m_page_heap_free(NULL, 0);
	v8m_page_heap_advise_dont_need(NULL, V8M_PAGE_SIZE);
	v8m_page_heap_advise_dont_need(NULL, 0);
	v8m_page_heap_get_stats(NULL);
	return 0;
}

static int check_owns_predicate(void)
{
	if (v8m_page_heap_owns(NULL)) {
		return fail("owns(NULL) returned true");
	}

	void *ptr = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (ptr == NULL) {
		return fail("alloc for owns check returned NULL");
	}
	if (!v8m_page_heap_owns(ptr)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(start of region) returned false");
	}
	/* Mid-region and last byte must both register as owned. */
	const unsigned char *mid =
	    (const unsigned char *)ptr + (V8M_PAGE_SIZE / 2U);
	if (!v8m_page_heap_owns(mid)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(mid) returned false");
	}
	const unsigned char *last =
	    (const unsigned char *)ptr + (V8M_PAGE_SIZE - 1U);
	if (!v8m_page_heap_owns(last)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(last byte) returned false");
	}
	/* The first byte just past the end is unowned. */
	const unsigned char *past = (const unsigned char *)ptr + V8M_PAGE_SIZE;
	if (v8m_page_heap_owns(past)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(end-exclusive) returned true");
	}

	/* A stack address — definitely never v8malloc-owned. */
	int stack_local = 0;
	if (v8m_page_heap_owns(&stack_local)) {
		v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
		return fail("owns(stack address) returned true");
	}

	v8m_page_heap_free(ptr, V8M_PAGE_SIZE);
	if (v8m_page_heap_owns(ptr)) {
		return fail("owns(freed pointer) returned true");
	}
	return 0;
}

static int check_hugepage_advice(void)
{
	/* Make sure config_init has run (the constructor does this).
	 * Force V8M_OPT_HUGE_PAGES=1 for the duration of the test so
	 * the at-threshold assertion holds regardless of the env-driven
	 * value (V8M_HUGE_PAGES=0 in the test runner's environment
	 * would otherwise flip the option and the assertion would
	 * spuriously fail). Restored at every exit. */
	v8m_config_init();
	int64_t saved = v8m_config_get(V8M_OPT_HUGE_PAGES);
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, 1);

	struct v8m_page_heap_stats before = {0};
	struct v8m_page_heap_stats after = {0};

	/* Sub-threshold allocation must NOT trigger the hugepage hint. */
	v8m_page_heap_get_stats(&before);
	void *small = v8m_page_heap_alloc(V8M_PAGE_SIZE, V8M_PAGE_SIZE);
	if (small == NULL) {
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("sub-threshold alloc returned NULL");
	}
	v8m_page_heap_get_stats(&after);
	if (after.hugepage_advise_calls != before.hugepage_advise_calls) {
		v8m_page_heap_free(small, V8M_PAGE_SIZE);
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("sub-threshold alloc emitted MADV_HUGEPAGE hint");
	}
	v8m_page_heap_free(small, V8M_PAGE_SIZE);

	/* At-threshold allocation must emit the hint. */
	v8m_page_heap_get_stats(&before);
	void *huge = v8m_page_heap_alloc(HUGEPAGE_BYTES, V8M_PAGE_SIZE);
	if (huge == NULL) {
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("at-threshold alloc returned NULL");
	}
	v8m_page_heap_get_stats(&after);
	if (after.hugepage_advise_calls <= before.hugepage_advise_calls) {
		v8m_page_heap_free(huge, HUGEPAGE_BYTES);
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail(
		    "hugepage_advise_calls did not advance at threshold");
	}
	v8m_page_heap_free(huge, HUGEPAGE_BYTES);

	/* Disabling V8M_OPT_HUGE_PAGES suppresses the hint even at
	 * threshold. */
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, 0);
	v8m_page_heap_get_stats(&before);
	void *huge_off = v8m_page_heap_alloc(HUGEPAGE_BYTES, V8M_PAGE_SIZE);
	if (huge_off == NULL) {
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("alloc with HUGE_PAGES=0 returned NULL");
	}
	v8m_page_heap_get_stats(&after);
	bool suppressed =
	    after.hugepage_advise_calls == before.hugepage_advise_calls;
	v8m_page_heap_free(huge_off, HUGEPAGE_BYTES);
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
	if (!suppressed) {
		return fail("HUGE_PAGES=0 did not suppress the hint");
	}
	return 0;
}

static int check_hugetlb_attempt(void)
{
	/* Force HUGE_PAGES=1 for the same reason as check_hugepage_advice
	 * — the env-driven case (V8M_HUGE_PAGES=0) would otherwise
	 * suppress the at-threshold attempt and the assertion would
	 * spuriously fail. Restored at every exit. */
	v8m_config_init();
	int64_t saved = v8m_config_get(V8M_OPT_HUGE_PAGES);
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, 1);

	/* A 2 MiB-multiple allocation aligned to 2 MiB triggers the
	 * MAP_HUGETLB primary attempt. We're agnostic to whether the
	 * kernel has reserved huge pages — what matters is that the
	 * attempt fired (calls counter advanced). On systems without
	 * reserved hugepages the failures counter advances in lockstep
	 * and the allocator falls back to the regular mmap path; on
	 * systems with reservations the failures counter stays put
	 * and the alloc returns directly from MAP_HUGETLB. Either
	 * outcome is acceptable here.
	 *
	 * Sub-2-MiB allocations or alignments below 2 MiB must NOT
	 * touch the calls counter. */
	struct v8m_page_heap_stats before = {0};
	struct v8m_page_heap_stats after = {0};

	/* Sub-threshold: 1 MiB at 64 KiB alignment must not attempt. */
	v8m_page_heap_get_stats(&before);
	void *small = v8m_page_heap_alloc((size_t)1024 * 1024, V8M_PAGE_SIZE);
	if (small == NULL) {
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("sub-threshold alloc returned NULL");
	}
	v8m_page_heap_get_stats(&after);
	if (after.hugetlb_alloc_calls != before.hugetlb_alloc_calls) {
		v8m_page_heap_free(small, (size_t)1024 * 1024);
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("sub-threshold alloc made a hugetlb attempt");
	}
	v8m_page_heap_free(small, (size_t)1024 * 1024);

	/* At-threshold: 2 MiB at 2 MiB alignment must attempt. */
	v8m_page_heap_get_stats(&before);
	void *huge = v8m_page_heap_alloc(HUGEPAGE_BYTES, HUGEPAGE_BYTES);
	if (huge == NULL) {
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("at-threshold alloc returned NULL");
	}
	v8m_page_heap_get_stats(&after);
	if (after.hugetlb_alloc_calls <= before.hugetlb_alloc_calls) {
		v8m_page_heap_free(huge, HUGEPAGE_BYTES);
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("hugetlb_alloc_calls did not advance");
	}
	v8m_page_heap_free(huge, HUGEPAGE_BYTES);

	/* HUGE_PAGES=0 suppresses the attempt even at threshold. */
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, 0);
	v8m_page_heap_get_stats(&before);
	void *suppressed_alloc =
	    v8m_page_heap_alloc(HUGEPAGE_BYTES, HUGEPAGE_BYTES);
	if (suppressed_alloc == NULL) {
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
		return fail("alloc with HUGE_PAGES=0 returned NULL");
	}
	v8m_page_heap_get_stats(&after);
	bool suppressed =
	    after.hugetlb_alloc_calls == before.hugetlb_alloc_calls;
	v8m_page_heap_free(suppressed_alloc, HUGEPAGE_BYTES);
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved);
	if (!suppressed) {
		return fail(
		    "HUGE_PAGES=0 did not suppress the hugetlb attempt");
	}
	return 0;
}

/*
 * Adaptive THP advice (huge-pages.md §5). Test-injects a tiny cold
 * threshold and a high seed EMA so the next THP-eligible alloc lands
 * in the demote branch; then injects a huge threshold so the
 * subsequent alloc lands in the promote branch. Verifies both
 * counters partition the work and that the test-only knob restores
 * cleanly.
 */
static int check_thp_adaptive_decision(void)
{
	v8m_config_init();
	/* The THP decision only fires when V8M_OPT_HUGE_PAGES is on
	 * (the page-heap path that calls `v8m_thp_decide_and_record`
	 * is gated on it). Force it on for the test so the env-driven
	 * V8M_HUGE_PAGES=0 case does not silently skip every alloc's
	 * decision and the demote/promote counters never advance. */
	int64_t saved_huge_pages = v8m_config_get(V8M_OPT_HUGE_PAGES);
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, 1);

	/* Save the live state so we can restore it; subsequent tests
	 * (and the rest of the suite) must observe the lazy-init
	 * baseline, not whatever this test left behind. */
	struct v8m_page_heap_stats saved = {0};
	v8m_page_heap_get_stats(&saved);
	uint64_t saved_threshold = saved.thp_cold_threshold_ticks;
	uint64_t saved_ema = saved.thp_ema_ticks;

	/* Force the demote branch: a threshold of 1 tick is exceeded by
	 * any positive inter-arrival delta. The injected EMA seeds the
	 * decision; the first alloc resets last_tsc (the inject helper
	 * clears it), and the second computes a real delta that lands
	 * past the threshold and triggers DEMOTE. */
	v8m_thp_test_inject(1U, 1000U);

	struct v8m_page_heap_stats before = {0};
	v8m_page_heap_get_stats(&before);
	void *first = v8m_page_heap_alloc(HUGEPAGE_BYTES, V8M_PAGE_SIZE);
	if (first == NULL) {
		v8m_thp_test_inject(saved_threshold, saved_ema);
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved_huge_pages);
		return fail("first THP-eligible alloc returned NULL");
	}
	void *second = v8m_page_heap_alloc(HUGEPAGE_BYTES, V8M_PAGE_SIZE);
	if (second == NULL) {
		v8m_page_heap_free(first, HUGEPAGE_BYTES);
		v8m_thp_test_inject(saved_threshold, saved_ema);
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved_huge_pages);
		return fail("second THP-eligible alloc returned NULL");
	}
	struct v8m_page_heap_stats after = {0};
	v8m_page_heap_get_stats(&after);
	uint64_t demote_delta =
	    after.thp_demote_calls - before.thp_demote_calls;
	v8m_page_heap_free(first, HUGEPAGE_BYTES);
	v8m_page_heap_free(second, HUGEPAGE_BYTES);
	if (demote_delta == 0U) {
		v8m_thp_test_inject(saved_threshold, saved_ema);
		(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved_huge_pages);
		return fail("DEMOTE branch did not fire under tiny threshold");
	}

	/* Force the promote branch: a huge threshold ensures any natural
	 * EMA stays well below it. */
	v8m_thp_test_inject(UINT64_MAX, 0U);
	v8m_page_heap_get_stats(&before);
	void *third = v8m_page_heap_alloc(HUGEPAGE_BYTES, V8M_PAGE_SIZE);
	void *fourth = v8m_page_heap_alloc(HUGEPAGE_BYTES, V8M_PAGE_SIZE);
	v8m_page_heap_get_stats(&after);
	uint64_t promote_delta =
	    after.thp_promote_calls - before.thp_promote_calls;
	uint64_t demote_delta_2 =
	    after.thp_demote_calls - before.thp_demote_calls;
	if (third != NULL) {
		v8m_page_heap_free(third, HUGEPAGE_BYTES);
	}
	if (fourth != NULL) {
		v8m_page_heap_free(fourth, HUGEPAGE_BYTES);
	}
	v8m_thp_test_inject(saved_threshold, saved_ema);
	(void)v8m_config_set(V8M_OPT_HUGE_PAGES, saved_huge_pages);
	if (promote_delta == 0U) {
		return fail("PROMOTE branch did not fire under huge threshold");
	}
	if (demote_delta_2 != 0U) {
		return fail("DEMOTE branch fired under huge threshold");
	}
	return 0;
}

/*
 * NUMA balance snapshot (numa.md §6.1). On a single-node host the
 * `imbalanced` flag must never flip — every byte lands on node 0,
 * so most_loaded_bytes always equals the average. On multi-node
 * hosts the structural invariants still hold (sum of per-node ==
 * total, most_loaded_bytes <= total). The accessor must tolerate
 * NULL and report a sensible node_count regardless.
 */
static int check_numa_balance_invariants(void)
{
	v8m_get_numa_balance(NULL); /* must not crash */

	struct v8m_numa_balance_stats snap = {0};
	v8m_get_numa_balance(&snap);

	if (snap.node_count == 0U) {
		return fail("node_count reported as zero");
	}
	uint64_t sum = 0;
	for (uint32_t node = 0; node < snap.node_count; node++) {
		sum += snap.per_node_bytes[node];
	}
	if (sum != snap.total_bytes) {
		return fail("sum(per_node_bytes) does not equal total_bytes");
	}
	if (snap.most_loaded_bytes > snap.total_bytes) {
		return fail("most_loaded_bytes exceeds total_bytes");
	}
	if (snap.most_loaded_node >= snap.node_count &&
	    snap.most_loaded_bytes > 0U) {
		return fail("most_loaded_node out of range");
	}
	if (snap.node_count == 1U && snap.imbalanced) {
		return fail("single-node host flagged as imbalanced");
	}
	if (snap.node_count > 0U &&
	    snap.average_bytes_per_node != snap.total_bytes / snap.node_count) {
		return fail("average_bytes_per_node disagrees with arithmetic");
	}

	/* Allocate + free a Large region; on a multi-NUMA host the
	 * per-node counter for the calling thread's node should
	 * advance during the live window and revert at free. On
	 * single-node hosts the counters stay at 0 (no bind happens),
	 * so we only sanity-check the no-leak invariant after free. */
	struct v8m_numa_balance_stats before = snap;
	void *region = v8m_page_heap_alloc(HUGEPAGE_BYTES, V8M_PAGE_SIZE);
	if (region == NULL) {
		return fail("alloc for balance test returned NULL");
	}
	struct v8m_numa_balance_stats during = {0};
	v8m_get_numa_balance(&during);
	v8m_page_heap_free(region, HUGEPAGE_BYTES);
	struct v8m_numa_balance_stats after = {0};
	v8m_get_numa_balance(&after);

	if (during.node_count > 1U &&
	    during.total_bytes < before.total_bytes + HUGEPAGE_BYTES) {
		return fail("multi-node alloc did not advance total_bytes");
	}
	if (after.total_bytes != before.total_bytes) {
		(void)fprintf(stderr,
			      "test_page_heap: per-node bytes leaked across "
			      "alloc/free (before=%llu after=%llu)\n",
			      (unsigned long long)before.total_bytes,
			      (unsigned long long)after.total_bytes);
		return 1;
	}
	return 0;
}

int main(void)
{
	int status = check_basic_alignment();
	if (status != 0) {
		return status;
	}
	status = check_big_alignment();
	if (status != 0) {
		return status;
	}
	status = check_invalid_arguments();
	if (status != 0) {
		return status;
	}
	status = check_distinct_addresses();
	if (status != 0) {
		return status;
	}
	status = check_stats_advance();
	if (status != 0) {
		return status;
	}
	/* check_advise_and_free_tolerate_null only ever returns 0;
	 * cppcheck flags the post-call status check as dead code. */
	(void)check_advise_and_free_tolerate_null();
	status = check_owns_predicate();
	if (status != 0) {
		return status;
	}
	status = check_hugepage_advice();
	if (status != 0) {
		return status;
	}
	status = check_hugetlb_attempt();
	if (status != 0) {
		return status;
	}
	status = check_thp_adaptive_decision();
	if (status != 0) {
		return status;
	}
	return check_numa_balance_invariants();
}
