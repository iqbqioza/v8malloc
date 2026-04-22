/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Time-based EMA refill controller (winning-algorithms.md §4.2).
 * Standalone tests because the controller has no production
 * consumer in v0 — L3/L4 don't yet exist. The tests use the
 * test-only `v8m_refill_controller_set_last_refill_tsc` knob to
 * drive the elapsed-time divisor without depending on wall-clock
 * timing.
 */

#include <stdint.h>
#include <stdio.h>

#include "v8m_arch.h" /* v8m_arch_rdtsc */
#include "v8m_refill_controller.h"
#include "v8m_size_class.h" /* V8M_NUM_SIZE_CLASSES */

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_refill_controller: %s\n", msg);
	return 1;
}

/* Init must zero the EMA + last-refill TSC and seed batch_size to
 * V8M_REFILL_BATCH_MIN so the first refill ships a sane minimum. */
static int check_init_state(void)
{
	struct v8m_refill_controller ctrl;
	v8m_refill_controller_init(&ctrl);
	for (uint32_t cls = 0; cls < V8M_NUM_SIZE_CLASSES; cls++) {
		if (ctrl.per_class[cls].ema_demand != 0U) {
			return fail("init left non-zero ema_demand");
		}
		if (ctrl.per_class[cls].last_refill_tsc != 0U) {
			return fail("init left non-zero last_refill_tsc");
		}
		if (ctrl.per_class[cls].batch_size != V8M_REFILL_BATCH_MIN) {
			return fail("init did not seed batch_size to MIN");
		}
	}
	/* Tolerates NULL — must not crash. */
	v8m_refill_controller_init(NULL);
	return 0;
}

/* First call (last_refill_tsc == 0) returns the seeded batch
 * (V8M_REFILL_BATCH_MIN) regardless of the actual_demand argument
 * because the rate predictor cannot fire without an elapsed delta. */
static int check_first_call_returns_min(void)
{
	struct v8m_refill_controller ctrl;
	v8m_refill_controller_init(&ctrl);
	uint32_t got = v8m_refill_controller_compute_batch(&ctrl, 5, 1000);
	if (got != V8M_REFILL_BATCH_MIN) {
		return fail("first-call batch was not MIN");
	}
	/* The EMA still updates on the first call. */
	if (ctrl.per_class[5].ema_demand == 0U) {
		return fail("first call did not update ema_demand");
	}
	return 0;
}

/*
 * High demand over a small elapsed window must drive the batch up
 * toward MAX. Inject `last_refill_tsc = now - 1` so elapsed is the
 * smallest positive value the controller can see, then feed
 * V8M_REFILL_BATCH_MAX × N as actual demand so the rate predictor's
 * (ema × target_ticks / 1) lands well past MAX and clamps.
 */
static int check_high_demand_clamps_to_max(void)
{
	struct v8m_refill_controller ctrl;
	v8m_refill_controller_init(&ctrl);
	const uint32_t cls = 7;

	/* Two-phase warm-up so the EMA accumulates before the clamp
	 * test: a single sample with a non-zero elapsed makes the
	 * compute path fire, then the next call with last_refill_tsc
	 * forced near `now` triggers the rate clamp. */
	v8m_refill_controller_set_last_refill_tsc(&ctrl, cls, v8m_arch_rdtsc());
	(void)v8m_refill_controller_compute_batch(&ctrl, cls, 100000U);

	v8m_refill_controller_set_last_refill_tsc(&ctrl, cls,
						  v8m_arch_rdtsc() - 1U);
	uint32_t got = v8m_refill_controller_compute_batch(&ctrl, cls, 100000U);
	if (got != V8M_REFILL_BATCH_MAX) {
		(void)fprintf(stderr,
			      "test_refill_controller: high-demand batch was "
			      "%u, expected MAX (%u)\n",
			      got, V8M_REFILL_BATCH_MAX);
		return 1;
	}
	return 0;
}

/*
 * Sustained zero demand must drive the batch back to MIN. The EMA
 * decays geometrically (α = 0.25), so after enough zero-demand
 * samples the rate predictor bottoms out at zero and the lower
 * clamp kicks in.
 */
