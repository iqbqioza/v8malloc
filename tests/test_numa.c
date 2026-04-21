/* SPDX-License-Identifier: Apache-2.0 */
/*
 * NUMA topology detection tests. Confirms that:
 *   - The init function is idempotent.
 *   - Node count is at least one (uniform-memory fallback when
 *     sysfs is absent).
 *   - Every CPU id within the configured cap maps to a node id
 *     strictly less than the reported node count.
 *   - Out-of-range CPU ids land on node 0 (the safe default).
 *   - The current-node helper returns a valid node id.
 *
 * The test does not assume more than one node — CI runners and
 * containers commonly present a single uniform node, and the
 * non-NUMA path must stay correct.
 */

#include <stdint.h>
#include <stdio.h>

#include "v8m_numa.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_numa: %s\n", msg);
	return 1;
}

static int check_node_count(void)
{
	v8m_numa_init();
	v8m_numa_init(); /* idempotent */
	uint32_t count = v8m_numa_node_count();
	if (count == 0U) {
		return fail("v8m_numa_node_count returned 0");
	}
	if (count > V8M_NUMA_MAX_NODES) {
		return fail("v8m_numa_node_count exceeded the cap");
	}
	return 0;
}

static int check_cpu_to_node_map(void)
{
	uint32_t count = v8m_numa_node_count();
	for (uint32_t cpu = 0; cpu < V8M_NUMA_MAX_CPUS; cpu++) {
		uint32_t node = v8m_numa_node_for_cpu(cpu);
		if (node >= count) {
			(void)fprintf(stderr,
				      "test_numa: cpu %u → node %u "
				      "exceeds node count %u\n",
				      cpu, node, count);
			return 1;
		}
	}
	if (v8m_numa_node_for_cpu(V8M_NUMA_MAX_CPUS) != 0U) {
		return fail("out-of-range CPU did not fall back to node 0");
	}
	if (v8m_numa_node_for_cpu(V8M_NUMA_MAX_CPUS + 1U) != 0U) {
		return fail("far out-of-range CPU did not fall back to node 0");
	}
	return 0;
}

static int check_current_node(void)
{
	uint32_t node = v8m_numa_current_node();
	if (node >= v8m_numa_node_count()) {
		return fail("current_node exceeds node count");
	}
	return 0;
}

static int check_distance_and_fallback(void)
{
	uint32_t count = v8m_numa_node_count();

	/* For every active node, rank 0 must equal the source node
	 * itself (distance to self is the smallest). Walking from
	 * rank 1 to count - 1 must produce a strictly non-decreasing
	 * distance sequence — that's the contract of the fallback
	 * order. */
	for (uint32_t from = 0; from < count; from++) {
		uint32_t self = v8m_numa_fallback_node(from, 0);
		if (self != from) {
			(void)fprintf(stderr,
				      "test_numa: fallback rank 0 from %u "
				      "is %u, expected %u\n",
				      from, self, from);
			return 1;
		}
		uint8_t prev_d = v8m_numa_node_distance(from, self);
		for (uint32_t rank = 1; rank < count; rank++) {
			uint32_t target = v8m_numa_fallback_node(from, rank);
			if (target >= count) {
				(void)fprintf(
				    stderr,
				    "test_numa: fallback %u/%u out of range\n",
				    from, rank);
				return 1;
			}
			uint8_t cur_d = v8m_numa_node_distance(from, target);
			if (cur_d < prev_d) {
				(void)fprintf(
				    stderr,
				    "test_numa: distance regressed at "
				    "%u rank %u: prev=%u cur=%u\n",
				    from, rank, prev_d, cur_d);
				return 1;
			}
			prev_d = cur_d;
		}
		/* Rank-saturation contract: ranks past the end return
		 * the source node itself. */
		if (v8m_numa_fallback_node(from, count) != from ||
		    v8m_numa_fallback_node(from, count + 100U) != from) {
			return fail("fallback rank past end did not saturate");
		}
	}

	/* Out-of-range source: returns the input unchanged. */
	if (v8m_numa_fallback_node(V8M_NUMA_MAX_NODES + 1, 0) !=
	    V8M_NUMA_MAX_NODES + 1) {
		return fail("fallback with out-of-range source misbehaved");
	}
	if (v8m_numa_node_distance(V8M_NUMA_MAX_NODES, 0) != 0U ||
	    v8m_numa_node_distance(0, V8M_NUMA_MAX_NODES) != 0U) {
		return fail("distance with out-of-range arg did not return 0");
	}
	return 0;
}

static int check_current_node_cache(void)
{
	/* Hammer the cached path with several refresh cycles' worth of
	 * calls. Every result must remain a valid node id; a stale
	 * cache that drifted out of [0, node_count) would fail this
	 * loop. The 4096-iteration bound covers four refreshes at the
	 * default 1024 interval, exercising both the cache-hit and
	 * cache-refresh branches. */
	uint32_t count = v8m_numa_node_count();
	for (int i = 0; i < 4096; i++) {
		uint32_t node = v8m_numa_current_node();
		if (node >= count) {
			(void)fprintf(stderr,
				      "test_numa: cached current_node %u "
				      "exceeded node count %u at iter %d\n",
				      node, count, i);
			return 1;
		}
	}
	return 0;
}

int main(void)
{
	int status = check_node_count();
	if (status != 0) {
		return status;
	}
	status = check_cpu_to_node_map();
	if (status != 0) {
		return status;
	}
	status = check_current_node();
	if (status != 0) {
		return status;
	}
	status = check_distance_and_fallback();
	if (status != 0) {
		return status;
	}
	return check_current_node_cache();
}
