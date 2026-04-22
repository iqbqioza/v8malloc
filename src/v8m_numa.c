/* SPDX-License-Identifier: Apache-2.0 */
/*
 * NUMA topology implementation. Reads /sys/devices/system/node at
 * init, parses each node's cpulist into the cpu→node table, and
 * caches the node count. The reader path (`v8m_numa_current_node`,
 * `v8m_numa_node_for_cpu`, `v8m_numa_node_count`) is lock-free —
 * the table is written exactly once, before any caller can observe
 * the released `g_initialized` flag.
 */

/* sched_getcpu and the readdir/strtoul cluster come in under
 * _GNU_SOURCE, which is already defined by the build for every
 * library translation unit. No additional define needed here. */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v8m_numa.h"

static atomic_bool g_initialized;
static uint32_t g_node_count = 1;
static uint32_t g_cpu_to_node[V8M_NUMA_MAX_CPUS];

/*
 * SLIT distance matrix. g_distance[from][to] is the relative cost
 * of accessing memory on `to` from a CPU on `from`, populated from
 * /sys/devices/system/node/nodeN/distance during init. The matrix
 * is square (V8M_NUMA_MAX_NODES on each side) but only the
 * top-left g_node_count × g_node_count submatrix is meaningful;
 * unset cells default to 0 (the "unknown" sentinel).
 */
static uint8_t g_distance[V8M_NUMA_MAX_NODES][V8M_NUMA_MAX_NODES];

/*
 * Per-source-node fallback order. g_fallback[from][rank] is the
 * node id at position `rank` in distance-ascending order from
 * `from`. Computed once at init from g_distance via insertion
 * sort (cheap because g_node_count <= V8M_NUMA_MAX_NODES = 64).
 */
static uint8_t g_fallback[V8M_NUMA_MAX_NODES][V8M_NUMA_MAX_NODES];

/*
 * Per-thread node cache. sched_getcpu is itself vDSO-fast on Linux
 * (one rdtscp + a memory load on x86_64), but the allocator hot
 * path will call v8m_numa_current_node millions of times per
 * second once the per-NUMA pool sharding lands. Refreshing once
 * every V8M_NUMA_REFRESH_INTERVAL calls amortizes the syscall to
 * effectively free while still tracking thread migrations within
 * a few microseconds. The interval is a power of two so the
 * refresh check collapses to a single AND.
 */
#define V8M_NUMA_REFRESH_INTERVAL 1024U
static __thread uint32_t t_cached_node;
static __thread uint32_t t_call_count;

/*
 * Read up to `max - 1` bytes from `path` into `buf`, NUL-terminate,
 * and return the number of bytes read. Returns -1 on open/read
 * failure. Uses the syscall-level `open`/`read` pair instead of
 * stdio so we avoid pulling FILE buffers into the constructor.
 */
static ssize_t read_file(const char *path, char *buf, size_t max)
{
	int file = open(path, O_RDONLY | O_CLOEXEC);
	if (file < 0) {
		return -1;
	}
	ssize_t total = 0;
	while ((size_t)total < max - 1U) {
		ssize_t bytes =
		    read(file, buf + total, max - 1U - (size_t)total);
		if (bytes < 0) {
			(void)close(file);
			return -1;
		}
		if (bytes == 0) {
			break;
		}
		total += bytes;
	}
	buf[total] = '\0';
	(void)close(file);
	return total;
}

/*
 * Parse a sysfs distance row of the form "10 20 30 20\n" — one
 * integer per node, whitespace-separated — and stamp the values
 * into `g_distance[from_node][...]`. Anything beyond
 * V8M_NUMA_MAX_NODES is silently truncated; values above 255 are
 * clamped (the SLIT field is one byte). Tolerant of trailing
 * whitespace.
 */
static void apply_distance(const char *row, uint32_t from_node)
{
	const char *cursor = row;
	uint32_t to_node = 0;
	while (*cursor != '\0' && to_node < V8M_NUMA_MAX_NODES) {
		while (isspace((unsigned char)*cursor)) {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}
		char *end = NULL;
		unsigned long value = strtoul(cursor, &end, 10);
		if (end == cursor) {
			break;
		}
		uint8_t clamped = (value > 255UL) ? 255U : (uint8_t)value;
		g_distance[from_node][to_node++] = clamped;
		cursor = end;
	}
}

/*
 * Build g_fallback for every active node from g_distance. Insertion
 * sort over (g_node_count - 1) elements with `from` itself pinned
 * at rank 0; ties (equal distances) break by lower node id, which
 * insertion sort gives us for free since we walk targets in
 * ascending node order.
 */
static void build_fallback_order(void)
{
	for (uint32_t from = 0; from < g_node_count; from++) {
		/* Identity ordering, then sort by distance. The first
		 * slot is always the source itself, which has the
		 * smallest distance (10 in SLIT, or 0 if the row was
		 * never populated). */
		for (uint32_t i = 0; i < g_node_count; i++) {
			g_fallback[from][i] = (uint8_t)i;
		}
		for (uint32_t i = 1; i < g_node_count; i++) {
			uint32_t hold_idx = i;
			while (hold_idx > 0) {
				uint8_t left = g_fallback[from][hold_idx - 1U];
				uint8_t right = g_fallback[from][hold_idx];
				if (g_distance[from][left] <=
				    g_distance[from][right]) {
					break;
				}
				g_fallback[from][hold_idx - 1U] = right;
				g_fallback[from][hold_idx] = left;
				hold_idx--;
			}
		}
	}
}

