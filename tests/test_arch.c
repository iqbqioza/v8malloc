/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Architecture abstraction layer (AAL) primitives test —
 * platform-abstraction.md §6.2. The spec calls for a per-primitive
 * test file split (test_cache_line.c / test_atomics.c /
 * test_bitops.c / test_tls.c / test_prefetch.c / test_page_size.c).
 * In v0 the AAL exposes only arch-detection macros, the cache-line
 * size constant, alignment / branch-hint attributes, and a
 * page-size macro; the atomic / bitops / TLS / prefetch primitives
 * the spec lists as wrappers are still used directly through
 * <stdatomic.h> and `__builtin_*` intrinsics. This file covers
 * what exists today in one consolidated test; the per-primitive
 * files land when the wrappers do.
 *
 * What this test checks:
 *   - Exactly one V8M_ARCH_* is defined (mutual exclusion);
 *     detection is consistent with the compiler's built-in
 *     __*__ predefined macros.
 *   - V8M_CACHE_LINE_SIZE matches the arch the spec mandates
 *     (64 / 128 / 256 — platform-abstraction.md §5.3) and is a
 *     power of two (a prereq for the allocator's bit-mask
 *     alignment tricks).
 *   - V8M_CACHELINE_ALIGNED places a struct on a boundary
 *     divisible by V8M_CACHE_LINE_SIZE; V8M_ALIGNED(N) honors
 *     arbitrary N.
 *   - V8M_LIKELY / V8M_UNLIKELY are hints only: both branches
 *     produce the same observable outcome.
 *   - V8M_PAGE_SIZE is a power of two, matches V8M_PAGE_MASK,
 *     and V8M_PAGE_SHIFT is consistent (the hot-path
 *     ptr_to_meta uses these invariants).
 *   - The bit-ops builtins the allocator relies on
 *     (__builtin_ctzll, clzll, popcountll) return the expected
 *     values on hand-built inputs — a stale compiler could
 *     silently miscompute these on an exotic target.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "v8m_arch.h"
#include "v8m_internal.h" /* V8M_PAGE_SHIFT / SIZE / MASK */

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_arch: %s\n", msg);
	return 1;
}

static int check_exactly_one_arch(void)
{
	int count = 0;
#ifdef V8M_ARCH_X86_64
	count++;
#endif
#ifdef V8M_ARCH_AARCH64
	count++;
#endif
#ifdef V8M_ARCH_RISCV64
	count++;
#endif
#ifdef V8M_ARCH_PPC64LE
	count++;
#endif
#ifdef V8M_ARCH_S390X
	count++;
#endif
#ifdef V8M_ARCH_LOONGARCH64
	count++;
#endif
	/* cppcheck-suppress knownConditionTrueFalse */
	if (count != 1) {
		(void)fprintf(stderr,
			      "test_arch: expected exactly 1 V8M_ARCH_* "
			      "defined, got %d\n",
			      count);
		return 1;
	}

	/* Cross-check: the compiler's own predefined __*__ macro for
	 * the running arch should agree with V8M_ARCH_*. If they
	 * disagree, v8m_arch.h has gone stale. */
#if defined(__x86_64__) && !defined(V8M_ARCH_X86_64)
	return fail("__x86_64__ set but V8M_ARCH_X86_64 not defined");
#elif defined(__aarch64__) && !defined(V8M_ARCH_AARCH64)
	return fail("__aarch64__ set but V8M_ARCH_AARCH64 not defined");
#elif defined(__riscv) && !defined(V8M_ARCH_RISCV64)
	return fail("__riscv set but V8M_ARCH_RISCV64 not defined");
#elif defined(__powerpc64__) && !defined(V8M_ARCH_PPC64LE)
	return fail("__powerpc64__ set but V8M_ARCH_PPC64LE not defined");
#elif defined(__s390x__) && !defined(V8M_ARCH_S390X)
	return fail("__s390x__ set but V8M_ARCH_S390X not defined");
#elif defined(__loongarch64) && !defined(V8M_ARCH_LOONGARCH64)
	return fail("__loongarch64 set but V8M_ARCH_LOONGARCH64 not defined");
#endif
	return 0;
}

