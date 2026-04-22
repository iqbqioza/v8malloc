/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Public allocation API. Implements the standard malloc family
 * (malloc, free, calloc, realloc, reallocarray, malloc_usable_size)
 * along with the aligned-allocation family (aligned_alloc,
 * posix_memalign, memalign, valloc, pvalloc) and the v8m_-prefixed
 * equivalents, all routing through the single-process v8m_dispatch
 * instance. Library load runs the constructor that initializes the
 * dispatch; library unload runs the destructor that tears it down.
 *
 * Pre-init / post-shutdown allocations fall through to the
 * bootstrap allocator so library constructors that run before us
 * (and any late shutdown allocations) still get serviced. Bootstrap
 * pointers survive the transition: free() recognizes them via the
 * range check and treats them as no-ops.
 */

#include <errno.h>
#include <fcntl.h> /* O_RDONLY for v8m_count_vmas */
#include <malloc.h> /* mallinfo/mallinfo2/mallopt/malloc_*  glibc extensions */
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v8m_arch.h" /* v8m_arch_rdtsc for the lifetime tracker */
#include "v8m_bg_purge.h"
#include "v8m_bootstrap.h"
#include "v8m_buddy_pool.h"
#include "v8m_config.h"
#include "v8m_dispatch.h"
#include "v8m_large.h"
#include "v8m_libc_fallback.h"
#include "v8m_numa.h"
#include "v8m_page.h"
#include "v8m_page_heap.h"
#include "v8m_pprof.h"
#include "v8m_signal_safe.h"
#include "v8m_size_class.h"
#include "v8m_slab_pool.h"
#include "v8m_thread_cache.h"
#include "v8malloc/v8malloc.h"

/* Init lifecycle (architecture.md §3.2 — three-state init machine,
 * extended here with TORN_DOWN for post-destructor distinction).
 *
 *   NONE       0  process started, no allocator init has begun yet
 *   RUNNING    1  inside v8m_constructor, dispatcher is mid-init —
 *                 a reentrant allocation must route to bootstrap
 *   READY      2  v8m_constructor done, dispatcher fully usable
 *   TORN_DOWN  3  v8m_destructor ran — distinct from NONE so the
 *                 future debug build can flag "allocation after
 *                 destructor" separately from "allocation before
 *                 constructor"; both states route to bootstrap so
 *                 there's no hot-path observable difference today
 *
 * The constructor publishes RUNNING with release ordering BEFORE
 * touching the dispatcher (so a reentrant alloc during dispatch
 * init sees RUNNING and bootstrap-routes), then publishes READY
 * after init completes. The destructor swaps to TORN_DOWN with
 * acquire ordering. dispatch_ready() returns true only for READY.
 */
enum v8m_init_state {
	V8M_INIT_NONE = 0,
	V8M_INIT_RUNNING = 1,
	V8M_INIT_READY = 2,
	V8M_INIT_TORN_DOWN = 3
};
static atomic_int g_init_state = V8M_INIT_NONE;
static struct v8m_dispatch g_dispatch;

/* OOM handler + soft-limit state. The handler pointer rides in an
 * atomic so set/install across threads is well-defined; the limit
 * is a relaxed atomic since malloc reads it on the fast path and
 * the writer rarely changes it. A per-thread reentrancy flag (set
 * during the handler call) keeps a sub-allocation that itself
 * fails from recursing back into the handler. */
static _Atomic(v8m_oom_handler_t) g_oom_handler;
static atomic_size_t g_soft_limit;
static __thread bool t_oom_in_handler;

/* Forward declaration: dispatch_ready is defined further down with
 * the rest of the lifecycle helpers, but the live-stats collector
 * needs to consult it. Keeping the body where it is preserves the
 * "lifecycle helpers stay together" structure of the file. */
static bool dispatch_ready(void);

/*
 * Live-mmap accounting recovered from the page-heap counters. Used
 * by every reporter (mallinfo / mallinfo2 / malloc_info /
 * malloc_stats / v8m_get_frag_metrics) so they agree on the same
 * snapshot. Counters are monotonic so subtraction is safe; pre-init
 * / post-shutdown returns all zeroes. Defined here (above the first
 * caller) instead of next to the legacy reporters so the new
 * v8m_get_frag_metrics in the namespaced section can reach it
 * without a forward declaration.
 */
struct v8m_live_stats {
	uint64_t live_regions;
	uint64_t live_bytes;
	uint64_t bytes_mapped;
	uint64_t bytes_unmapped;
	uint64_t mmap_calls;
	uint64_t munmap_calls;
	uint64_t advise_calls;
};

static void v8m_collect_live_stats(struct v8m_live_stats *out)
{
	struct v8m_page_heap_stats stats = {0};
	size_t live_regions = 0;
	if (dispatch_ready()) {
		v8m_page_heap_get_stats(&stats);
		live_regions = v8m_page_heap_live_region_count();
	}
	out->bytes_mapped = stats.bytes_mapped;
	out->bytes_unmapped = stats.bytes_unmapped;
	out->mmap_calls = stats.mmap_calls;
	out->munmap_calls = stats.munmap_calls;
	out->advise_calls = stats.advise_calls;
	/* live_regions comes from the region map directly; the
	 * mmap/munmap call counters cannot be used because the
	 * over-allocate-and-trim strategy emits multiple munmaps
	 * per mmap, leaving the call-count difference net-negative. */
	out->live_regions = (uint64_t)live_regions;
	out->live_bytes = stats.bytes_mapped - stats.bytes_unmapped;
}

static void abort_with(const char *msg)
{
	(void)write(STDERR_FILENO, msg, strlen(msg));
	abort();
}

/* pthread_atfork wrappers — pthread_atfork takes parameter-less
 * function pointers, so the handlers thunk through to the dispatch
 * helpers using the global g_dispatch instance. Each one early-outs
 * unless the dispatcher is fully initialized; that protects against
 * the (unusual) case where another library forks during a
 * constructor chain that runs before ours. */

static void v8m_atfork_prepare(void)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) ==
	    V8M_INIT_READY) {
		v8m_dispatch_prefork(&g_dispatch);
	}
}

static void v8m_atfork_parent(void)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) ==
	    V8M_INIT_READY) {
		v8m_dispatch_postfork_parent(&g_dispatch);
	}
}

static void v8m_atfork_child(void)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) ==
	    V8M_INIT_READY) {
		v8m_dispatch_postfork_child(&g_dispatch);
	}
}

/* Bg-purge per-tick callback. Routed through the dispatch so the
 * bg_purge module stays oblivious to the dispatcher singleton.
 * Skips when the dispatcher is not yet (or no longer) READY so we
 * never touch a teardown-in-progress pool. */
static void v8m_api_bg_tick(void)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) !=
	    V8M_INIT_READY) {
		return;
	}
	(void)v8m_dispatch_bg_tick(&g_dispatch);
}

/* Drain hook for the thread-cache pthread_key destructor. Routes
 * the drain to the dispatcher's slab pool. Skips when the dispatch
 * is not READY so a thread that exits during library teardown does
 * not touch a half-destroyed pool. */
static void v8m_api_drain_thread_cache(struct v8m_thread_cache *cache)
{
	if (atomic_load_explicit(&g_init_state, memory_order_acquire) !=
	    V8M_INIT_READY) {
		return;
	}
	(void)v8m_thread_cache_drain_all(cache, &g_dispatch.slab);
}

