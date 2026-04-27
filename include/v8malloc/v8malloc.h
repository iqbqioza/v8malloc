/* SPDX-License-Identifier: Apache-2.0 */
/*
 * v8malloc — high-performance memory allocator for Linux.
 *
 * This header is the sole public entry point. Consumers include it as
 *
 *	#include <v8malloc/v8malloc.h>
 *
 * The library may be used in three ways:
 *
 *  1. LD_PRELOAD interposition — every malloc/free in the process is
 *     transparently routed through v8malloc with no source changes.
 *  2. Direct linking against libv8malloc.so / libv8malloc.a — replaces
 *     the libc allocator at link time.
 *  3. Side-by-side use via the namespaced v8m_* API below — useful when
 *     mixing allocators is required (the default libc allocator stays
 *     in place for everything else).
 *
 * See man v8malloc(3) for the full surface; the design is documented in
 * the project's .claude/docs/ tree.
 */

#ifndef V8MALLOC_V8MALLOC_H
#define V8MALLOC_V8MALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Compile-time version ------------------------------------------------ */

#define V8M_VERSION_MAJOR 0
#define V8M_VERSION_MINOR 1
#define V8M_VERSION_PATCH 0

#define V8M_VERSION_STRING "0.1.0"

/*
 * Numeric form: (major * 10000) + (minor * 100) + patch. Useful for
 * preprocessor comparisons such as `#if V8M_VERSION >= 10200`.
 */
#define V8M_VERSION                                                            \
	((V8M_VERSION_MAJOR * 10000) + (V8M_VERSION_MINOR * 100) +             \
	 V8M_VERSION_PATCH)

/* --- Symbol visibility --------------------------------------------------- */

#if defined(V8MALLOC_BUILDING)
#define V8M_EXPORT __attribute__((visibility("default")))
#else
#define V8M_EXPORT
#endif

/* --- Runtime version API ------------------------------------------------- */

/*
 * Return the version string the library was compiled with, e.g. "0.1.0".
 * The returned pointer has static storage duration; callers must not
 * free or modify it.
 */
V8M_EXPORT const char *v8m_version(void);

V8M_EXPORT int v8m_version_major(void);
V8M_EXPORT int v8m_version_minor(void);
V8M_EXPORT int v8m_version_patch(void);

/* --- Allocation API (v8m_-prefixed, namespaced) ------------------- */

#include <stdbool.h>
#include <stddef.h>

/*
 * Namespaced allocation API. The standard `malloc`, `free`,
 * `calloc`, `realloc`, `reallocarray`, and `malloc_usable_size`
 * symbols are also exported by the library and route to the same
 * implementations; consumers get those prototypes from <stdlib.h>
 * and <malloc.h> as usual. The v8m_* names exist so a program can
 * call into v8malloc explicitly even when the standard symbols are
 * resolved to a different allocator.
 */
V8M_EXPORT void *v8m_malloc(size_t size);
V8M_EXPORT void v8m_free(void *ptr);
V8M_EXPORT void *v8m_calloc(size_t nmemb, size_t size);
V8M_EXPORT void *v8m_realloc(void *ptr, size_t size);
V8M_EXPORT void *v8m_reallocarray(void *ptr, size_t nmemb, size_t size);
V8M_EXPORT size_t v8m_malloc_usable_size(void *ptr);

/*
 * Aligned-allocation family. Same routing as v8m_malloc but the
 * returned pointer is `alignment`-aligned. `alignment` must be a
 * power of two; `posix_memalign` additionally requires `alignment`
 * to be a multiple of sizeof(void *). The largest alignment v0
 * supports is V8M_BUDDY_MAX_BLOCK (256 KiB) for size requests in
 * the slab/buddy range and V8M_PAGE_SIZE / 2 (32 KiB) for larger
 * sizes — requests beyond that return NULL with errno = EINVAL.
 */
V8M_EXPORT void *v8m_aligned_alloc(size_t alignment, size_t size);
V8M_EXPORT int v8m_posix_memalign(void **memptr, size_t alignment, size_t size);
V8M_EXPORT void *v8m_memalign(size_t alignment, size_t size);
V8M_EXPORT void *v8m_valloc(size_t size);
V8M_EXPORT void *v8m_pvalloc(size_t size);

/* --- Runtime configuration -------------------------------------- */

#include <stdint.h>

/*
 * Tunables. Values are seeded from the V8M_* environment variables
 * at library init (see man v8malloc(3)) and can be overridden at
 * runtime through v8m_set_option. The integer ids are stable
 * across v0; new options may be appended within a major release.
 */