/*
 * Parse a sysfs cpulist string of the form "0-3,8,12-15" and stamp
 * `node_id` into g_cpu_to_node for every covered CPU. Tolerates
 * trailing whitespace / newline left by sysfs.
 */
static void apply_cpulist(const char *list, uint32_t node_id)
{
	const char *cursor = list;
	while (*cursor != '\0') {
		while (isspace((unsigned char)*cursor) || *cursor == ',') {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}
		char *end = NULL;
		unsigned long start = strtoul(cursor, &end, 10);
		if (end == cursor) {
			break; /* malformed — bail */
		}
		unsigned long stop = start;
		cursor = end;
		if (*cursor == '-') {
			cursor++;
			stop = strtoul(cursor, &end, 10);
			if (end == cursor) {
				break;
			}
			cursor = end;
		}
		for (unsigned long cpu = start;
		     cpu <= stop && cpu < V8M_NUMA_MAX_CPUS; cpu++) {
			g_cpu_to_node[cpu] = node_id;
		}
	}
}

void v8m_numa_init(void)
{
	bool expected = false;
	if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected,
						     true, memory_order_acq_rel,
						     memory_order_acquire)) {
		return;
	}

	/* Default: every CPU on node 0. The sysfs scan below overrides
	 * the entries it finds; CPUs that don't appear in any nodeN/
	 * cpulist (rare, e.g. offline cores) keep the default. */
	for (uint32_t i = 0; i < V8M_NUMA_MAX_CPUS; i++) {
		g_cpu_to_node[i] = 0;
	}

	DIR *dir = opendir("/sys/devices/system/node");
	if (dir == NULL) {
		/* No sysfs NUMA → treat the box as a single uniform
		 * node. Every reader returns 0 / 1 / node-0. */
		g_node_count = 1;
		return;
	}

	uint32_t max_node_seen = 0;
	bool any_node = false;
	/* readdir is flagged MT-unsafe because it shares a buffer; we
	 * only call it during one-shot init from the constructor,
	 * before any user thread can race against us. */
	const struct dirent *entry;
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "node", 4) != 0) {
			continue;
		}
		const char *suffix = entry->d_name + 4;
		if (!isdigit((unsigned char)*suffix)) {
			continue;
		}
		char *end = NULL;
		unsigned long node_id = strtoul(suffix, &end, 10);
		if (*end != '\0' || node_id >= V8M_NUMA_MAX_NODES) {
			continue;
		}

		char path[512];
		int written = snprintf(path, sizeof(path),
				       "/sys/devices/system/node/%s/cpulist",
				       entry->d_name);
		if (written <= 0 || (size_t)written >= sizeof(path)) {
			continue;
		}
		char cpulist[1024];
		if (read_file(path, cpulist, sizeof(cpulist)) < 0) {
			continue;
		}
		apply_cpulist(cpulist, (uint32_t)node_id);

		/* The distance row is optional — kernels without ACPI
		 * SLIT or sysfs-distance support omit it. Missing rows
		 * leave g_distance zero, which collapses the fallback
		 * order to identity and is a safe degradation. */
		written = snprintf(path, sizeof(path),
				   "/sys/devices/system/node/%s/distance",
				   entry->d_name);
		if (written > 0 && (size_t)written < sizeof(path)) {
			char distance[1024];
			if (read_file(path, distance, sizeof(distance)) >= 0) {
				apply_distance(distance, (uint32_t)node_id);
			}
		}

		any_node = true;
		if ((uint32_t)node_id + 1U > max_node_seen) {
			max_node_seen = (uint32_t)node_id + 1U;
		}
	}
	(void)closedir(dir);

	g_node_count = any_node ? max_node_seen : 1U;
	build_fallback_order();
}

uint32_t v8m_numa_node_count(void)
{
	return g_node_count;
}

uint32_t v8m_numa_node_for_cpu(uint32_t cpu)
{
	if (cpu >= V8M_NUMA_MAX_CPUS) {
		return 0;
	}
	return g_cpu_to_node[cpu];
}

uint8_t v8m_numa_node_distance(uint32_t from_node, uint32_t to_node)
{
	if (from_node >= V8M_NUMA_MAX_NODES || to_node >= V8M_NUMA_MAX_NODES) {
		return 0;
	}
	return g_distance[from_node][to_node];
}

uint32_t v8m_numa_fallback_node(uint32_t from, uint32_t rank)
{
	if (from >= g_node_count) {
		return from;
	}
	if (rank >= g_node_count) {
		/* Saturate: out-of-range ranks return the source node
		 * itself rather than producing UB. Callers that walk
		 * the order until exhaustion don't need the
		 * sentinel — they just stop after node_count
		 * iterations. */
		return from;
	}
	return g_fallback[from][rank];
}

uint32_t v8m_numa_current_node(void)
{
	/* `count == 0` on the very first call in a thread (TLS init
	 * zero-fills t_call_count) and on every multiple of the
	 * refresh interval thereafter, so the first reader always
	 * gets a fresh sched_getcpu and subsequent fast-path calls
	 * use the cached value. */
	uint32_t count = t_call_count++;
	if ((count & (V8M_NUMA_REFRESH_INTERVAL - 1U)) == 0U) {
		int cpu = sched_getcpu();
		t_cached_node =
		    (cpu < 0) ? 0U : v8m_numa_node_for_cpu((uint32_t)cpu);
	}
	return t_cached_node;
}

uint32_t v8m_numa_current_cpu(void)
{
	int cpu = sched_getcpu();
	if (cpu < 0 || cpu >= (int)V8M_NUMA_MAX_CPUS) {
		return 0;
	}
	return (uint32_t)cpu;
}