static int check_cache_line_size(void)
{
	size_t size = V8M_CACHE_LINE_SIZE;
	/* cppcheck-suppress knownConditionTrueFalse */
	if (size == 0U || (size & (size - 1U)) != 0U) {
		return fail("V8M_CACHE_LINE_SIZE is not a power of two");
	}
	/* Per platform-abstraction.md §5.3: x86_64 / aarch64 /
	 * riscv64 / loongarch64 → 64; ppc64le → 128; s390x → 256. */
#if defined(V8M_ARCH_X86_64) || defined(V8M_ARCH_AARCH64) ||                   \
    defined(V8M_ARCH_RISCV64) || defined(V8M_ARCH_LOONGARCH64)
	/* cppcheck-suppress knownConditionTrueFalse */
	if (size != 64U) {
		return fail("expected V8M_CACHE_LINE_SIZE == 64 on this arch");
	}
#elif defined(V8M_ARCH_PPC64LE)
	/* cppcheck-suppress knownConditionTrueFalse */
	if (size != 128U) {
		return fail("expected V8M_CACHE_LINE_SIZE == 128 on ppc64le");
	}
#elif defined(V8M_ARCH_S390X)
	/* cppcheck-suppress knownConditionTrueFalse */
	if (size != 256U) {
		return fail("expected V8M_CACHE_LINE_SIZE == 256 on s390x");
	}
#endif
	return 0;
}

struct V8M_CACHELINE_ALIGNED cacheline_probe {
	/* cppcheck-suppress unusedStructMember */
	uint64_t sentinel;
};

static int check_alignment_macros(void)
{
	/* cppcheck-suppress unassignedVariable */
	struct cacheline_probe probe;
	uintptr_t addr = (uintptr_t)&probe;
	if ((addr & (V8M_CACHE_LINE_SIZE - 1U)) != 0U) {
		return fail("V8M_CACHELINE_ALIGNED did not cache-line align");
	}

	/* V8M_ALIGNED(N) on a stack struct should align the struct
	 * to N. Test with 128 which exceeds every natural alignment. */
	struct V8M_ALIGNED(128) align_probe {
		/* cppcheck-suppress unusedStructMember */
		uint64_t value;
	};
	/* cppcheck-suppress unassignedVariable */
	struct align_probe a_probe;
	if (((uintptr_t)&a_probe & 127U) != 0U) {
		return fail("V8M_ALIGNED(128) did not honor alignment");
	}
	return 0;
}

static int check_branch_hints(void)
{
	int counter = 0;
	for (int i = 0; i < 100; i++) {
		if (V8M_LIKELY(i % 2 == 0)) {
			counter++;
		}
	}
	if (counter != 50) {
		return fail("V8M_LIKELY affected observable outcome");
	}
	counter = 0;
	for (int i = 0; i < 100; i++) {
		if (V8M_UNLIKELY(i % 3 == 0)) {
			counter++;
		}
	}
	if (counter != 34) {
		return fail("V8M_UNLIKELY affected observable outcome");
	}
	return 0;
}

static int check_page_size(void)
{
	if (V8M_PAGE_SIZE == 0U ||
	    (V8M_PAGE_SIZE & (V8M_PAGE_SIZE - 1U)) != 0U) {
		return fail("V8M_PAGE_SIZE is not a power of two");
	}
	/* These two invariants are tautological by construction at the
	 * macros' current definitions — cppcheck correctly sees both
	 * sides as equivalent. That's the point: if a maintainer bumps
	 * V8M_PAGE_SHIFT without updating V8M_PAGE_SIZE / V8M_PAGE_MASK
	 * (or vice versa), these checks stop being tautologies and
	 * start firing. Suppress the style warning so the gate still
	 * exists. */
	/* cppcheck-suppress duplicateExpression */
	/* NOLINTNEXTLINE(misc-redundant-expression) */
	if ((size_t)1 << V8M_PAGE_SHIFT != V8M_PAGE_SIZE) {
		return fail("V8M_PAGE_SHIFT / V8M_PAGE_SIZE inconsistent");
	}
	/* cppcheck-suppress duplicateExpression */
	/* NOLINTNEXTLINE(misc-redundant-expression) */
	if (V8M_PAGE_MASK != ~(uintptr_t)(V8M_PAGE_SIZE - 1U)) {
		return fail(
		    "V8M_PAGE_MASK does not mask off V8M_PAGE_SIZE - 1");
	}
	/* Spot-check ptr-to-page-base against the mask. */
	uintptr_t sample = (uintptr_t)0x12345678abcd0123ULL;
	uintptr_t base = sample & V8M_PAGE_MASK;
	if ((base & (V8M_PAGE_SIZE - 1U)) != 0U) {
		return fail("V8M_PAGE_MASK failed to align a sample pointer");
	}
	return 0;
}

