/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Adaptive Transparent Huge Page advice — implementation. See the
 * matching header for the design notes.
 */

#include "v8m_thp.h"

#include <stdatomic.h>
#include <stdint.h>

#include "v8m_arch.h" /* v8m_arch_rdtsc, v8m_arch_tsc_frequency_mhz */

static _Atomic uint64_t g_thp_last_alloc_tsc;
static _Atomic uint64_t g_thp_ema_ticks;
static _Atomic uint64_t g_thp_cold_threshold_ticks;

static _Atomic uint64_t g_thp_promote_calls;
static _Atomic uint64_t g_thp_demote_calls;
static _Atomic uint64_t g_thp_age_demote_calls;

uint64_t v8m_thp_cold_threshold_ticks_snapshot(void)
{
	return atomic_load_explicit(&g_thp_cold_threshold_ticks,
				    memory_order_relaxed);
}

uint64_t v8m_thp_cold_threshold_ticks(void)
{
	uint64_t cached = atomic_load_explicit(&g_thp_cold_threshold_ticks,
					       memory_order_relaxed);
	if (cached != 0U) {
		return cached;
	}
	uint64_t mhz = (uint64_t)v8m_arch_tsc_frequency_mhz();
	if (mhz == 0U) {
		mhz = 1000U; /* non-x86_64 fallback returns 1000 by contract:
			      * v8m_arch_rdtsc returns nanoseconds, so the
			      * "mhz" basis becomes ticks-per-microsecond. */
	}
	uint64_t ticks = mhz * 1000U * 1000U; /* ≈ 1 second */
	atomic_store_explicit(&g_thp_cold_threshold_ticks, ticks,
			      memory_order_relaxed);
	return ticks;
}

enum v8m_thp_advice v8m_thp_decide_and_record(void)
{
	uint64_t now = v8m_arch_rdtsc();
	uint64_t last = atomic_exchange_explicit(&g_thp_last_alloc_tsc, now,
						 memory_order_relaxed);
	if (last == 0U || now <= last) {
		/* First THP-eligible alloc since process start (or a
		 * monotonic-clock wrap on the rdtsc fallback path) — no
		 * inter-arrival delta to fold into the EMA. Default to
		 * PROMOTE (current behaviour). */
		return V8M_THP_PROMOTE;
	}
	uint64_t delta = now - last;
	uint64_t prev_ema =
	    atomic_load_explicit(&g_thp_ema_ticks, memory_order_relaxed);
	uint64_t new_ema =
	    (prev_ema == 0U) ? delta : ((prev_ema * 3U + delta) / 4U);
	atomic_store_explicit(&g_thp_ema_ticks, new_ema, memory_order_relaxed);
	if (new_ema > v8m_thp_cold_threshold_ticks()) {
		return V8M_THP_DEMOTE;
	}
	return V8M_THP_PROMOTE;
}

void v8m_thp_record_promote(void)
{
	atomic_fetch_add_explicit(&g_thp_promote_calls, 1U,
				  memory_order_relaxed);
}

void v8m_thp_record_demote(void)
{
	atomic_fetch_add_explicit(&g_thp_demote_calls, 1U,
				  memory_order_relaxed);
}

void v8m_thp_record_age_demote(void)
{
	atomic_fetch_add_explicit(&g_thp_age_demote_calls, 1U,
				  memory_order_relaxed);
}

uint64_t v8m_thp_promote_calls(void)
{
	return atomic_load_explicit(&g_thp_promote_calls, memory_order_relaxed);
}

uint64_t v8m_thp_demote_calls(void)
{
	return atomic_load_explicit(&g_thp_demote_calls, memory_order_relaxed);
}

uint64_t v8m_thp_age_demote_calls(void)
{
	return atomic_load_explicit(&g_thp_age_demote_calls,
				    memory_order_relaxed);
}

uint64_t v8m_thp_ema_ticks(void)
{
	return atomic_load_explicit(&g_thp_ema_ticks, memory_order_relaxed);
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void v8m_thp_test_inject(uint64_t cold_threshold_ticks, uint64_t ema_ticks)
{
	atomic_store_explicit(&g_thp_cold_threshold_ticks, cold_threshold_ticks,
			      memory_order_relaxed);
	atomic_store_explicit(&g_thp_ema_ticks, ema_ticks,
			      memory_order_relaxed);
	/* Reset last-alloc TSC so the next decision recomputes from a
	 * clean baseline rather than mixing the test-injected EMA with
	 * a stale delta. */
	atomic_store_explicit(&g_thp_last_alloc_tsc, 0U, memory_order_relaxed);
}