enum v8m_option {
	V8M_OPT_VERBOSE = 0, /* 0 / 1 — emit diagnostics to stderr */
	V8M_OPT_PURGE_INTERVAL, /* seconds between background purges */
	V8M_OPT_THREAD_CACHE_MAX, /* max objects held per TLC bin */
	V8M_OPT_HUGE_PAGES, /* 0 / 1 — try MAP_HUGETLB / MADV_HUGEPAGE */
	V8M_OPT_NUMA_AWARE, /* 0 / 1 — bind allocations to local node */
	V8M_OPT_DEBUG, /* 0 / 1 — guard pages, double-free checks */
	V8M_OPT_PROFILE, /* 0 / 1 — emit allocation profile */
	V8M_OPT_COMPACT_THRESHOLD, /* page utilization % below which a
				    * page becomes a compaction candidate */
	V8M_OPT_VMA_WARN_THRESHOLD, /* /proc/self/maps line count above
				     * which the bg purge thread emits a
				     * one-line warning to stderr; 0
				     * disables. Default 1024. */
	V8M_OPT_NUMA_AGGRESSIVE_MIGRATION, /* 0 / 1 — when on, the per-thread
					    * cache's GC tick checks if the
					    * calling thread's NUMA node has
					    * changed since the last tick and
					    * issues `move_pages()` to
					    * relocate the cached free slots'
					    * pages to the new node. Off by
					    * default — relocation is
					    * expensive and only useful for
					    * threads with locality-sensitive
					    * working sets. */
	V8M_OPT_DEFERRED_COALESCE, /* 0 / 1 — when on, the buddy pool
				    * skips the immediate buddy-merge on
				    * every free; the merge happens lazily
				    * on the next alloc that would
				    * otherwise miss. Avoids the
				    * coalesce/split cycle in
				    * alloc-free-alloc-free patterns at
				    * the cost of slightly weaker
				    * fragmentation guarantees on the
				    * write-mostly path. Off by default —
				    * the immediate-coalesce baseline is
				    * still the better fit for most
				    * workloads. */
	V8M_OPT_LIFETIME_TRACKING, /* 0 / 1 — when on, sample 1-in-N
				    * allocations and record (caller PC,
				    * alloc TSC) into a per-thread ring
				    * buffer; the matching free measures
				    * the elapsed lifetime and folds it
				    * into the per-caller-PC EMA bucket
				    * (fragmentation.md §5.2). Aggregate
				    * stats surface via
				    * v8m_get_lifetime_stats. Off by
				    * default — the per-free ring scan
				    * costs ~50ns per call when on, only
				    * worth it for diagnostic runs that
				    * inform the future class-routing
				    * cycle. */
	V8M_OPT_COUNT
};

/*
 * Set the tunable identified by `opt` to `value`. Returns 0 on
 * success or -1 with errno set to EINVAL if `opt` is out-of-range.
 * Pre-init (before our constructor runs) the call always returns
 * -1 with EAGAIN — callers should defer until after main() starts
 * or set the corresponding V8M_* environment variable.
 */
V8M_EXPORT int v8m_set_option(int opt, int64_t value);

/*
 * Read a tunable into `*out`. Same return semantics as
 * v8m_set_option. `out` must be non-NULL.
 */
V8M_EXPORT int v8m_get_option(int opt, int64_t *out);

/* --- Runtime statistics ----------------------------------------- */

/*
 * Snapshot of allocator-wide counters per api.md §4.2. The first
 * twelve fields are the spec'd surface; the trailing fields are
 * v8malloc-specific extensions retained for backward compatibility
 * (some are aliases of spec fields — `current_usage == live_bytes`,
 * `total_allocated == mmap_bytes == bytes_mapped`, etc.). Most
 * counters are monotonic across the process lifetime; the
 * exceptions are `current_usage`, `peak_usage`, `live_regions`,
 * and `live_bytes`, which reflect the current state.
 *
 * v0 caveats:
 *   - `total_allocated` / `total_freed` mirror cumulative bytes
 *     mmap'd / munmap'd from the kernel (the page heap's
 *     observable surface). Per-call user-byte accounting requires
 *     hooking every backend and is not yet wired.
 *   - `numa_local_allocs` / `numa_remote_allocs` use the page
 *     heap's mbind success / failure counts as a proxy for v0
 *     since per-allocation node tracking is not yet wired.
 */
