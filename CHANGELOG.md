# Changelog

All notable changes to v8malloc are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
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
