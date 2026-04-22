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
/*
 * RISC-V 64. Tier 2. Toolchain-driven feature use: stdatomic
 * lowers to LR.D/SC.D on baseline rv64gc and to AMOCAS when the
 * target advertises the Zacas extension; __builtin_clzll /
 * ctzll / popcountll lower to CLZ.D / CTZ.D / CPOP.D when Zbb
 * is on, otherwise fall back to the documented software
 * emulation. The cache line is implementation-defined (64 B on
 * SiFive U74 and T-Head C910 — the two parts most QEMU rootfses
 * model); a future cycle adds a runtime probe via
 * sysconf(_SC_LEVEL1_DCACHE_LINESIZE) for hosts that diverge.
 * Memory model is RVWMO; the FENCE instructions stdatomic
 * emits cover the model.
 */
#define V8M_ARCH_RISCV64 1
#define V8M_CACHE_LINE_SIZE 64
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#define V8M_ARCH_PPC64LE 1
#define V8M_CACHE_LINE_SIZE 128
#elif defined(__s390x__)
#define V8M_ARCH_S390X 1
#define V8M_CACHE_LINE_SIZE 256
#elif defined(__loongarch64)
/*
 * LoongArch 64. Tier 3. The GCC 13 / Clang 16 floor we already
 * gate is the published "good support" line for this target. Weak
 * memory model: stdatomic memory_order_acquire/release/seq_cst
 * lower to DBAR; LL.D/SC.D back the C11 _Atomic CAS path; tp
 * ($r2) carries __thread storage with a single load. Cache line
 * is 64 B on 3A5000 / 3A6000.
 */
#define V8M_ARCH_LOONGARCH64 1
#define V8M_CACHE_LINE_SIZE 64
#else
#error "Unsupported architecture (see platform-abstraction.md)"
#endif

/* --- Kernel huge-page geometry ------------------------------------- */
/*
 * Default kernel huge-page size, in bytes. Drives both the
 * MAP_HUGETLB attempt threshold in src/v8m_page_heap.c (we only
 * try MAP_HUGETLB for size+alignment that meet this threshold) and
 * the MADV_HUGEPAGE hint threshold in the same file. Per
 * huge-pages.md §4.2 / platform-abstraction.md §5.4, every Tier 1/2
 * arch we ship today uses 2 MiB by default; s390x uses 1 MiB
 * (slabs_per_huge = 16 instead of 32 follows directly).
 *
 * This is the size the kernel picks when MAP_HUGETLB is set without
 * an explicit size flag. The 1 GiB Gigantic path uses MAP_HUGE_1GB
 * to override; nothing here gates that path.
 */
#if defined(V8M_ARCH_S390X)
#define V8M_HUGE_PAGE_SIZE ((size_t)1 * 1024 * 1024)
#else
#define V8M_HUGE_PAGE_SIZE ((size_t)2 * 1024 * 1024)
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
