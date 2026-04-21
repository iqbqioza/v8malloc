/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Configuration tests. Verify defaults when env vars are absent,
 * env-var overrides for every option, fall-back-to-default on a
 * non-numeric env value, set/get round-trip, and the
 * out-of-range guard. _GNU_SOURCE comes from the test target's
 * compile-definitions so setenv/unsetenv are visible.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_config.h"
#include "v8malloc/v8malloc.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_config: %s\n", msg);
	return 1;
}

/* Names match V8M_OPT_* in declaration order. Used to sweep every
 * option in the env-override test. */
static const char *const env_names[] = {
    "V8M_VERBOSE",    "V8M_PURGE_INTERVAL",    "V8M_THREAD_CACHE_MAX",
    "V8M_HUGE_PAGES", "V8M_NUMA_AWARE",	       "V8M_DEBUG",
    "V8M_PROFILE",    "V8M_COMPACT_THRESHOLD",
};

/* setenv / unsetenv are not thread-safe by POSIX; the test is
 * single-threaded so the warnings are noise. NOLINT the whole
 * env-manipulation surface in this file. */
/* NOLINTBEGIN(concurrency-mt-unsafe) */
static void clear_all_env(void)
{
	for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++) {
		(void)unsetenv(env_names[i]);
	}
}

static int check_defaults(void)
{
	clear_all_env();
	v8m_config_init();

	if (v8m_config_get(V8M_OPT_VERBOSE) != 0) {
		return fail("default VERBOSE != 0");
	}
	if (v8m_config_get(V8M_OPT_PURGE_INTERVAL) != 10) {
		return fail("default PURGE_INTERVAL != 10");
	}
	if (v8m_config_get(V8M_OPT_THREAD_CACHE_MAX) != 256) {
		return fail("default THREAD_CACHE_MAX != 256");
	}
	if (v8m_config_get(V8M_OPT_HUGE_PAGES) != 1) {
		return fail("default HUGE_PAGES != 1");
	}
	if (v8m_config_get(V8M_OPT_NUMA_AWARE) != 1) {
		return fail("default NUMA_AWARE != 1");
	}
	if (v8m_config_get(V8M_OPT_DEBUG) != 0) {
		return fail("default DEBUG != 0");
	}
	if (v8m_config_get(V8M_OPT_PROFILE) != 0) {
		return fail("default PROFILE != 0");
	}
	if (v8m_config_get(V8M_OPT_COMPACT_THRESHOLD) != 25) {
		return fail("default COMPACT_THRESHOLD != 25");
	}
	return 0;
}

static int check_env_overrides(void)
{
	clear_all_env();
	if (setenv("V8M_VERBOSE", "1", 1) != 0 ||
	    setenv("V8M_PURGE_INTERVAL", "42", 1) != 0 ||
	    setenv("V8M_THREAD_CACHE_MAX", "1024", 1) != 0 ||
	    setenv("V8M_HUGE_PAGES", "0", 1) != 0 ||
	    setenv("V8M_NUMA_AWARE", "0", 1) != 0 ||
	    setenv("V8M_DEBUG", "1", 1) != 0 ||
	    setenv("V8M_PROFILE", "1", 1) != 0 ||
	    setenv("V8M_COMPACT_THRESHOLD", "50", 1) != 0) {
		return fail("setenv failed");
	}
	v8m_config_init();

	if (v8m_config_get(V8M_OPT_VERBOSE) != 1) {
		return fail("env VERBOSE not picked up");
	}
	if (v8m_config_get(V8M_OPT_PURGE_INTERVAL) != 42) {
		return fail("env PURGE_INTERVAL not picked up");
	}
	if (v8m_config_get(V8M_OPT_THREAD_CACHE_MAX) != 1024) {
		return fail("env THREAD_CACHE_MAX not picked up");
	}
	if (v8m_config_get(V8M_OPT_HUGE_PAGES) != 0) {
		return fail("env HUGE_PAGES not picked up");
	}
	if (v8m_config_get(V8M_OPT_NUMA_AWARE) != 0) {
		return fail("env NUMA_AWARE not picked up");
	}
	if (v8m_config_get(V8M_OPT_DEBUG) != 1) {
		return fail("env DEBUG not picked up");
	}
	if (v8m_config_get(V8M_OPT_PROFILE) != 1) {
		return fail("env PROFILE not picked up");
	}
	if (v8m_config_get(V8M_OPT_COMPACT_THRESHOLD) != 50) {
		return fail("env COMPACT_THRESHOLD not picked up");
	}
	return 0;
}

static int check_invalid_env_falls_back(void)
{
	clear_all_env();
	/* Garbage value, partial value, leading non-digit. */
	if (setenv("V8M_PURGE_INTERVAL", "abc", 1) != 0 ||
	    setenv("V8M_THREAD_CACHE_MAX", "12xy", 1) != 0 ||
	    setenv("V8M_COMPACT_THRESHOLD", "", 1) != 0) {
		return fail("setenv failed (invalid)");
	}
	v8m_config_init();

	if (v8m_config_get(V8M_OPT_PURGE_INTERVAL) != 10) {
		return fail("non-numeric env did not fall back to default");
	}
	if (v8m_config_get(V8M_OPT_THREAD_CACHE_MAX) != 256) {
		return fail("partial-numeric env did not fall back to default");
	}
	if (v8m_config_get(V8M_OPT_COMPACT_THRESHOLD) != 25) {
		return fail("empty env did not fall back to default");
	}
	return 0;
}
/* NOLINTEND(concurrency-mt-unsafe) */

static int check_set_get_round_trip(void)
{
	clear_all_env();
	v8m_config_init();

	if (v8m_config_set(V8M_OPT_THREAD_CACHE_MAX, 4096) != 0) {
		return fail("v8m_config_set returned non-zero on valid opt");
	}
	if (v8m_config_get(V8M_OPT_THREAD_CACHE_MAX) != 4096) {
		return fail("set/get round trip failed");
	}

	/* Negative values are passed through; consumers decide their
	 * meaning. */
	if (v8m_config_set(V8M_OPT_PURGE_INTERVAL, -1) != 0) {
		return fail("set rejected negative value");
	}
	if (v8m_config_get(V8M_OPT_PURGE_INTERVAL) != -1) {
		return fail("negative value not preserved");
	}
	return 0;
}

static int check_out_of_range(void)
{
	/* The point of this test is to drive the bounds check with
	 * inputs that are deliberately outside the enum's value
	 * range; suppress the analyzer's enum-out-of-range warning
	 * accordingly. */
	/* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
	if (v8m_config_set((enum v8m_option)V8M_OPT_COUNT, 1) != -1) {
		return fail("set with out-of-range opt did not return -1");
	}
	if (v8m_config_set((enum v8m_option) - 1, 1) != -1) {
		return fail("set with negative opt did not return -1");
	}
	if (v8m_config_get((enum v8m_option)V8M_OPT_COUNT) != 0) {
		return fail("get with out-of-range opt did not return 0");
	}
	if (v8m_config_get((enum v8m_option) - 1) != 0) {
		return fail("get with negative opt did not return 0");
	}
	/* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
	return 0;
}

int main(void)
{
	int status = check_defaults();
	if (status != 0) {
		return status;
	}
	status = check_env_overrides();
	if (status != 0) {
		return status;
	}
	status = check_invalid_env_falls_back();
	if (status != 0) {
		return status;
	}
	status = check_set_get_round_trip();
	if (status != 0) {
		return status;
	}
	return check_out_of_range();
}
