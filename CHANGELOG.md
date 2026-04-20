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
- OSS scaffolding: `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`,
  `SECURITY.md`, GitHub issue and pull-request templates,
  `man/v8malloc.3`.

[Unreleased]: https://github.com/iqbqioza/v8malloc/compare/HEAD...HEAD