struct v8m_stats {
	/* Spec fields (api.md §4.2) */
	uint64_t total_allocated;
	uint64_t total_freed;
	uint64_t current_usage;
	uint64_t peak_usage;
	uint64_t total_alloc_count;
	uint64_t total_free_count;
	uint64_t mmap_count;
	uint64_t munmap_count;
	uint64_t mmap_bytes;
	uint64_t huge_page_count;
	uint64_t numa_local_allocs;
	uint64_t numa_remote_allocs;

	/* v8malloc extensions (not in spec; some alias spec fields
	 * for backward source compatibility with pre-spec callers). */
	uint64_t mmap_calls; /* alias of mmap_count */
	uint64_t munmap_calls; /* alias of munmap_count */
	uint64_t advise_calls; /* MADV_DONTNEED hint count */
	uint64_t bytes_mapped; /* alias of mmap_bytes */
	uint64_t bytes_unmapped; /* alias of total_freed */
	uint64_t live_regions; /* live mmap region count */
	uint64_t live_bytes; /* alias of current_usage */
};

/*
 * Snapshot the statistics into `*out`. Tolerates a NULL pointer
 * by no-op'ing. Pre-init returns all zeroes.
 */
V8M_EXPORT void v8m_get_stats(struct v8m_stats *out);

/*
 * Reset resettable counters per api.md §4.2. v8malloc keeps the
 * cumulative page-heap counters monotonic (zeroing them mid-flight
 * would silently break diagnostic tooling), so this clears only
 * the resettable subset: `peak_usage` re-baselines to the current
 * `current_usage`, and `total_alloc_count` / `total_free_count`
 * reset to zero. Other fields are left untouched. Pre-init is a
 * no-op.
 */
V8M_EXPORT void v8m_reset_stats(void);

/*
 * Emit a human-readable summary to `stream` (defaults to stderr
 * when `stream` is NULL). Spec signature per api.md §4.2.
 */
V8M_EXPORT void v8m_dump_stats(void *stream);

/*
 * Per-class breakdown for the Large/Huge direct-mmap path. The
 * `*_alloc_count` / `*_free_count` fields are monotonic across
 * the process lifetime; `live_count = alloc - free`.
 * `*_bytes_in_use` is the current sum of mmap_size for live
 * allocations of that class — useful for sizing huge-page
 * reservations (huge-pages.md §8).
 */
struct v8m_huge_stats {
	uint64_t large_alloc_count;
	uint64_t large_free_count;
	uint64_t large_bytes_in_use;
	uint64_t huge_alloc_count;
	uint64_t huge_free_count;
	uint64_t huge_bytes_in_use;
};

/*
 * Snapshot the Large/Huge counters into `*out`. Tolerates NULL.
 * Pre-init returns all zeroes.
 */
V8M_EXPORT void v8m_get_huge_stats(struct v8m_huge_stats *out);

/*
 * Per-thread allocator stats. Reports cache hit/miss counters and
 * remote-free queue activity for the calling thread. v0 has no
 * thread cache yet, so every field reads as zero — the public
 * surface lands now to lock the contract.
 */
struct v8m_thread_stats {
	uint64_t fast_path_allocs;
	uint64_t slow_path_allocs;
	uint64_t fast_path_frees;
	uint64_t remote_frees_received;
	uint64_t bin_overflow_flushes;
};

/*
 * Snapshot the calling thread's stats. Tolerates NULL.
 */
V8M_EXPORT void v8m_get_thread_stats(struct v8m_thread_stats *out);

/*
 * Number of size classes the histogram below indexes. Mirrors the
 * internal V8M_NUM_SIZE_CLASSES; the matching `_Static_assert` in
 * v8m_thread_cache.c keeps the two in lock-step. Bumping this is a
 * binary-compat break for `struct v8m_size_class_histogram`.
 */
#define V8M_PUBLIC_NUM_SIZE_CLASSES 41

