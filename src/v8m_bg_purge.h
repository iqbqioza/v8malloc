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

#endif
