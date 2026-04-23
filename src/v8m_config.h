/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Runtime configuration. Every tunable lives in a single small array
 * of atomic 64-bit integers; v8m_config_init() seeds it from the
 * V8M_* environment variables documented in AGENT.md §7 and
 * api.md §4.4 (defaults applied when an env var is absent or
 * unparseable).
 *
 * Reads on the hot path are relaxed-atomic loads — no locks. Writes
 * happen at process startup and via v8m_config_set() (the public
 * v8m_set_option / v8m_get_option API will wrap these in a later
 * cycle).
 *
 * Pre-init reads see zero for every option, which differs from the
 * default for the duration policies (PURGE_INTERVAL, etc.).
 * Consumers must arrange for v8m_config_init() to run before they
 * read; the library constructor handles that for the normal case.
 */

#ifndef V8M_CONFIG_H
#define V8M_CONFIG_H

#include <stdatomic.h>
#include <stdint.h>

/*
 * The option identifiers (V8M_OPT_*) are the canonical public
 * enum, defined in <v8malloc/v8malloc.h>. The internal config
 * layer uses the same set so that v8m_set_option / v8m_get_option
 * are pure forwarders without an id translation step.
 */
#include "v8malloc/v8malloc.h"

/*
 * Read every V8M_* environment variable and apply the value (or its
 * default) to the corresponding option. Idempotent — safe to call
 * multiple times; each call re-reads the environment, so tests can
 * reset state by `setenv` followed by another v8m_config_init.
 */
void v8m_config_init(void);

/*
 * Set / get a tunable. Returns 0 on success, -1 if `opt` is
 * out-of-range. v8m_config_get on an out-of-range option returns 0.
 *
 * `v8m_config_get` is `static inline` so the malloc fast path's
 * config checks (LIFETIME_TRACKING, DEBUG, etc.) collapse to a
 * single relaxed-atomic load — no function call, no PLT trampoline,
 * no v8m_config_get stack frame on the hot path. The backing array
 * is a non-static extern so the inline can reach it from any TU.
 */
extern _Atomic int64_t v8m_config_values[V8M_OPT_COUNT];

int v8m_config_set(enum v8m_option opt, int64_t value);

static inline int64_t v8m_config_get(enum v8m_option opt)
{
	if ((unsigned)opt >= (unsigned)V8M_OPT_COUNT) {
		return 0;
	}
	return atomic_load_explicit(&v8m_config_values[opt],
				    memory_order_relaxed);
}

#endif /* V8M_CONFIG_H */