/*
 * Per-class allocation request histogram (size-classes.md §9 —
 * "workload-adaptive size classes"). Foundation for the dynamic
 * size-class adjustment row: every public alloc is sampled
 * (1-in-V8M_HISTOGRAM_SAMPLE_RATE — 64 today) and the sample's raw
 * user-requested size is added to its class bucket. The histogram
 * is therefore an unbiased estimator of the true distribution at
 * 1/N the recording cost — multiply each `request_count[cls]` by
 * the sample rate for the implied true count.
 *
 * Index `cls` corresponds to the `v8m_class_to_size[cls]` mapping
 * (Tiny: 0..7, Small: 8..31, Medium: 32..37, Large: 38..40).
 * Requests above V8M_LARGE_MAX_SIZE (2 MiB) accumulate in the
 * `huge_request_*` overflow pair instead — they bypass the size
 * class table and route through the direct mmap path.
 *
 * `request_bytes[cls]` carries the sum of raw user-requested sizes
 * (NOT the rounded class size); the per-bucket internal-frag rate
 * is therefore `(request_count[cls] * v8m_class_to_size[cls]) -
 * request_bytes[cls]`. This is what the future hot-reload step of
 * §9 will consume to decide whether a new sub-class would shrink
 * the dominant waste bucket.
 *
 * Snapshot semantics: aggregated across every live thread cache plus
 * a process-wide carry-over for caches that have already exited and
 * for the rare bootstrap / signal-safe paths that bypassed TLC. A
 * concurrent allocation may bump a per-cache counter mid-aggregation;
 * the snapshot is statistically accurate but not strictly
 * point-in-time.
 */
struct v8m_size_class_histogram {
	uint64_t request_count[V8M_PUBLIC_NUM_SIZE_CLASSES];
	uint64_t request_bytes[V8M_PUBLIC_NUM_SIZE_CLASSES];
	uint64_t huge_request_count;
	uint64_t huge_request_bytes;
};

/*
 * Snapshot the size-class histogram into `*out`. Tolerates NULL.
 * Pre-init returns all zeroes.
 */
V8M_EXPORT void
v8m_get_size_class_histogram(struct v8m_size_class_histogram *out);

/*
 * Per-class slab utilization snapshot (fragmentation.md §4.1).
 * Reports pages_in_use / slots_total / slots_used / utilization
 * percentage for each Tiny/Small size class (0..31). Indices ≥ 32
 * cover Medium and above which are served by the buddy / Large
 * paths and have no slab backing — those entries always read as
 * zero so iteration loops can stay simple. Caller passes a buffer
 * of `V8M_PUBLIC_NUM_SIZE_CLASSES` entries; the function fills
 * exactly that many. Pre-init returns all zeroes.
 *
 * `slab_pages_in_use` already reports the aggregate via
 * `v8m_get_frag_metrics`; this accessor is the per-class
 * breakdown that surfaces the dominant utilization buckets a
 * fragmentation-aware operator wants to see.
 */
struct v8m_slab_class_breakdown {
	uint64_t pages_in_use;
	uint64_t slots_total;
	uint64_t slots_used;
	uint32_t utilization_pct;
	uint32_t reserved;
};

V8M_EXPORT void v8m_get_slab_class_breakdown(
    struct v8m_slab_class_breakdown out[V8M_PUBLIC_NUM_SIZE_CLASSES]);

/* Per-lifetime-arena slab breakdown — declared further down,
 * after the `enum v8m_lifetime_class` definition. */

/*
 * Architecture / runtime probe snapshot. Programmatic counterpart
 * to the `v8malloc isa: …` line the constructor writes to stderr
 * under V8M_VERBOSE — same data, but available to a process that
 * wants the values without parsing the log line. `arch_name`
 * carries the same short name the verbose line emits ("x86_64",
 * "aarch64", "riscv64", "ppc64le", "s390x", "loongarch64", or
 * "unknown" on an unrecognised arch). `cache_line_bytes` is the
 * runtime-probed L1 dcache line width (CTR_EL0 on aarch64,
 * compile-time constant elsewhere). `build_cache_line_bytes` is
 * the value V8M_CACHELINE_ALIGNED used at struct layout time —
 * comparing the two surfaces a host that needs wider padding than
 * the build assumed (Apple M1 P-cores at 128 B vs the 64 B
 * default). `tsc_mhz` is the TSC frequency on x86_64, 1000 (= 1
 * tick per ns) on every other arch. `has_lse` is the AArch64 LSE
 * atomics availability bit; false on every non-aarch64 build.
 */
struct v8m_arch_info {
	char arch_name[16];
	uint32_t cache_line_bytes;
	uint32_t build_cache_line_bytes;
	uint32_t tsc_mhz;
	uint8_t has_lse; /* aarch64 LSE atomics (HWCAP_ATOMICS) */
	uint8_t has_zbb; /* riscv64 Zbb basic bit-manip extension */
	uint8_t has_zacas; /* riscv64 Zacas AMOCAS extension */
	uint8_t reserved;
};

V8M_EXPORT void v8m_get_arch_info(struct v8m_arch_info *out);

