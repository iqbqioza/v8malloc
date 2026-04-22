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

#include <stddef.h>

#include "v8m_arch.h"

#if defined(V8M_ARCH_AARCH64)
#include <stdint.h>
#include <sys/auxv.h>
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
