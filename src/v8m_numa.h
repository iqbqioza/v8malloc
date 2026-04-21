/* SPDX-License-Identifier: Apache-2.0 */
/*
 * NUMA topology detection. Reads /sys/devices/system/node once at
 * library init and caches a CPU → node mapping that lookups never
 * need to revisit the kernel for. Per-NUMA pool sharding (the layer
 * that uses this map to route allocations to a node-local arena)
 * lands in a follow-on cycle; this module is the foundation it
 * stands on.
 *
 * On systems where sysfs is absent (containers without /sys, NUMA
 * disabled in the kernel) the module reports a single node and
 * maps every CPU to node 0 — every caller can treat the topology
 * as uniform and the rest of the allocator behaves identically to
 * its non-NUMA path.
 *
 * Design source: .claude/docs/numa.md.
 */

#ifndef V8M_NUMA_H
#define V8M_NUMA_H

#include <stdint.h>

/*
 * Hard caps for v0. The largest production NUMA boxes today have
 * dozens of nodes and thousands of CPUs; the values below leave
 * comfortable headroom while keeping the static map small. Bumping
 * either constant only widens an internal table — no API changes.
 */
#define V8M_NUMA_MAX_NODES 64
#define V8M_NUMA_MAX_CPUS 4096

/*
 * Discover the system's NUMA topology and populate the internal
 * CPU → node table. Idempotent — the second and subsequent calls
 * return immediately. Safe to invoke from the library constructor
 * before user threads exist; later threads see a fully-initialized
 * snapshot via acquire/release ordering on the init flag.
 */
void v8m_numa_init(void);

/*
 * Number of NUMA nodes detected. Returns at least 1 (the
 * "uniform memory" interpretation when sysfs is absent or NUMA is
 * disabled).
 */
uint32_t v8m_numa_node_count(void);

/*
 * NUMA node id for `cpu`. Returns 0 for out-of-range CPUs and for
 * CPUs that do not appear in any node's cpulist (treat as the
 * default node). Lookup is a constant-time array access.
 */
uint32_t v8m_numa_node_for_cpu(uint32_t cpu);

/*
 * NUMA node id of the calling thread's current CPU. Wraps
 * sched_getcpu() and the cpu→node table; on sched_getcpu failure
 * (very rare) returns node 0. Future cycles wire a vDSO
 * fast-path with refresh-on-N-th-call as called for in
 * .claude/docs/numa.md §2.2.
 */
uint32_t v8m_numa_current_node(void);

#endif /* V8M_NUMA_H */
