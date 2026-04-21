# Changelog

All notable changes to v8malloc are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- MB-03 producer/consumer benchmark
  (`bench/mb_03_producer_consumer.c`, benchmarks.md §2.3). N
  producer threads each `malloc` fixed-size objects and hand
  them to a paired consumer via a bounded 1024-slot SPSC ring;
  the consumer `free`s. Every alloc/free pair crosses a thread
  boundary, so the bench measures the cost of the cross-thread
  free path that the future remote-free MPSC queue will
  optimize. Pair sweep (1, 2, 4, 8, 16), capped at
  `min(nproc/2, 32)` by default (`V8M_BENCH_MAX_PAIRS=N`
  overrides). Size sweep matches the spec exactly: 64 B / 256 B
  / 1 KiB. Reports per-row `handoffs` / `handoffs_per_sec` /
  `per_pair` so the single-mutex scaling story is legible.
  Also serves as the regression gate for the slab-pool
  partials-list double-insertion bug fixed in the same cycle as
  MB-04: any reintroduction of a stale partials-list entry
  segfaults this bench within seconds of the timed loop.

### Fixed
- Slab pool partials-list duplicate-insertion bug
  (`src/v8m_slab_pool.c`). When a `free` triggered a
  full → partial transition on a page that was still tracked as
  `cls->current`, the was_full branch unconditionally inserted
  the page into `cls->partials`, leaving the page tracked from
  two slots simultaneously (current + partials head). When the
  page later drained to empty, `unlink_from_class` removed only
  one occurrence; the other survived the subsequent
  `page_heap_free → munmap`, so the partials list ended up
  holding a dangling pointer to an unmapped page. The next
  allocation that reached `try_partials` would walk the list,
  read `(*link)->used_count` from the freed memory, and SIGSEGV
  at the slab-page base (or at a stale magic byte if the kernel
  had already remapped the address for a different VMA).
  
  Surfaced first by an attempted MB-03 producer/consumer bench
  (deleted at the time as "blocked by a slab-pool race") and
  re-surfaced reliably by the new single-thread MB-04. Fix:
  `else if (was_full)` becomes `else if (was_full && cls->current
  != meta)` — if meta is already the current page, the partials
  insertion is redundant and the duplicate entry is the bug.
  Verified by running MB-04 to completion at live_count = 2000
  (~2.2 M ops in 500 ms), the failing repro before the fix.