__attribute__((constructor(101))) static void v8m_constructor(void)
{
	/* Publish RUNNING before touching anything malloc-shaped so a
	 * reentrant alloc during dispatch init sees a non-READY state
	 * and routes through bootstrap. Ordering: the store is release,
	 * the matching load in dispatch_ready / atfork handlers is
	 * acquire. */
	atomic_store_explicit(&g_init_state, V8M_INIT_RUNNING,
			      memory_order_release);

	/* Resolve libc fallbacks first — dlsym may itself allocate, and
	 * those calls hit our malloc override before dispatch_ready is
	 * true, falling through to the bootstrap allocator. Doing the
	 * resolution before dispatch_init keeps the recursion bounded
	 * to the bootstrap path. */
	v8m_libc_fallback_init();
	v8m_config_init();
	v8m_numa_init();
	if (v8m_dispatch_init(&g_dispatch) != 0) {
		abort_with("v8malloc: dispatch init failed\n");
	}
	/* Opt the global dispatcher into the TLC fast path. Per-thread
	 * caches funnel through this single dispatcher; isolated test
	 * fixtures that create their own dispatcher leave use_tlc false
	 * to avoid mixing slab pages from different pools. */
	v8m_dispatch_set_use_tlc(&g_dispatch, true);
	/* Wire the global per-NUMA huge-page pool and route the global
	 * slab pool's fresh-page acquisitions through it. Failure to
	 * init the per-NUMA pool is non-fatal — the slab pool falls
	 * back to discrete page-heap allocation. Test fixtures that
	 * create their own slab pools never opt in, so their pages
	 * stay on the discrete path. */
	if (v8m_slab_pool_global_init() == 0) {
		v8m_slab_pool_set_use_numa_pool(&g_dispatch.slab, true);
	}
	/* Register fork handlers before publishing READY so that any
	 * thread that calls fork() the moment we go live sees the
	 * locks acquired in deterministic order. pthread_atfork itself
	 * may allocate; that goes through bootstrap. */
	if (pthread_atfork(v8m_atfork_prepare, v8m_atfork_parent,
			   v8m_atfork_child) != 0) {
		abort_with("v8malloc: pthread_atfork registration failed\n");
	}
	atomic_store_explicit(&g_init_state, V8M_INIT_READY,
			      memory_order_release);

	/* Verbose-mode ISA / cache-line / TSC summary. Runs after
	 * READY so the runtime probes (LSE, CTR_EL0, TSC calibration)
	 * have everything they need. The `v8malloc isa:` prefix is
	 * what a multi-arch CI matrix lane greps to confirm the right
	 * binary is running on the right runner — resolution of the
	 * riscv64 follow-on diagnostic note in TODO.md §Phase 3. */
	if (v8m_config_get(V8M_OPT_VERBOSE) != 0) {
		char isa_line[160];
		size_t len =
		    v8m_arch_format_isa_summary(isa_line, sizeof(isa_line));
		if (len > 0U) {
			(void)write(STDERR_FILENO, isa_line, len);
			(void)write(STDERR_FILENO, "\n", 1);
		}
	}

	/* TLC plumbing — wires the pthread_key whose destructor
	 * reclaims a thread's cache on thread exit. Failure here
	 * leaks per-thread caches at thread exit; the allocator stays
	 * usable, so we log and continue rather than aborting. */
	if (v8m_thread_cache_module_init() != 0) {
		(void)write(
		    STDERR_FILENO,
		    "v8malloc: thread-cache module init failed\n",
		    sizeof("v8malloc: thread-cache module init failed\n") - 1U);
	}
	/* Install the drain hook so the destructor flushes cached
	 * slots back to the slab pool before freeing the cache
	 * struct. Without this, every thread exit would leak its
	 * cached objects (the slab pages would still consider them
	 * allocated until the surrounding pages drained empty by
	 * other means). */
	v8m_thread_cache_set_drain_hook(v8m_api_drain_thread_cache);

	/* Install the per-tick callback before the bg thread starts so
	 * the very first scan pass already exercises it. The hook is a
	 * thin wrapper around v8m_dispatch_bg_tick; defining it here
	 * keeps v8m_bg_purge.c free of v8m_dispatch.h knowledge. */
	v8m_bg_purge_set_tick_hook(v8m_api_bg_tick);

	/* Background purge thread spawns last so the dispatcher and
	 * fork handlers are fully usable before the thread can run.
	 * Failure to spawn is non-fatal: the allocator stays usable,
	 * we just lose the periodic scan. */
	int bg_rc = v8m_bg_purge_init();
	if (bg_rc != 0) {
		(void)write(
		    STDERR_FILENO, "v8malloc: bg purge thread spawn failed\n",
		    sizeof("v8malloc: bg purge thread spawn failed\n") - 1U);
	}
}

__attribute__((destructor(101))) static void v8m_destructor(void)
{
	/* Mark the dispatcher as shut down so any allocation from a
	 * post-destructor call site (rare but possible) sees the
	 * "not ready" state and serves from bootstrap. We deliberately
	 * do NOT call v8m_dispatch_destroy here:
	 *
	 *   On process exit, glibc's _IO_cleanup atexit handler
	 *   flushes stdio AFTER our destructor runs, and the stdout
	 *   buffer was malloc'd through us — tearing the dispatcher
	 *   down (unmapping the buddy arena that held the buffer)
	 *   leaves the flush writing to an unmapped page and produces
	 *   silent output loss. The OS reclaims our mmap'd regions
	 *   when the process exits, so skipping the in-process
	 *   teardown is harmless in the LD_PRELOAD / static-link case.
	 *
	 *   The dlopen/dlclose case (where the destructor needs to
	 *   actually return memory because the process keeps running)
	 *   is documented as v0-unsupported; a future cycle adds the
	 *   atexit-based teardown that defers munmaps until after
	 *   stdio cleanup.
	 */
	/* If V8M_PROFILE is set, emit two snapshots:
	 *   1. A malloc_info XML dump to stderr — human-readable
	 *      summary that grep tooling can pull from logs.
	 *   2. A pprof-format heap profile (open question #5) to a
	 *      file path resolved from $V8M_PROFILE_PATH (default
	 *      `/tmp/v8malloc-PID.pb`) so existing `pprof` /
	 *      `go tool pprof` tools can ingest the per-size-class
	 *      breakdown. The .pb is uncompressed protobuf — pprof
	 *      reads both .pb and .pb.gz. Failures (cannot open
	 *      path, encoder overflow) are logged to stderr but
	 *      non-fatal: the destructor must still complete so the
	 *      OS can reap our mappings. */
	if (v8m_config_get(V8M_OPT_PROFILE) != 0) {
		(void)malloc_info(0, stderr);
		/* getenv / strerror are not multi-thread-safe per POSIX,
		 * but the destructor runs after the bg purge thread has
		 * joined and we are on the only remaining thread of the
		 * process. The clang-tidy concurrency check has no model
		 * for that single-thread-at-shutdown invariant. */
		/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
		const char *path = getenv("V8M_PROFILE_PATH");
		char path_buf[64];
		if (path == NULL) {
			/* Default to the .pb.gz extension so the path
			 * matches the resolution of open question #5
			 * verbatim. The gzip wrapper uses STORED DEFLATE
			 * blocks (no actual compression) — pprof reads
			 * either form. The .gz extension is what tools
			 * key off when auto-detecting format. */
			(void)snprintf(path_buf, sizeof(path_buf),
				       "/tmp/v8malloc-%d.pb.gz", (int)getpid());
			path = path_buf;
		}
		if (v8m_pprof_dump_heap_to_path(path) != 0) {
			(void)fprintf(
			    stderr,
			    "v8malloc PROFILE: pprof dump to %s "
			    "failed (%s)\n",
			    path,
			    /* NOLINTNEXTLINE(concurrency-mt-unsafe) */
			    strerror(errno));
		} else if (v8m_config_get(V8M_OPT_VERBOSE) != 0) {
			(void)fprintf(stderr,
				      "v8malloc PROFILE: pprof heap profile "
				      "written to %s\n",
				      path);
		}
	}

	/* If V8M_DEBUG is set, emit a one-line leak summary so a
	 * bug that drops a free() on the floor surfaces at process
	 * exit instead of being absorbed by the OS reaping our
	 * mappings. Reads the same `live_bytes` / `live_regions`
	 * counters `v8m_get_stats` exposes — no per-allocation
	 * tracking, just the aggregate. The threshold cuts out the
	 * "one slab page residue from internal state" false
	 * positive (the allocator routinely retains a single slab
	 * page across the destructor for late libc / stderr-buffer
	 * allocations); a real per-allocation leak detector with
	 * call-site capture lands in a future cycle (api.md §6.2). */
	if (v8m_config_get(V8M_OPT_DEBUG) != 0) {
		struct v8m_live_stats live = {0};
		v8m_collect_live_stats(&live);
		const uint64_t bytes_threshold = (uint64_t)1 << 20U; /* 1 MiB */
		const uint64_t region_threshold = 4U;
		if (live.live_bytes > bytes_threshold ||
		    live.live_regions > region_threshold) {
			(void)fprintf(stderr,
				      "v8malloc DEBUG: leak summary at exit — "
				      "live_bytes=%llu live_regions=%llu "
				      "(thresholds: bytes>%llu regions>%llu)\n",
				      (unsigned long long)live.live_bytes,
				      (unsigned long long)live.live_regions,
				      (unsigned long long)bytes_threshold,
				      (unsigned long long)region_threshold);
		}
	}

	/* Stop the bg purge thread before flipping state so the
	 * thread's last loop body sees READY (avoids it spinning on
	 * stale state during shutdown). The shutdown path uses a
	 * condvar signal, so the join completes within a futex hop
	 * regardless of the configured purge interval. Clear the
	 * tick hook first so a synchronous v8m_purge() that races
	 * the destructor cannot enter the dispatch after teardown. */
	v8m_bg_purge_set_tick_hook(NULL);
	v8m_bg_purge_shutdown();
	/* Clear the drain hook so a tail destructor (a thread that
	 * exits after our shutdown) does not call into a partially
	 * torn-down dispatcher. */
	v8m_thread_cache_set_drain_hook(NULL);
	v8m_thread_cache_module_shutdown();
	(void)atomic_exchange_explicit(&g_init_state, V8M_INIT_TORN_DOWN,
				       memory_order_acquire);
}

