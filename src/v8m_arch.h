/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Architecture abstraction layer macros. Hides per-architecture
 * details (cache-line size, alignment helpers, branch hints) behind a
 * stable interface. Atomics, prefetch, CLZ/CTZ/POPCOUNT helpers, and
 * TLS plumbing are added here as later cycles need them; see
 * .claude/docs/platform-abstraction.md for the full design.
 *
 * Linux-only; the build system rejects non-Linux targets, and this
 * header additionally rejects unsupported architectures at compile
 * time so a misconfigured cross build fails loudly.
 */

#ifndef V8M_ARCH_H
#define V8M_ARCH_H

/* --- OS check (also enforced by CMake) ----------------------------- */

#if !defined(__linux__)
#error "v8malloc supports Linux only"
#endif

/* --- Architecture detection ---------------------------------------- */

#if defined(__x86_64__)
#define V8M_ARCH_X86_64 1
#define V8M_CACHE_LINE_SIZE 64
#elif defined(__aarch64__)
#define V8M_ARCH_AARCH64 1
#define V8M_CACHE_LINE_SIZE 64
#elif defined(__riscv) && (__riscv_xlen == 64)
#define V8M_ARCH_RISCV64 1
#define V8M_CACHE_LINE_SIZE 64
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#define V8M_ARCH_PPC64LE 1
#define V8M_CACHE_LINE_SIZE 128
#elif defined(__s390x__)
#define V8M_ARCH_S390X 1
#define V8M_CACHE_LINE_SIZE 256
#elif defined(__loongarch64)
#define V8M_ARCH_LOONGARCH64 1
#define V8M_CACHE_LINE_SIZE 64
#else
#error "Unsupported architecture (see platform-abstraction.md)"
#endif

/* --- Compiler attributes ------------------------------------------- */

#define V8M_ALIGNED(n) __attribute__((aligned(n)))
#define V8M_CACHELINE_ALIGNED V8M_ALIGNED(V8M_CACHE_LINE_SIZE)
#define V8M_NOINLINE __attribute__((noinline))
#define V8M_ALWAYS_INLINE __attribute__((always_inline)) inline
#define V8M_PURE __attribute__((pure))
#define V8M_CONST_FN __attribute__((const))
#define V8M_UNUSED __attribute__((unused))

/*
 * Branch hints. Use sparingly — modern branch predictors almost
 * always do the right thing. Reserve these for the allocator's hot
 * paths where mispredict cost is measurable.
 */
#define V8M_LIKELY(x) __builtin_expect(!!(x), 1)
#define V8M_UNLIKELY(x) __builtin_expect(!!(x), 0)

#endif /* V8M_ARCH_H */
