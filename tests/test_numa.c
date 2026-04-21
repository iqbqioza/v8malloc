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
	return check_current_node();
}