/*
 * Pre-warm the calling thread's allocator state. The thread-local
 * cache is normally created lazily on the thread's first
 * allocation, which adds a one-time ~µs spike; latency-sensitive
 * threads (real-time worker pools, low-tail-latency request
 * handlers) can call `v8m_init_thread()` from their startup hook
 * to fold that work into a non-critical phase. Idempotent — a
 * thread that has already touched a v8malloc allocation calls
 * this as a no-op.
 *
 * Returns 0 on success, -1 on failure (errno set: `ENOMEM` if the
 * cache allocation failed, `EAGAIN` if the dispatcher is not
 * READY yet — typically pre-constructor; the caller can retry
 * after main() starts). On failure the thread's allocations still
 * succeed via the slow path; the helper is purely a
 * pre-warm / latency-shaping hint.
 */
V8M_EXPORT int v8m_init_thread(void);

/*
 * Explicitly release the calling thread's allocator state without
 * waiting for thread exit. Drains the per-thread cache's bins
 * back to the slab pool, drops the calling thread's current-CPU
 * L2 cache contribution, and clears the TLS slot so the next
 * allocation re-creates a fresh cache. Useful for long-lived
 * worker threads that go idle for an extended period — releasing
 * the per-thread cache returns its slots to the pool and lets
 * the OS reclaim drained-cache pages on the bg purge tick.
 *
 * Returns 0 on success, -1 if the dispatcher is not READY
 * (errno = EAGAIN). Idempotent — a thread that never touched a
 * v8malloc allocation calls this as a no-op.
 */
V8M_EXPORT int v8m_release_thread(void);

/*
 * Stable short name for the option id (e.g. "VERBOSE",
 * "HUGE_PAGES", "NUMA_AWARE"). Returns NULL when `option_id` is
 * out of range. Combined with `v8m_get_option(out, id)`, callers
 * can iterate `0..V8M_OPT_COUNT` to build a pretty-printed config
 * dump without compiling against the internal name table.
 *
 * The returned string is statically allocated; caller does not
 * own it. Same name format the constructor uses for the
 * `v8malloc opts:` line under V8M_VERBOSE — lowercase except for
 * intra-word boundaries (e.g. "purge_interval", "vma_warn_threshold").
 */
V8M_EXPORT const char *v8m_option_name(int option_id);

/*
 * Debug helper — walks the allocator's internal data structures
 * and counts invariant violations. Returns the number of issues
 * detected (0 = healthy). Designed for fuzzing, regression
 * testing, and support diagnostics; safe to call from any
 * context that an allocation is safe in.
 *
 * Current checks:
 *   - Page-heap region map is sorted ascending by start address.
 *   - No two regions overlap.
 *   - Slab pool drained-cache count stays inside its cap.
 *   - Buddy pool in_use / drained flags are mutually consistent
 *     (a drained slot is also in_use).
 *
 * On detection, each issue prints a short diagnostic to stderr
 * (one line per violation) so a fuzzer that runs the validator
 * after every operation can capture the offending state. The
 * return value is the line count emitted. Pre-init returns 0
 * (no state to validate yet).
 */
V8M_EXPORT int v8m_validate_internal_state(void);

/*
 * Hot-reload the active size-class size table. Atomically swaps
 * the pointer the slab-init paths read on every fresh-page
 * formatting. Already-allocated pages keep the size baked into
 * their meta header at init time, so the swap only affects future
 * allocations — matching the spec's "leave old pages as-is" rule.
 *
 * `new_table` must be non-NULL and must contain
 * V8M_PUBLIC_NUM_SIZE_CLASSES (41) entries; the size at index `cls`
 * replaces `v8m_class_to_size[cls]` for new slab-page formatting.
 * The caller retains ownership of the table — the size class
 * machinery does not copy. Pass `NULL` to revert to the v0
 * baseline. Returns 0 on success or -EINVAL on a malformed
 * argument (size at index 0 must be ≥ 8 bytes).
 */
V8M_EXPORT int v8m_install_size_class_table(const uint32_t *new_table);

/*
 * Read the live size for `cls`. Returns 0 for out-of-range cls.
 */
V8M_EXPORT uint32_t v8m_size_class_to_bytes(int cls);

