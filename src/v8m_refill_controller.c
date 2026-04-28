/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Time-based EMA refill controller — implementation. See the
 * matching header for the design notes and v0 status.
 */

#include "v8m_refill_controller.h"

#include <stdint.h>
#include <string.h>

#include "v8m_arch.h" /* v8m_arch_rdtsc, v8m_arch_tsc_frequency_mhz */
#include "v8m_size_class.h" /* V8M_NUM_SIZE_CLASSES — surfaces through the header */

void v8m_refill_controller_init(struct v8m_refill_controller *ctrl)
{
	if (ctrl == NULL) {
		return;
	}
	(void)memset(ctrl, 0, sizeof(*ctrl));
	for (uint32_t cls = 0; cls < V8M_NUM_SIZE_CLASSES; cls++) {
		ctrl->per_class[cls].batch_size = V8M_REFILL_BATCH_MIN;
	}
}

void v8m_refill_controller_set_last_refill_tsc(
    struct v8m_refill_controller *ctrl, uint32_t cls, uint64_t tsc)
{
	if (ctrl == NULL || cls >= V8M_NUM_SIZE_CLASSES) {
		return;
	}
	ctrl->per_class[cls].last_refill_tsc = tsc;
}

/*
 * Ticks-per-microsecond, derived once from the arch TSC frequency
 * helper. The atomic + lazy-init pattern matches the lifetime
 * tracker's threshold derivation; the racy double-init is harmless
 * because every initializer computes the same value. On x86_64 the
 * value is the CPU's TSC frequency in MHz; on every other arch the
 * helper returns 1000 by contract (v8m_arch_rdtsc returns
 * nanoseconds, so "ticks per µs" is exactly 1000).
 */
static uint64_t ticks_per_micro(void)
{
	static uint64_t cached;
	if (__builtin_expect(cached != 0U, 1)) {
		return cached;
	}
	uint32_t mhz = v8m_arch_tsc_frequency_mhz();
	cached = (mhz != 0U) ? (uint64_t)mhz : 1000U;
	return cached;
}

/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
uint32_t v8m_refill_controller_compute_batch(struct v8m_refill_controller *ctrl,
					     uint32_t cls,
					     uint32_t actual_demand)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
{
	if (ctrl == NULL || cls >= V8M_NUM_SIZE_CLASSES) {
		return V8M_REFILL_BATCH_MIN;
	}
	struct v8m_refill_controller_class_state *state = &ctrl->per_class[cls];

	uint64_t now = v8m_arch_rdtsc();
	uint64_t last = state->last_refill_tsc;
	uint64_t elapsed = (last == 0U || now <= last) ? 0U : (now - last);

	/* EMA update: ema_new = (3 × ema_old + actual_demand) / 4. The
	 * spec note (line 191) is explicit that the EMA must track
	 * ACTUAL demand, not the previous batch size — self-referencing
	 * the batch size produces a stable loop that no longer responds
	 * to demand changes. */
	state->ema_demand =
	    (uint32_t)((3U * (uint64_t)state->ema_demand + actual_demand) >>
		       2U);

	uint32_t batch;
	if (elapsed > 0U) {
		uint64_t target_ticks =
		    (uint64_t)V8M_REFILL_TARGET_HOLD_MICROS * ticks_per_micro();
		uint64_t numerator = (uint64_t)state->ema_demand * target_ticks;
		uint64_t computed = numerator / elapsed;
		if (computed > V8M_REFILL_BATCH_MAX) {
			computed = V8M_REFILL_BATCH_MAX;
		}
		batch = (uint32_t)computed;
	} else {
		/* First call (or wrap): no elapsed delta available, so the
		 * rate predictor cannot fire. Keep the existing batch
		 * size — initialized to V8M_REFILL_BATCH_MIN by
		 * v8m_refill_controller_init — so the first refill ships
		 * at least the lower clamp. */
		batch = state->batch_size;
	}

	if (batch < V8M_REFILL_BATCH_MIN) {
		batch = V8M_REFILL_BATCH_MIN;
	}
	if (batch > V8M_REFILL_BATCH_MAX) {
		batch = V8M_REFILL_BATCH_MAX;
	}
	state->batch_size = batch;
	state->last_refill_tsc = now;
	return batch;
}
