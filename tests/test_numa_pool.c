/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-NUMA-node huge-page pool primitive. The dispatcher does not
 * route allocations through this pool yet; tests exercise the
 * primitive in isolation against the real page heap (huge pages
 * are mmap'd via `v8m_page_heap_alloc(V8M_HUGE_PAGE_SIZE,
 * V8M_HUGE_PAGE_SIZE)`).
 */

#include <pthread.h> /* IWYU pragma: keep — pthread_mutex_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "v8m_arch.h" /* V8M_CACHE_LINE_SIZE, V8M_HUGE_PAGE_SIZE */
#include "v8m_huge_slab.h" /* V8M_HUGE_SLABS_PER_HUGE */
#include "v8m_internal.h" /* V8M_PAGE_SIZE */
#include "v8m_numa_pool.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_numa_pool: %s\n", msg);
	return 1;
}

/* The per-node bookkeeping block must be cache-line aligned so two
 * threads working on adjacent nodes don't false-share. */
static int check_cacheline_alignment(void)
{
	if ((sizeof(struct v8m_numa_pool_node) % V8M_CACHE_LINE_SIZE) != 0U) {
		(void)fprintf(stderr,
			      "test_numa_pool: per-node size %zu is not a "
			      "multiple of cache line %zu — adjacent nodes "
			      "share lines\n",
			      sizeof(struct v8m_numa_pool_node),
			      (size_t)V8M_CACHE_LINE_SIZE);
		return 1;
	}
	if (offsetof(struct v8m_numa_pool, nodes) % V8M_CACHE_LINE_SIZE != 0U) {
		return fail("nodes[] offset is not cache-line aligned");
	}
	return 0;
}

static int check_init_destroy(void)
{
	struct v8m_numa_pool pool;
	if (v8m_numa_pool_init(&pool) != 0) {
		return fail("init returned non-zero");
	}
	for (uint32_t i = 0; i < 4; i++) {
		const struct v8m_numa_pool_node *node = &pool.nodes[i];
		if (node->numa_node != i) {
			return fail("init did not seed numa_node");
		}
		if (node->partials != NULL || node->fulls != NULL) {
			return fail("init left a non-empty list");
		}
		if (node->huge_pages_alive != 0U ||
		    node->slab_carve_calls != 0U ||
		    node->slab_release_calls != 0U) {
			return fail("init left non-zero counters");
		}
	}
	v8m_numa_pool_destroy(&pool);
	if (pool.nodes[0].numa_node != 0U &&
	    pool.nodes[0].huge_pages_alive != 0U) {
		return fail("destroy did not zero descriptor");
	}
	v8m_numa_pool_destroy(NULL); /* tolerate */
	v8m_numa_pool_destroy(&pool); /* double-destroy safe */
	if (v8m_numa_pool_init(NULL) != -22 /* -EINVAL */) {
		return fail("init(NULL) did not return -EINVAL");
	}
	return 0;
}

static int check_carve_basic(void)
{
	struct v8m_numa_pool pool;
	if (v8m_numa_pool_init(&pool) != 0) {
		return fail("init failed");
	}
	int result = 0;
	void *first = v8m_numa_pool_carve_slab(&pool, 0U);
	if (first == NULL) {
		result = fail("first carve returned NULL");
		goto out;
	}
	if (((uintptr_t)first & (V8M_PAGE_SIZE - 1U)) != 0U) {
		result = fail("first carve not page-aligned");
		goto out;
	}
	if (!v8m_numa_pool_owns(&pool, first)) {
		result = fail("owns() did not recognise the carve");
		goto out;
	}
	if (pool.nodes[0].huge_pages_alive != 1U) {
		result =
		    fail("first carve did not allocate exactly 1 huge page");
		goto out;
	}
	if (pool.nodes[0].slab_carve_calls != 1U) {
		result = fail("slab_carve_calls did not advance to 1");
		goto out;
	}

	const void *second = v8m_numa_pool_carve_slab(&pool, 0U);
	if (second == NULL || second == first) {
		result = fail("second carve returned NULL or duplicate");
		goto out;
	}
	if (pool.nodes[0].huge_pages_alive != 1U) {
		result = fail("second carve allocated a new huge page "
			      "before filling the first");
		goto out;
	}
out:
	v8m_numa_pool_destroy(&pool);
	return result;
}

/* Fill an entire huge page worth of slabs and confirm the next
 * carve allocates a fresh huge page (huge_pages_alive bumps to 2)
 * and the original descriptor migrated to the fulls list. */
static int check_full_huge_page_migration(void)
{
	struct v8m_numa_pool pool;
	if (v8m_numa_pool_init(&pool) != 0) {
		return fail("init failed");
	}
	int result = 0;
	void *slabs[V8M_HUGE_SLABS_PER_HUGE] = {NULL};
	for (uint32_t i = 0; i < V8M_HUGE_SLABS_PER_HUGE; i++) {
		slabs[i] = v8m_numa_pool_carve_slab(&pool, 0U);
		if (slabs[i] == NULL) {
			result = fail("carve returned NULL filling huge page");
			goto out;
		}
	}
	if (pool.nodes[0].huge_pages_alive != 1U) {
		result =
		    fail("filled-but-not-overflowed pool exceeded 1 huge page");
		goto out;
	}
	if (pool.nodes[0].fulls == NULL) {
		result = fail("filled descriptor did not migrate to fulls");
		goto out;
	}
	const void *overflow = v8m_numa_pool_carve_slab(&pool, 0U);
	if (overflow == NULL) {
		result = fail("overflow carve returned NULL");
		goto out;
	}
	if (pool.nodes[0].huge_pages_alive != 2U) {
		result =
		    fail("overflow carve did not allocate a 2nd huge page");
		goto out;
	}
out:
	v8m_numa_pool_destroy(&pool);
	return result;
}

/* Release returns a slab to the pool. After a full release of every
 * carve the pool's huge_pages_alive drops to zero (each emptied
 * descriptor returns its huge page to the OS). */
static int check_release_drains_pool(void)
{
	struct v8m_numa_pool pool;
	if (v8m_numa_pool_init(&pool) != 0) {
		return fail("init failed");
	}
	int result = 0;
	void *slabs[V8M_HUGE_SLABS_PER_HUGE] = {NULL};
	for (uint32_t i = 0; i < V8M_HUGE_SLABS_PER_HUGE; i++) {
		slabs[i] = v8m_numa_pool_carve_slab(&pool, 0U);
		if (slabs[i] == NULL) {
			result = fail("setup carve returned NULL");
			goto out;
		}
	}
	for (uint32_t i = 0; i < V8M_HUGE_SLABS_PER_HUGE; i++) {
		if (!v8m_numa_pool_release_slab(&pool, slabs[i])) {
			result = fail("release rejected a valid slab");
			goto out;
		}
	}
	if (pool.nodes[0].huge_pages_alive != 0U) {
		result = fail("huge_pages_alive nonzero after full drain");
		goto out;
	}
	if (pool.nodes[0].partials != NULL || pool.nodes[0].fulls != NULL) {
		result = fail("lists not empty after full drain");
		goto out;
	}
out:
	v8m_numa_pool_destroy(&pool);
	return result;
}

/* Release rejects pointers that no node owns. */
static int check_release_rejects_foreign(void)
{
	struct v8m_numa_pool pool;
	if (v8m_numa_pool_init(&pool) != 0) {
		return fail("init failed");
	}
	int local = 0;
	bool released = v8m_numa_pool_release_slab(&pool, &local);
	v8m_numa_pool_destroy(&pool);
	if (released) {
		return fail("release accepted a foreign pointer");
	}
	return 0;
}

/* Out-of-range numa_node returns NULL on carve. */
static int check_carve_out_of_range(void)
{
	struct v8m_numa_pool pool;
	if (v8m_numa_pool_init(&pool) != 0) {
		return fail("init failed");
	}
	const void *got = v8m_numa_pool_carve_slab(&pool, 0xFFFFU);
	v8m_numa_pool_destroy(&pool);
	if (got != NULL) {
		return fail("out-of-range carve returned non-NULL");
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_cacheline_alignment();
	result |= check_init_destroy();
	result |= check_carve_basic();
	result |= check_full_huge_page_migration();
	result |= check_release_drains_pool();
	result |= check_release_rejects_foreign();
	result |= check_carve_out_of_range();
	if (result == 0) {
		(void)printf("test_numa_pool: OK\n");
	}
	return result;
}