static int check_zero_demand_clamps_to_min(void)
{
	struct v8m_refill_controller ctrl;
	v8m_refill_controller_init(&ctrl);
	const uint32_t cls = 11;

	/* Seed the EMA with a high baseline so we can observe decay. */
	v8m_refill_controller_set_last_refill_tsc(&ctrl, cls, v8m_arch_rdtsc());
	(void)v8m_refill_controller_compute_batch(&ctrl, cls, 10000U);

	/* Now feed zero demand many times. Each iteration uses a
	 * realistic elapsed (let the actual rdtsc tick) so the divisor
	 * stays large enough to keep the rate-predictor's product near
	 * zero. */
	uint32_t got = V8M_REFILL_BATCH_MAX;
	for (int i = 0; i < 64; i++) {
		got = v8m_refill_controller_compute_batch(&ctrl, cls, 0U);
	}
	if (got != V8M_REFILL_BATCH_MIN) {
		(void)fprintf(stderr,
			      "test_refill_controller: zero-demand batch "
			      "settled at %u, expected MIN (%u)\n",
			      got, V8M_REFILL_BATCH_MIN);
		return 1;
	}
	if (ctrl.per_class[cls].ema_demand >= 10000U) {
		return fail("zero-demand EMA did not decay");
	}
	return 0;
}

/*
 * EMA smoothing: a single demand spike must NOT immediately push
 * the EMA all the way to the spike value. With α = 0.25, one
 * sample of value V against an EMA of 0 yields V/4 — the spike's
 * influence is bounded.
 */
static int check_ema_smoothing(void)
{
	struct v8m_refill_controller ctrl;
	v8m_refill_controller_init(&ctrl);
	const uint32_t cls = 3;

	/* Drive once with elapsed > 0 so the EMA actually updates. */
	v8m_refill_controller_set_last_refill_tsc(&ctrl, cls, v8m_arch_rdtsc());
	(void)v8m_refill_controller_compute_batch(&ctrl, cls, 1000U);
	uint32_t after_spike = ctrl.per_class[cls].ema_demand;
	if (after_spike >= 1000U) {
		return fail("single spike pushed EMA to full sample value");
	}
	if (after_spike == 0U) {
		return fail("EMA did not absorb the spike at all");
	}
	/* The exact value with α = 0.25 starting from ema=0 is
	 * (3*0 + 1000) / 4 = 250. Allow a 1-tick rounding tolerance. */
	if (after_spike < 249U || after_spike > 251U) {
		(void)fprintf(stderr,
			      "test_refill_controller: EMA after spike was "
			      "%u, expected ≈ 250\n",
			      after_spike);
		return 1;
	}
	return 0;
}

/* Out-of-range cls and NULL ctrl return MIN without crashing. */
static int check_invalid_args_return_min(void)
{
	if (v8m_refill_controller_compute_batch(NULL, 0, 100) !=
	    V8M_REFILL_BATCH_MIN) {
		return fail("NULL ctrl did not return MIN");
	}
	struct v8m_refill_controller ctrl;
	v8m_refill_controller_init(&ctrl);
	if (v8m_refill_controller_compute_batch(&ctrl,
						V8M_NUM_SIZE_CLASSES + 5U,
						100) != V8M_REFILL_BATCH_MIN) {
		return fail("out-of-range cls did not return MIN");
	}
	/* The setter must also tolerate the same. */
	v8m_refill_controller_set_last_refill_tsc(NULL, 0, 100);
	v8m_refill_controller_set_last_refill_tsc(&ctrl, V8M_NUM_SIZE_CLASSES,
						  100);
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_init_state();
	result |= check_first_call_returns_min();
	result |= check_high_demand_clamps_to_max();
	result |= check_zero_demand_clamps_to_min();
	result |= check_ema_smoothing();
	result |= check_invalid_args_return_min();
	if (result == 0) {
		(void)printf("test_refill_controller: OK\n");
	}
	return result;
}
