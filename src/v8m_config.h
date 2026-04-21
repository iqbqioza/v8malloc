/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Runtime configuration. Every tunable lives in a single small array
 * of atomic 64-bit integers; v8m_config_init() seeds it from the
 * V8M_* environment variables documented in AGENT.md §7 and
 * api.md §4.4 (defaults applied when an env var is absent or
 * unparseable).
 *
 * Reads on the hot path are relaxed-atomic loads — no locks. Writes
 * happen at process startup and via v8m_config_set() (the public
 * v8m_set_option / v8m_get_option API will wrap these in a later
 * cycle).
 *
 * Pre-init reads see zero for every option, which differs from the
 * default for the duration policies (PURGE_INTERVAL, etc.).
 * Consumers must arrange for v8m_config_init() to run before they
 * read; the library constructor handles that for the normal case.
 */

#ifndef V8M_CONFIG_H
#define V8M_CONFIG_H

#include <stdint.h>

enum v8m_option {
	V8M_OPT_VERBOSE = 0,	  /* 0 / 1 — emit diagnostics to stderr */
	V8M_OPT_PURGE_INTERVAL,	  /* seconds between background purges */
	V8M_OPT_THREAD_CACHE_MAX, /* max objects held per bin in the TLC */
	V8M_OPT_HUGE_PAGES, /* 0 / 1 — try MAP_HUGETLB / madvise(HUGEPAGE) */
	V8M_OPT_NUMA_AWARE, /* 0 / 1 — bind allocations to local node */
	V8M_OPT_DEBUG,	    /* 0 / 1 — guard pages, double-free checks, ... */
	V8M_OPT_PROFILE,    /* 0 / 1 — emit allocation profile */
	V8M_OPT_COMPACT_THRESHOLD, /* page utilization % below which a
				    * page is a compaction candidate */
	V8M_OPT_COUNT
};

/*
 * Read every V8M_* environment variable and apply the value (or its
 * default) to the corresponding option. Idempotent — safe to call
 * multiple times; each call re-reads the environment, so tests can
 * reset state by `setenv` followed by another v8m_config_init.
 */
void v8m_config_init(void);

/*
 * Set / get a tunable. Returns 0 on success, -1 if `opt` is
 * out-of-range. v8m_config_get on an out-of-range option returns 0.
 */
int v8m_config_set(enum v8m_option opt, int64_t value);
int64_t v8m_config_get(enum v8m_option opt);

#endif /* V8M_CONFIG_H */