static bool dispatch_ready(void)
{
	return atomic_load_explicit(&g_init_state, memory_order_acquire) ==
	       V8M_INIT_READY;
}

/* --- v8m_-prefixed API --------------------------------------------- */

/*
 * True iff serving `size` more bytes would push live page-heap
 * bytes above the soft limit. A limit of 0 disables the check.
 * Reads `live_bytes` from the page heap stats — slightly stale
 * under concurrency but the limit is advisory, so an occasional
 * over-shoot is acceptable.
 */
static bool over_soft_limit(size_t size)
{
	size_t limit =
	    atomic_load_explicit(&g_soft_limit, memory_order_relaxed);
	if (limit == 0U) {
		return false;
	}
	struct v8m_page_heap_stats stats = {0};
	v8m_page_heap_get_stats(&stats);
	uint64_t live = stats.bytes_mapped - stats.bytes_unmapped;
	if (live + size <= limit) {
		return false;
	}
	/* Live bytes count drained-but-still-mapped buddy arenas; under
	 * pressure those are reclaimable, so force-release them and
	 * recheck before refusing. Without this an OOM handler that
	 * frees a buddy block to make headroom would never see the
	 * limit drop, even though the kernel could give the pages back
	 * immediately. */
	(void)v8m_dispatch_purge_drained(&g_dispatch);
	v8m_page_heap_get_stats(&stats);
	live = stats.bytes_mapped - stats.bytes_unmapped;
	return live + size > limit;
}

/*
 * Invoke the installed OOM handler (if any) and return whether a
 * retry was requested. Reentrancy-safe: a sub-allocation made by
 * the handler that itself fails will see `t_oom_in_handler == true`
 * and skip the recursive callback. */
static bool oom_handler_says_retry(size_t size)
{
	if (t_oom_in_handler) {
		return false;
	}
	v8m_oom_handler_t handler =
	    atomic_load_explicit(&g_oom_handler, memory_order_acquire);
	if (handler == NULL) {
		return false;
	}
	t_oom_in_handler = true;
	int retry = handler(size);
	t_oom_in_handler = false;
	return retry != 0;
}

/* Shared malloc body parameterized on the user's caller PC. Every
 * public entry that ultimately serves a malloc-shaped allocation
 * (`v8m_malloc`, `v8m_calloc`, `v8m_realloc`'s alloc paths) routes
 * through this helper passing its own `__builtin_return_address(0)`
 * — that way the predictive prefetch table and the lifetime tracker
 * see the user's actual call site, not the v8malloc internal frame
 * that would result from one entry point thunking through another. */
static void *do_malloc_pc(size_t size, const void *caller_pc)
{
	if (!dispatch_ready()) {
		/* Pre-init / post-shutdown — serve from bootstrap.
		 * size == 0 still produces a unique pointer per our
		 * malloc(0) policy. */
		return v8m_bootstrap_alloc(size > 0U ? size : 1U);
	}
	if (over_soft_limit(size)) {
		if (oom_handler_says_retry(size) && !over_soft_limit(size)) {
			/* The handler released enough memory to fit. */
		} else {
			errno = ENOMEM;
			return NULL;
		}
	}
	/* Predictive prefetch (winning-algorithms.md §9): hash the
	 * caller PC, look up the most recently observed class for
	 * that call site, prefetch the matching bin head into L1.
	 * Cheap (one indexed byte read + one prefetch hint) and
	 * harmless when the table has not learned this call site
	 * yet. v8m_thread_cache_peek avoids creating a cache just
	 * for the prefetch — the dispatcher's own cache_get will
	 * create one if needed for the actual alloc. */
	struct v8m_thread_cache *cache = v8m_thread_cache_peek();
	if (cache != NULL) {
		v8m_thread_cache_predict_prefetch(cache, caller_pc);
	}
	/* Lifetime arena routing: hand the dispatcher the user's
	 * caller PC so it can classify and pick the matching slab
	 * arena (default, ephemeral, short, long). The hint is reset
	 * to NULL after the alloc so a stale value can't leak into a
	 * subsequent allocation that runs without v8m_malloc on the
	 * stack (e.g. internal v8malloc machinery). */
	v8m_dispatch_set_caller_pc(caller_pc);
	void *ptr = v8m_dispatch_alloc(&g_dispatch, size);
	if (ptr == NULL && oom_handler_says_retry(size)) {
		ptr = v8m_dispatch_alloc(&g_dispatch, size);
	}
	v8m_dispatch_set_caller_pc(NULL);
	if (ptr == NULL) {
		errno = ENOMEM;
		return ptr;
	}
	/* Update the predict slot with the class we actually served.
	 * Re-peek the cache because the dispatcher may have just
	 * created one if this was the thread's first allocation. */
	cache = v8m_thread_cache_peek();
	if (cache != NULL) {
		v8m_thread_cache_predict_update(cache, caller_pc,
						v8m_size_class(size));
	}
	/* Lifetime tracker (fragmentation.md §5.2). The helper exits
	 * early when V8M_OPT_LIFETIME_TRACKING is off, so the cost is
	 * one config load + one branch in the common path. */
	v8m_thread_cache_lifetime_record_alloc(ptr, caller_pc,
					       v8m_arch_rdtsc());
	return ptr;
}

