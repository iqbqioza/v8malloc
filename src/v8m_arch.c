/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Architecture abstraction layer — runtime CPU feature probes.
 * The header v8m_arch.h owns the compile-time half (architecture
 * detection macros, cache-line constant, attribute helpers); this
 * TU owns the runtime half. Every probe is a one-shot read that
 * costs at most a single syscall (getauxval) or system-register
 * fetch (mrs); no caching is needed because the kernel's auxv
 * table and the CPU's CTR_EL0 register are constant for the
 * process lifetime.
 *
 * Each probe ships a portable fallback so non-aarch64 builds get
 * a sensible default and callers can treat them as architecture-
 * independent.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "v8m_arch.h"

#if defined(V8M_ARCH_AARCH64)
#include <sys/auxv.h>
#endif

#if defined(V8M_ARCH_X86_64)
#include <x86intrin.h> /* __rdtsc */
#endif

bool v8m_arch_has_lse(void)
{
#if defined(V8M_ARCH_AARCH64)
	/* HWCAP_ATOMICS lives at bit 8 of AT_HWCAP — the constant
	 * comes in via <sys/auxv.h> on glibc / musl, but defining it
	 * locally keeps the build green on toolchains that ship an
	 * older auxv.h. */
#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1UL << 8)
#endif
	return (getauxval(AT_HWCAP) & HWCAP_ATOMICS) != 0UL;
#else
	return false;
#endif
}

size_t v8m_arch_runtime_cache_line_size(void)
{
#if defined(V8M_ARCH_AARCH64)
	/* CTR_EL0 is unprivileged on AArch64 (the kernel can trap
	 * the read and emulate, but the architectural mandate is
	 * that EL0 reads succeed). DminLine is bits [19:16], the
	 * log2 of dcache line size in 4-byte words, so the byte
	 * count is `4 << DminLine`. */
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uint32_t d_min_line = (uint32_t)((ctr >> 16U) & 0xFU);
	return (size_t)(4U << d_min_line);
#else
	return (size_t)V8M_CACHE_LINE_SIZE;
#endif
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;
	/* CLOCK_MONOTONIC_RAW is provided by <time.h> on glibc / musl
	 * via the transitively-included <bits/time.h>; tidy's IWYU
	 * pass prefers the deeper kernel-private header which we do
	 * not want to pull. */
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	(void)clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	return ((uint64_t)now.tv_sec * 1000000000ULL) + (uint64_t)now.tv_nsec;
}

uint64_t v8m_arch_rdtsc(void)
{
#if defined(V8M_ARCH_X86_64)
	return (uint64_t)__rdtsc();
#else
	return monotonic_ns();
#endif
}

uint32_t v8m_arch_tsc_frequency_mhz(void)
{
#if defined(V8M_ARCH_X86_64)
	/* Lazy-calibrated, cached in an atomic. The first caller pays
	 * the calibration cost; everyone else gets the cached value.
	 * Calibration is short (~5 ms wall time) but unbounded in
	 * principle if the kernel scheduler delays the second
	 * clock_gettime, so we cap the result to a plausible band so
	 * the EMA controller never sees a wildly wrong tick rate. */
	static _Atomic uint32_t cached = 0;
	uint32_t value = atomic_load_explicit(&cached, memory_order_relaxed);
	if (value != 0U) {
		return value;
	}

	uint64_t t0_ns = monotonic_ns();
	uint64_t t0_tsc = (uint64_t)__rdtsc();
	struct timespec interval = {.tv_sec = 0, .tv_nsec = 5000000L};
	(void)nanosleep(&interval, NULL);
	uint64_t t1_tsc = (uint64_t)__rdtsc();
	uint64_t t1_ns = monotonic_ns();

	uint64_t tsc_delta = (t1_tsc > t0_tsc) ? (t1_tsc - t0_tsc) : 0ULL;
	uint64_t ns_delta = (t1_ns > t0_ns) ? (t1_ns - t0_ns) : 0ULL;
	uint64_t mhz =
	    (ns_delta == 0ULL) ? 0ULL : (tsc_delta * 1000ULL) / ns_delta;

	/* Plausible bounds: 100 MHz floor (low-power embedded x86)
	 * up to 10 GHz ceiling (well above any shipping silicon).
	 * A scheduler hiccup that pushes the calibration outside
	 * this band falls back to a sensible 3 GHz default rather
	 * than poisoning every downstream batch-size computation. */
	if (mhz < 100ULL || mhz > 10000ULL) {
		mhz = 3000ULL;
	}
	value = (uint32_t)mhz;
	atomic_store_explicit(&cached, value, memory_order_relaxed);
	return value;
#else
	/* On non-x86_64 builds v8m_arch_rdtsc returns nanoseconds, so
	 * "ticks per microsecond" is exactly 1000. */
	return 1000U;
#endif
}