/*
 * Lifetime-class tracker (fragmentation.md §5.2 — caller-address-based
 * lifetime separation). Foundation row: opt-in via
 * V8M_OPT_LIFETIME_TRACKING; off by default. When on, sample one in
 * every N allocations, record (caller PC, alloc TSC) into a per-thread
 * ring buffer; the matching free locates the entry, measures the
 * elapsed TSC ticks, and folds the lifetime into a per-caller-PC EMA
 * bucket. Each completed sample is also classified into one of three
 * lifetime classes by EMA-vs-threshold comparison and the global
 * counters bumped. Class routing (allocate ephemeral / short / long
 * objects to distinct arenas) is the future cycle this tracker
 * unblocks.
 *
 * `samples_recorded` counts allocation-side ring writes;
 * `samples_completed` counts ring entries the matching free found and
 * resolved; `samples_evicted` counts entries the next sampled
 * allocation overwrote because the original allocation outlived the
 * 64-slot ring. The three class counters partition `samples_completed`
 * (sum of the three equals `samples_completed` minus a small
 * post-classification race window).
 */
struct v8m_lifetime_stats {
	uint64_t samples_recorded;
	uint64_t samples_completed;
	uint64_t samples_evicted;
	uint64_t ephemeral_count;
	uint64_t short_count;
	uint64_t long_count;
};

/*
 * Snapshot the aggregated lifetime tracker into `*out`. Tolerates
 * NULL. Pre-init or with the option off returns all zeroes (no
 * samples accumulated). Aggregates across every live thread cache
 * plus a process-wide carry-over for caches that have already exited.
 */
V8M_EXPORT void v8m_get_lifetime_stats(struct v8m_lifetime_stats *out);

/*
 * Lifetime classification of a caller-PC. EPHEMERAL when the
 * per-PC EMA is below 100 µs (function scope), SHORT below 100
 * ms, LONG above. UNKNOWN when the calling thread has no TLC,
 * the option is off, or the caller PC has no recorded samples
 * yet. The arena-routing layer consults this to place
 * allocations on a lifetime-class-specific arena.
 */
enum v8m_lifetime_class {
	V8M_LIFETIME_EPHEMERAL = 0,
	V8M_LIFETIME_SHORT = 1,
	V8M_LIFETIME_LONG = 2,
	V8M_LIFETIME_UNKNOWN = 3
};

V8M_EXPORT enum v8m_lifetime_class v8m_estimate_lifetime(const void *caller_pc);

/*
 * Per-lifetime-arena slab breakdown. Same shape as
 * `v8m_get_slab_class_breakdown` but reads from one of the four
 * dispatcher arenas:
 *   V8M_LIFETIME_UNKNOWN  → default arena (the one
 *                           v8m_get_slab_class_breakdown reports)
 *   V8M_LIFETIME_EPHEMERAL → ephemeral lifetime arena
 *   V8M_LIFETIME_SHORT     → short lifetime arena
 *   V8M_LIFETIME_LONG      → long lifetime arena
 *
 * Only meaningful when `V8M_OPT_LIFETIME_TRACKING` is on and the
 * dispatcher has accumulated enough per-PC samples to route
 * allocations to the matching arena; otherwise the lifetime arenas
 * read as empty. Pre-init returns all zeroes. Out-of-range
 * `lifetime` values fall through to the default arena.
 */
V8M_EXPORT void v8m_get_slab_class_breakdown_lifetime(
    enum v8m_lifetime_class lifetime,
    struct v8m_slab_class_breakdown out[V8M_PUBLIC_NUM_SIZE_CLASSES]);

/*
 * Per-NUMA-node memory balance snapshot (numa.md §6.1 — inter-node
 * rebalancing detection). `per_node_bytes[n]` is the live byte total
 * of page-heap regions currently bound to node `n` via the mbind path
 * the constructor wires up; the sum across nodes equals
 * `total_bytes`. `most_loaded_node` / `most_loaded_bytes` identify
 * the highest-pressure node, and `imbalanced` flips to true when
 * that node holds ≥ 150 % of the average across the live nodes (the
 * spec's overload trigger). Single-node hosts and pre-init reporters
 * never flip the flag. Foundation row: detection lands here so a
 * future cycle can wire the action half (suppress new allocations
 * from the overloaded node, page migration via `move_pages()`, TLC
 * shrink) on top.
 */
#define V8M_PUBLIC_NUMA_MAX_NODES 64

struct v8m_numa_balance_stats {
	uint64_t per_node_bytes[V8M_PUBLIC_NUMA_MAX_NODES];
	uint64_t total_bytes;
	uint32_t node_count;
	uint32_t most_loaded_node;
	uint64_t most_loaded_bytes;
	uint64_t average_bytes_per_node;
	bool imbalanced;
};

/*
 * Snapshot the per-node memory balance into `*out`. Tolerates NULL.
 * Pre-init returns all zeroes.
 */
V8M_EXPORT void v8m_get_numa_balance(struct v8m_numa_balance_stats *out);

