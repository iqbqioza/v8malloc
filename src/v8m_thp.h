/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Adaptive Transparent Huge Page advice (huge-pages.md §5). The page
 * heap delegates the per-allocation promote/demote decision to this
 * module: it tracks the EMA of inter-allocation TSC ticks for THP-
 * eligible mappings, and when the EMA exceeds a cold threshold
 * (≈ 1 second of TSC ticks by default) the decision flips from
 * MADV_HUGEPAGE to MADV_NOHUGEPAGE so the kernel does not waste
 * effort promoting a region the program is unlikely to touch.
 *
 * Splitting the decision out of the page heap keeps the page-heap
 * file focused on mmap/munmap/madvise + region bookkeeping, and
 * lets the THP heuristic be tuned, instrumented, or fuzzed in
 * isolation. The per-region age tracker (the `promoted_at_tsc`
 * field on each `region_entry`) stays in `v8m_page_heap.c` because
 * it is part of the region map; the sweep that consumes it
 * (`v8m_page_heap_thp_age_sweep`) reads the threshold via this
 * module.
 */

#ifndef V8M_THP_H
#define V8M_THP_H

#include <stdint.h>

enum v8m_thp_advice {
	V8M_THP_PROMOTE = 0,
	V8M_THP_DEMOTE = 1,
};

/*
 * Cold threshold in TSC ticks. Lazy-initialised on first call from
 * `v8m_arch_tsc_frequency_mhz()` to ≈ 1 second of ticks; the test
 * injection knob can override the value. Subsequent calls return the
 * cached threshold.
 */
uint64_t v8m_thp_cold_threshold_ticks(void);

/*
 * Decide PROMOTE vs DEMOTE for the next THP-eligible allocation and
 * fold the inter-arrival delta into the EMA. The EMA only weighs
 * deltas after the first call (the first call returns PROMOTE
 * unconditionally because there is no delta to fold yet).
 */
enum v8m_thp_advice v8m_thp_decide_and_record(void);

/*
 * Counter accessors. All increment when the page heap actually
 * emits the corresponding madvise(); a read-back via the page
 * heap's stats snapshot uses these. `age_demote_calls` is bumped
 * by the per-region age sweep, distinct from the alloc-time
 * `demote_calls` which counts the EMA-driven decision.
 */
void v8m_thp_record_promote(void);
void v8m_thp_record_demote(void);
void v8m_thp_record_age_demote(void);

uint64_t v8m_thp_promote_calls(void);
uint64_t v8m_thp_demote_calls(void);
uint64_t v8m_thp_age_demote_calls(void);
uint64_t v8m_thp_ema_ticks(void);

/*
 * Test-only knob: override the cold threshold and the live EMA so a
 * test can deterministically exercise the promote / demote branches
 * without depending on wall-clock timing. Resets the last-alloc TSC
 * so the next decision recomputes from a clean baseline. Pass any
 * non-zero `cold_threshold_ticks` to lock the threshold (the lazy
 * initializer's early-return treats non-zero as "already computed");
 * zero re-enables the lazy derivation.
 */
void v8m_thp_test_inject(uint64_t cold_threshold_ticks, uint64_t ema_ticks);

#endif /* V8M_THP_H */
