/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Background purge thread (fragmentation.md §6.3). Wakes every
 * V8M_OPT_PURGE_INTERVAL seconds and runs the periodic-scan hook.
 *
 * v0 scope:
 *   - Thread lifecycle (spawn / shutdown / join) is fully wired
 *   - Sleep is condvar-based so shutdown is instant, not "wait
 *     out the current interval"
 *   - The scan body is a stub (logs page-heap stats when
 *     V8M_OPT_VERBOSE is on, else no-op) since today's slab and
 *     buddy pools already eagerly release empty pages on free
 *
 * Future work hooks into the same loop:
 *   - Per-NUMA pool empty-page sweep (`madvise(MADV_DONTNEED)`
 *     on idle pages) — fragmentation.md §6.3
 *   - TLC bin shrink toward `bin_capacity_low_water_mark` —
 *     thread-cache.md §5.2
 *   - VMA-count threshold warning — huge-pages.md §6.2
 */

#ifndef V8M_BG_PURGE_H
#define V8M_BG_PURGE_H

#include <stdbool.h>

/*
 * Spawn the background purge thread. Returns 0 on success, an
 * errno-style value from pthread_create on failure. Idempotent:
 * a second call without an intervening shutdown is a no-op.
 *
 * Failure to spawn is non-fatal — the allocator stays usable, the
 * caller just loses periodic background work. The constructor
 * logs to stderr but does not abort.
 */
int v8m_bg_purge_init(void);

/*
 * Signal the thread to exit and join. Safe to call when the thread
 * was never spawned (returns 0 immediately). Wakeup is via a
 * condvar signal so the join completes within a futex hop, not
 * after the current sleep interval expires.
 */
void v8m_bg_purge_shutdown(void);

/*
 * True iff the background thread is running.
 */
bool v8m_bg_purge_running(void);

/*
 * Run one scan pass synchronously on the calling thread. Same body
 * the bg thread runs each tick — VMA-threshold check, optional
 * verbose stats log. Useful from `v8m_purge()` so an explicit
 * caller can trigger the diagnostics without waiting up to one
 * `V8M_OPT_PURGE_INTERVAL` second for the next bg tick. Safe to
 * call before / after `v8m_bg_purge_init` / `_shutdown` — the scan
 * touches no thread state.
 */
void v8m_bg_purge_run_once(void);

/*
 * Per-tick callback invoked by the bg thread (and by
 * v8m_bg_purge_run_once on synchronous calls). The pointer is set
 * by the public-API layer at constructor time so v8m_bg_purge does
 * not have to know about the dispatcher singleton; clearing it
 * back to NULL during shutdown is a no-op-safe idiom. The hook is
 * called outside any pool lock so the implementation is free to
 * take whichever locks it needs.
 */
typedef void (*v8m_bg_purge_tick_hook)(void);
void v8m_bg_purge_set_tick_hook(v8m_bg_purge_tick_hook hook);

/*
 * pthread_atfork plumbing for the bg-purge mutex. The bg-purge
 * thread holds `g_lock` briefly each tick (around the condvar
 * wait setup); a fork in that window leaves the child with the
 * lock inherited in held state. The bg-purge thread does not
 * exist in the child, but a child that calls `v8m_bg_purge_*` —
 * including the destructor's shutdown — would deadlock on the
 * lock acquire. Called by the api's atfork chain.
 */
void v8m_bg_purge_prefork(void);
void v8m_bg_purge_postfork_parent(void);
void v8m_bg_purge_postfork_child(void);

#endif