/*
 * Fragmentation snapshot. Reports the page-heap-derived metrics plus
 * the aggregate slab utilization across every Tiny/Small class. The
 * slab counters cover pages currently held in the per-class
 * `current` and `partials` lists — empty pages are returned to the
 * page heap on free, and full pages are off the lists by design, so
 * the walk reports the actively-partitioned population that drives
 * operational utilization decisions (fragmentation.md §4.1).
 * Per-class breakdown is a follow-up API once the per-class field
 * group warrants its own struct.
 */
struct v8m_frag_metrics {
	uint64_t live_regions; /* mmap'd page-heap regions */
	uint64_t live_bytes; /* bytes_mapped - bytes_unmapped */
	uint64_t bytes_per_region; /* live_bytes / live_regions, or 0 */
	uint64_t region_map_capacity; /* hard cap (4096 in v0) */
	uint64_t region_map_used_pct; /* live_regions / capacity * 100 */
	uint64_t large_live_count;
	uint64_t huge_live_count;
	uint64_t vma_count; /* /proc/self/maps lines, or 0 if unreadable */
	uint64_t slab_pages_in_use; /* slab pages on current+partials */
	uint64_t slab_slots_total; /* sum of capacity across those pages */
	uint64_t slab_slots_used; /* sum of used_count across those pages */
	uint64_t
	    slab_utilization_pct; /* slab_slots_used / slab_slots_total * 100 */
};

/*
 * Snapshot fragmentation metrics. Tolerates NULL. Pre-init
 * returns all zeroes.
 */
V8M_EXPORT void v8m_get_frag_metrics(struct v8m_frag_metrics *out);

/*
 * Process-wide VMA count, read live from /proc/self/maps. Useful
 * for surfacing the kernel-side fragmentation cost — every Huge
 * allocation that does NOT coalesce with an existing region grows
 * the line count, and a runaway count slows mmap / fork / page
 * fault paths in the kernel. Returns 0 if /proc/self/maps cannot
 * be opened (e.g. some seccomp sandboxes), which the caller can
 * treat as "unknown" rather than "zero".
 *
 * Cost: one open + sequential read + close, roughly O(VMA_count).
 * Cheap enough for periodic logging, too expensive for the alloc
 * fast path.
 */
V8M_EXPORT uint64_t v8m_count_vmas(void);

/*
 * Async-signal-safe emergency allocation
 * (architecture.md §6). `malloc` / `free` are not
 * async-signal-safe in general — the slab pool's `pthread_mutex`
 * would happily deadlock if a signal handler fires on a thread
 * that already holds it. This is the escape hatch: a small BSS
 * bump pool, atomic-only, no locks, no syscalls, safe to call
 * from any signal handler.
 *
 * Returns NULL on exhaustion (the buffer is intentionally tiny —
 * a one-page emergency budget — so signal handlers must check).
 * The returned pointer can be passed to `free()` like any other
 * v8malloc allocation; the dispatcher recognises it via a range
 * check and treats the free as a no-op (the buffer leaks for the
 * process lifetime by design).
 */
/* NOLINTNEXTLINE(readability-redundant-declaration) */
V8M_EXPORT void *v8m_signal_safe_alloc(size_t size);

/*
 * Force a purge cycle: page-heap regions that have drained
 * (e.g., empty buddy arenas, slab pages with `used_count == 0`)
 * are returned to the kernel. v0's slab and buddy pools already
 * release empty pages eagerly on free, so the synchronous purge
 * call is currently a no-op — the public surface lands to lock
 * the contract for the future bg purge thread cycle.
 */
V8M_EXPORT void v8m_purge(void);

/*
 * Per-thread variant. Once the thread cache lands this releases
 * any cached objects bound to the calling thread back to the
 * pools. No-op in v0.
 */
V8M_EXPORT void v8m_purge_thread(void);

/* --- v8m_-namespaced glibc-compat extensions -------------------- */
/*
 * Mirrors of the standard glibc statistics / tuning extensions
 * that the library also exports under their unprefixed names.
 * Programs that link side-by-side with another allocator can call
 * these to talk to v8malloc explicitly when the standard symbols
 * have been resolved elsewhere. Forward to the same shared
 * `v8m_collect_live_stats` snapshot, so every reporter agrees.
 *
 * `struct mallinfo` and `struct mallinfo2` are declared in
 * `<malloc.h>`. Consumers needing the namespaced variants must
 * `#include <malloc.h>` ahead of this header (or `<stdio.h>` for
 * the FILE * argument of `v8m_malloc_info`).
 */
