/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MB-07 NUMA local-allocation rate (benchmarks.md §2.7). Verifies
 * that allocations served by v8malloc are residing on the same
 * NUMA node as the calling thread — the spec's pass criterion is
 * ≥95 % local hit rate. Per-thread is the right granularity here:
 * a single thread pinned to one node should see 100 % local.
 *
 * For each allocation the bench:
 *   1. Touches the first byte to force the kernel to back the page
 *      with a physical frame on the current node (get_mempolicy
 *      with MPOL_F_NODE on an unfaulted page reports the policy,
 *      not the residency, which is the wrong answer).
 *   2. Calls `get_mempolicy(NULL, NULL, 0, addr, MPOL_F_NODE |
 *      MPOL_F_ADDR)` to read the actual residency node id.
 *   3. Compares against `v8m_numa_current_node()`.
 *
 * Output is one row per size band, parseable into a CSV-style
 * ingest. On a single-NUMA host (every CI runner today, every
 * laptop) every row reads 100 % — the bench then doubles as a
 * regression gate so a per-NUMA pool-sharding change cannot
 * silently route a thread's allocation to the wrong node.
 *
 * Single-thread by design. The spec does not parameterize MB-07
 * across thread counts; multi-thread coverage would require pinning
 * each worker to a CPU on a different node, which is a follow-on
 * once the NUMA pool sharding lands.
 *
 * Knobs (env vars):
 *   V8M_BENCH_ITERS   per-size iteration count, default 256
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "v8m_numa.h" /* v8m_numa_current_node, v8m_numa_node_count */

/* MPOL_F_NODE and MPOL_F_ADDR live in <linux/mempolicy.h> which
 * pulls in conflicting kernel typedefs on glibc. Define just the
 * three flags we need so the syscall wrapper compiles cleanly. */
#ifndef MPOL_F_NODE
#define MPOL_F_NODE (1 << 0)
#endif
#ifndef MPOL_F_ADDR
#define MPOL_F_ADDR (1 << 1)
#endif

static long get_mempolicy_node(const void *addr)
{
	int node = -1;
	/* Direct syscall — glibc does not export get_mempolicy
	 * universally and libnuma is not in the bench's link line.
	 * SYS_get_mempolicy is stable across every supported arch. */
	long ret = syscall(SYS_get_mempolicy, &node, NULL, 0UL, addr,
			   (unsigned long)(MPOL_F_NODE | MPOL_F_ADDR));
	if (ret != 0) {
		return -1;
	}
	return node;
}

/* One-shot probe so the bench reports up-front whether the kernel
 * supports get_mempolicy on this host (some containerized runtimes
 * filter the syscall, every WSL2 / no-NUMA kernel returns ENOSYS).
 * Pre-touches an anchor page to force residency before the probe. */
static int kernel_supports_mempolicy(int *out_errno)
{
	void *probe = malloc(4096);
	if (probe == NULL) {
		*out_errno = errno;
		return 0;
	}
	((volatile unsigned char *)probe)[0] = 1U;
	int node = -1;
	long ret = syscall(SYS_get_mempolicy, &node, NULL, 0UL, probe,
			   (unsigned long)(MPOL_F_NODE | MPOL_F_ADDR));
	int saved = errno;
	free(probe);
	if (ret != 0) {
		*out_errno = saved;
		return 0;
	}
	*out_errno = 0;
	return 1;
}

static const size_t g_sizes[] = {
    16,
    64,
    512,
    4096,
    65536,
    262144,
    (size_t)512 * 1024,
    (size_t)2 * 1024 * 1024,
};
static const size_t g_size_count = sizeof(g_sizes) / sizeof(g_sizes[0]);

static int env_int(const char *name, int fallback)
{
	/* getenv is single-threaded by spec; the bench is too. */
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	const char *value = getenv(name);
	if (value == NULL || *value == '\0') {
		return fallback;
	}
	long parsed = strtol(value, NULL, 10);
	if (parsed <= 0 || parsed > INT32_MAX) {
		return fallback;
	}
	return (int)parsed;
}

/* Two int counters describe distinct quantities by name; struct
 * wrapping would be overkill for one internal call site. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void measure_one(size_t size, int iters, uint32_t expected_node)
{
	int local = 0;
	int unknown = 0;
	for (int i = 0; i < iters; i++) {
		void *ptr = malloc(size);
		if (ptr == NULL) {
			(void)fprintf(
			    stderr, "mb_07: malloc(%zu) returned NULL\n", size);
			break;
		}
		((volatile unsigned char *)ptr)[0] = (unsigned char)i;
		long node = get_mempolicy_node(ptr);
		if (node < 0) {
			unknown++;
		} else if ((uint32_t)node == expected_node) {
			local++;
		}
		free(ptr);
	}
	int sampled = iters - unknown;
	double local_rate = 0.0;
	if (sampled > 0) {
		/* sampled > 0 in this branch; the analyzer cannot infer
		 * that across the env-tainted `iters` flow. */
		/* NOLINTNEXTLINE(clang-analyzer-core.DivideZero) */
		local_rate = 100.0 * (double)local / (double)sampled;
	}
	(void)printf("%-12zu %-6d %-7d %-9d %-7.2f\n", size, iters, local,
		     unknown, local_rate);
}

int main(void)
{
	int iters = env_int("V8M_BENCH_ITERS", 256);
	if (iters < 10) {
		iters = 10;
	}
	if (iters > 100000) {
		iters = 100000;
	}

	uint32_t node = v8m_numa_current_node();
	uint32_t node_count = v8m_numa_node_count();

	(void)printf("# MB-07 NUMA local-allocation rate\n");
	(void)printf("# iters=%d current_node=%u node_count=%u\n", iters, node,
		     node_count);
	if (node_count <= 1U) {
		(void)printf("# (single-node host: every row reports the "
			     "kernel-reported residency vs node 0 — the bench "
			     "is the regression gate for the future per-NUMA "
			     "pool work)\n");
	}

	int probe_errno = 0;
	if (!kernel_supports_mempolicy(&probe_errno)) {
		/* strerror is single-call thread-unsafe; the bench is
		 * single-threaded at this point. */
		/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
		const char *err_str = strerror(probe_errno);
		(void)printf("# get_mempolicy unavailable on this kernel "
			     "(errno=%d %s) — skipping every row.\n",
			     probe_errno, err_str);
		(void)printf("# Rerun on a kernel with NUMA support compiled "
			     "in (CONFIG_NUMA=y) and not filtered by a seccomp "
			     "policy.\n");
		return 0;
	}
	(void)printf("%-12s %-6s %-7s %-9s %-7s\n", "size_bytes", "iters",
		     "local", "unknown", "local_%");

	for (size_t i = 0; i < g_size_count; i++) {
		measure_one(g_sizes[i], iters, node);
	}
	return 0;
}