/* Shared aligned-alloc body parameterized on the user's caller PC.
 * Mirror of `do_malloc_pc` for the alignment-aware path: prefetch
 * + arena routing + lifetime tracker hooks all consult the user's
 * actual call site. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void *do_aligned_alloc_pc(size_t alignment, size_t size,
				 const void *caller_pc)
{
	void *ptr = NULL;
	struct v8m_thread_cache *cache = v8m_thread_cache_peek();
	if (cache != NULL) {
		v8m_thread_cache_predict_prefetch(cache, caller_pc);
	}
	v8m_dispatch_set_caller_pc(caller_pc);
	ptr = v8m_dispatch_alloc_aligned(&g_dispatch, size, alignment);
	v8m_dispatch_set_caller_pc(NULL);
	if (ptr == NULL) {
		return NULL;
	}
	cache = v8m_thread_cache_peek();
	if (cache != NULL) {
		v8m_thread_cache_predict_update(cache, caller_pc,
						v8m_size_class(size));
	}
	v8m_thread_cache_lifetime_record_alloc(ptr, caller_pc,
					       v8m_arch_rdtsc());
	return ptr;
}

V8M_EXPORT void *v8m_malloc(size_t size)
{
	return do_malloc_pc(size, __builtin_return_address(0));
}

/* Double-free detection ring buffer (api.md §6.2). Off the hot
 * path entirely when V8M_OPT_DEBUG is 0 — the ring touch is gated
 * behind the config check. When DEBUG is on, every free consults
 * the ring; if the pointer is already there, abort with a
 * diagnostic. The ring is small (4096 entries = 32 KiB) and the
 * overwrite policy is round-robin: a sustained free rate above
 * 4096 ops between detections will miss the duplicate. That's the
 * tradeoff for not paying per-pointer hash-table cost. The full
 * call-site-aware leak detector lands later. */
#define V8M_DOUBLE_FREE_RING_SIZE 4096
static void *g_double_free_ring[V8M_DOUBLE_FREE_RING_SIZE];
static atomic_size_t g_double_free_ring_idx;
/* NOLINTNEXTLINE(misc-include-cleaner) — pthread.h is included above */
static pthread_mutex_t g_double_free_ring_lock = PTHREAD_MUTEX_INITIALIZER;

static void abort_on_double_free(const void *ptr)
{
	(void)fprintf(stderr, "v8malloc DEBUG: double-free detected at %p\n",
		      ptr);
	abort();
}

static void debug_check_double_free(void *ptr)
{
	(void)pthread_mutex_lock(&g_double_free_ring_lock);
	for (size_t i = 0; i < V8M_DOUBLE_FREE_RING_SIZE; i++) {
		if (g_double_free_ring[i] == ptr) {
			(void)pthread_mutex_unlock(&g_double_free_ring_lock);
			abort_on_double_free(ptr);
			return; /* unreachable */
		}
	}
	size_t slot = atomic_fetch_add_explicit(&g_double_free_ring_idx, 1U,
						memory_order_relaxed) %
		      V8M_DOUBLE_FREE_RING_SIZE;
	g_double_free_ring[slot] = ptr;
	(void)pthread_mutex_unlock(&g_double_free_ring_lock);
}

V8M_EXPORT void v8m_free(void *ptr)
{
	if (ptr == NULL) {
		return;
	}
	if (v8m_ptr_is_bootstrap(ptr)) {
		/* Bootstrap allocations have no per-pointer free path
		 * — they're released only when the buffer is reset,
		 * which never happens in v0. */
		return;
	}
	if (v8m_ptr_is_signal_safe(ptr)) {
		/* Signal-safe emergency allocations leak by design;
		 * the buffer is small and any per-pointer reclamation
		 * would need a free list, which would not be
		 * async-signal-safe under contention. */
		return;
	}
	if (!dispatch_ready()) {
		return;
	}
	if (v8m_config_get(V8M_OPT_DEBUG) != 0) {
		debug_check_double_free(ptr);
	}
	/* Lifetime tracker — same opt-in early-exit pattern as
	 * record_alloc. The TSC read on this branch is cheap (single
	 * rdtsc on x86_64, clock_gettime fallback elsewhere) but only
	 * runs when the option is on, which the helper checks itself. */
	v8m_thread_cache_lifetime_record_free(ptr, v8m_arch_rdtsc());
	v8m_dispatch_free(&g_dispatch, ptr);
}