struct mallinfo;
struct mallinfo2;

V8M_EXPORT struct mallinfo v8m_mallinfo(void);
V8M_EXPORT struct mallinfo2 v8m_mallinfo2(void);
V8M_EXPORT void v8m_malloc_stats(void);
/* `stream` is `FILE *`; declared as `void *` so this header doesn't
 * need to drag in `<stdio.h>`. Callers cast their FILE pointer
 * implicitly through the void * conversion. */
V8M_EXPORT int v8m_malloc_info(int options, void *stream);
V8M_EXPORT int v8m_mallopt(int param, int value);
V8M_EXPORT int v8m_malloc_trim(size_t pad);

/* --- Failure-path hooks ----------------------------------------- */

/*
 * Out-of-memory handler signature. Invoked on the (rare) allocation
 * path that exhausts the page heap or trips the soft limit. The
 * handler receives the size that failed to satisfy and returns a
 * non-zero value to ask the allocator to retry the request once
 * (giving the program a chance to free other allocations first), or
 * zero to let the allocation fail with errno = ENOMEM.
 *
 * The handler runs on the calling thread with the dispatcher fully
 * initialized. It must be reentrancy-safe: if it allocates and the
 * sub-allocation also fails, the nested call returns NULL with
 * errno = ENOMEM rather than recursing back into the handler.
 */
typedef int (*v8m_oom_handler_t)(size_t requested_size);

/*
 * Install or clear the OOM handler. Pass NULL to clear (the default).
 * The previous handler is returned so callers can chain.
 */
V8M_EXPORT v8m_oom_handler_t v8m_set_oom_handler(v8m_oom_handler_t handler);

/*
 * Cap the bytes the page heap may have mapped at any one time.
 * Counted as `bytes_mapped - bytes_unmapped` (matches the
 * `live_bytes` field of struct v8m_stats). Allocations that would
 * push the live byte total above the limit fail with NULL /
 * errno = ENOMEM (after invoking the OOM handler if one is set,
 * to give the program a chance to free other allocations first).
 *
 * Pass 0 to disable the limit (the default). The limit applies to
 * the page heap only — bootstrap allocations are not counted.
 *
 * Returns 0 on success, or -1 with errno = EAGAIN when the
 * dispatcher is not yet READY (e.g. called from a constructor
 * that runs before v8malloc's own constructor).
 */
V8M_EXPORT int v8m_set_soft_limit(size_t bytes);

/*
 * Read the current soft limit. Returns 0 when no limit is set.
 */
V8M_EXPORT size_t v8m_get_soft_limit(void);

/* --- Pointer introspection -------------------------------------- */

/*
 * Backend that issued a given pointer. The ordering is stable
 * across v0; new backends are appended.
 */
enum v8m_ptr_backend {
	V8M_PTR_FOREIGN = 0, /* not v8malloc-issued (or NULL) */
	V8M_PTR_BOOTSTRAP, /* served from the pre-init bootstrap buffer */
	V8M_PTR_SLAB, /* slab pool — Tiny + Small classes */
	V8M_PTR_BUDDY, /* buddy pool — Medium classes */
	V8M_PTR_LARGE /* direct mmap path — Large + Huge */
};

/*
 * Snapshot of what v8malloc knows about a pointer. `usable_size`
 * matches what `malloc_usable_size(ptr)` would report; `size_class`
 * is the slab class id (0..31) for SLAB-backed pointers and -1
 * everywhere else.
 */
struct v8m_ptr_info {
	int backend; /* enum v8m_ptr_backend */
	size_t usable_size;
	int size_class; /* -1 if backend != V8M_PTR_SLAB */
};

/*
 * Quick predicate: true iff `ptr` was issued by v8malloc and is
 * recognized by the dispatcher. NULL is not a valid pointer.
 * Mid-allocation pointers (offsets inside a v8malloc region but
 * not the start of any individual allocation) are not recognized.
 */
V8M_EXPORT bool v8m_is_valid_ptr(const void *ptr);

/*
 * Fill `*out` with everything v8malloc knows about `ptr`. Returns
 * 0 on success, -1 with `errno = EINVAL` for NULL `out`, NULL
 * `ptr`, or a foreign / mid-allocation `ptr`. On failure `*out`
 * is left in a defined zero state with backend = V8M_PTR_FOREIGN.
 */
V8M_EXPORT int v8m_ptr_info(const void *ptr, struct v8m_ptr_info *out);

#ifdef __cplusplus
}
#endif

#endif /* V8MALLOC_V8MALLOC_H */