### Added
- MB-04 mixed-size workload benchmark
  (`bench/mb_04_mixed.c`, benchmarks.md §2.4). Single-thread
  workload driving a bounded working set of `live_count`
  concurrent allocations through alloc/free with sizes drawn
  from the spec's six-band distribution (8 B/32 B at 40 %, …,
  > 64 KiB at 3 %). Each iteration picks a random slot and
  replaces its allocation with a fresh size from the
  distribution, so the working set holds at roughly
  `live_count` live objects — the spec's "concurrently live
  objects" knob. NULL returns are tolerated and counted in
  `alloc_failures` (the > 64 KiB tail occasionally exhausts the
  buddy pool's 16 MiB cap; throughput reflects only successful
  ops). Bringing this up surfaced and unblocked the slab-pool
  partials-list double-insertion bug fixed in this same cycle.
  Multi-thread variants (the spec's "threads: 1, 8, 64") land
  with finer-grained per-pool synchronization. Knobs:
  `V8M_BENCH_DURATION_MS` (default 1000), `V8M_BENCH_WARMUP_MS`
  (default 100), `V8M_BENCH_LIVE_COUNT` (default 10000, capped
  at 10 M), `V8M_BENCH_SEED` (default 0x1234).

- MB-06 large-allocation latency benchmark
  (`bench/mb_06_large_latency.c`, benchmarks.md §2.6).
  Single-thread per-iteration timing of the Large / Huge mmap
  path, reported separately for the three phases the kernel
  charges differently: `alloc` latency (mmap + region-map
  insert), `fault` latency (the first-touch `memset` that pulls
  in physical pages on demand — page-fault dominated), and
  `free` latency (region-map remove + munmap). Each phase
  reports both p50 and p99 across `V8M_BENCH_ITERS` iterations
  per size, so the spec's "Large allocation ≤0.5× glibc"
  median-comparison criterion (benchmarks.md §6) is directly
  legible. The memset is fenced with a one-byte
  `__asm__ volatile("" : : "r"(ptr) : "memory")` compiler
  barrier — without it gcc dead-code-eliminates the memset
  (its result is only ever consumed by free) and the fault
  numbers come out as zero.

  Sweeps the spec's exact size ladder (256 KiB → 256 MiB).
  256 KiB sits at the buddy/Large boundary so the bench also
  surfaces the routing transition. Multi-thread coverage (the
  spec's "threads: 1, 8") lands once the page-heap region map
  grows finer-grained synchronization. Knobs:
  `V8M_BENCH_ITERS` (default 100, clamped to [10, 10000] so
  taint-flow analysis is happy), `V8M_BENCH_MAX_SIZE_MB`
  (default 256; lower for limited-RAM CI runners).

- Benchmark runner script (`scripts/bench-run.sh`). Pins the
  host into the benchmarks.md §1.3 measurement environment
  before invoking the supplied bench command — switches every
  online CPU's frequency governor to `performance`, disables
  transparent huge pages, and disables ASLR — then restores the
  prior settings on exit (trap on EXIT/INT/TERM). Tuning steps
  need root; without root the script prints a warning and runs
  the bench anyway so dev-laptop iteration still benefits from
  the harness. `--drop-caches` opt-in for fault-heavy workloads.
  Usage: `scripts/bench-run.sh -- ./build/bench/bench/mb_01_throughput`.

- Exhaustive `malloc_usable_size` contract test
  (`tests/test_usable_size.c`). Sweeps every Tiny / Small slab
  request in `[1, V8M_SMALL_MAX_SIZE]` (4 096 sizes) and asserts
  `usable == v8m_class_to_size[v8m_size_class(req)]` — slab slots
  are exact-sized, so this is the strict equality version of the
  contract. Sweeps every buddy power-of-two boundary plus the
  size just below it and asserts `req <= usable <=
  V8M_BUDDY_MAX_BLOCK`. Sweeps four Large / Huge sizes
  (512 KiB → 4 MiB) and asserts `usable >= req`. Plus the
  writable-window check: two anchor allocations holding 0x42
  surround a probe; the test memsets every byte up to
  `malloc_usable_size(probe)` with 0xCD, frees the probe, and
  verifies both anchors still hold 0x42 — a misclaimed
  usable_size that overflows the backing allocation would
  corrupt the anchor pattern. Also covers the
  `malloc_usable_size(NULL) == 0` contract and the aligned_alloc
  path's `usable >= req` even when alignment bumps the request
  into a larger class. Complements the single-sample basic
  check in `tests/test_api.c`.

- Exhaustive calloc / reallocarray test
  (`tests/test_calloc.c`). A 9-cell sweep across every backend
  (slab Tiny → slab Small → buddy Medium → buddy boundary →
  Large mmap → Huge mmap), each cell calling `calloc(nmemb,
  size)`, verifying every byte is zero, dirtying with 0xAB, and
  freeing — then a second pass repeats the sweep so a
  missed-zero bug on slot reuse surfaces (the second-pass calloc
  may land on a slot the first pass dirtied). Plus boundary
  overflow tests beyond the trivial SIZE_MAX × SIZE_MAX (the
  hard cases — small × large that still wraps, e.g.
  (SIZE_MAX/2)+2 × 2), the calloc(0, n) / calloc(n, 0) /
  calloc(0, 0) lifecycle edges, reallocarray's matching
  overflow contract, and reallocarray byte-preservation across
  grow + shrink. Complements test_api's basic happy path.

### Notes
- MB-03 producer/consumer benchmark exposed and reverted. A
  first-cut implementation (one SPSC ring per pair, producer
  malloc → consumer free) ran into a slab-pool use-after-free
  under sustained cross-thread alloc/free at the same size
  class: the empty-page reclamation (`v8m_page_heap_free` →
  `munmap` while holding the slab pool mutex) interleaved with
  a concurrent `v8m_slab_pool_alloc` such that a producer wrote
  to a page the consumer had just unmapped. Both threads were
  serialized on the pool mutex, so the race is somewhere
  subtler — most likely `cls->current` becoming stale across
  unmap+remap when `mmap(NULL, ...)` happens to return the same
  virtual address. Reverted the bench to keep the tree green;
  MB-03 is unblocked once the thread cache lands and empty-page
  reclamation can be made lazy. Tracked in TODO.md.

- Exhaustive realloc-semantics test (`tests/test_realloc.c`).
  Walks a 10-cell size ladder (8 B → 4 MiB) upward through every
  backend boundary — slab Tiny → slab Small → buddy Medium →
  buddy boundary → Large mmap → Huge mmap — reallocing one buffer
  through every cell. Stamps a deterministic byte pattern keyed
  on the absolute offset (so the pattern is one coherent run
  across multiple grow-with-copy steps), and verifies the prefix
  survives every transition. The downward shrink walk does the
  same in reverse — start from the largest cell, stamp the whole
  buffer, shrink one cell at a time, verify the surviving prefix
  after each step. Plus a 256-byte-step intra-class shrink loop
  within Small, a slab→Huge→slab round-trip with prefix
  verification at each end, and the standard edge cases
  (realloc(NULL, n), realloc(ptr, 0), realloc(NULL, 0)).
  Complements the basic realloc happy-path check in
  `tests/test_api.c`. Caught a stamp-vs-verify off-by-one in the
  test itself during development (the stamp helper was
  buffer-relative, the verify helper offset-relative); the fix
  threads an absolute (start, end) range through the stamp helper.

- Multi-arch weekly CI workflow
  (`.github/workflows/multi-arch.yml`). Runs the full build +
  ctest suite under QEMU-user emulation against every Tier 1 /
  Tier 2 architecture on the platform-abstraction.md support
  list — aarch64, ppc64le, s390x, riscv64 — exercising the
  per-arch atomics, alignment, and cache-line constants the
  architecture-abstraction layer (`src/v8m_arch.h`) selects on.
  Schedule: every Monday 06:00 UTC plus `workflow_dispatch` for
  ad-hoc reruns. Per-arch run takes 5–15 minutes (QEMU is slow);
  splitting from the per-PR gate keeps PR turnaround under five
  minutes. Uses `uraimo/run-on-arch-action@v3` which mounts
  `$GITHUB_WORKSPACE` inside the container at the same path so
  cmake paths line up with the host. riscv64 carries
  `continue-on-error: true` because qemu-user emulation
  occasionally faults on the runner kernels; the other three
  arches gate the workflow. loongarch64 (Tier 3) is intentionally
  absent until Debian's loongarch64 toolchain lands in the
  standard repos.

- Exhaustive alignment-sweep test (`tests/test_alignment.c`).
  Walks every power-of-two alignment from 16 (max_align_t) up to
  V8M_BUDDY_MAX_BLOCK across five representative request sizes
  that hit each backend (slab Tiny, slab Small, buddy Medium,
  buddy boundary, Large mmap), covering `aligned_alloc`,
  `posix_memalign`, `memalign`, `valloc`, and `pvalloc`. For every
  accepted (alignment, size) pair, asserts the returned pointer
  satisfies the alignment, round-trips a `memset` + `free`, and
  (for posix_memalign) leaves `*memptr` unclobbered on every
  EINVAL path. A `combo_supported` predicate skips combinations
  the dispatcher rejects (alignment > V8M_BUDDY_MAX_BLOCK overall,
  alignment > V8M_PAGE_SIZE/2 for Large); the over-cap behaviour
  is exercised by a separate oversize-rejection check.
  Complements the basic happy-path coverage in `test_api.c` —
  test_alignment is the contract: a future refactor of the buddy /
  large routing cannot weaken the alignment guarantee on any cell
  of the matrix without this test catching it.

- MB-02 multi-thread scalability benchmark
  (`bench/mb_02_scalability.c`, benchmarks.md §2.2). Sweeps the
  spec's 1 / 2 / 4 / 8 / 16 / 32 / 64 / 128 thread counts at the
  fixed 64 B common-case size, reports total throughput,
  per-thread throughput, and the scalability ratio anchored on
  the 1-thread number. Each per-thread-count run barriers the
  workers in, runs the alloc / write / free loop for the shared
  duration, barriers them out, sums per-thread iters. The thread
  sweep is capped to `min(nproc, 32)` by default so small CI
  boxes do not spend wall time thrashing 128 contended threads;
  `V8M_BENCH_MAX_THREADS=128` opts back into the spec range.
  Knobs (env vars): `V8M_BENCH_DURATION_MS` (default 1000),
  `V8M_BENCH_WARMUP_MS` (default 100), `V8M_BENCH_SIZE` (default
  64), `V8M_BENCH_MAX_THREADS`. Reuses the `v8malloc_add_bench`
  scaffolding from MB-01; CMakeLists explicitly links pthread.

- Library project layout (`include/`, `src/`, `tests/`, `bench/`,
  `cmake/`, `man/`).
- CMake build producing both shared and static libraries with semver
  alignment and a single `V8MALLOC_1.0` linker version script.
- `pkg-config` (`v8malloc.pc`) and CMake (`v8mallocConfig.cmake`)
  package descriptors.
- Public version API: `v8m_version`, `v8m_version_major`,
  `v8m_version_minor`, `v8m_version_patch`, plus matching compile-time
  macros.
- Size-class machinery: branchless `v8m_size_class()` mapping an
  allocation request to one of 41 size classes (Tiny / Small / Medium
  / Large) or the `V8M_CLASS_HUGE` sentinel, plus the pinned
  `v8m_class_to_size[]` reverse-lookup table. Round-trip-verified
  exhaustively over every size in [0, 2 MiB].
- Architecture-abstraction-layer skeleton (`src/v8m_arch.h`): per-arch
  detection gate, `V8M_CACHE_LINE_SIZE`, alignment / branch-hint /
  visibility macros. Rejects non-Linux and unsupported architectures
  at compile time.
- Page-metadata machinery: `v8m_page_meta` and `v8m_tiny_page_meta`
  headers with pinned common-prefix offsets, the `V8M_MAGIC` sentinel
  (the ASCII bytes of "v8malloc"), branchless `v8m_ptr_to_meta()` /
  `v8m_page_meta_valid()`, and static asserts on struct sizes and
  page-size power-of-two invariants. Exhaustive `test_page_meta`
  walks every byte offset in a pair of 64 KiB pages to verify the
  reverse-lookup contract.
- Bootstrap allocator (`v8m_bootstrap_alloc`, `v8m_ptr_is_bootstrap`):
  lock-free bump-pointer allocator over a 64 KiB page-aligned static
  buffer, satisfying `malloc()` calls that arrive before the main
  allocator has finished initializing. Pointers are 16-byte aligned
  to match `max_align_t`; aborts on buffer exhaustion by design. The
  range-check predicate is the first gate the free-path will consult
  so pre-init pointers are recognized before the page-metadata
  machinery runs. Test suite covers sequential ordering, alignment,
  foreign-pointer rejection, and an 8-thread × 32-allocation race
  that verifies no two allocations overlap.
- MPSC lock-free queue for cross-thread frees
  (`v8m_mpsc_init/push/drain`): single-CAS-loop push from any
  thread, single-`atomic_exchange` drain on the owner thread,
  release/acquire ordering only — no `seq_cst` on the hot path.
  Intrusive: freed objects reuse their first word as the queue link.
  Verified by a stress test with 8 producer threads × 1024 pushes
  each running concurrently with a draining consumer; every node is
  observed exactly once and the queue ends up empty.
- Page heap (L4) primitives: `v8m_page_heap_alloc` reserves
  mmap-backed virtual regions of arbitrary power-of-two alignment
  (≥ 64 KiB) using the over-allocate-and-trim pattern; companion
  `v8m_page_heap_free` and `v8m_page_heap_advise_dont_need` wrap
  munmap and `madvise(MADV_DONTNEED)`. Lifetime counters
  (mmap/munmap/advise calls, bytes mapped/unmapped) are exposed via
  `v8m_page_heap_get_stats`. Region tracking for foreign-pointer
  detection is deferred; it lands in a dedicated cycle alongside
  the init/fini machinery.
- Tiny slab page (`v8m_slab_tiny_init/alloc/free`): bitmap-managed
  layout for size classes 0..7 (8 B–64 B objects), with a fixed
  2 KiB header reservation so the data area is aligned to the
  largest Tiny object size and the allocator hot path needs no
  bounds check. `alloc` is O(1) amortized via `search_hint` +
  `__builtin_ctzll`; `free` is O(1) direct bit-clear. Slots beyond
  page capacity are pre-marked used so the scan ignores them.
  `is_empty` / `is_full` read the atomic `used_count` so remote
  observers can pick empty pages without synchronizing with the
  owner thread.
- Small slab page (`v8m_slab_small_init/alloc/free`): intrusive
  free-list layout for size classes 8..31 (80 B – 4 KiB objects).
  Free slots thread a singly-linked list through their own first
  `sizeof(void *)` bytes; alloc pops from the head, free pushes to
  the head, both O(1) with no scan. `v8m_slab_small_data_offset`
  bumps the data area up to `object_size` for classes whose object
  exceeds the 2 KiB header reservation (today only class 31, 4 KiB)
  so returned pointers honour the class's natural alignment.
  Tiny and Small share a unified `V8M_SLAB_HEADER_SIZE` constant in
  `v8m_internal.h`.
- Direct-mmap path for Large (256 KiB – 2 MiB) and Huge (> 2 MiB)
  allocations (`v8m_large_alloc/free/usable_size`): one page-heap
  region per request, with a `v8m_large_page_meta` header at offset
  0 (extends `v8m_page_meta` with `mmap_size`) and the user's
  pointer returned at offset `V8M_SLAB_HEADER_SIZE` so the shared
  ptr-to-meta mask recovers the header. Huge requests keep a
  `UINT16_MAX` sentinel in the `size_class` field so a later cycle's
  `MAP_HUGETLB` optimization can specialize without breaking the
  common-prefix layout.
- Buddy allocator for Medium-class allocations
  (`v8m_buddy_init/alloc/free`): 7-level buddy spanning 4 KiB to
  256 KiB over a single 256 KiB-aligned arena. Free blocks are
  threaded into per-level doubly-linked lists by overlaying
  `v8m_buddy_node` (next/prev) on the block's own first 16 bytes;
  per-level `alloc_bitmap` and `split_bitmap` each fit in a single
  `uint64_t` (level 0 has at most 64 blocks). Allocation rounds the
  request up to the next level, walks upward to the smallest
  available level, then splits down placing each right child on
  the lower level's free list. Free does immediate coalescing via
  `idx ^ 1` whenever the buddy is free and not split.
- Runtime configuration (`v8m_config_init/get/set`): one atomic
  int64 per option, seeded from the V8M_* environment variables
  documented in AGENT.md §7 with documented defaults (verbose 0,
  purge interval 10 s, thread-cache max 256, HugePages 1, NUMA
  aware 1, debug 0, profile 0, compaction threshold 25 %).
  v8m_config_init re-reads the environment on every call so tests
  can reset state, and out-of-range option ids fail the
  set/get bounds check rather than scribbling on memory. Test
  suite covers defaults, env overrides for every option, fallback
  to default on unparseable env values, set/get round-trip, and
  the out-of-range guard.
- MB-01 single-thread throughput benchmark
  (`bench/mb_01_throughput.c`, benchmarks.md §3.1) and the
  surrounding `bench/` scaffolding. The benchmark sweeps eight
  sizes spanning every backend (8 B / 64 B / 512 B / 4 KiB slab,
  16 KiB / 64 KiB / 256 KiB buddy, 2 MiB Huge mmap), with a
  per-size timed loop measuring alloc/write/free throughput.
  Output is space-separated columns (size_bytes / iters /
  elapsed_us / ns_per_op / ops_per_sec) so cross-allocator
  comparison runs (jemalloc / mimalloc / tcmalloc / glibc) ingest
  cleanly into a spreadsheet or `diff`. Knobs come from env vars
  (`V8M_BENCH_DURATION_MS`, `V8M_BENCH_WARMUP_MS`) so LD_PRELOAD
  comparison runs need no recompile. New `bench/CMakeLists.txt`
  is gated on the existing `V8MALLOC_BUILD_BENCH` option;
  `v8malloc_add_bench` is the helper future MB-XX files use.

- Coverage line-coverage gate. `make coverage` now parses the
  lcov summary after building the HTML report and **fails** when
  line coverage drops below `COVERAGE_MIN` (85% by default;
  override on the command line). Current coverage is 86.5%, so
  the gate is live. Without lcov / genhtml installed the gate is
  silently skipped and the raw `.gcda` data is still available
  for IDE tooling.

- Process-exit destructor no longer unmaps. The previous
  destructor at priority 101 called `v8m_dispatch_destroy`,
  which unmapped every page-heap region — including buddy
  arenas that held malloc'd allocations still owned by glibc's
  stdio (e.g. the stdout buffer). When `_IO_cleanup`'s atexit
  handler ran afterwards to flush stdio, the write hit an
  unmapped page and produced silent output loss. The destructor
  now only swaps the init state; the OS reclaims our mappings
  on process exit, which is harmless in the LD_PRELOAD /
  static-link case. (Caught by the bench landing in this same
  cycle — `mb_01_throughput`'s output was disappearing
  entirely.) The dlopen/dlclose case where a process keeps
  running after our unload remains v0-unsupported; the
  atexit-based teardown that defers munmaps until after stdio
  cleanup lands in a follow-up cycle.

- GitHub Actions CI workflow (`.github/workflows/ci.yml`). Three
  parallel jobs gate every PR and push to `main`: matrix build +
  ctest under both gcc and clang, UBSan build + ctest under
  clang, and the format-check / tidy / cppcheck linters. Multi-
  arch coverage stays in a separate weekly workflow (TODO) so
  PR turnaround stays under five minutes.

- Release pipeline script (`scripts/release.sh`). Run from a
  checked-out `vMAJOR.MINOR.PATCH` tagged commit; the script
  asserts the working tree is clean, the tag matches
  `V8M_VERSION_STRING` in the public header, builds the release
  tree + runs the full ctest suite, then produces a `git archive`
  source tarball with sha256 (and detached GPG signature when a
  key is available). `--gh-release` opens a draft GitHub release
  with the matching CHANGELOG section as the body and the
  artifacts attached; the draft never auto-publishes — a
  maintainer always inspects before flipping visibility.
  `--skip-tests` shortens iteration on the script itself.
  README §Cutting a release documents the workflow.

- MAP_HUGETLB primary attempt for Huge allocations
  (huge-pages.md §4.1). `v8m_page_heap_alloc` now tries
  `mmap(MAP_HUGETLB)` first when the request is shaped for it
  (size is a 2 MiB multiple, alignment is at least 2 MiB,
  `V8M_OPT_HUGE_PAGES` allows it); on success the kernel returns
  a 2 MiB-aligned region backed by reserved huge pages with no
  over-allocate-and-trim needed. On failure (no reserved huge
  pages — typical in containers and CI) the allocator falls
  through to the existing mmap + MADV_HUGEPAGE path, which
  remains the supported fallback.

  `v8m_large_alloc` bumps the page-heap alignment to 2 MiB and
  rounds the mmap_size to the same multiple for Huge requests
  (size > V8M_LARGE_MAX_SIZE) so they're shaped for the primary
  attempt. The aligned variant (`v8m_large_alloc_aligned`) keeps
  the v0 V8M_PAGE_SIZE alignment so callers that asked for a
  specific user alignment aren't silently over-aligned.

  Two new counters land on `struct v8m_page_heap_stats`:
  `hugetlb_alloc_calls` (every attempt) and
  `hugetlb_alloc_failures` (the subset that fell back). A new
  `check_hugetlb_attempt` test in `test_page_heap` verifies the
  calls counter advances at-threshold and stays put both
  sub-threshold and when `V8M_OPT_HUGE_PAGES=0`.

- C++ Itanium-ABI mangled aliases for the non-throwing operator
  new / delete variants. Sixteen new exports under V8MALLOC_1.0
  cover sized delete (C++14), aligned new/delete (C++17),
  nothrow new/delete (`std::nothrow_t` overloads), and the
  combined sized+aligned and nothrow+aligned deletes:

      _ZdlPv  _ZdaPv  _ZdlPvm  _ZdaPvm
      _ZdlPvSt11align_val_t  _ZdaPvSt11align_val_t
      _ZdlPvmSt11align_val_t _ZdaPvmSt11align_val_t
      _ZnwmRKSt9nothrow_t    _ZnamRKSt9nothrow_t
      _ZdlPvRKSt9nothrow_t   _ZdaPvRKSt9nothrow_t
      _ZnwmSt11align_val_tRKSt9nothrow_t
      _ZnamSt11align_val_tRKSt9nothrow_t
      _ZdlPvSt11align_val_tRKSt9nothrow_t
      _ZdaPvSt11align_val_tRKSt9nothrow_t

  All implemented as thin wrappers around `v8m_free` /
  `v8m_malloc` / `v8m_aligned_alloc` (the void-pointer-only
  delete forms use `__attribute__((alias("v8m_free")))` directly).
  The throwing forms (`_Znwm`, `_Znam`,
  `_ZnwmSt11align_val_t`, `_ZnamSt11align_val_t`) intentionally
  stay in libstdc++ — they need to construct `std::bad_alloc` on
  failure and that requires the C++ runtime; libstdc++'s
  shipping implementations call `malloc` internally and pick up
  v8malloc transparently through the LD_PRELOAD chain.

- Memory-pressure stress test (`tests/test_memory_pressure.c`,
  benchmarks.md §4 ST-02). Five-phase scenario: build a working
  set of Large-class allocations, plant a soft limit at the
  current `live_bytes` (zero headroom), assert the next sizeable
  allocation fails with `errno = ENOMEM`, free part of the set
  and confirm the same allocation succeeds, install an OOM
  handler that releases a working-set slot on demand and verify
  the retry path lets the request through, then drain everything
  and confirm `live_bytes` returns within 1 MiB of the
  pre-test baseline. Working-set slots use the Large direct-mmap
  path (> 256 KiB) so freeing one slot drops `live_bytes`
  immediately — buddy-pool sizes wouldn't release until the
  whole arena drained, which would defeat the recovery check.

- libFuzzer driver (`tests/fuzz_alloc.c` +
  `V8MALLOC_BUILD_FUZZ`). Each `LLVMFuzzerTestOneInput` call
  interprets the input bytes as an instruction stream over a
  table of 32 live pointer slots; supported ops cover every
  backend-routed entry — `malloc`, `free`, `realloc`, `calloc`,
  `aligned_alloc` — with sizes derived from the input so the
  fuzzer naturally explores every size class, the buddy boundary,
  the Large/Huge cutoff, and the alignment guard rails. Per-slot
  byte patterns are stamped after every alloc / realloc and
  re-verified before any subsequent op on the same slot, so a
  use-after-free or two-slot aliasing bug surfaces as a pattern
  mismatch. State persists across invocations to deepen the
  search space.

  New `make fuzz` target rebuilds with clang + UBSan + libFuzzer
  and runs a 60-second smoke (`FUZZ_TIME=600 make fuzz` for
  longer campaigns). Initial 30 008-iteration smoke run completed
  in 31 seconds with no crashes and no UB. Requires
  `libclang-rt-N-dev` for the libFuzzer runtime — gcc is rejected
  with a fatal CMake error since libFuzzer is clang-only.

  README §Fuzzing documents the workflow. fuzz_alloc.c is excluded
  from the dev tidy run (it only compiles with `-fsanitize=fuzzer`
  so it's absent from the dev compile_commands.json); format /
  format-check still cover it.

- Distance-ordered NUMA fallback (numa.md §5.2). The library
  constructor now also reads
  `/sys/devices/system/node/nodeN/distance` for every detected
  node, populates a `g_distance[from][to]` SLIT matrix
  (`uint8_t`, clamped at 255), and computes a
  `g_fallback[from][rank]` table sorted by ascending distance via
  insertion sort (cheap with the 64-node cap). Two new exports —
  `v8m_numa_node_distance(from, to)` and
  `v8m_numa_fallback_node(from, rank)` — let allocator paths pick
  the cheapest cross-node target when the local node runs out of
  capacity. `rank == 0` is always the source node itself; ranks
  past the end saturate back to the source so callers can walk
  `0..node_count` without bounds checks. Missing distance rows
  (kernels without ACPI SLIT or sysfs-distance support) leave
  `g_distance` zero, which collapses the fallback order to
  identity — a safe degradation. test_numa walks every node × rank
  combination and asserts the fallback distances are non-decreasing.

- Per-thread cached `v8m_numa_current_node` with 1-in-1024
  refresh (numa.md §2.2). Two new `__thread` slots cache the
  resolved node id and a call counter; only every 1024-th call
  pays for `sched_getcpu` plus the cpu→node lookup. The interval
  is a power of two so the refresh check collapses to a single
  AND. Picks up thread migrations within a few microseconds while
  amortizing the syscall to effectively free on the allocator hot
  path. test_numa hammers the cached path with 4096 iterations
  (four refresh cycles) to exercise both branches.

- Coverage build variant. New `V8MALLOC_BUILD_COVERAGE` CMake
  option adds `--coverage -O0 -g` to compile and link, mutually
  exclusive with the sanitizers. New `coverage` CMake preset
  configures `build/coverage`, and `make coverage` runs the
  preset, executes the suite, and post-processes `.gcda` files
  through `lcov` + `genhtml` into
  `build/coverage/html/index.html` when those tools are
  installed. Without lcov / genhtml the raw gcov data still
  lives under `build/coverage/` for IDE tooling. README
  §Coverage documents the workflow.

- Sanitizer build variants. New `V8MALLOC_BUILD_UBSAN` and
  `V8MALLOC_BUILD_TSAN` CMake options propagate `-fsanitize=...`
  to both library and tests. UBSan is the supported configuration
  (full ctest passes with `-fno-sanitize-recover=undefined`); TSan
  builds but fails the test suite due to its shadow-memory scheme
  clashing with our early constructor mmaps and is documented as
  experimental in README. AddressSanitizer is intentionally
  unsupported — its malloc interceptor takes precedence at load
  time, so an ASan-instrumented binary bypasses v8malloc entirely.
  README §Sanitizers explains the tradeoffs.

  Bringing UBSan up shook out two bugs:
  * The bootstrap allocator's 64 KiB BSS buffer overflowed during
    UBSan's runtime init (its constructor chain runs ahead of
    ours). Bumped to 256 KiB so sanitizer runtimes fit alongside
    the existing dlsym / pthread_atfork constructor-chain
    allocations.
  * test_api's `posix_memalign(NULL, ...)` check tripped UBSan's
    nonnull-attribute runtime check (the volatile-pointer trick
    only dodges the compile-time diagnostic). Switched the test
    to call `v8m_posix_memalign` directly — our function doesn't
    carry glibc's `__nonnull` attribute, so UBSan no longer
    flags the deliberate NULL pass.

- Thread-churn stress test (`tests/test_thread_churn.c`,
  benchmarks.md §4 ST-04). 100 batches × 100 threads = 10 000
  thread creations total, each worker doing a short
  allocate / free run across every backend. After the run the
  test snapshots `v8m_get_stats` and asserts `live_regions` did
  not grow beyond a tight cap (32) — a pool bug that leaked one
  page per thread would surface as a 10 000-region increase. Runs
  in ~5 seconds.

- Rounded out the public v8m_* API surface. Six new exports land
  under V8MALLOC_1.0:

    - `v8m_get_huge_stats(struct v8m_huge_stats *)` reports
      per-class alloc/free counts and bytes_in_use for the Large
      (256 KiB – 2 MiB) and Huge (> 2 MiB) direct-mmap paths. The
      counters are maintained by new atomic state in
      `src/v8m_large.c` and surfaced through
      `v8m_large_get_stats`.
    - `v8m_get_thread_stats(struct v8m_thread_stats *)` locks the
      per-thread contract (fast_path_allocs, slow_path_allocs,
      fast_path_frees, remote_frees_received,
      bin_overflow_flushes). v0 returns zeros — the `__thread`
      counters wire in with the thread cache cycle; the surface
      lands now so consumers can compile against the final shape.
    - `v8m_get_frag_metrics(struct v8m_frag_metrics *)` reports
      live regions, live bytes, average bytes/region, region-map
      utilization %, and Large/Huge live counts. Per-class slab
      utilization joins the struct once the slab pool exposes the
      walked counters.
    - `v8m_purge(void)` and `v8m_purge_thread(void)` no-op
      returning 0. The slab and buddy pools already reclaim
      empty pages eagerly on free, so there's nothing
      synchronous to do in v0; the public hook locks the
      contract for the future bg purge thread.

  Plus six v8m_-namespaced wrappers for the glibc-compat
  extensions (`v8m_mallinfo`, `v8m_mallinfo2`, `v8m_malloc_stats`,
  `v8m_malloc_info`, `v8m_mallopt`, `v8m_malloc_trim`) so programs
  that link side-by-side with another allocator can call into
  v8malloc explicitly even when the unprefixed names resolve
  elsewhere. `v8m_malloc_info` takes `void *` for the FILE
  argument so the public header doesn't have to pull in
  `<stdio.h>`.

  New `struct v8m_huge_stats`, `struct v8m_thread_stats`, and
  `struct v8m_frag_metrics` live in the public header.
  `struct v8m_live_stats` / `v8m_collect_live_stats` move to the
  top of `src/v8m_api.c` so the new metric reporters can reach
  them. `dispatch_ready` gets a forward declaration so the
  collector can consult it without reordering the lifecycle
  helpers.

- Resolved every remaining open question in TODO.md:
    - **#2 Bootstrap handoff:** bootstrap allocations leak for
      the process lifetime by design (the 64 KiB buffer caps the
      leak; per-pointer free would defeat the bump allocator).
    - **#4 mallopt vs env vars:** env vars win. Our mallopt is a
      no-op; `v8m_set_option` takes runtime effect without
      persistence.
    - **#5 Profile-mode format:** pprof, dumped on process exit
      to `/tmp/v8malloc-PID.pb.gz` (overridable via
      `V8M_PROFILE_PATH`). Implementation lands with the profile
      mode itself.
    - **#6 Symbol-versioning granularity:** single
      `V8MALLOC_1.0` node, mirroring jemalloc / mimalloc /
      tcmalloc. New nodes only on major releases.
    - **#8 malloc_get_state / malloc_set_state:** skip — modern
      glibc (≥ 2.34) no longer declares or exports them.

- LD_PRELOAD interposition test (`tests/preload_target.c` +
  `test_ld_preload`). The target is a standalone program with no
  compile-time dependency on libv8malloc; CMake injects the
  shared library via `LD_PRELOAD=$<TARGET_FILE:v8malloc_shared>`
  in the test's environment. The target then verifies (a)
  `v8m_version` resolves through `dlsym(RTLD_DEFAULT, ...)` —
  proving our library was actually loaded — and (b) malloc /
  realloc / free across every backend size still round-trip
  correctly. Without the dlsym check, the malloc workload would
  silently exercise libc and the test would pass without proving
  anything.

- End-to-end concurrency test through the public allocation API
  (`tests/test_threading.c`). 8 worker threads × 512 ops each
  cycle through 11 sizes spanning every backend (slab Tiny / slab
  Small / buddy Medium / large mmap / huge mmap), stamp a
  per-thread byte pattern across the allocation, and verify it
  before freeing — inter-thread corruption (two threads receiving
  the same pointer) would surface as a pattern mismatch on the
  very first verify. Every 8th op detours through `realloc` and
  confirms the original bytes survive the move. Complements the
  per-pool stress tests (`test_slab_pool`, `test_buddy_pool`,
  `test_dispatch`) by exercising the constructor-installed
  dispatcher and the same path LD_PRELOAD users hit.

- Page-utilization-aware allocation pick. The slab pool's
  `try_partials` no longer pops the LIFO head; it scans the
  partials list and promotes the most-utilized page (the one
  with the highest `used_count`). Concentrating new allocations
  on near-full pages lets less-utilized pages drain back to
  empty (and the page heap) faster, the optimization called for
  in fragmentation.md §4.3 as `v8m_select_allocation_page`.
  Linear scan in v0; the future per-class priority queue or
  utilization-bucketed list keeps the cost bounded once the
  partials count grows large. New `check_partials_pick_most_utilized`
  in `test_slab_pool` sets up two partial pages (12 used vs 10
  used) with the less-utilized one at the LIFO head and verifies
  the next allocation lands in the more-utilized page.

- Pointer introspection: `v8m_is_valid_ptr` and
  `v8m_ptr_info(ptr, &out)`. Both reuse the existing region map +
  page-meta + buddy machinery, so no new tracking — they just
  surface what the dispatcher already knows. The new
  `enum v8m_ptr_backend` (FOREIGN / BOOTSTRAP / SLAB / BUDDY /
  LARGE) and `struct v8m_ptr_info` (backend, usable_size,
  size_class) live in the public header. `v8m_ptr_info` returns
  -1/EINVAL for NULL `out`, NULL `ptr`, foreign pointers, and
  mid-allocation pointers; on failure `*out` is left in a
  defined zero state with `backend = V8M_PTR_FOREIGN` so callers
  can branch on the field without checking the return code.
  Mid-allocation strictness on the buddy path is enforced by an
  alignment check (every buddy block is naturally `block_size`-
  aligned within its V8M_BUDDY_MAX_BLOCK-aligned arena, so
  `((uintptr_t)ptr & (size - 1U)) == 0` is exact).
  test_api covers all four backends plus invalid-input paths.

- Transparent huge page hint for Large/Huge regions. Every
  `v8m_page_heap_alloc` of >= 2 MiB now emits
  `madvise(MADV_HUGEPAGE)` after the mmap+trim, asking the kernel
  to back the region with one or more 2 MiB transparent huge
  pages. The hint is best-effort — if THP is disabled
  system-wide, the kernel ignores it and the allocation falls
  back to ordinary 4 KiB pages without any allocator-side
  difference. Honors `V8M_OPT_HUGE_PAGES`: setting it to 0
  suppresses the hint while keeping the allocation correct. The
  page-heap stats grow a `hugepage_advise_calls` counter so
  callers can verify the optimization is firing.

- Open question #7 (fallback policy when `MAP_HUGETLB` fails)
  resolved: honor `V8M_OPT_HUGE_PAGES`. The env-var contract
  acts as the single opt-out for both the future MAP_HUGETLB
  attempt and the MADV_HUGEPAGE hint emitted on the fallback
  path; the eventual MAP_HUGETLB attempt will share the same
  gate.

- Failure-path hooks: `v8m_set_oom_handler`,
  `v8m_set_soft_limit`, `v8m_get_soft_limit`. The OOM handler is
  invoked from `v8m_malloc` whenever the dispatcher returns NULL
  or the soft limit blocks the request; if the handler returns
  non-zero, malloc retries the allocation once. Per-thread
  reentrancy guard (`__thread bool t_oom_in_handler`) keeps a
  sub-allocation made by the handler that itself fails from
  recursing back into the callback. The soft limit caps
  `bytes_mapped - bytes_unmapped` (matches the `live_bytes` field
  of `struct v8m_stats`); allocations that would push past the
  limit fail with NULL / `errno = ENOMEM` after consulting the
  handler. Pass 0 to disable. Both the handler pointer and the
  limit value live in atomics so cross-thread set/install is
  well-defined. Tests cover: setter round-trip, soft limit
  blocking a Large allocation, handler-driven retry succeeding
  after the handler frees an anchored allocation, and the
  no-retry path correctly setting `errno`.

- Open question #3 (soft-limit enforcement policy) resolved:
  the OOM handler runs first; if it returns non-zero the malloc
  retries once. No allocator-driven aggressive purge runs at
  this point — the handler is the single user-controllable
  release point, and the future background purge thread will
  shape the live byte total independently rather than racing the
  failing path.

- Public configuration & statistics API. New `v8m_set_option` /
  `v8m_get_option` thin-forward to `v8m_config_set/get`; new
  `v8m_get_stats(struct v8m_stats *)` exposes the page-heap
  counters and live-region count via the public header without
  the caller having to go through the glibc-deprecated
  `mallinfo` path. `v8m_dump_stats` is the namespaced sibling of
  `malloc_stats`. The `enum v8m_option` (V8M_OPT_VERBOSE,
  V8M_OPT_PURGE_INTERVAL, …) moves to the public header as the
  canonical definition; the internal `src/v8m_config.h`
  re-includes it instead of redefining, so internal code keeps
  the same V8M_OPT_* names with no duplication. Out-of-range
  option ids return -1 with `errno = EINVAL`; pre-init calls to
  the setter / getter return -1 with `errno = EAGAIN`. All four
  symbols ship in the V8MALLOC_1.0 linker version node. Tests
  in `test_api` cover the round-trip, error paths, NULL out
  pointer, and that `v8m_get_stats.live_regions` advances after
  a Large allocation.

- NUMA topology detection (`v8m_numa_init`,
  `v8m_numa_node_count`, `v8m_numa_node_for_cpu`,
  `v8m_numa_current_node`). The library constructor scans
  `/sys/devices/system/node/nodeN/cpulist` once and populates a
  static cpu→node table; readers are lock-free constant-time
  array lookups, with `v8m_numa_current_node` wrapping
  `sched_getcpu()` and the cached map. The vDSO fast-path /
  refresh-on-N-th-call optimization called for in
  `.claude/docs/numa.md` §2.2 lands later. When sysfs is
  absent (containers, NUMA disabled in the kernel) the module
  reports a single uniform node so every caller follows the
  non-NUMA code path. Caps: 64 nodes, 4096 CPUs (well above any
  current Linux box). New `test_numa` validates idempotent init,
  out-of-range fallbacks, and the invariant that every cpu→node
  result is < node count. Per-NUMA pool sharding and
  `mbind(MPOL_BIND, …)` build on top of this in a follow-on
  cycle.

- glibc statistics & tuning extensions: `mallinfo`, `mallinfo2`,
  `mallopt`, `malloc_stats`, `malloc_info`, and `malloc_trim`.
  All routed through a shared `v8m_collect_live_stats` snapshot of
  the page heap so the same counters back every reporter.
  `mallinfo` / `mallinfo2` populate `hblks` from the new
  `v8m_page_heap_live_region_count` (the previous candidate of
  `mmap_calls - munmap_calls` is net-negative because the
  over-allocate-and-trim strategy emits multiple munmaps per
  mmap), `hblkhd` / `arena` / `uordblks` from
  `bytes_mapped - bytes_unmapped`. `malloc_stats` writes a
  human-readable digest to stderr; `malloc_info` emits a minimal
  v8malloc-tagged XML document; `mallopt` is an accepting no-op
  (returns 1) so legacy software that calls it unconditionally
  keeps working — runtime configuration lives behind the
  `V8M_*` env vars and `v8m_config_set/get`. `malloc_trim`
  honestly returns 0 (no chunk-top to release in v0). All six
  exit through the V8MALLOC_1.0 linker version node.

- Modern glibc (≥ 2.34) no longer declares `malloc_get_state` /
  `malloc_set_state`, so we skip them rather than ship aliases
  for symbols nothing imports. Documented in TODO.md.

- Page-heap region map (`v8m_page_heap_owns`). Every successful
  `v8m_page_heap_alloc` now records its returned range in a
  bounded array (cap: 4096 live regions; mutex-protected linear
  scan on lookup), and `v8m_page_heap_free` removes the matching
  entry via swap-remove. The new `v8m_page_heap_owns` predicate
  lets the dispatcher decide foreign-vs-ours on the free path
  without reading at the pointer's page-aligned base — the magic
  check that would follow faults on truly-foreign pointers whose
  base lies in an unmapped page. On region-table overflow the
  page-heap alloc rolls the mmap back and returns NULL, keeping
  the invariant that every live pointer is classifiable.

- Foreign-pointer free is safe again. `v8m_dispatch_free` now
  calls `v8m_page_heap_owns` first: if the pointer never came
  from our mmap, it is forwarded to the captured
  `v8m_libc_free`, resolving the crash risk that forced last
  cycle's silent-drop revert. The dispatcher's doc comment now
  reflects the three-step routing (owns → magic check → buddy).
  Tests that were held back last cycle re-enter the suite — the
  check for libc-allocated pointers fed through the overridden
  free() runs four size buckets that span libc's small/large-bin
  split without crashing.

- Open question #1 (region map representation) resolved in
  favor of a bounded array + linear scan for v0; upgrade to a
  radix tree deferred until live-region count or foreign-free
  rate make the O(N) cost visible.

- Fork safety via `pthread_atfork`. The library constructor now
  registers a triple of handlers that acquire the slab pool and
  buddy pool mutexes (in fixed slab→buddy order) before fork()
  and release them in reverse in both parent and child. Without
  the prepare handler, a forked child could inherit a pool mutex
  locked by a parent thread that didn't survive the syscall, and
  the child's first malloc would deadlock. The dispatcher exposes
  the helpers as `v8m_dispatch_prefork` /
  `v8m_dispatch_postfork_parent` / `v8m_dispatch_postfork_child`
  so the public-API layer doesn't reach into pool internals; the
  registration runs after `v8m_dispatch_init` succeeds and aborts
  if `pthread_atfork` itself fails. New `test_fork` spins a worker
  thread hammering malloc/free against the very mutexes the
  prepare handler must acquire, fork()s in the middle of that
  storm, drives an allocate/free cycle in both parent and child,
  and waits for clean exits.

- Foreign-pointer free routing reverted to a silent drop. The
  `v8m_libc_free` wiring added in the previous cycle requires a
  safe foreign-vs-ours decision before reading at the pointer's
  page-aligned base, and the magic-check that powers that
  decision can fault when the foreign pointer's base sits in an
  unmapped page. The libc-fallback module, the `__libc_*`
  aliases, and the dlsym capture all stand; only the
  `v8m_dispatch_free`-side forward is held back until the
  page-heap region map (TODO.md open question #1) lands.

- Libc fallback for foreign pointers
  (`v8m_libc_fallback_init/ready/free/malloc`): the library
  constructor now resolves the next free/malloc/calloc/realloc on
  the dynamic search path via `dlsym(RTLD_NEXT, ...)` before
  v8m_dispatch_init runs. The dispatcher's free path replaces the
  v0 silent-drop on foreign pointers with a forward to the
  captured libc free, so pre-init allocations made by other
  library constructors no longer leak. Resolution is safe to call
  from inside the constructor: any malloc dlsym performs internally
  hits our pre-init bootstrap allocator. Falls back to silent-drop
  only when RTLD_NEXT does not resolve (statically-linked-only
  case). Test exercises round-tripping a libc allocation through
  the v8malloc-overridden free across four size buckets, plus
  verifies the fallback was captured.

- glibc internal `__libc_malloc` / `__libc_free` / `__libc_calloc`
  / `__libc_realloc` / `__libc_memalign` / `__libc_valloc` /
  `__libc_pvalloc` aliases. Defined as `__attribute__((alias("v8m_*"))`
  on the public implementations so call sites inside libc that
  bypass the public symbols (and code linked against
  libc_nonshared.a) still hit v8malloc. Exported through the
  V8MALLOC_1.0 linker version node alongside the standard names.

- Aligned allocation family (`aligned_alloc`, `posix_memalign`,
  `memalign`, `valloc`, `pvalloc`) plus matching `v8m_*`
  variants. `v8m_dispatch_alloc_aligned` is the new entry
  point: requests with alignment ≤ 16 collapse to the regular
  malloc path; larger alignments either pick the smallest slab
  class whose object size is divisible by the alignment, route
  to the buddy pool (whose blocks are inherently power-of-two
  aligned), or fall through to the new `v8m_large_alloc_aligned`
  which widens the Large/Huge header offset to the requested
  alignment while keeping the meta page-base-recoverable. The
  supported alignment cap is V8M_BUDDY_MAX_BLOCK (256 KiB) for
  buddy-eligible sizes and V8M_PAGE_SIZE/2 (32 KiB) for
  Large/Huge sizes; requests beyond return NULL with errno set
  to EINVAL or ENOMEM. `v8m_large_usable_size` now recovers the
  header offset from the user pointer's low bits so the new
  variable-offset allocations report correct sizes. Test suite
  covers slab/buddy/large alignment paths, posix_memalign's
  `EINVAL` paths (zero / non-power-of-two / non-`sizeof(void *)`
  alignment, NULL memptr), the `*memptr` non-clobber on failure,
  and valloc/pvalloc page-rounding behaviour.

- Public POSIX allocation API (`malloc`, `free`, `calloc`,
  `realloc`, `reallocarray`, `malloc_usable_size`) plus the
  matching `v8m_`-prefixed variants. Both name groups route
  through the same v8m_dispatch instance; the standard names
  are exported through the V8MALLOC_1.0 linker version node so
  LD_PRELOAD substitution works. A library constructor at
  priority 101 runs `v8m_config_init` then `v8m_dispatch_init`
  on a global dispatcher; pre-init / post-shutdown allocations
  fall through to the bootstrap allocator, and bootstrap
  pointers survive the transition (free recognizes them via the
  range check and treats them as no-ops). calloc and reallocarray
  guard against the nmemb*size overflow with `errno = ENOMEM`,
  malloc_usable_size returns the backend's actual block byte
  size, and realloc copies `min(old_usable, new_size)` bytes
  before freeing the old allocation. aligned_alloc /
  posix_memalign / memalign / valloc / pvalloc need a custom-
  alignment path through the backends and ship in their own
  cycle. Test suite covers basic round-trip across slab/buddy/
  large size categories, calloc zeroing + overflow, the four
  realloc shapes (NULL, grow with data preservation, shrink,
  ptr+0), reallocarray overflow, and malloc_usable_size. The
  existing test_slab_*, test_page_meta, and test_buddy tests
  switched their fake-page allocators from libc's
  `aligned_alloc` to `v8m_page_heap_alloc/_free` so the malloc
  override no longer misroutes their fake-page frees through
  the dispatcher.

- Allocation dispatcher (`v8m_dispatch_init/destroy/alloc/free`):
  the first end-to-end allocator, composing the slab pool,
  buddy pool, and direct-mmap Large/Huge path behind one
  `alloc(size)` / `free(ptr)` interface. Allocation routes by
  size: classes 0..31 → slab pool, sizes ≤ 256 KiB but above
  the slab range → buddy pool, everything larger → direct mmap.
  Free uses the v8m_page_meta magic check at the page base to
  tell slab/large pages apart from buddy and foreign pointers;
  buddy allocations are recognized via the buddy pool's
  range-check ownership, foreign pointers are silently dropped
  for v0 (the libc fallback via `dlsym(RTLD_NEXT, "free")`
  lands with the public API / init cycle).
  v8m_buddy_pool_free's return value is now "true iff owned and
  freed" so the dispatcher can disambiguate ownership from
  reclamation; the buddy-pool tests pick up the reclamation
  signal from page-heap stats instead. Test suite covers
  init/destroy, round-trip alloc/free for representative sizes
  in every backend (Tiny, Small, Medium, Large, Huge), correct
  routing per size category (verified by inspecting the page
  metadata produced by each path), distinct addresses across 32
  mixed-size allocations, NULL/foreign-pointer tolerance, and
  an 8-thread × 64-op concurrent mixed-size stress.

- Buddy pool (`v8m_buddy_pool_init/destroy/alloc/free`) plus
  two helpers added to the buddy module: `v8m_buddy_block_size`
  recovers the level of a previously-allocated block by walking
  the alloc bitmaps (no per-allocation header needed), and
  `v8m_buddy_is_empty` reports whether any allocation remains.
  The pool keeps a fixed-cap array of 64 buddy arenas (16 MiB
  Medium-class capacity); alloc tries every in-use arena before
  lazily acquiring a fresh 256 KiB-aligned region from the page
  heap, and free locates the owning arena by bounds-checking
  the in-use slots, then reclaims the arena once it drains to
  empty. A single per-pool mutex serializes operations,
  matching the slab pool's structure. Test suite covers init/
  destroy, bounds (size 0, > 256 KiB, NULL, foreign pointer),
  single-allocation round-trip, multi-arena fill (verified by
  page-heap mmap_calls advancing), arena reclamation on full
  drain (verified by munmap_calls advancing), and an 8-thread
  × 64-op concurrency stress.

- Slab pool (`v8m_slab_pool_init/destroy/alloc/free`):
  per-class management of Tiny + Small slab pages. For each of
  the 32 slab classes (0..31) the pool holds a `current` page
  and a `partials` chain; the underlying tiny-vs-small slab is
  dispatched by `meta->size_class < V8M_SMALL_FIRST_CLASS`.
  Allocation tries the current page, falls through to partials,
  and finally acquires a fresh page from the page heap. Free
  reads `is_full` before the slab free so it can detect the
  full → partial transition and re-insert the recovered page;
  pages that drain to empty are returned to the page heap. A
  single per-pool mutex serializes operations — the lock-free
  thread-local cache lands on top of this in a future cycle.
  Test suite covers init/destroy, bounds, single alloc/free per
  class, multi-page allocation when one page exhausts, the
  full → partial recovery path (verified by page-heap stats
  staying flat after a recovered page is reused), and an
  8-thread × 256-op concurrency stress.
- OSS scaffolding: `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`,
  `SECURITY.md`, GitHub issue and pull-request templates,
  `man/v8malloc.3`.

[Unreleased]: https://github.com/iqbqioza/v8malloc/compare/HEAD...HEAD
