/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Diagnostic / introspection surface — split out of v8m_api.c. The
 * functions here are read-only snapshots of process-wide state that
 * a maintainer or operator queries at runtime: ISA capabilities, the
 * stable short name of a config option, the size-class table swap,
 * a /proc/self/maps VMA count, and the lifetime-classify hint.
 *
 * They share two properties that motivated the split:
 *   1. NONE of them touch the dispatcher singleton (`g_dispatch`),
 *      so they don't need an accessor to reach into v8m_api.c —
 *      they sit independently on top of the per-module surfaces
 *      (v8m_arch.h, v8m_size_class.h, v8m_thread_cache.h).
 *   2. They have nothing to do with the malloc/free hot path; their
 *      callers are diagnostics tools, tests, or bg-tick reporters.
 *
 * Public ABI: every export here is in V8MALLOC_1.0 via
 * `v8malloc.map`. The split is purely a TU-level reorganisation;
 * the symbol set, ordering, and signatures are unchanged.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "v8m_api_internal.h"
#include "v8m_arch.h"
#include "v8m_size_class.h"
#include "v8m_thread_cache.h"
#include "v8malloc/v8malloc.h"

V8M_EXPORT int v8m_install_size_class_table(const uint32_t *new_table)
{
	if (new_table != NULL && new_table[0] < 8U) {
		errno = EINVAL;
		return -1;
	}
	(void)v8m_size_class_install_table(new_table);
	return 0;
}

V8M_EXPORT uint32_t v8m_size_class_to_bytes(int cls)
{
	if (cls < 0 || (uint32_t)cls >= V8M_NUM_SIZE_CLASSES) {
		return 0U;
	}
	return v8m_size_class_size((uint32_t)cls);
}

/* Mirror of arch_name() in src/v8m_arch.c. The arch.c helper is
 * static so we can't share it across TUs; the surface is small
 * enough that duplicating it costs less than exposing a private
 * API. A future cycle can promote arch_name() to v8m_arch.h if
 * another consumer needs it. */
static const char *api_arch_name(void)
{
#if defined(V8M_ARCH_X86_64)
	return "x86_64";
#elif defined(V8M_ARCH_AARCH64)
	return "aarch64";
#elif defined(V8M_ARCH_RISCV64)
	return "riscv64";
#elif defined(V8M_ARCH_PPC64LE)
	return "ppc64le";
#elif defined(V8M_ARCH_S390X)
	return "s390x";
#elif defined(V8M_ARCH_LOONGARCH64)
	return "loongarch64";
#else
	return "unknown";
#endif
}

V8M_EXPORT void v8m_get_arch_info(struct v8m_arch_info *out)
{
	if (out == NULL) {
		return;
	}
	(void)memset(out, 0, sizeof(*out));
	const char *name = api_arch_name();
	size_t name_len = strlen(name);
	if (name_len >= sizeof(out->arch_name)) {
		name_len = sizeof(out->arch_name) - 1U;
	}
	(void)memcpy(out->arch_name, name, name_len);
	out->arch_name[name_len] = '\0';
	out->cache_line_bytes = (uint32_t)v8m_arch_runtime_cache_line_size();
	out->build_cache_line_bytes = (uint32_t)V8M_CACHE_LINE_SIZE;
	out->tsc_mhz = v8m_arch_tsc_frequency_mhz();
	out->has_lse = v8m_arch_has_lse() ? 1U : 0U;
	out->has_zbb = v8m_arch_has_zbb() ? 1U : 0U;
	out->has_zacas = v8m_arch_has_zacas() ? 1U : 0U;
}

/* Stable short names for V8M_OPT_* — caller-visible via the
 * `v8m_option_name(id)` accessor. Indexed by the option id; the
 * static_assert keeps the table length in lock-step with the enum
 * so a future option that lands without an entry fails the
 * build instead of silently returning NULL. */
static const char *const g_option_names[V8M_OPT_COUNT] = {
    [V8M_OPT_VERBOSE] = "verbose",
    [V8M_OPT_PURGE_INTERVAL] = "purge_interval",
    [V8M_OPT_THREAD_CACHE_MAX] = "thread_cache_max",
    [V8M_OPT_HUGE_PAGES] = "huge_pages",
    [V8M_OPT_NUMA_AWARE] = "numa_aware",
    [V8M_OPT_DEBUG] = "debug",
    [V8M_OPT_PROFILE] = "profile",
    [V8M_OPT_COMPACT_THRESHOLD] = "compact_threshold",
    [V8M_OPT_VMA_WARN_THRESHOLD] = "vma_warn_threshold",
    [V8M_OPT_NUMA_AGGRESSIVE_MIGRATION] = "numa_aggressive_migration",
    [V8M_OPT_DEFERRED_COALESCE] = "deferred_coalesce",
    [V8M_OPT_LIFETIME_TRACKING] = "lifetime_tracking",
};

V8M_EXPORT const char *v8m_option_name(int option_id)
{
	if (option_id < 0 || option_id >= V8M_OPT_COUNT) {
		return NULL;
	}
	return g_option_names[option_id];
}

V8M_EXPORT enum v8m_lifetime_class v8m_estimate_lifetime(const void *caller_pc)
{
	if (!v8m_api_dispatch_ready()) {
		return V8M_LIFETIME_UNKNOWN;
	}
	return v8m_thread_cache_lifetime_classify(caller_pc);
}

V8M_EXPORT uint64_t v8m_count_vmas(void)
{
	/* Open /proc/self/maps with raw read() to avoid an alloc on
	 * the fast path — fopen pulls in libio buffers that would
	 * route back through v8malloc. open + read + close are
	 * malloc-free. Each line in /proc/self/maps describes one
	 * VMA, so the line count is the VMA count. */
	int maps_fd = open("/proc/self/maps", O_RDONLY);
	if (maps_fd < 0) {
		return 0;
	}
	uint64_t lines = 0;
	char buf[4096];
	for (;;) {
		ssize_t got = read(maps_fd, buf, sizeof(buf));
		if (got <= 0) {
			break;
		}
		for (ssize_t i = 0; i < got; i++) {
			if (buf[i] == '\n') {
				lines++;
			}
		}
	}
	(void)close(maps_fd);
	return lines;
}