V8M_EXPORT void *v8m_calloc(size_t nmemb, size_t size)
{
	if (size != 0U && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	size_t total = nmemb * size;
	/* Pass calloc's own caller PC into do_malloc_pc so the predict
	 * table and lifetime tracker see the user's call site, not the
	 * v8m_calloc body. */
	void *ptr = do_malloc_pc(total, __builtin_return_address(0));
	if (ptr != NULL && total > 0U) {
		(void)memset(ptr, 0, total);
	}
	return ptr;
}

/* `ptr` is non-const to match the POSIX malloc_usable_size(void *)
 * signature even though the implementation never writes through it. */
/* cppcheck-suppress constParameterPointer */
V8M_EXPORT size_t v8m_malloc_usable_size(void *ptr)
{
	if (ptr == NULL) {
		return 0;
	}
	if (v8m_ptr_is_bootstrap(ptr)) {
		/* Bootstrap doesn't track per-allocation sizes; the
		 * caller can use v8m_bootstrap_remaining for an upper
		 * bound on what's safe to read. */
		return 0;
	}
	if (!dispatch_ready()) {
		return 0;
	}
	return v8m_dispatch_usable_size(&g_dispatch, ptr);
}

V8M_EXPORT void *v8m_realloc(void *ptr, size_t size)
{
	/* Capture once at entry so both the realloc(NULL, n) shortcut
	 * and the alloc-and-move path attribute the new allocation to
	 * the user's actual call site. */
	const void *caller_pc = __builtin_return_address(0);
	if (ptr == NULL) {
		return do_malloc_pc(size, caller_pc);
	}
	if (size == 0U) {
		v8m_free(ptr);
		return NULL;
	}

	bool is_bootstrap = v8m_ptr_is_bootstrap(ptr);
	size_t old_usable = is_bootstrap ? v8m_bootstrap_remaining(ptr)
					 : v8m_malloc_usable_size(ptr);

	if (!is_bootstrap && old_usable >= size) {
		return ptr; /* shrink / fits in place */
	}

	void *new_ptr = do_malloc_pc(size, caller_pc);
	if (new_ptr == NULL) {
		return NULL;
	}

	size_t copy = (old_usable < size) ? old_usable : size;
	if (copy > 0U) {
		(void)memcpy(new_ptr, ptr, copy);
	}
	v8m_free(ptr);
	return new_ptr;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (size != 0U && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	return v8m_realloc(ptr, nmemb * size);
}

/*
 * is_pow2 — true iff `value` is a non-zero power of two. Used to
 * validate the alignment argument of every aligned-alloc entry.
 */
static bool is_pow2(size_t value)
{
	return value != 0U && (value & (value - 1U)) == 0U;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_aligned_alloc(size_t alignment, size_t size)
{
	if (!is_pow2(alignment)) {
		errno = EINVAL;
		return NULL;
	}
	if (!dispatch_ready()) {
		/* Pre-init allocations cannot honour custom alignment;
		 * the bootstrap pointer is only 16-byte aligned. */
		if (alignment <= 16U) {
			return v8m_bootstrap_alloc(size > 0U ? size : 1U);
		}
		errno = ENOMEM;
		return NULL;
	}
	void *ptr =
	    do_aligned_alloc_pc(alignment, size, __builtin_return_address(0));
	if (ptr == NULL) {
		errno = ENOMEM;
	}
	return ptr;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int v8m_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (memptr == NULL) {
		return EINVAL;
	}
	/* posix_memalign requires alignment to be a power of two AND a
	 * multiple of sizeof(void *). */
	if (!is_pow2(alignment) || (alignment % sizeof(void *)) != 0U) {
		return EINVAL;
	}
	if (!dispatch_ready()) {
		if (alignment <= 16U) {
			void *ptr = v8m_bootstrap_alloc(size > 0U ? size : 1U);
			if (ptr == NULL) {
				return ENOMEM;
			}
			*memptr = ptr;
			return 0;
		}
		return ENOMEM;
	}
	void *ptr =
	    do_aligned_alloc_pc(alignment, size, __builtin_return_address(0));
	if (ptr == NULL) {
		return ENOMEM;
	}
	*memptr = ptr;
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_memalign(size_t alignment, size_t size)
{
	/* memalign(3) is the looser glibc cousin of aligned_alloc — it
	 * doesn't require size to be a multiple of alignment. The C11
	 * relaxation made aligned_alloc match this behaviour, so the two
	 * are functionally identical here. */
	return v8m_aligned_alloc(alignment, size);
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_valloc(size_t size)
{
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0) {
		page = 4096; /* defensive default */
	}
	return v8m_aligned_alloc((size_t)page, size);
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void *v8m_pvalloc(size_t size)
{
	long page_signed = sysconf(_SC_PAGESIZE);
	size_t page = (page_signed > 0) ? (size_t)page_signed : 4096U;
	/* pvalloc rounds size up to the next page boundary. Treat 0 as
	 * one page, matching glibc. */
	if (size == 0U) {
		size = page;
	}
	if (size > SIZE_MAX - (page - 1U)) {
		errno = ENOMEM;
		return NULL;
	}
	size_t rounded = (size + page - 1U) & ~(page - 1U);
	return v8m_aligned_alloc(page, rounded);
}

/* --- v8m_-prefixed configuration & stats -------------------------- */

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int v8m_set_option(int opt, int64_t value)
{
	if (!dispatch_ready()) {
		errno = EAGAIN;
		return -1;
	}
	if (opt < 0 || opt >= V8M_OPT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	if (v8m_config_set((enum v8m_option)opt, value) != 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int v8m_get_option(int opt, int64_t *out)
{
	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (opt < 0 || opt >= V8M_OPT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	if (!dispatch_ready()) {
		errno = EAGAIN;
		return -1;
	}
	*out = v8m_config_get((enum v8m_option)opt);
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT void v8m_get_stats(struct v8m_stats *out)
{
	if (out == NULL) {
		return;
	}
	struct v8m_page_heap_stats stats = {0};
	size_t live_regions = 0;
	if (dispatch_ready()) {
		v8m_page_heap_get_stats(&stats);
		live_regions = v8m_page_heap_live_region_count();
	}
	out->mmap_calls = stats.mmap_calls;
	out->munmap_calls = stats.munmap_calls;
	out->advise_calls = stats.advise_calls;
	out->bytes_mapped = stats.bytes_mapped;
	out->bytes_unmapped = stats.bytes_unmapped;
	out->live_regions = (uint64_t)live_regions;
	out->live_bytes = stats.bytes_mapped - stats.bytes_unmapped;
}

V8M_EXPORT void v8m_dump_stats(void)
{
	malloc_stats();
}

V8M_EXPORT void v8m_get_huge_stats(struct v8m_huge_stats *out)
{
	if (out == NULL) {
		return;
	}
	struct v8m_large_stats raw = {0};
	if (dispatch_ready()) {
		v8m_large_get_stats(&raw);
	}
	out->large_alloc_count = raw.large_alloc_count;
	out->large_free_count = raw.large_free_count;
	out->large_bytes_in_use = raw.large_bytes_in_use;
	out->huge_alloc_count = raw.huge_alloc_count;
	out->huge_free_count = raw.huge_free_count;
	out->huge_bytes_in_use = raw.huge_bytes_in_use;
}

V8M_EXPORT void v8m_get_thread_stats(struct v8m_thread_stats *out)
{
	if (out == NULL) {
		return;
	}
	/* TLC absent in v0; the public surface lands ahead of the
	 * implementation so consumers can compile against the contract.
	 * Once the thread cache lands, the per-thread counters move
	 * into `__thread` storage and this getter snapshots them. */
	out->fast_path_allocs = 0;
	out->slow_path_allocs = 0;
	out->fast_path_frees = 0;
	out->remote_frees_received = 0;
	out->bin_overflow_flushes = 0;
}

V8M_EXPORT void
v8m_get_size_class_histogram(struct v8m_size_class_histogram *out)
{
	if (out == NULL) {
		return;
	}
	if (!dispatch_ready()) {
		(void)memset(out, 0, sizeof(*out));
		return;
	}
	v8m_thread_cache_aggregate_histogram(out);
}

V8M_EXPORT int v8m_install_size_class_table(const uint32_t *new_table)
{
	if (new_table != NULL && new_table[0] < 8U) {
		errno = EINVAL;
		return -1;
	}
	(void)v8m_size_class_install_table(new_table);
	return 0;
}

V8M_EXPORT uint32_t v8m_size_class_to_bytes(int cls)
{
	if (cls < 0 || (uint32_t)cls >= V8M_NUM_SIZE_CLASSES) {
		return 0U;
	}
	return v8m_size_class_size((uint32_t)cls);
}

/* Mirror of arch_name() in src/v8m_arch.c. The arch.c helper is
 * static so we can't share it across TUs; the surface is small
 * enough that duplicating it costs less than exposing a private
 * API. A future cycle can promote arch_name() to v8m_arch.h if
 * another consumer needs it. */
static const char *api_arch_name(void)
{
#if defined(V8M_ARCH_X86_64)
	return "x86_64";
#elif defined(V8M_ARCH_AARCH64)
	return "aarch64";
#elif defined(V8M_ARCH_RISCV64)
	return "riscv64";
#elif defined(V8M_ARCH_PPC64LE)
	return "ppc64le";
#elif defined(V8M_ARCH_S390X)
	return "s390x";
#elif defined(V8M_ARCH_LOONGARCH64)
	return "loongarch64";
#else
	return "unknown";
#endif
}

V8M_EXPORT int v8m_init_thread(void)
{
	if (!dispatch_ready()) {
		errno = EAGAIN;
		return -1;
	}
	const struct v8m_thread_cache *cache = v8m_thread_cache_get_or_create();
	if (cache == NULL) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

V8M_EXPORT int v8m_release_thread(void)
{
	if (!dispatch_ready()) {
		errno = EAGAIN;
		return -1;
	}
	/* Drain TLC bins + the calling thread's L2 contribution
	 * before releasing the TLS slot. Without the drain, the
	 * subsequent free of the cache struct would lose the cached
	 * slots (the slab pages would still consider them allocated
	 * until the surrounding pages drained empty by other means).
	 * Same drain shape `v8m_purge_thread` runs, but here we go
	 * one step further and reset the TLS slot too. */
	struct v8m_thread_cache *cache = v8m_thread_cache_peek();
	if (cache != NULL) {
		(void)v8m_thread_cache_drain_all(cache, &g_dispatch.slab);
	}
	(void)v8m_dispatch_drain_local_l2(&g_dispatch);
	v8m_thread_cache_release_local();
	return 0;
}

V8M_EXPORT void v8m_get_arch_info(struct v8m_arch_info *out)
{
	if (out == NULL) {
		return;
	}
	(void)memset(out, 0, sizeof(*out));
	const char *name = api_arch_name();
	size_t name_len = strlen(name);
	if (name_len >= sizeof(out->arch_name)) {
		name_len = sizeof(out->arch_name) - 1U;
	}
	(void)memcpy(out->arch_name, name, name_len);
	out->arch_name[name_len] = '\0';
	out->cache_line_bytes = (uint32_t)v8m_arch_runtime_cache_line_size();
	out->build_cache_line_bytes = (uint32_t)V8M_CACHE_LINE_SIZE;
	out->tsc_mhz = v8m_arch_tsc_frequency_mhz();
	out->has_lse = v8m_arch_has_lse() ? 1U : 0U;
}

V8M_EXPORT void v8m_get_slab_class_breakdown(
    struct v8m_slab_class_breakdown out[V8M_PUBLIC_NUM_SIZE_CLASSES])
{
	if (out == NULL) {
		return;
	}
	(void)memset(out, 0,
		     sizeof(struct v8m_slab_class_breakdown) *
			 V8M_PUBLIC_NUM_SIZE_CLASSES);
	if (!dispatch_ready()) {
		return;
	}
	struct v8m_slab_pool_class_stats raw[V8M_MEDIUM_FIRST_CLASS] = {0};
	v8m_slab_pool_get_class_stats(&g_dispatch.slab, raw,
				      V8M_MEDIUM_FIRST_CLASS);
	for (uint32_t i = 0; i < V8M_MEDIUM_FIRST_CLASS; i++) {
		out[i].pages_in_use = raw[i].pages_in_use;
		out[i].slots_total = raw[i].slots_total;
		out[i].slots_used = raw[i].slots_used;
		out[i].utilization_pct =
		    raw[i].slots_total == 0U
			? 0U
			: (uint32_t)((raw[i].slots_used * 100U) /
				     raw[i].slots_total);
	}
	/* Indices [V8M_MEDIUM_FIRST_CLASS, V8M_PUBLIC_NUM_SIZE_CLASSES)
	 * cover Medium / Large / Huge classes that have no slab
	 * backing — leave them zeroed by the memset above. */
}

V8M_EXPORT void v8m_get_lifetime_stats(struct v8m_lifetime_stats *out)
{
	if (out == NULL) {
		return;
	}
	if (!dispatch_ready()) {
		(void)memset(out, 0, sizeof(*out));
		return;
	}
	v8m_thread_cache_aggregate_lifetime(out);
}

V8M_EXPORT enum v8m_lifetime_class v8m_estimate_lifetime(const void *caller_pc)
{
	if (!dispatch_ready()) {
		return V8M_LIFETIME_UNKNOWN;
	}
	return v8m_thread_cache_lifetime_classify(caller_pc);
}

V8M_EXPORT void v8m_get_numa_balance(struct v8m_numa_balance_stats *out)
{
	if (out == NULL) {
		return;
	}
	if (!dispatch_ready()) {
		(void)memset(out, 0, sizeof(*out));
		return;
	}
	v8m_page_heap_get_numa_balance(out);
}

V8M_EXPORT uint64_t v8m_count_vmas(void)
{
	/* Open /proc/self/maps with raw read() to avoid an alloc on
	 * the fast path — fopen pulls in libio buffers that would
	 * route back through v8malloc. open + read + close are
	 * malloc-free. Each line in /proc/self/maps describes one
	 * VMA, so the line count is the VMA count. */
	int maps_fd = open("/proc/self/maps", O_RDONLY);
	if (maps_fd < 0) {
		return 0;
	}
	uint64_t lines = 0;
	char buf[4096];
	for (;;) {
		ssize_t got = read(maps_fd, buf, sizeof(buf));
		if (got <= 0) {
			break;
		}
		for (ssize_t i = 0; i < got; i++) {
			if (buf[i] == '\n') {
				lines++;
			}
		}
	}
	(void)close(maps_fd);
	return lines;
}

V8M_EXPORT void v8m_get_frag_metrics(struct v8m_frag_metrics *out)
{
	if (out == NULL) {
		return;
	}
	struct v8m_live_stats live = {0};
	struct v8m_large_stats large = {0};
	struct v8m_slab_pool_aggregate_stats slab = {0};
	if (dispatch_ready()) {
		v8m_collect_live_stats(&live);
		v8m_large_get_stats(&large);
		v8m_slab_pool_get_aggregate_stats(&g_dispatch.slab, &slab);
	}
	out->live_regions = live.live_regions;
	out->live_bytes = live.live_bytes;
	out->bytes_per_region = (live.live_regions == 0U)
				    ? 0U
				    : live.live_bytes / live.live_regions;
	out->region_map_capacity = 4096U; /* matches V8M_REGION_MAP_CAPACITY */
	out->region_map_used_pct = (live.live_regions * 100U) / 4096U;
	out->large_live_count =
	    large.large_alloc_count - large.large_free_count;
	out->huge_live_count = large.huge_alloc_count - large.huge_free_count;
	out->vma_count = v8m_count_vmas();
	out->slab_pages_in_use = slab.pages_in_use;
	out->slab_slots_total = slab.slots_total;
	out->slab_slots_used = slab.slots_used;
	out->slab_utilization_pct =
	    (slab.slots_total == 0U)
		? 0U
		: (slab.slots_used * 100U) / slab.slots_total;
}

V8M_EXPORT int v8m_purge(void)
{
	/* Drain the calling thread's TLC bins back to the slab pool
	 * so single-thread workloads (the thread never exits, the
	 * pthread_key destructor never fires) see their cached free
	 * slots returned to the pool. Force-release every drained
	 * buddy arena next (skips the idle-tick grace window so an
	 * explicit caller gets immediate VMA + RSS relief), then run
	 * the bg-purge scan pass for its diagnostic side effects
	 * (VMA-threshold check, optional verbose stats line). Slab
	 * pages and Large/Huge regions are already released eagerly
	 * on free, so there is nothing else to do for them here. */
	if (dispatch_ready()) {
		struct v8m_thread_cache *cache = v8m_thread_cache_peek();
		if (cache != NULL) {
			(void)v8m_thread_cache_drain_all(cache,
							 &g_dispatch.slab);
		}
		/* Drain the calling thread's current-CPU L2 too —
		 * slots cached there hold slab pages alive past the
		 * tier the dispatch's drained-arena counter measures. */
		(void)v8m_dispatch_drain_local_l2(&g_dispatch);
		(void)v8m_dispatch_purge_drained(&g_dispatch);
	}
	v8m_bg_purge_run_once();
	return 0;
}

V8M_EXPORT int v8m_purge_thread(void)
{
	/* Drain the calling thread's TLC bins back to the slab pool.
	 * Distinct from v8m_purge in that it does NOT touch the
	 * process-wide buddy arenas or the bg-purge scan body —
	 * useful for callers that want to release per-thread
	 * caching pressure without the global side effects. Also
	 * drains the calling thread's current-CPU L2 so cached
	 * slots there release the slab pages they pin. */
	if (dispatch_ready()) {
		struct v8m_thread_cache *cache = v8m_thread_cache_peek();
		if (cache != NULL) {
			(void)v8m_thread_cache_drain_all(cache,
							 &g_dispatch.slab);
		}
		(void)v8m_dispatch_drain_local_l2(&g_dispatch);
	}
	return 0;
}

/* --- v8m_-namespaced glibc-compat wrappers ------------------------ */

V8M_EXPORT struct mallinfo v8m_mallinfo(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	return mallinfo();
#pragma GCC diagnostic pop
}

V8M_EXPORT struct mallinfo2 v8m_mallinfo2(void)
{
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	return mallinfo2();
}

V8M_EXPORT void v8m_malloc_stats(void)
{
	malloc_stats();
}

V8M_EXPORT int v8m_malloc_info(int options, void *stream)
{
	return malloc_info(options, (FILE *)stream);
}

V8M_EXPORT int v8m_mallopt(int param, int value)
{
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	return mallopt(param, value);
}

V8M_EXPORT int v8m_malloc_trim(size_t pad)
{
	/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
	return malloc_trim(pad);
}

V8M_EXPORT v8m_oom_handler_t v8m_set_oom_handler(v8m_oom_handler_t handler)
{
	return atomic_exchange_explicit(&g_oom_handler, handler,
					memory_order_acq_rel);
}

V8M_EXPORT void v8m_set_soft_limit(size_t bytes)
{
	atomic_store_explicit(&g_soft_limit, bytes, memory_order_relaxed);
}

V8M_EXPORT size_t v8m_get_soft_limit(void)
{
	return atomic_load_explicit(&g_soft_limit, memory_order_relaxed);
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int v8m_ptr_info(const void *ptr, struct v8m_ptr_info *out)
{
	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	out->backend = V8M_PTR_FOREIGN;
	out->usable_size = 0;
	out->size_class = -1;

	if (ptr == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (v8m_ptr_is_bootstrap(ptr)) {
		out->backend = V8M_PTR_BOOTSTRAP;
		/* Bootstrap doesn't track per-allocation sizes; report
		 * an upper bound so callers see a non-zero value. */
		out->usable_size = v8m_bootstrap_remaining(ptr);
		return 0;
	}
	if (!dispatch_ready() || !v8m_page_heap_owns(ptr)) {
		errno = EINVAL;
		return -1;
	}

	const struct v8m_page_meta *meta = v8m_ptr_to_meta(ptr);
	if (v8m_page_meta_valid(meta)) {
		if (meta->size_class < V8M_MEDIUM_FIRST_CLASS) {
			out->backend = V8M_PTR_SLAB;
			out->usable_size = meta->object_size;
			out->size_class = meta->size_class;
		} else {
			out->backend = V8M_PTR_LARGE;
			out->usable_size = v8m_large_usable_size(ptr);
		}
		return 0;
	}

	/* Owned by the page heap with no slab/large header → buddy.
	 * v8m_buddy_pool_block_size returns the enclosing block size
	 * for any pointer inside a live allocation, which is what
	 * dispatch_free wants. For introspection we want stricter
	 * start-of-block semantics, so reject pointers that aren't
	 * `bytes`-aligned (every buddy arena is V8M_BUDDY_MAX_BLOCK-
	 * aligned, so block starts are inherently bytes-aligned and
	 * the check is exact). */
	size_t bytes = v8m_buddy_pool_block_size(&g_dispatch.buddy, ptr);
	if (bytes > 0U && ((uintptr_t)ptr & (bytes - 1U)) == 0U) {
		out->backend = V8M_PTR_BUDDY;
		out->usable_size = bytes;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

V8M_EXPORT bool v8m_is_valid_ptr(const void *ptr)
{
	struct v8m_ptr_info info;
	return v8m_ptr_info(ptr, &info) == 0;
}

/* --- glibc statistics / tuning extensions -------------------------- */
/*
 * The functions below are not part of the POSIX core; they exist so
 * that legacy software linked against glibc keeps working when
 * v8malloc replaces the allocator. The numbers we report are
 * derived from the page-heap counters — there's no per-class chunk
 * accounting in v0, so the chunk-level fields stay zero. The
 * picture sharpens once the thread cache + per-class stats land.
 */

/* `int` truncation is intentional — the legacy mallinfo struct
 * predates 64-bit address spaces. Modern code should call mallinfo2
 * instead; mallinfo lives on for ABI compatibility. */
/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT struct mallinfo mallinfo(void)
{
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	struct mallinfo info = {0};
	info.hblks = (int)live.live_regions;
	info.hblkhd = (int)live.live_bytes;
	info.arena = (int)live.live_bytes;
	info.uordblks = (int)live.live_bytes;
	return info;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT struct mallinfo2 mallinfo2(void)
{
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	struct mallinfo2 info = {0};
	info.hblks = live.live_regions;
	info.hblkhd = live.live_bytes;
	info.arena = live.live_bytes;
	info.uordblks = live.live_bytes;
	return info;
}

V8M_EXPORT void malloc_stats(void)
{
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	(void)fprintf(stderr,
		      "v8malloc statistics (page-heap layer):\n"
		      "  mmap   calls: %llu\n"
		      "  munmap calls: %llu\n"
		      "  advise calls: %llu\n"
		      "  bytes mapped:   %llu\n"
		      "  bytes unmapped: %llu\n"
		      "  live regions:   %llu\n"
		      "  live bytes:     %llu\n",
		      (unsigned long long)live.mmap_calls,
		      (unsigned long long)live.munmap_calls,
		      (unsigned long long)live.advise_calls,
		      (unsigned long long)live.bytes_mapped,
		      (unsigned long long)live.bytes_unmapped,
		      (unsigned long long)live.live_regions,
		      (unsigned long long)live.live_bytes);
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int malloc_info(int options, FILE *stream)
{
	(void)options; /* Reserved for future per-arena breakdowns. */
	if (stream == NULL) {
		errno = EINVAL;
		return -1;
	}
	struct v8m_live_stats live;
	v8m_collect_live_stats(&live);
	struct v8m_page_heap_stats ph_stats = {0};
	struct v8m_large_stats large_stats = {0};
	if (dispatch_ready()) {
		v8m_page_heap_get_stats(&ph_stats);
		v8m_large_get_stats(&large_stats);
	}
	uint64_t vmas = v8m_count_vmas();
	(void)fprintf(
	    stream,
	    "<malloc version=\"v8malloc-%s\">\n"
	    "  <heap nr=\"0\">\n"
	    "    <total type=\"mmap\" count=\"%llu\" size=\"%llu\"/>\n"
	    "    <system type=\"current\" size=\"%llu\"/>\n"
	    "    <aspace type=\"total\" size=\"%llu\"/>\n"
	    "    <large alloc=\"%llu\" free=\"%llu\" bytes_in_use=\"%llu\"/>\n"
	    "    <huge alloc=\"%llu\" free=\"%llu\" bytes_in_use=\"%llu\"/>\n"
	    "    <hugetlb attempts=\"%llu\" failures=\"%llu\"/>\n"
	    "    <gigantic attempts=\"%llu\" failures=\"%llu\"/>\n"
	    "    <hugepage_advise calls=\"%llu\"/>\n"
	    "    <mbind calls=\"%llu\" failures=\"%llu\"/>\n"
	    "    <vma count=\"%llu\"/>\n"
	    "  </heap>\n"
	    "</malloc>\n",
	    V8M_VERSION_STRING, (unsigned long long)live.live_regions,
	    (unsigned long long)live.live_bytes,
	    (unsigned long long)live.live_bytes,
	    (unsigned long long)live.bytes_mapped,
	    (unsigned long long)large_stats.large_alloc_count,
	    (unsigned long long)large_stats.large_free_count,
	    (unsigned long long)large_stats.large_bytes_in_use,
	    (unsigned long long)large_stats.huge_alloc_count,
	    (unsigned long long)large_stats.huge_free_count,
	    (unsigned long long)large_stats.huge_bytes_in_use,
	    (unsigned long long)ph_stats.hugetlb_alloc_calls,
	    (unsigned long long)ph_stats.hugetlb_alloc_failures,
	    (unsigned long long)ph_stats.gigantic_alloc_calls,
	    (unsigned long long)ph_stats.gigantic_alloc_failures,
	    (unsigned long long)ph_stats.hugepage_advise_calls,
	    (unsigned long long)ph_stats.mbind_calls,
	    (unsigned long long)ph_stats.mbind_failures,
	    (unsigned long long)vmas);
	return 0;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
/* The mallopt(int, int) signature is fixed by glibc's ABI, so the
 * "easily swappable" linter check is irrelevant here. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
V8M_EXPORT int mallopt(int param, int value)
{
	(void)param;
	(void)value;
	/* The glibc parameter ids (M_TRIM_THRESHOLD, M_MMAP_MAX, ...)
	 * don't map cleanly to v8malloc's internals — runtime
	 * configuration lives behind the V8M_* environment variables
	 * and the v8m_config_set/get pair. mallopt's role here is
	 * pure ABI compatibility for legacy code that calls it
	 * unconditionally. Per the contract, returning 1 means
	 * "the parameter was accepted". */
	return 1;
}

/* cppcheck-suppress staticFunction
 * — the function is part of the public ABI exported by v8malloc.map. */
V8M_EXPORT int malloc_trim(size_t pad)
{
	(void)pad;
	/* No-op for v0: the page heap returns regions to the kernel
	 * via munmap as they drain; there is no separate "top chunk"
	 * to release. Returning 0 honestly reports that no memory
	 * was given back as a direct result of this call. */
	return 0;
}

/* --- POSIX malloc family overrides -------------------------------- */

V8M_EXPORT void *malloc(size_t size)
{
	return v8m_malloc(size);
}

V8M_EXPORT void free(void *ptr)
{
	v8m_free(ptr);
}

V8M_EXPORT void *calloc(size_t nmemb, size_t size)
{
	return v8m_calloc(nmemb, size);
}

V8M_EXPORT void *realloc(void *ptr, size_t size)
{
	return v8m_realloc(ptr, size);
}

V8M_EXPORT void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
	return v8m_reallocarray(ptr, nmemb, size);
}

V8M_EXPORT size_t malloc_usable_size(void *ptr)
{
	return v8m_malloc_usable_size(ptr);
}

V8M_EXPORT void *aligned_alloc(size_t alignment, size_t size)
{
	return v8m_aligned_alloc(alignment, size);
}

V8M_EXPORT int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	return v8m_posix_memalign(memptr, alignment, size);
}

V8M_EXPORT void *memalign(size_t alignment, size_t size)
{
	return v8m_memalign(alignment, size);
}

V8M_EXPORT void *valloc(size_t size)
{
	return v8m_valloc(size);
}

V8M_EXPORT void *pvalloc(size_t size)
{
	return v8m_pvalloc(size);
}

/* --- glibc internal aliases --------------------------------------- */
/*
 * glibc's internal call sites — and any code linked against
 * libc_nonshared.a that bypasses the public symbols — reach the
 * allocator through the __libc_* prefixed names. Defining them as
 * aliases of our public implementations interposes on those call
 * sites too. See architecture.md §3.3. The names are reserved by
 * the implementation per C, but that is precisely why glibc uses
 * them; the linter's reserved-identifier / readability rules are
 * irrelevant here, hence the blanket NOLINTs. */
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 * readability-identifier-naming,bugprone-easily-swappable-parameters) */
V8M_EXPORT void *__libc_malloc(size_t size)
    __attribute__((alias("v8m_malloc")));
V8M_EXPORT void __libc_free(void *ptr) __attribute__((alias("v8m_free")));
V8M_EXPORT void *__libc_calloc(size_t nmemb, size_t size)
    __attribute__((alias("v8m_calloc")));
V8M_EXPORT void *__libc_realloc(void *ptr, size_t size)
    __attribute__((alias("v8m_realloc")));
V8M_EXPORT void *__libc_memalign(size_t alignment, size_t size)
    __attribute__((alias("v8m_memalign")));
V8M_EXPORT void *__libc_valloc(size_t size)
    __attribute__((alias("v8m_valloc")));
V8M_EXPORT void *__libc_pvalloc(size_t size)
    __attribute__((alias("v8m_pvalloc")));
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 * readability-identifier-naming,bugprone-easily-swappable-parameters) */

/* --- C++ operator new / delete (non-throwing variants) ------------- */
/*
 * Itanium-C++-ABI mangled names for the operator-new/delete
 * variants that don't require throwing std::bad_alloc on failure.
 * The throwing forms (`_Znwm` operator new(size_t) and friends)
 * stay in libstdc++ because we can't construct the
 * std::bad_alloc instance from C; libstdc++'s shipped
 * implementations call malloc internally, so they pick up
 * v8malloc transparently through the LD_PRELOAD chain or static
 * link.
 *
 * Naming notes:
 *   _Zdl  → operator delete
 *   _Zda  → operator delete[]
 *   _Znw  → operator new
 *   _Zna  → operator new[]
 *   Pv    → (void *)
 *   m     → size_t
 *   St11align_val_t → std::align_val_t (size_t-typed enum class)
 *   RKSt9nothrow_t  → const std::nothrow_t&
 *
 * `std::nothrow_t` is an empty struct passed by const-reference;
 * in the C ABI it shows up as a `const void *` parameter we
 * receive but never deref. `std::align_val_t` is a `size_t`-typed
 * scoped enum, indistinguishable from `size_t` at the C ABI
 * level.
 *
 * All names tripped the same lint cluster as the __libc_* aliases
 * (reserved identifier, case style, swappable parameters); the
 * NOLINT* block below silences them once for the entire group.
 */
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 *             readability-identifier-naming,bugprone-easily-swappable-parameters)
 */

/* Forward declarations satisfy -Wmissing-prototypes for the
 * mangled C++ symbols below. They have no callers in C — the
 * mangled names exist only so a C++ program's call sites resolve
 * directly to v8malloc — but the warning fires on any visible
 * non-static definition without a prior prototype. */
V8M_EXPORT void _ZdlPv(void *ptr);
V8M_EXPORT void _ZdaPv(void *ptr);
V8M_EXPORT void _ZdlPvm(void *ptr, size_t size);
V8M_EXPORT void _ZdaPvm(void *ptr, size_t size);
V8M_EXPORT void _ZdlPvSt11align_val_t(void *ptr, size_t alignment);
V8M_EXPORT void _ZdaPvSt11align_val_t(void *ptr, size_t alignment);
V8M_EXPORT void _ZdlPvmSt11align_val_t(void *ptr, size_t size,
				       size_t alignment);
V8M_EXPORT void _ZdaPvmSt11align_val_t(void *ptr, size_t size,
				       size_t alignment);
V8M_EXPORT void *_ZnwmRKSt9nothrow_t(size_t size, const void *nothrow);
V8M_EXPORT void *_ZnamRKSt9nothrow_t(size_t size, const void *nothrow);
V8M_EXPORT void _ZdlPvRKSt9nothrow_t(void *ptr, const void *nothrow);
V8M_EXPORT void _ZdaPvRKSt9nothrow_t(void *ptr, const void *nothrow);
V8M_EXPORT void *_ZnwmSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow);
V8M_EXPORT void *_ZnamSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow);
V8M_EXPORT void _ZdlPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow);
V8M_EXPORT void _ZdaPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow);

/* operator delete(void*) and operator delete[](void*). The pointer
 * signatures match v8m_free exactly, so an alias suffices. */
V8M_EXPORT void _ZdlPv(void *ptr) __attribute__((alias("v8m_free")));
V8M_EXPORT void _ZdaPv(void *ptr) __attribute__((alias("v8m_free")));

/* Sized delete (C++14): the size hint comes from the caller and
 * we ignore it — v8m_free already recovers the size from meta. */
V8M_EXPORT void _ZdlPvm(void *ptr, size_t size)
{
	(void)size;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvm(void *ptr, size_t size)
{
	(void)size;
	v8m_free(ptr);
}

/* Aligned delete (C++17): the alignment hint is similarly
 * recoverable — we routed through v8m_aligned_alloc which placed
 * the pointer at the right offset, so v8m_free is enough. */
V8M_EXPORT void _ZdlPvSt11align_val_t(void *ptr, size_t alignment)
{
	(void)alignment;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvSt11align_val_t(void *ptr, size_t alignment)
{
	(void)alignment;
	v8m_free(ptr);
}

/* Sized + aligned delete (C++17). */
V8M_EXPORT void _ZdlPvmSt11align_val_t(void *ptr, size_t size, size_t alignment)
{
	(void)size;
	(void)alignment;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvmSt11align_val_t(void *ptr, size_t size, size_t alignment)
{
	(void)size;
	(void)alignment;
	v8m_free(ptr);
}

/* Nothrow new / delete: nothrow_t is an empty class, so the const
 * reference shows up as an unused `const void *`. v8m_malloc /
 * v8m_free already match the no-throw, return-NULL-on-failure
 * contract. */
V8M_EXPORT void *_ZnwmRKSt9nothrow_t(size_t size, const void *nothrow)
{
	(void)nothrow;
	return v8m_malloc(size);
}
V8M_EXPORT void *_ZnamRKSt9nothrow_t(size_t size, const void *nothrow)
{
	(void)nothrow;
	return v8m_malloc(size);
}
V8M_EXPORT void _ZdlPvRKSt9nothrow_t(void *ptr, const void *nothrow)
{
	(void)nothrow;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvRKSt9nothrow_t(void *ptr, const void *nothrow)
{
	(void)nothrow;
	v8m_free(ptr);
}

/* Nothrow aligned new (C++17). The alignment is the second
 * positional argument and the standard guarantees alignment is a
 * power of two ≥ alignof(std::max_align_t). */
V8M_EXPORT void *_ZnwmSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow)
{
	(void)nothrow;
	return v8m_aligned_alloc(alignment, size);
}
V8M_EXPORT void *_ZnamSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow)
{
	(void)nothrow;
	return v8m_aligned_alloc(alignment, size);
}

/* Aligned + nothrow delete (C++17). */
V8M_EXPORT void _ZdlPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow)
{
	(void)alignment;
	(void)nothrow;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow)
{
	(void)alignment;
	(void)nothrow;
	v8m_free(ptr);
}

/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 *           readability-identifier-naming,bugprone-easily-swappable-parameters)
 */
