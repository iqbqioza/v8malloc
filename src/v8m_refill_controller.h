/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Time-based EMA refill controller (winning-algorithms.md §4.2).
 * Computes the refill batch size for a tier-to-tier transfer based
 * on the EMA of observed demand and the elapsed wall time since the
 * previous refill. The goal is a batch sized so the next refill
 * lands ≈ V8M_REFILL_TARGET_HOLD_MICROS into the future, regardless
 * of whether the caller's allocation rate is bursty or steady.
 *
 * **Scope**: per the spec the controller targets the L2↔L3 / L3↔L4
 * boundary, where the cost of a single refill is dominated by lock
 * acquisition and TLB pressure and benefits from rate prediction.
 * The much hotter L1↔L2 boundary uses the simpler capacity-based
 * batch sizing (`capacity >> 1`, clamped) — it runs on every TLC
 * miss and cannot afford the rdtsc/EMA work below
 * (thread-cache.md §3.2).
 *
 * **v0 status**: L3 and L4 do not yet exist as discrete tiers in
 * the dispatcher — the v0 hierarchy is TLC → L2 (core_cache) →
 * slab/buddy pools, so this controller has no production consumer
 * yet. The module ships standalone so the algorithm + test coverage
 * land per spec; a future cycle that introduces L3 (a per-NUMA
 * pool) will consume it on the L2↔L3 refill path. The standalone
 * shape is mechanical glue: instance the controller, call
 * `v8m_refill_controller_compute_batch` at refill time with the
 * actual demand seen since the previous refill, use the returned
 * count for the batch.
 */

#ifndef V8M_REFILL_CONTROLLER_H
#define V8M_REFILL_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#include "v8m_size_class.h" /* V8M_NUM_SIZE_CLASSES */

/*
 * Batch-size clamps. The lower bound prevents the rate predictor
 * from collapsing to a single-object refill on a class with no
 * observed demand (the next refill would itself fault on every
 * subsequent allocation); the upper bound caps the worst-case
 * refill latency a single L3/L4 round trip can incur.
 */
#define V8M_REFILL_BATCH_MIN 4U
#define V8M_REFILL_BATCH_MAX 256U

/*
 * Target hold duration: the controller sizes each batch so it lasts
 * roughly this long at the observed demand rate before the next
 * refill is needed. 100 µs is the spec value (winning-algorithms.md
 * §4.2 line 196) — long enough to amortize an L3/L4 lock + TLB
 * round trip across many allocations, short enough that a sudden
 * demand drop reclaims unused slots within a few hundred
 * microseconds.
 */
#define V8M_REFILL_TARGET_HOLD_MICROS 100U

/*
 * Per-class controller state. `ema_demand` smooths the actual
 * demand observed at each refill (α = 0.25, matching the bin-capacity
 * controller); `batch_size` is the most recent decision; the rate
 * predictor at the next refill divides `ema_demand` by the elapsed
 * ticks to estimate the rate. `last_refill_tsc == 0` is the
 * "first-call" sentinel that bypasses the divisor and returns the
 * existing `batch_size` (initially V8M_REFILL_BATCH_MIN) so the
 * very first refill ships a sane minimum without dividing by zero.
 */
struct v8m_refill_controller_class_state {
	uint32_t ema_demand;
	uint32_t batch_size;
	uint64_t last_refill_tsc;
};

struct v8m_refill_controller {
	struct v8m_refill_controller_class_state
	    per_class[V8M_NUM_SIZE_CLASSES];
};

/*
 * Initialize a controller to its baseline: zero EMA demand, zero
 * last-refill TSC (so the first compute_batch call returns the
 * baseline batch size), and `batch_size = V8M_REFILL_BATCH_MIN` so
 * the first refill ships at least the lower clamp. Tolerates NULL.
 */
void v8m_refill_controller_init(struct v8m_refill_controller *ctrl);

/*
 * Compute the next refill batch size for `cls`, given the actual
 * demand observed since the previous compute_batch call (the count
 * of allocations the consumer served from the previously-refilled
 * batch). Updates the per-class EMA + last-refill TSC and returns
 * the new batch size in [V8M_REFILL_BATCH_MIN, V8M_REFILL_BATCH_MAX].
 *
 * Algorithm (winning-algorithms.md §4.2):
 *   ema_demand   = (3 × ema_demand_old + actual_demand) / 4
 *   demand_rate  = ema_demand / elapsed_ticks
 *   target_ticks = V8M_REFILL_TARGET_HOLD_MICROS × ticks-per-µs
 *   batch        = clamp(demand_rate × target_ticks, MIN, MAX)
 *
 * Tolerates NULL `ctrl` and out-of-range `cls` by returning
 * V8M_REFILL_BATCH_MIN — the caller is then free to use that as
 * the fallback batch.
 */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
uint32_t v8m_refill_controller_compute_batch(struct v8m_refill_controller *ctrl,
					     uint32_t cls,
					     uint32_t actual_demand);

/*
 * Test-only knob: inject an explicit `last_refill_tsc` for `cls` so
 * tests can drive the elapsed-time divisor deterministically without
 * depending on wall-clock timing. Production callers should never
 * touch this. Tolerates NULL `ctrl` and out-of-range `cls`.
 */
void v8m_refill_controller_set_last_refill_tsc(
    struct v8m_refill_controller *ctrl, uint32_t cls, uint64_t tsc);

#endif /* V8M_REFILL_CONTROLLER_H */
