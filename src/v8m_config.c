/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Runtime configuration backing store. One atomic int64 per option;
 * defaults and env-var names live in two parallel const arrays
 * indexed by `enum v8m_option`. v8m_config_init() walks the table,
 * calling getenv/strtoll for each entry.
 */

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_config.h"
#include "v8malloc/v8malloc.h"

static const int64_t v8m_config_defaults[V8M_OPT_COUNT] = {
    [V8M_OPT_VERBOSE] = 0,
    [V8M_OPT_PURGE_INTERVAL] = 10,
    [V8M_OPT_THREAD_CACHE_MAX] = 256,
    [V8M_OPT_HUGE_PAGES] = 1,
    [V8M_OPT_NUMA_AWARE] = 1,
    [V8M_OPT_DEBUG] = 0,
    [V8M_OPT_PROFILE] = 0,
    [V8M_OPT_COMPACT_THRESHOLD] = 25,
    [V8M_OPT_VMA_WARN_THRESHOLD] = 1024,
};

static const char *const v8m_config_env_names[V8M_OPT_COUNT] = {
    [V8M_OPT_VERBOSE] = "V8M_VERBOSE",
    [V8M_OPT_PURGE_INTERVAL] = "V8M_PURGE_INTERVAL",
    [V8M_OPT_THREAD_CACHE_MAX] = "V8M_THREAD_CACHE_MAX",
    [V8M_OPT_HUGE_PAGES] = "V8M_HUGE_PAGES",
    [V8M_OPT_NUMA_AWARE] = "V8M_NUMA_AWARE",
    [V8M_OPT_DEBUG] = "V8M_DEBUG",
    [V8M_OPT_PROFILE] = "V8M_PROFILE",
    [V8M_OPT_COMPACT_THRESHOLD] = "V8M_COMPACT_THRESHOLD",
    [V8M_OPT_VMA_WARN_THRESHOLD] = "V8M_VMA_WARN_THRESHOLD",
};

static _Atomic int64_t v8m_config_values[V8M_OPT_COUNT];

/*
 * Parse `text` as a base-10 signed integer. Returns true and writes
 * the value on success; false if the entire string did not parse, was
 * empty, or overflowed.
 */
static bool parse_int64(const char *text, int64_t *out)
{
	if (text == NULL || *text == '\0') {
		return false;
	}
	errno = 0;
	char *end = NULL;
	long long parsed = strtoll(text, &end, 10);
	if (end == text || *end != '\0') {
		return false;
	}
	if (errno == ERANGE) {
		return false;
	}
	if (parsed < INT64_MIN || parsed > INT64_MAX) {
		return false;
	}
	*out = (int64_t)parsed;
	return true;
}

void v8m_config_init(void)
{
	for (int i = 0; i < V8M_OPT_COUNT; i++) {
		int64_t value = v8m_config_defaults[i];
		/* getenv is documented as not thread-safe (a concurrent
		 * setenv can invalidate the returned pointer); we only
		 * call it during library init before any user thread
		 * has a chance to run. */
		/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
		const char *env = getenv(v8m_config_env_names[i]);
		int64_t parsed = 0;
		if (env != NULL && parse_int64(env, &parsed)) {
			value = parsed;
		}
		atomic_store_explicit(&v8m_config_values[i], value,
				      memory_order_relaxed);
	}
}

int v8m_config_set(enum v8m_option opt, int64_t value)
{
	if ((unsigned)opt >= (unsigned)V8M_OPT_COUNT) {
		return -1;
	}
	atomic_store_explicit(&v8m_config_values[opt], value,
			      memory_order_relaxed);
	return 0;
}

int64_t v8m_config_get(enum v8m_option opt)
{
	if ((unsigned)opt >= (unsigned)V8M_OPT_COUNT) {
		return 0;
	}
	return atomic_load_explicit(&v8m_config_values[opt],
				    memory_order_relaxed);
}