static int check_runtime_probes(void)
{
	/* The runtime probes are portable: on non-aarch64 builds they
	 * return sensible defaults (`false` for LSE, V8M_CACHE_LINE_SIZE
	 * for the cache line). On aarch64 builds the values come from
	 * AT_HWCAP and CTR_EL0 — we can't assert a specific outcome
	 * there (it depends on the host CPU and kernel config), only
	 * that the probe runs without crashing and returns something
	 * sane. */
	bool lse = v8m_arch_has_lse();
#if !defined(V8M_ARCH_AARCH64)
	if (lse) {
		return fail("v8m_arch_has_lse returned true on non-aarch64");
	}
#else
	/* Silence -Wunused-but-set on aarch64 where the value is free. */
	(void)lse;
#endif

	size_t line = v8m_arch_runtime_cache_line_size();
	if (line == 0U || (line & (line - 1U)) != 0U) {
		return fail("runtime cache line size is not a power of two");
	}
	/* CTR_EL0 encodes DminLine as log2(words); the realistic range
	 * for a dcache line is 16 B (tiny embedded) up to 256 B
	 * (mainframe-class). Anything outside that points at a broken
	 * probe. */
	if (line < 16U || line > 256U) {
		return fail("runtime cache line size out of plausible range");
	}
#if !defined(V8M_ARCH_AARCH64)
	/* Non-aarch64 builds return the compile-time constant
	 * verbatim — anything else would mean the portable fallback
	 * drifted. */
	if (line != (size_t)V8M_CACHE_LINE_SIZE) {
		return fail(
		    "non-aarch64 runtime line size diverged from compile-time");
	}
#endif
	return 0;
}

static int check_bitops_builtins(void)
{
	/* The allocator relies on __builtin_ctzll, __builtin_clzll,
	 * and __builtin_popcountll across the slab bitmap + buddy
	 * level-picking paths. These intrinsics are compile-time for
	 * every target the repo supports, but a broken cross toolchain
	 * (or an unexpected -mllvm reshuffle) could miscompute them.
	 * Spot-check hand-built inputs. */
	if (__builtin_ctzll(0x1ULL) != 0) {
		return fail("ctzll(0x1) != 0");
	}
	if (__builtin_ctzll(0x8000000000000000ULL) != 63) {
		return fail("ctzll(0x800...000) != 63");
	}
	if (__builtin_clzll(0x1ULL) != 63) {
		return fail("clzll(0x1) != 63");
	}
	if (__builtin_clzll(0x8000000000000000ULL) != 0) {
		return fail("clzll(0x800...000) != 0");
	}
	if (__builtin_popcountll(0ULL) != 0) {
		return fail("popcountll(0) != 0");
	}
	if (__builtin_popcountll(0xFFFFFFFFFFFFFFFFULL) != 64) {
		return fail("popcountll(all-ones) != 64");
	}
	if (__builtin_popcountll(0xF0F0F0F0F0F0F0F0ULL) != 32) {
		return fail("popcountll(alternating nibbles) != 32");
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_exactly_one_arch();
	result |= check_cache_line_size();
	result |= check_alignment_macros();
	result |= check_branch_hints();
	result |= check_page_size();
	result |= check_runtime_probes();
	result |= check_bitops_builtins();
	if (result == 0) {
		(void)printf("test_arch: OK\n");
	}
	return result;
}
