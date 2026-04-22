/* SPDX-License-Identifier: Apache-2.0 */

#include "v8m_bg_purge.h"

#include <errno.h>
#include <pthread.h> /* IWYU pragma: keep — pthread_t, pthread_mutex_t, pthread_cond_t */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h> /* IWYU pragma: keep — clock_gettime + CLOCK_REALTIME */

#include "v8m_config.h"
#include "v8m_page_heap.h"
#include "v8malloc/v8malloc.h"

/* NOLINTBEGIN(misc-include-cleaner) — pthread.h IS included above; the
 * IWYU-style cleaner does not always pick that up for typedef'd
 * names. */
static pthread_t g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
/* NOLINTEND(misc-include-cleaner) */
static atomic_bool g_running = false;
static atomic_bool g_should_stop = false;

/*
 * One pass of the periodic scan. v0 stub: when V8M_OPT_VERBOSE is
 * set, emit a single line summarising live page-heap state to
 * stderr so a long-running process leaves a periodic trace. Real
 * purge work (per-NUMA empty-page sweep, TLC bin shrink, etc.)
 * hooks in here as those subsystems land.
 */
static void run_scan_pass(void)
{
	if (v8m_config_get(V8M_OPT_VERBOSE) == 0) {
		return;
	}
	struct v8m_page_heap_stats stats = {0};
	v8m_page_heap_get_stats(&stats);
	uint64_t live_regions = (uint64_t)v8m_page_heap_live_region_count();
	uint64_t vma_count = v8m_count_vmas();
	(void)fprintf(stderr,
		      "v8m_bg_purge: live_regions=%llu vma=%llu "
		      "bytes_mapped=%llu bytes_unmapped=%llu\n",
		      (unsigned long long)live_regions,
		      (unsigned long long)vma_count,
		      (unsigned long long)stats.bytes_mapped,
		      (unsigned long long)stats.bytes_unmapped);
}

/*
 * Wait up to `interval_seconds` on the condvar OR until the stop
 * flag is set. Returns true if shutdown was signalled. Uses
 * CLOCK_REALTIME because pthread_cond_timedwait expects an absolute
 * deadline against that clock by default; the small clock-skew
 * window is acceptable for a coarse sleep.
 */
static bool wait_for_next_tick(int64_t interval_seconds)
{
	if (interval_seconds <= 0) {
		interval_seconds = 1;
	}
	struct timespec deadline;
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	(void)clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += (time_t)interval_seconds;

	(void)pthread_mutex_lock(&g_lock);
	while (!atomic_load_explicit(&g_should_stop, memory_order_acquire)) {
		int wait_rc =
		    pthread_cond_timedwait(&g_cond, &g_lock, &deadline);
		if (wait_rc == ETIMEDOUT) {
			break;
		}
		/* Spurious wake or signalled wake — loop and re-check
		 * the stop flag. */
	}
	bool stop = atomic_load_explicit(&g_should_stop, memory_order_acquire);
	(void)pthread_mutex_unlock(&g_lock);
	return stop;
}

static void *bg_purge_thread_main(void *arg)
{
	(void)arg;
	for (;;) {
		int64_t interval = v8m_config_get(V8M_OPT_PURGE_INTERVAL);
		if (wait_for_next_tick(interval)) {
			break;
		}
		run_scan_pass();
	}
	return NULL;
}

int v8m_bg_purge_init(void)
{
	if (atomic_load_explicit(&g_running, memory_order_acquire)) {
		return 0;
	}
	atomic_store_explicit(&g_should_stop, false, memory_order_release);
	int create_rc =
	    pthread_create(&g_thread, NULL, bg_purge_thread_main, NULL);
	if (create_rc != 0) {
		return create_rc;
	}
	atomic_store_explicit(&g_running, true, memory_order_release);
	return 0;
}

void v8m_bg_purge_shutdown(void)
{
	if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
		return;
	}
	(void)pthread_mutex_lock(&g_lock);
	atomic_store_explicit(&g_should_stop, true, memory_order_release);
	(void)pthread_cond_signal(&g_cond);
	(void)pthread_mutex_unlock(&g_lock);
	(void)pthread_join(g_thread, NULL);
	atomic_store_explicit(&g_running, false, memory_order_release);
}

bool v8m_bg_purge_running(void)
{
	return atomic_load_explicit(&g_running, memory_order_acquire);
}
