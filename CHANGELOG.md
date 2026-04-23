# Changelog

All notable changes to v8malloc are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed
- **Refactor: split self-contained diagnostic surface into
  `src/v8m_api_diag.c`.** The five exported diagnostics that
  don't touch the dispatcher singleton —
  `v8m_install_size_class_table`, `v8m_size_class_to_bytes`,
  `v8m_get_arch_info`, `v8m_option_name`,
  `v8m_estimate_lifetime`, `v8m_count_vmas` (and the static
  `api_arch_name` + `g_option_names` table) — move to a new TU.
  v8m_api.c shrinks 1462 → 1364 lines; the new TU is 160 lines.
  `g_dispatch`-coupled diagnostics (`v8m_get_stats`,
  `v8m_get_huge_stats`, `v8m_get_thread_stats`, the slab
  breakdown, lifetime stats, NUMA balance, frag metrics,
  ptr_info, validate_internal_state) stay in v8m_api.c — moving
  them would require exposing the dispatcher singleton via an
  accessor, more churn than payoff. Public ABI preserved; every
  symbol verified via `nm -D libv8malloc.so`.

- **Refactor: extract `pre_alloc_soft_limit_gate` +
  `post_alloc_record` helpers from `do_malloc_pc` +
  `do_aligned_alloc_pc`.** The previous cycle's bug fix
  (aligned-alloc silently bypassed `V8M_OPT_SOFT_LIMIT` and the
  OOM handler) was rooted in two near-identical functions that had
  drifted. Hoisting the shared shape into one helper for the
  pre-alloc gate (soft-limit check + handler retry) and one for
  the post-alloc bookkeeping (predict-prefetch update + lifetime
  record) means the malloc and aligned-alloc paths can no longer
  disagree on what the policy is. Behaviour preserved.

- **Refactor: bump-pointer primitive shared by bootstrap +
  signal-safe.** New `src/v8m_bump.{h,c}` ships a tiny atomic-bump
  module (`v8m_bump_alloc` / `_owns` / `_remaining`); the bootstrap
  allocator and the async-signal-safe emergency allocator are now
  thin policy wrappers around it (each owns the buffer + the
  out-of-budget contract — bootstrap aborts, signal-safe returns
  NULL). Hoists ~80 lines of duplication and prevents the next
  divergence (the SIZE_MAX overflow fix in the previous cycle had
  to land in two places because the same code was copied).

- **Refactor: extract adaptive THP advice from page_heap.c into
  `src/v8m_thp.{h,c}`.** The EMA / cold-threshold / promote-vs-
  demote decision moves to its own module; `v8m_page_heap.c`
  delegates via `v8m_thp_decide_and_record()` and reads the
  threshold via `v8m_thp_cold_threshold_ticks()` from the per-
  region age sweep. Counter accessors (`v8m_thp_promote_calls()`
  etc.) replace the prior atomic-load boilerplate at the snapshot
  call sites. The per-region age tracker (the `promoted_at_tsc`
  field on each region_entry) stays in page_heap.c because it
  belongs to the region map. Test-only injector moves with the
  rest: `v8m_page_heap_thp_test_inject` → `v8m_thp_test_inject`
  (only consumer is `tests/test_page_heap.c`).

- **Refactor: split `src/v8m_api.c` (1860 → 1459 lines) into three
  TUs.** New `src/v8m_api_libc_compat.c` carries the glibc
  reporters (`mallinfo` / `mallinfo2` / `malloc_stats` /
  `malloc_info` / `mallopt` / `malloc_trim`), the POSIX
  malloc-family overrides (`malloc` / `free` / `calloc` / ...),
  and the `__libc_*` glibc-internal aliases. New
  `src/v8m_api_cxx.c` carries the 16 C++ Itanium ABI
  operator-new/delete mangled names. New `src/v8m_api_internal.h`
  exposes the shared `v8m_api_collect_live_stats` snapshot helper
  and `v8m_api_dispatch_ready` predicate so the split TUs route
  through the same source of truth as the core. The
  `__attribute__((alias))` aliases that were within v8m_api.c
  become thin wrappers in the split TUs (alias requires same-TU
  definition); the public ABI is unchanged — every export still
  appears under V8MALLOC_1.0.

### Performance
- Hot-path `rdtsc` removed from `v8m_malloc` / `v8m_free` when
  `V8M_OPT_LIFETIME_TRACKING` is off (the default). The lifetime-
  tracker helpers used to take `tsc` as a parameter and the
  callers computed it via `v8m_arch_rdtsc()` unconditionally before
  the helper's own option check decided to drop the sample. Moved
  the TSC read inside the helpers, after the option + cache-presence
  gate, so the default hot path pays nothing — not even an `rdtsc`
  instruction.

- Slab-pool batch refill on TLC miss when the L2 core cache is
  empty. The cold-start path through `try_tlc_fast_paths` would
  return NULL on L2 underflow, which let the dispatcher fall
  through to `v8m_slab_pool_alloc` for one slot at a time. With
  this change, the slow path now batch-allocates `batch_size`
  slots from the slab pool under one lock acquisition (new
  `v8m_slab_pool_alloc_batch`) and installs them into the TLC bin,
  amortising the lock cost across the chain. Helps batch-alloc
  workloads where the free side never publishes into the L2.

### Fixed
- Buddy pool's deferred-coalesce fallback no longer skips
  drained arenas. With `V8M_OPT_DEFERRED_COALESCE=1`, an arena
  could become "drained" (alloc_bitmap fully clear, MADV_DONTNEED
  applied) while still holding scattered low-level free blocks
  in its bookkeeping; the fallback's `slot->drained` skip then
  prevented the only path that could merge those blocks back
  into a top-level block satisfying a subsequent max-size
  request. Fix: include drained slots in the coalesce sweep.

- `tests/test_page_heap.c::check_hugepage_advice` and
  `check_hugetlb_attempt` and `check_thp_adaptive_decision` now
  force `V8M_OPT_HUGE_PAGES=1` for the duration of their
  assertions and restore the saved value at every exit. Without
  this, the env-driven `V8M_HUGE_PAGES=0` case (operator runs
  the suite with huge pages off) silently flipped the test's
  expectation and assertions like "hugepage_advise_calls did not
  advance" spuriously failed. Mirrors the same pattern
  test_buddy_pool already used for V8M_OPT_DEBUG.

- Fork-safety hole closed across all module-level mutexes.
  Previous cycles covered the dispatcher-owned pool locks and
  the page-heap region / anchor mutexes; three more were still
  uncovered and caused deadlocks in the child: the thread-cache
  registry mutex (`g_registry_lock`), the bg-purge tick mutex
  (`g_lock`), and the DEBUG-mode double-free ring mutex
  (`g_double_free_ring_lock`). With `V8M_OPT_DEBUG=1`,
  `test_fork_stress` deadlocked on the ring lock inherited in
  held-by-dead-thread state; without DEBUG, the same deadlock was
  latent on the registry lock whenever a worker happened to be
  inside a TLC create / destroy when another thread forked. Fix:
  added `v8m_thread_cache_prefork` / `_postfork_parent` /
  `_postfork_child`, `v8m_bg_purge_prefork` / `_postfork_parent` /
  `_postfork_child`, and wired the double-free ring lock directly
  into the api's `v8m_atfork_*` handlers. `bg_purge_postfork_child`
  also resets `g_running = false` so a child that calls
  `v8m_bg_purge_shutdown` does not try to join a thread that was
  never spawned in the child process.

- DEBUG-mode double-free detector no longer false-positives on
  libc-owned pointers. `v8m_free` on a foreign pointer (one that
  libc owns, the dispatcher routes via `v8m_libc_free`) was
  recording the pointer in the ring just like any v8malloc-owned
  free. libc's own address-reuse pattern across distinct libc
  allocs then tripped the detector as soon as a libc alloc
  returned an address v8malloc had previously handed back. Fix:
  gate the debug check on `v8m_page_heap_owns(ptr)` so foreign
  pointers skip both the check and the ring insertion.

- DEBUG-mode double-free detector no longer false-positives on
  the legitimate alloc/free reuse pattern. The 4096-entry ring
  recorded every freed pointer but never cleared an entry when
  that address was subsequently re-allocated, so the lifecycle
  `free(P) → malloc returns P → free(P)` was misclassified as a
  double-free. With V8M_DEBUG=1, every realistic workload tripped
  the detector almost immediately. Fix: added
  `debug_clear_double_free_record(ptr)` invoked from
  `post_alloc_record` (single linear scan, only fires when DEBUG
  is on) so the next free of a fresh allocation is correctly
  unflagged.

- DEBUG-mode UAF poison no longer destroyed by the buddy pool's
  drained-arena MADV_DONTNEED. When an arena went empty, the
  buddy pool issued MADV_DONTNEED to release physical frames
  while keeping the VMA intact for cheap revival. The next
  fault-in returns zero pages — which the UAF verify on the
  revival alloc reads as `byte=0x00, expected=0xdf` and aborts.
  The slab pool's drained cache re-runs init (which re-poisons)
  on revival, so it was unaffected; the buddy pool only re-runs
  `v8m_buddy_alloc` (which only verifies, never re-poisons). Fix:
  skip the MADV when V8M_OPT_DEBUG is on. The DEBUG cost of
  holding RSS for idle arenas is acceptable — DEBUG mode is
  already paying for poison + verify everywhere else.
  `tests/test_buddy_pool.c::check_single_round_trip` updated to
  skip its `advise_calls` assertion when DEBUG is on so the
  V8M_DEBUG=1 env-driven case does not flip the expectation.

- Fork safety hole: the dispatcher's prefork/postfork hooks
  only locked the default slab pool and the buddy pool,
  silently leaving three lifetime arenas
  (`slab_lifetime[0..2]`), the page-heap region map
  (`g_region_lock`), and the lazy anchor reservation
  (`g_anchor.lock`) unguarded. With
  `V8M_OPT_LIFETIME_TRACKING` on, a thread mid-alloc on a
  lifetime arena when another thread `fork()`s would let the
  child inherit the lifetime arena's mutex in held-by-dead-
  thread state — the child then deadlocked on the first
  lifetime-routed allocation. Same shape applied to the
  region-map and anchor mutexes (held by any
  page-heap call during fork). Fix: extended
  `v8m_dispatch_prefork` to lock the lifetime arenas and to
  call new `v8m_page_heap_prefork` / `_postfork_parent` /
  `_postfork_child` hooks that own the page-heap-side mutex
  set; release order mirrors acquire order so the global
  acquire chain is dispatcher-pools → page-heap →
  anchor. Existing `tests/test_fork*.c` continue to pass.

- Aligned allocation paths (`v8m_aligned_alloc`,
  `v8m_posix_memalign`, `v8m_memalign`, `v8m_valloc`,
  `v8m_pvalloc`) now honour `V8M_OPT_SOFT_LIMIT` and the OOM
  handler retry, mirroring the malloc path. The previous
  `do_aligned_alloc_pc` implementation skipped both the
  pre-allocation soft-limit check and the post-allocation OOM
  handler retry, so a workload that mixed plain `malloc` with
  `posix_memalign` or `aligned_alloc` would have its limit
  silently bypassed by the aligned allocations and the OOM
  handler would never fire on aligned-alloc failures. Both
  paths now route through the same shape — `over_soft_limit` →
  optional handler retry → dispatch → OOM handler retry on
  NULL — so the policy applies uniformly.

- THP stats snapshot no longer triggers lazy-init of the cold
  threshold as a side effect. The previous refactor routed the
  `thp_cold_threshold_ticks` field of `v8m_page_heap_stats`
  through `v8m_thp_cold_threshold_ticks()`, which would lazy-
  init the threshold (mhz × 1e6) on first call — meaning a stats
  reader could silently materialise a value that no THP decision
  had actually consumed. Restored the original "0 means no
  decision made yet" semantics by adding a non-init
  `v8m_thp_cold_threshold_ticks_snapshot()` accessor and routing
  the page-heap stats snapshot through it.

- Bootstrap and signal-safe emergency allocators now reject
  near-`SIZE_MAX` requests up front instead of letting
  `(size + ALIGN-1) & ~(ALIGN-1)` wrap to a small `aligned`
  value. Without the guard, a request with
  `size > SIZE_MAX - V8M_BOOTSTRAP_ALIGN+1` would silently round
  to a 16-byte slot — the caller, believing they had `SIZE_MAX`
  bytes, would then write far past the slot and corrupt
  neighbouring bootstrap allocations (or, on the signal-safe
  path, neighbouring emergency-budget slots). Bootstrap aborts
  via the existing OOM path (process-fatal is the right
  behaviour for a near-`SIZE_MAX` ask before the real allocator
  exists); signal-safe returns NULL to honour its
  "handler must check the return" contract. Coverage in
  `tests/test_signal_safe.c::check_size_overflow_rejected`.

### Added
- Public `v8m_validate_internal_state()` debug helper. Walks the
  page-heap region map (sort + non-overlap), the buddy pool's
  arena array (drained-implies-in-use, in_use-implies-arena_base),
  and every slab pool's drained cache (count <= cap, no NULL
  entries below count) — including all four lifetime arenas.
  Returns the count of invariant violations detected; each
  violation prints a one-line diagnostic to stderr so a fuzzer
  that runs the validator after every operation can capture the
  offending state. Backed by three new internal helpers:
  `v8m_page_heap_validate`, `v8m_buddy_pool_validate`,
  `v8m_slab_pool_validate`. Pre-init returns 0. Exported under
  V8MALLOC_1.0; coverage in
  `tests/test_api.c::check_validate_internal_state_api` (rest +
  post-workload assert 0 issues).

### Added
- riscv64 `riscv_hwprobe()` ISA query for Zbb / Zacas detection
  (resolution of the riscv64 follow-on note in TODO.md §Phase 3).
  New `v8m_arch_has_zbb()` + `v8m_arch_has_zacas()` probe via the
  Linux 6.4+ `riscv_hwprobe` syscall, querying the
  `RISCV_HWPROBE_KEY_IMA_EXT_0` bitmask. Result cached in an
  atomic on first call so subsequent reads are lock-free; probe
  failure (older kernel, sandbox restriction) caches a sentinel
  so we don't retry the syscall on every call. Always false on
  non-riscv64 builds. The constants and struct layout are
  defined locally to avoid pulling `<asm/hwprobe.h>` (not in
  every distro's UAPI yet). The `v8malloc isa:` line under
  `V8M_VERBOSE` now includes `zbb=yes/no zacas=yes/no` fields;
  `struct v8m_arch_info` exposes the same as `has_zbb` /
  `has_zacas` byte flags (consuming two of the prior three
  reserved bytes). On non-riscv64 hosts both fields read 0.

### Added
- Public `v8m_get_slab_class_breakdown_lifetime(lifetime, out)` API.
  Same shape as `v8m_get_slab_class_breakdown` but reads from one
  of the four dispatcher arenas (default + EPHEMERAL/SHORT/LONG
  lifetime arenas), so an operator with `V8M_OPT_LIFETIME_TRACKING`
  on can see the per-arena utilization that the lifetime routing
  produces. `V8M_LIFETIME_UNKNOWN` routes to the default arena
  (matching `v8m_get_slab_class_breakdown` exactly); the three
  classified values route to their dedicated lifetime arena pools.
  Out-of-range values fall through to default. Backed by a new
  shared `api_fill_slab_breakdown` core so the
  raw-stats-to-public-struct conversion lives in one place.
  Exported under V8MALLOC_1.0; coverage in
  `tests/test_api.c::check_slab_class_breakdown_lifetime_api`
  (NULL tolerance, UNKNOWN-vs-default agreement, lifetime arena
  zeroes when tracking is off).

### Added
- Public `v8m_option_name(option_id)` API + companion config dump
  on `V8M_VERBOSE`. The accessor returns the stable short name
  for each `V8M_OPT_*` (e.g. "verbose", "huge_pages",
  "purge_interval"); combined with `v8m_get_option(out, id)`,
  callers can iterate `0..V8M_OPT_COUNT` to build a pretty-printed
  config dump without compiling against the internal name table.
  The constructor uses the same name format to emit a
  `v8malloc opts: verbose=1 purge_interval=10 …` line right after
  the `v8malloc isa:` line under `V8M_VERBOSE`. snprintf-into-stack
  emit so the diagnostic stays malloc-free. Multi-arch CI matrix
  lanes can grep both stable prefixes. Coverage in
  `tests/test_api.c::check_option_name_api` (out-of-range NULL,
  every in-range id resolves to non-empty, well-known stability
  spot-check).

### Added
- Public `v8m_init_thread()` / `v8m_release_thread()` API for
  explicit per-thread allocator state lifecycle. `v8m_init_thread`
  pre-warms the calling thread's TLC so latency-sensitive worker
  pools can fold the one-time first-alloc spike into a
  non-critical startup phase; idempotent. `v8m_release_thread`
  drains the TLC bins back to the slab pool, drops the calling
  thread's L2 contribution, and resets the TLS slot so the next
  allocation re-creates a fresh cache — useful for long-lived
  workers that go idle for an extended period (returns slots to
  the pool and lets the bg purge tick reclaim drained pages).
  Both return -1 with `errno = EAGAIN` when the dispatcher is
  not READY (pre-constructor); `v8m_init_thread` additionally
  returns -1 with `errno = ENOMEM` on cache alloc failure. New
  internal `v8m_thread_cache_release_local()` is the worker —
  mirrors the pthread_key destructor minus the sticky
  `t_in_destructor` latch so future allocations create a fresh
  cache. Both exported under V8MALLOC_1.0; coverage in
  `tests/test_api.c::check_init_release_thread_api` (idempotent
  init, alloc/free across release, re-init after release).

### Added
- Public `v8m_get_arch_info(out)` API — programmatic counterpart
  to the `v8malloc isa: …` line the constructor writes to stderr
  under `V8M_VERBOSE`. Fills `struct v8m_arch_info` with
  `arch_name` (string), `cache_line_bytes` (runtime probe),
  `build_cache_line_bytes` (compile-time `V8M_CACHE_LINE_SIZE`),
  `tsc_mhz`, and `has_lse` (AArch64 LSE bit). Comparing
  `cache_line_bytes` to `build_cache_line_bytes` surfaces a host
  that needs wider padding than the build assumed (Apple M1
  P-cores at 128 B vs the 64 B default). Exported under
  `V8MALLOC_1.0`. Coverage in `tests/test_api.c::check_arch_info_api`
  asserts the field shape (NUL-termination, power-of-two cache
  lines, non-zero TSC, in-bounds `has_lse`, zeroed reserved).

### Fixed
- CI failure on `test_guard_page` under UBSan. Two coupled issues
  surfaced once UBSan landed in the CI matrix: (a) UBSan
  intercepts `SIGSEGV` and aborts the process, so the child the
  guard test fork()s died with `SIGABRT` instead of `SIGSEGV` —
  fix sets `UBSAN_OPTIONS=handle_segv=0` (plus the matching
  `ASAN_OPTIONS` / `TSAN_OPTIONS`) via the CTest
  `ENVIRONMENT` property on `test_guard_page` and
  `test_double_free`, letting the kernel deliver the signal
  directly. (b) The slab/buddy UAF detector verify path
  (landed two cycles ago) ran under runtime DEBUG flips,
  which fired false-positive aborts on slab pages whose slots
  were initialized while DEBUG was off (the pre-poison was
  skipped, so the first verify after the flip saw mmap-zero
  bytes instead of poison). Fix latches the UAF detector at
  first call instead of reading `V8M_OPT_DEBUG` on every
  poison/verify — `V8M_DEBUG=1` in the env at process start
  enables the detector for the lifetime of the process,
  runtime `v8m_set_option(V8M_OPT_DEBUG, 1)` toggles do not.
  Other DEBUG features (double-free ring, guard pages, red
  zones) keep their runtime gating because they are not
  state-dependent across the flip.

### Added
- Persisted bench baseline regression check
  (`scripts/bench-regression-check.sh` — resolution of the
  ">5 % perf-regression gate" follow-on noted in the weekly
  bench workflow). Two modes: `--capture <bench> <baseline>`
  records a baseline file from a fresh bench run; the default
  `<bench> <baseline> [threshold_pct]` mode runs the bench,
  parses the rate column (`ops_per_sec`/`handoffs_per_sec`
  preferred, `ns_per_op`/`latency_us` fallback), and exits 1
  when any row regressed past the threshold (default 5 %, env
  override `BENCH_REGRESSION_THRESHOLD_PCT`). Mawk-compatible —
  no Python, no gawk extensions. Wired into
  `.github/workflows/bench-weekly.yml` after the MB-01 capture
  step (currently `continue-on-error: true` until the baseline
  has stabilised across a few weekly runs). New
  `bench/baseline/README.md` documents how maintainers establish
  and commit a baseline.

### Added
- Gzip-wrapped pprof emit (`v8m_pprof_dump_heap_gz`). The new
  helper writes the encoded Profile message inside an RFC 1952
  gzip stream — 10-byte header + STORED DEFLATE blocks
  (BTYPE=00, no compression) + 8-byte CRC32+ISIZE footer. The
  STORED-block strategy avoids pulling libz onto the malloc-free
  destructor path; pprof reads either form transparently. The
  destructor's default profile path is now `/tmp/v8malloc-PID.pb.gz`
  (matches the resolution of open question #5 verbatim);
  `$V8M_PROFILE_PATH` overrides. Path-based wrapper detects the
  `.gz` suffix and routes accordingly. `gunzip -t` validates the
  emitted file. Coverage in `tests/test_pprof.c::check_gzip_dump`
  asserts the gzip magic bytes (0x1f 0x8b), the inner stored
  block header, and the embedded sample_type tag at the expected
  offset.
- Runtime ISA summary on `V8M_VERBOSE`. The constructor now
  emits one line `v8malloc isa: arch=<name> cache_line=<size>
  lse=<yes|no> tsc_mhz=<freq> build_cache_line=<size>` to stderr
  right after publishing READY, surfacing which arch lane the
  runtime took plus the runtime-probed cache-line and TSC values.
  Resolution of the riscv64 follow-on note in TODO.md §Phase 3
  ("ISA-summary line earns its weight"). New helper
  `v8m_arch_format_isa_summary(buf, cap)` formats the line; same
  format runs on every arch so a multi-arch CI matrix lane can
  grep for the stable `v8malloc isa:` prefix. Coverage in
  `tests/test_arch.c::check_isa_summary`.

### Added
- Per-class slab utilization breakdown (fragmentation.md §4.1).
  New public `v8m_get_slab_class_breakdown(out)` exported under
  V8MALLOC_1.0 fills a buffer of `V8M_PUBLIC_NUM_SIZE_CLASSES`
  `struct v8m_slab_class_breakdown` entries with per-class
  pages_in_use / slots_total / slots_used / utilization_pct.
  Tiny/Small classes (0..31) carry actual data; Medium and above
  have no slab backing and read as zero. Backed by the new
  internal `v8m_slab_pool_get_class_stats` which walks the per-
  class current + partials lists under the pool lock and
  partitions the counters that the existing aggregate accessor
  rolled up. Surfaces the dominant utilization buckets that drive
  fragmentation decisions (the aggregate
  `slab_pages_in_use` already exists via `v8m_get_frag_metrics`;
  this is the per-bucket detail an operator chasing waste wants).

### Performance
- Use-after-free WRITE detector for buddy blocks in DEBUG mode.
  Extends the slab-side UAF poison pattern that landed last
  cycle to the buddy backend. `v8m_buddy_init` pre-poisons the
  arena body [16, V8M_BUDDY_MAX_BLOCK) so the first-alloc verify
  sees the expected pattern (kernel-delivered mmap-zero would
  otherwise falsely fire). `v8m_buddy_free_no_coalesce` poisons
  the freed block before list_push (which then overwrites the
  first sizeof(v8m_buddy_node) bytes with prev/next). The
  immediate-coalesce path's `coalesce_upward` poisons the entire
  merged block before the final list_push — the full-block stamp
  also covers the sibling's old prev/next bytes that would
  otherwise create a non-poison gap inside the merged block.
  `v8m_buddy_alloc` verifies [node+16, node+target_size) at the
  end, after the split-down loop, and aborts with a diagnostic
  naming "buddy" plus the offending offset on mismatch. Helpers
  are no-ops when DEBUG is off; release builds pay zero cost.

### Performance
- Sorted-array region map with binary-search ownership check
  (the spec's "future radix-tree replacement" optimization). The
  page heap's `g_regions` array is now kept sorted ascending by
  `start`; `v8m_page_heap_owns` runs a binary search for the
  largest region whose `start ≤ addr` and closes the verdict
  with one `addr < region->end` check — O(log N) vs the prior
  O(N) linear scan. Insert (`region_register_with_flags`) and
  unregister (`region_unregister_with_flags`) memmove the array
  tail to preserve the sort; the cost is O(N - idx) on alloc /
  free large but is well-amortized by the binary search the
  ownership-check fast path now enjoys (called on every free
  for the foreign-pointer detection branch). Two new helpers
  `region_lower_bound` and `region_find_by_start` centralize the
  search; `region_record_node` and `region_record_promote` route
  through `region_find_by_start` instead of their previous
  linear walks.
- NUMA active page migration via `move_pages()` (numa.md §6.2
  second action item). Extends `v8m_page_heap_numa_rebalance`:
  when the imbalance trigger fires AND a non-overloaded
  fallback exists, picks one region on the most-loaded node and
  invokes `move_pages()` to relocate up to 256 OS pages (≈ 1 MiB
  on x86_64) per tick. Larger regions get partial migration this
  tick and the rest next tick; the per-tick bound prevents a
  deeply imbalanced run from stalling the bg purge thread on a
  single tick's worth of migration syscalls. Combined with the
  alloc-time diversion (already wired in `bind_to_local_node`),
  the rebalance drains overload over a few ticks. New counter
  `v8m_page_heap_numa_migration_calls` tracks syscall count.
  Failed move_pages still counts (best-effort).
- TLC capacity shrink under NUMA overload (numa.md §6.2 third
  action item). When the calling thread's node is suppressed by
  the page-heap rebalance action, the TLC's GC tick halves
  `bin_capacity` (clamped to MIN) so the per-thread cache holds
  fewer slots — relieves the per-thread footprint that the
  rebalance is trying to address. Once the suppressed flag
  clears, the normal EMA-driven growth restores capacity. New
  public accessor `v8m_page_heap_node_is_suppressed(node)` is
  what the TLC reads on each tick.
- Use-after-free WRITE detector for slab pages in DEBUG mode.
  New `src/v8m_debug.{h,c}` ships `v8m_debug_uaf_poison` /
  `v8m_debug_uaf_verify` / `v8m_debug_uaf_poison_data_area`
  helpers; the slab tiny + small backends call them from their
  alloc / free / init paths. When `V8M_DEBUG=1`, every freed
  slot is stamped with the poison pattern (0xDF; distinct from
  the Large red-zone byte 0xCD so memory dumps can disambiguate
  trailing-overflow canaries from UAF poison) and every alloc
  verifies the poison is intact before handing the slot to the
  user — abort with a diagnostic naming the backend, slot, and
  offset on mismatch. Slot init pre-poisons the data area so
  the first-alloc verify sees the expected pattern (kernel
  delivered mmap-zero would otherwise falsely fire). Helpers
  are no-ops when DEBUG is off; release builds pay zero cost.
  Buddy guard pages remain future work — buddy arenas are
  multi-tenant and the per-block link layout would need finer
  routing than the slab path's intrusive next-pointer.

### Added
- pprof-format heap profile dump on exit (resolution of open
  question #5). `V8M_PROFILE=1` now emits a pprof Profile message
  (uncompressed protobuf, `.pb`) to a path resolved from
  `$V8M_PROFILE_PATH` (default `/tmp/v8malloc-PID.pb`) right
  after the malloc_info XML snapshot. New module
  `src/v8m_pprof.{h,c}` ships a self-contained protobuf encoder
  (varint LEB128 + length-delimited submessages) that walks the
  per-class request histogram and writes one Sample / Location /
  Function per non-zero class plus a Huge overflow row. Encoder
  is malloc-free (single 64 KiB BSS scratch) so it stays safe to
  invoke from the destructor after the dispatcher has been torn
  down. Coverage in `tests/test_pprof.c` (smoke test asserts the
  emitted file starts with the sample_type tag and the
  string_table contains `alloc_objects`). Real call-stack capture
  is a future cycle that needs per-allocation PC unwinding.

### Performance
- Slab-page deferred munmap (the spec's slab analog of buddy's
  drained-arena pattern). `struct v8m_slab_pool` gained an
  8-entry drained cache; when `v8m_slab_pool_free` empties a
  page, `release_slab_page` applies `MADV_DONTNEED` (kernel
  reclaims physical frames, virtual mapping intact) and parks
  the raw page pointer instead of releasing immediately. The
  next `acquire_slab_page` pops the cache LIFO so a revival
  within the bg-purge grace window reuses the slot without an
  mmap/munmap round trip. Slab metadata co-locates with the data
  area, so dropped pages get re-inited (fresh meta header) on
  revival — no separate handshake needed. Sweep
  (`v8m_slab_pool_sweep_idle`) wired into `v8m_dispatch_bg_tick`
  ages parked entries one tick per pass and releases those past
  4 ticks (≈ 4 s at the default purge interval). `v8m_purge`
  calls `v8m_slab_pool_purge_drained` for every arena to give an
  explicit caller immediate VMA / RSS relief. Diagnostic
  accessor `v8m_slab_pool_drained_count`. Coverage in
  `tests/test_slab_pool.c::check_drained_cache_round_trip`.
- Bounded-budget partials priority queue. The slab pool's
  `try_partials` previously walked the entire partials list
  per refill picking the max `used_count` (O(N) per refill).
  Now scans the first `V8M_PARTIALS_SCAN_BUDGET = 4` partials
  and picks the max among them — constant cost per refill, and
  since the insert path always pushes at head with `used_count =
  capacity - 1`, the recently-inserted entries clustered at the
  head are where the max overwhelmingly lives. Older entries
  deeper in the list have had more time to lose `used_count`
  and are by construction less utilized on average. The
  bounded scan retains the max-pick semantics — distinguishes
  it from a pure head-pop, which is wrong because partial pages
  can accumulate frees after insertion (a newer head entry can
  have lower `used_count` than an older entry that took fewer
  frees, as `tests/test_slab_pool.c::check_partials_pick_most_utilized`
  encodes).
- Predict prefetch + lifetime tracker hooks now wire through
  the full malloc family. `v8m_calloc` / `v8m_realloc` (the
  alloc path and the realloc(NULL, n) shortcut) /
  `v8m_aligned_alloc` / `v8m_posix_memalign` route through the
  new internal `do_malloc_pc(size, caller_pc)` and
  `do_aligned_alloc_pc` helpers, each entry point capturing its
  own `__builtin_return_address(0)` so the predict table and
  lifetime tracker see the user's actual call site rather than
  the v8malloc internal frame that would result from one entry
  point thunking through another.

### Added
- Public `v8m_estimate_lifetime(caller_pc)` and the
  `enum v8m_lifetime_class { EPHEMERAL, SHORT, LONG, UNKNOWN }`
  it returns. Backed by `v8m_thread_cache_lifetime_classify`
  which walks the calling thread's per-caller-PC EMA bucket.
  Exported under V8MALLOC_1.0.
- Public `v8m_install_size_class_table(const uint32_t *)` and
  `v8m_size_class_to_bytes(int cls)` for runtime size-class
  hot-reload. The new `_Atomic(const uint32_t *) v8m_active_class_table`
  is the swappable indirection slab-init paths now read; the
  `v8m_size_class_size(cls)` inline helper performs the relaxed
  load on the hot path. Exported under V8MALLOC_1.0.

### Performance
- Lifetime-class arena routing (the spec's `v8m_estimate_lifetime`
  → distinct-arena placement). The dispatcher gained 3 lifetime
  arenas (`slab_lifetime[3]`) initialized alongside the default
  `slab` pool; `v8m_dispatch_alloc` consults the classifier (via
  the per-thread `t_dispatch_caller_pc` hint set by `v8m_malloc`
  with `__builtin_return_address(0)`) and routes EPHEMERAL /
  SHORT / LONG allocations to their dedicated pools. Lifetime
  arenas bypass the TLC. Free path reads the new `arena_id` field
  on `v8m_page_meta` (and `v8m_tiny_page_meta` at the same offset
  for layout parity) to return the page to its source pool. New
  `v8m_slab_pool_alloc_arena(pool, cls, owner, arena_id)` is the
  arena-aware variant; the original `v8m_slab_pool_alloc` calls
  it with `V8M_ARENA_DEFAULT`. Off when `V8M_OPT_LIFETIME_TRACKING`
  is off or the per-PC EMA hasn't accumulated enough samples to
  classify (returns UNKNOWN → default arena).
- Per-region THP age sweep. Each `region_entry` stamps
  `promoted_at_tsc` when alloc-time advised MADV_HUGEPAGE; the
  bg purge tick now calls `v8m_page_heap_thp_age_sweep()` which
  walks every region and applies MADV_NOHUGEPAGE to those whose
  stamp is older than the cold threshold. Diagnostic counter
  `v8m_thp_age_demote_calls` (accessor:
  `v8m_page_heap_thp_age_demote_calls()`). A workload with one
  hot huge-eligible region and many cold ones now gets the right
  per-region advice — the prior global EMA could only pick one
  side for every subsequent alloc.

### Performance
- Production wiring of four primitive modules that previously
  shipped standalone:
  - **Anchor reservation**: the page heap's `anchor_or_mmap`
    helper now carves THP-eligible allocations (≥
    `V8M_HUGE_PAGE_SIZE`) from the global anchor's PROT_NONE
    region before falling back to discrete mmap. The
    `region_entry` carries an `is_anchor` flag so the matching
    free routes through `v8m_anchor_reservation_release`
    (`MADV_DONTNEED` + `mprotect(PROT_NONE)`) instead of munmap.
    Saves one VMA per carve. Two new `struct v8m_page_heap_stats`
    counters: `anchor_carve_calls`, `anchor_carve_failures`. The
    anchor's carve path was extended to align on absolute address
    (base + bump_offset) rather than just bump_offset, so 2 MiB
    carves succeed on a 4 KiB-aligned anchor base.
  - **Per-NUMA huge-page pool**: the slab pool's fresh-page
    acquisition now routes through the global per-NUMA pool
    (`g_slab_numa_pool`) when the dispatcher singleton's
    `use_numa_pool` flag is set. Test fixtures that create their
    own slab pools leave the flag false and stay on the discrete
    page-heap path. Numa-pool descriptor allocations route
    through mmap directly to avoid the malloc-recurse-into-slab
    deadlock.
  - **Time-based EMA refill controller**: the dispatcher
    instances `g_l2_refill` and consults it for the L1↔L2 batch
    size in both `try_tlc_fast_paths` (refill) and
    `slab_overflow_to_l2_or_slab` (overflow), replacing the
    fixed `V8M_DISPATCH_L2_BATCH = 32` constant. Frequent TLC
    underflows push the batch up; sparse underflows shrink it.
  - **NUMA imbalance action half**: the bg purge tick now calls
    `v8m_page_heap_numa_rebalance` which toggles per-node atomic
    `v8m_per_node_suppressed` flags based on the live balance
    snapshot. `bind_to_local_node` consults the flag at
    allocation time and diverts to the nearest non-suppressed
    neighbour via `v8m_numa_fallback_node`. Diagnostic counter
    `v8m_page_heap_numa_rebalance_diversions()` exposes how many
    allocations the action rerouted.
  All 38 tests pass with the wirings live.

### Added
- Per-NUMA-node huge-page pool primitive
  (`src/v8m_numa_pool.{h,c}`). Owns a list of
  `v8m_huge_slab` descriptors per NUMA node; carves
  V8M_PAGE_SIZE-sized slab pages from those huge
  pages on demand and migrates descriptors between
  partials / fulls lists as their occupancy changes.
  An emptied descriptor is unlinked and its huge page
  is returned to the page heap on the same call —
  bounded memory footprint without a sweep tick. Each
  per-node bookkeeping block is `V8M_CACHELINE_ALIGNED`
  (`struct v8m_numa_pool_node`) so adjacent nodes'
  counters and lock cannot share a line; a thread
  carving from node 0 cannot bounce node 1's cache
  line. Per-node counters: `huge_pages_alive`,
  `huge_pages_allocated`, `huge_pages_released`,
  `slab_carve_calls`, `slab_release_calls`. Huge pages
  are sourced via `v8m_page_heap_alloc(V8M_HUGE_PAGE_SIZE,
  V8M_HUGE_PAGE_SIZE)`, so the page heap's MAP_HUGETLB
  primary attempt + MADV_HUGEPAGE fallback both apply.
  Ships standalone — the dispatcher does not yet route
  slab allocations through this pool; that wiring lands
  with the slab pool refactor that consumes the
  per-NUMA shape. Coverage in `tests/test_numa_pool.c`
  (cache-line alignment of per-node entries, init /
  destroy lifecycle including double-destroy + NULL
  tolerance, basic carve with page-aligned + owned slot,
  full huge page migrates to fulls list, the next carve
  allocates a fresh huge page, full drain returns
  every huge page to the OS, foreign release is
  rejected, out-of-range numa_node returns NULL).
- Anchor reservation primitive
  (`src/v8m_anchor_reservation.{h,c}`).
  Reserves one large virtual range up front via
  `mmap(PROT_NONE | MAP_NORESERVE)` (default 256 MiB,
  caller-tunable) and carves sub-regions out of it via
  `mprotect(PROT_READ | PROT_WRITE)` — each carve
  commits its pages to the existing PROT_NONE mapping
  rather than minting a new VMA, so the kernel-side
  cost of mmap / fork / page-fault scales with the
  anchor count rather than the per-allocation count.
  Bump-only allocation by design: `release()` applies
  `MADV_DONTNEED` + `mprotect(PROT_NONE)` to drop the
  physical pages and trap use-after-free, but the
  virtual slot is not reclaimable — the simpler
  semantics keep the carve fast-path branch-free, and
  the page-heap's existing region-table free path
  handles within-region reuse for the integration
  cycle that consumes this primitive. Stats counters
  (`carve_calls`, `carve_failures`, `release_calls`)
  on the descriptor + a lock-free `owns()` predicate
  and `remaining()` accessor for the future routing
  decision. Ships standalone because the page-heap
  integration that consumes it requires routing
  decisions about which size classes opt in (Huge is
  the largest VMA contributor; Large and below carry
  the existing region-table cost) and those decisions
  are simpler to land on top of a frozen primitive.
  Coverage in `tests/test_anchor_reservation.c` (init
  / destroy lifecycle including default-cap and NULL
  tolerance, basic carve with alignment + writability
  + non-overlap, exhaustion bumps `carve_failures` and
  preserves bump_offset, release does NOT recycle the
  slot, release rejects foreign / oversized / NULL
  / zero-byte pointers, owns predicate edges,
  non-power-of-two alignment rejection).
- Huge-page slab carve primitive
  (`src/v8m_huge_slab.{h,c}`). Implements the
  `struct v8m_huge_slab` verbatim: per-huge-page
  descriptor with a 32-bit bitmap (each bit tracks one
  V8M_PAGE_SIZE = 64 KiB slab page within the
  V8M_HUGE_PAGE_SIZE = 2 MiB / 1 MiB-on-s390x huge
  page), `numa_node` for the future per-NUMA pool's
  routing, `next` link for partial / full / empty
  list chaining. `v8m_huge_slab_alloc` carves the next
  free slot via `__builtin_ctz`, `v8m_huge_slab_free`
  clears the matching bit while rejecting misaligned /
  out-of-range / double-free pointers,
  `v8m_huge_slab_is_full` / `is_empty` /
  `live_count` round out the predicate surface that
  the future per-NUMA HugePage pool will consume.
  `V8M_HUGE_SLABS_PER_HUGE` = `V8M_HUGE_PAGE_SIZE /
  V8M_PAGE_SIZE` (32 default, 16 on s390x), exactly
  matching the per-arch table. Two
  `_Static_assert`s pin the bitmap-fits-in-32-bits
  invariant. Ships standalone because the per-NUMA
  HugePage pool that consumes it is multi-cycle work;
  shipping the primitive on its own now means the
  future pool cycle wires up against a tested, frozen
  API. Coverage in `tests/test_huge_slab.c` (init
  state, fill + drain round-trip with per-slot
  alignment / range / uniqueness validation, free
  rejection of bad pointers, partial slabs_per_huge
  clamping, NULL tolerance).
- Time-based EMA refill controller module
  (`src/v8m_refill_controller.{h,c}`). Implements the algorithm verbatim: per-class EMA of actual demand
  (α = 0.25), rate predictor that divides by elapsed
  TSC ticks since the previous refill, batch sized so
  the next refill lands ≈ V8M_REFILL_TARGET_HOLD_MICROS
  (100 µs) into the future, clamped to
  [V8M_REFILL_BATCH_MIN, V8M_REFILL_BATCH_MAX] = [4,
  256]. Ships standalone because L3 / L4 do not yet
  exist as discrete tiers in the v0 dispatcher. A future cycle that introduces a per-NUMA L3
  pool will instance the controller and consume the
  computed batch on its refill path. Test-only
  `v8m_refill_controller_set_last_refill_tsc` knob
  drives the elapsed-time divisor deterministically.
  Coverage in `tests/test_refill_controller.c` (init
  state, first-call MIN, high-demand MAX clamp,
  zero-demand MIN clamp, EMA smoothing of a single
  spike, NULL/out-of-range tolerance).
- Per-NUMA-node memory balance snapshot
  (`v8m_get_numa_balance`). The page heap
  now stamps each registered region with the NUMA node
  its `mbind()` landed on (`uint16_t node` on
  `region_entry`, `V8M_REGION_NODE_UNBOUND` sentinel),
  bumps a per-node atomic byte counter on bind, and
  decrements it on free. New public
  `struct v8m_numa_balance_stats` carries
  `per_node_bytes[64]`, `total_bytes`, `node_count`,
  `most_loaded_node`, `most_loaded_bytes`,
  `average_bytes_per_node`, and the
  `imbalanced` flag — true when the most-loaded node
  holds ≥ 150 % of the average across live nodes
  (encoded as `most * 2 > average * 3` to dodge floats
  on the snapshot path). Single-node hosts and pre-init
  reporters never flip the flag. New
  `bind_and_account` helper centralizes the
  bind → record_node → counter-bump invariant in one
  place so the four call sites in `v8m_page_heap_alloc`
  don't repeat the pattern. The action half
  (suppress new allocations from the overloaded node,
  page migration via `move_pages()`, TLC capacity
  shrink) is the future cycle that lands once a
  per-NUMA pool exists to route around. Public
  `v8m_get_numa_balance` exported under V8MALLOC_1.0.
  Coverage in
  `tests/test_page_heap.c::check_numa_balance_invariants`
  (sum / leak / single-node-flag invariants;
  multi-NUMA-only assertion gated on `node_count > 1`).

### Performance
- Adaptive THP advice. The page heap now tracks the EMA of inter-arrival
  TSC ticks for THP-eligible allocations (≥
  `V8M_HUGE_PAGE_SIZE`, with `V8M_OPT_HUGE_PAGES` on)
  and replaces the unconditional `MADV_HUGEPAGE` hint
  with a density-driven decision: hot/warm workloads
  retain `MADV_HUGEPAGE` (promote), cold workloads —
  EMA above ≈ 1 second of inter-arrival ticks — get
  `MADV_NOHUGEPAGE` (demote) so the kernel does not
  waste effort promoting regions the program is
  unlikely to actively touch. Threshold derived once
  from `v8m_arch_tsc_frequency_mhz()`. Two new counters
  on `struct v8m_page_heap_stats`: `thp_promote_calls`,
  `thp_demote_calls`; plus `thp_ema_ticks` and
  `thp_cold_threshold_ticks` snapshots of the live
  decision state. Test-only knob
  `v8m_page_heap_thp_test_inject` lets tests drive the
  decision deterministically without depending on
  wall-clock timing (internal symbol — not in
  `v8malloc.map`). Coverage in
  `tests/test_page_heap.c::check_thp_adaptive_decision`.
  The full per-region access tracking + page-by-page
  promotion needs a region-stats
  table that does not exist in v0; the EMA-driven
  global decision captures the intent at
  page-heap granularity.

### Added
- Caller-address-based lifetime tracker
  (`v8m_get_lifetime_stats`). Foundation for
  ephemeral/short/long arena routing.
  Opt-in via `V8M_OPT_LIFETIME_TRACKING` (env
  `V8M_LIFETIME_TRACKING`, default 0); when on,
  `v8m_malloc` samples 1-in-`V8M_LIFETIME_SAMPLE_RATE`
  (256 today) and writes (caller PC, alloc TSC) into a
  per-TLC ring of 64 slots; the matching `v8m_free`
  linearly scans the ring on every call (cheap — 64
  pointer comparisons), measures the elapsed TSC ticks,
  folds the sample into a per-caller-PC EMA bucket
  (32 buckets per TLC, linear-probe lookup), and
  classifies the sample into ephemeral / short / long
  by comparison against thresholds derived once from
  `v8m_arch_tsc_frequency_mhz()` (100 µs and 100 ms in
  ticks). Aggregate counters: `samples_recorded`,
  `samples_completed`, `samples_evicted`,
  `ephemeral_count`, `short_count`, `long_count`. Per-TLC
  storage uses no atomics on the hot path; a global
  carry-over (atomic) absorbs the per-cache fold-in at
  thread exit. Public `struct v8m_lifetime_stats` +
  `v8m_get_lifetime_stats(out)` exported under
  V8MALLOC_1.0; option ABI extends `enum v8m_option`.
  Off by default — the per-free ring scan costs ~50 ns
  per call when on, only worth it for diagnostic runs
  that inform the future class-routing cycle. Coverage
  in `tests/test_thread_cache.c::check_lifetime_tracker_records`
  + `check_lifetime_tracker_off_is_inert`.
- Size-class request histogram + public snapshot API
  (`v8m_get_size_class_histogram`).
  Foundation for the dynamic size-class adjustment row:
  every dispatcher allocation is sampled
  (1-in-`V8M_HISTOGRAM_SAMPLE_RATE` = 64 today) and the
  raw user-requested size lands in the bucket matching
  the routed class. `request_bytes[cls]` tracks the sum
  of raw requests so internal-frag per bucket is
  `request_count[cls] * v8m_class_to_size[cls] -
  request_bytes[cls]` — what the future hot-reload step
  will consume to decide whether a new sub-class
  would shrink the dominant waste bucket. Per-TLC
  counters with no atomics on the hot path; a global
  carry-over absorbs the bootstrap / signal-safe
  fallback path and the per-cache fold-in at thread
  exit so the snapshot does not lose history.
  Aggregation walks a registry list under a brief mutex;
  allocating threads are unaffected. Public struct
  `struct v8m_size_class_histogram` carries 41 class
  buckets plus a `huge_request_*` overflow pair.
  Coverage in
  `tests/test_thread_cache.c::check_size_class_histogram`.
- End-to-end MPSC remote-free drain coverage
  (`tests/test_remote_free.c::check_alloc_remote_realloc`). The existing
  `test_remote_free.c` covered the MPSC primitive in
  isolation (multi-producer / single-consumer stress); the
  new third check exercises the dispatcher integration. Owner thread allocates a Tiny slot,
  exposes its TLC pointer; producer thread pushes that
  slot directly onto the owner's `cache->remote` MPSC
  queue (simulating the cross-thread free routing the
  dispatcher will do once thread-owned slab pages land);
  owner drains its TLC + L2 via `v8m_purge_thread` so the
  next alloc forces the TLC slow path; the realloc
  returns the same pointer the producer pushed,
  confirming
  `v8m_thread_cache_drain_remote → bin install → fast-path
  pop` works end to end. The producer-side routing piece
  (dispatcher choosing MPSC over local bin based on
  `meta->owner_thread`) is the only part that remains for
  the thread-owned-slab refactor.

### Performance
- False-sharing audit extended to the L1 thread cache and L2
  core cache (`src/v8m_thread_cache.{h,c}`, `src/v8m_core_cache.c`). The L1 cache's
  `remote` MPSC queue head (producer-written when
  cross-thread frees route here) is now followed by a
  V8M_CACHELINE_ALIGNED `initialized` field, which forces
  every consumer-only field onto a fresh cache line — a
  future producer writing `remote.head` cannot invalidate
  the line containing the owner thread's hot
  `bin_heads[0]` read. The cache struct's allocation
  switched from `malloc` to `aligned_alloc(V8M_CACHE_LINE_SIZE,
  rounded_size)` so the struct base is cache-line aligned
  in memory (without that, the field-level alignment
  attribute is honoured at the struct-relative offset only).
  The L2 core cache gains a `static_assert` that its
  `sizeof` is a multiple of `V8M_CACHE_LINE_SIZE`, which
  pins the existing V8M_CACHELINE_ALIGNED guarantee on
  neighbouring entries in `g_caches[V8M_NUMA_MAX_CPUS]` —
  a future field add that bumps the struct past the next
  cache-line boundary fails the build until the layout is
  re-padded. The intentional padding shows up as ~96 B of
  fill in `struct v8m_thread_cache`; documented and
  suppressed against
  `clang-analyzer-optin.performance.Padding` since
  reordering would defeat the isolation. Today the MPSC
  queue is dormant (slab pages are pool-owned in v0, no
  cross-thread frees route through it) so the immediate
  perf delta is zero; the audit lands now so the layout is
  ready when the thread-owned-slab refactor wires
  cross-thread free routing.

- TLC↔L2 plumbing with single-CAS batch push
  (`src/v8m_core_cache.{h,c}`, `src/v8m_thread_cache.{h,c}`, `src/v8m_dispatch.{h,c}`, `src/v8m_api.c`). New `v8m_core_cache_push_batch`
  pushes a pre-linked chain onto a per-core L2 stack with a
  single tagged-pointer CAS — amortizes the CAS over every
  node in the batch (typically 32 = V8M_DISPATCH_L2_BATCH).
  Companion `v8m_core_cache_pop_batch` returns up to N
  nodes assembled into a forward chain (loop of single
  pops, since concurrent consumers walking a shared chain
  would race on internal next-pointer reads). Wired into
  the dispatcher: TLC bin overflow now drains a chain via
  the new `v8m_thread_cache_drain_chain` and ships it to
  the calling CPU's L2 with one CAS, falling back to the
  per-object `flush_half` only when the L2 is unreachable;
  TLC bin underflow on the slow path pulls a batch from
  the L2 via `v8m_thread_cache_install_chain` before
  falling through to the slab pool. Single-thread workloads
  see the L2 act as a transparent caching layer for slots
  that overflow the bin's adaptive capacity; multi-thread
  workloads on the same CPU share the batch. Both
  `v8m_purge` and `v8m_purge_thread` now also drain the
  calling thread's current-CPU L2 via the new
  `v8m_dispatch_drain_local_l2` so explicit purges return
  cached slots through every layer. Refactored the
  dispatcher's slab-class fast path into the small helpers
  `try_tlc_fast_paths` and `slab_overflow_to_l2_or_slab`
  to keep clang-tidy's cognitive-complexity score in band.
  Coverage in
  `tests/test_core_cache.c::check_batch_round_trip`
  asserts a 32-node push_batch + pop_batch round-trip
  preserves every pointer.

### Added
- L2 per-core core cache primitive
  (`src/v8m_core_cache.{h,c}`, `src/v8m_numa.{h,c}`). New
  `struct v8m_core_cache` carries one Treiber stack per size
  class, V8M_CACHELINE_ALIGNED so neighbouring entries in the
  global per-CPU table do not share a cache line.
  `v8m_core_cache_push` and `v8m_core_cache_pop` use a
  CAS-loop on a tagged 64-bit head — top 16 bits hold an
  ABA-protection tag (incremented on every successful CAS),
  bottom 48 bits hold the canonical-form pointer (every
  pointer the allocator hands out fits in 48 bits on
  x86_64 / aarch64 user mappings). The global table is sized
  at `V8M_NUMA_MAX_CPUS = 4096` cores in BSS; only the cores
  the workload actually touches get physically backed pages
  (~384 B per touched core). `v8m_core_cache_for_current_cpu`
  pairs with the new `v8m_numa_current_cpu` helper to route
  pushes / pops to the calling thread's CPU. Wiring into the
  TLC overflow / refill paths lands with the
  `push_batch / pop_batch to amortize CAS` cycle ; today the L2 is callable from tests but is not
  yet on the alloc / free hot path. Coverage in
  `tests/test_core_cache.c`: single-thread LIFO round-trip,
  empty-stack pop returns NULL, out-of-range guards, and a
  4 producer × 4 consumer × 1024-pushes-each conservation
  stress that asserts every pushed pointer surfaces on the
  consumer side exactly once.

### Added
- Deferred coalescing for the buddy pool — opt-in
  (`src/v8m_buddy.{h,c}`, `src/v8m_buddy_pool.c`, `src/v8m_config.c`, `include/v8malloc/v8malloc.h`). New `V8M_OPT_DEFERRED_COALESCE` option (env
  `V8M_DEFERRED_COALESCE`, default 0) gates a buddy-pool
  variant where `v8m_buddy_pool_free` skips the
  immediate buddy-merge — the freed block lands on
  `free_lists[level]` directly via the new
  `v8m_buddy_free_no_coalesce` helper. The merge happens
  lazily on the next alloc that would otherwise return
  NULL: `v8m_buddy_pool_alloc` calls the new
  `v8m_buddy_coalesce_all` sweep on every in-use arena
  and retries `try_existing_arenas`. This avoids the
  coalesce/split round trip in alloc-free-alloc-free
  same-size patterns at the cost of slightly higher
  steady-state fragmentation; off by default because the
  immediate-coalesce baseline is still the better fit for
  most workloads. The calls for a 3-buffer epoch GC
  to avoid a TOCTOU race on the epoch swap; v8malloc's
  per-pool mutex serializes all buddy operations, so the
  epoch scheme would add complexity without buying
  safety. The lazy coalesce-on-alloc-miss design captures
  the "avoid wasteful coalesce/split cycles" intent
  without it. Coverage in
  `tests/test_buddy_pool.c::check_deferred_coalesce`
  asserts (a) freed buddy pairs stay on `free_lists[0]`
  with `free_lists[1]` empty, (b) an 8 KiB alloc that
  needs the merged block succeeds (whether via
  coalesce-and-retry or higher-level split).

### Added
- NUMA aggressive migration (level 3) — opt-in
  (`src/v8m_thread_cache.{h,c}`, `src/v8m_config.c`, `include/v8malloc/v8malloc.h`). New
  `V8M_OPT_NUMA_AGGRESSIVE_MIGRATION` option (env
  `V8M_NUMA_AGGRESSIVE_MIGRATION`, default 0) gates a
  per-thread migration check that runs from the cache's
  GC tick. When the calling thread's NUMA node has changed
  since the previous tick AND the option is on AND the
  topology has more than one node, the helper walks every
  cached slot in the bins, deduplicates the containing
  page bases (capped at 256 unique pages), and issues
  `move_pages()` to relocate them to the new node so
  subsequent allocations from those pages stay local.
  Cache gains `last_numa_node` (UINT32_MAX = unset) and
  `numa_migration_calls` (diagnostic counter); both reset
  on cache create. The option is off by default because the
  relocation is expensive (one syscall per migration event)
  and only useful for threads with locality-sensitive
  working sets — production deployments that pin threads to
  CPUs see no migrations and pay nothing for the wiring.
  The check itself runs unconditionally on the GC tick to
  keep `last_numa_node` current; only the move_pages call
  is gated. Coverage in
  `tests/test_thread_cache.c::check_numa_migration_records_node`
  injects a synthetic node delta and verifies the tick
  updates the field with the option both off and on
  (the move_pages effect is unverifiable on a single-NUMA
  CI host but the wiring path is exercised end-to-end).

### Added
- Predictive prefetch table for the TLC
  (`src/v8m_thread_cache.{h,c}`, `src/v8m_api.c`). Each thread cache now carries a
  `uint8_t predict_table[V8M_PREDICT_TABLE_SIZE = 1024]`
  storing the most recently observed size class for the
  call site that hashes to each slot. `v8m_malloc` captures
  the caller's PC via `__builtin_return_address(0)` and
  drives two new helpers:
  `v8m_thread_cache_predict_prefetch` issues a
  `__builtin_prefetch` for the bin head matching the
  predicted class before the dispatch alloc;
  `v8m_thread_cache_predict_update` records the actual
  served class after. Hash is `(pc >> 4) & (size - 1)` —
  drops instruction-alignment noise, single-AND mask. Hot
  paths that allocate the same class repeatedly from the
  same call site (the common pattern: a constructor or
  factory function in a loop) get the bin head warm in L1
  before the pop reads it. The slot defaults to
  V8M_PREDICT_NONE (0xFF) on cache create, so the prefetch
  helper skips entries it has not learned yet — no wasted
  prefetches on first-touch traffic. Coverage in
  `tests/test_thread_cache.c::check_predict_table_round_trip`
  asserts the lookup/update bookkeeping (consistent
  hashing, out-of-range classes rejected, NULL-cache and
  unknown-PC paths safe). The prefetch hint itself has no
  observable post-state, so its effect surfaces only in
  benchmark runs.

- Adaptive bin-capacity controller for the TLC
  (`src/v8m_thread_cache.{h,c}`). The cache now tracks
  per-class `alloc_count_per_class` / `free_count_per_class`
  on every bin pop/push and runs `v8m_thread_cache_gc_tick`
  every `V8M_TLC_GC_INTERVAL` (1024) operations. Each tick
  computes per-class demand (`max(allocs - frees, 0)`),
  folds it into an exponentially-weighted moving average
  (`ema = (3 × ema_old + demand) / 4`, α = 0.25), and
  recomputes `bin_capacity = clamp(ema × 2,
  V8M_BIN_CAPACITY_MIN, V8M_BIN_CAPACITY_MAX)`. Hot classes
  grow to V8M_BIN_CAPACITY_MAX (256) for higher cache hit
  rates; idle classes decay to V8M_BIN_CAPACITY_MIN (16) so
  the cache does not retain capacity for traffic that has
  stopped. Excess slots above the new (smaller) capacity are
  not eagerly flushed — the next free that crosses the new
  threshold pays the existing half-bin batch flush, keeping
  the GC tick cheap. Trigger uses a `gc_countdown` field
  that decrements on each alloc/free; the count-then-branch
  pattern is one decrement plus one well-predicted branch on
  the hot path. New `gc_generation` counter for diagnostics
  and tests. `v8m_thread_cache_gc_tick` is exported so tests
  can drive the controller deterministically without issuing
  a full interval's worth of operations. Coverage in
  `tests/test_thread_cache.c::check_adaptive_capacity_grows_with_demand`
  (verifies EMA growth, capacity = EMA×2, and decay back to
  MIN under sustained idle ticks) and `check_gc_countdown_fires`
  (verifies the countdown wire actually fires the tick from
  the alloc/free fast paths).

- TLC slow-path remote-free drain
  (`src/v8m_thread_cache.{h,c}`, `src/v8m_dispatch.c`). New
  `v8m_thread_cache_drain_remote` helper drains the cache's
  cross-thread MPSC queue and pushes each drained slot onto
  the matching local bin keyed by the slot's page-meta size
  class; it's invoked from `v8m_dispatch_alloc`'s TLC slow
  path right before falling through to the slab pool, so a
  follow-up retry can satisfy from drained slots without a
  pool-mutex round trip. The drain is a no-op in v0 (slab
  pages are pool-owned, so nothing pushes to the queue); it
  lights up when the thread-owned-slab refactor wires the
  owner-thread routing on the free path. Coverage in
  `tests/test_thread_cache.c::check_drain_remote_routes_to_bin`
  white-boxes the drain by manually pushing two real Tiny
  slabs to the cache's MPSC queue and verifying both land on
  `bin_heads[0]` after the drain returns 2.

### Performance
- Thread-cache (L1) fast path (`src/v8m_thread_cache.{h,c}`, `src/v8m_dispatch.{h,c}`, `Malloc fast path`, `Free fast path`, `Bin overflow`, `Per-class bin_capacity[]`). The TLC scaffolding from
  the previous cycle is now wired into the dispatcher: malloc
  pops from `bin_heads[size_class]`, free pushes back to the
  same bin, and a push that crosses `bin_capacity[size_class]`
  triggers a half-bin batch flush via `v8m_slab_pool_free`.
  Cached objects use their own first 8 bytes as the intrusive
  next-pointer (the same pattern slab pages already use), so
  caching adds zero per-object metadata. Per-class capacities
  ship at `V8M_BIN_CAPACITY_DEFAULT` (64) and stay clamped to
  the documented `[V8M_BIN_CAPACITY_MIN, V8M_BIN_CAPACITY_MAX]`
  band (16..256); the future adaptive controller (line 131)
  tunes them at runtime within that band.
  Routing is gated on a new `v8m_dispatch::use_tlc` flag —
  the public-API singleton (`g_dispatch`) opts in, isolated
  test fixtures that create their own dispatcher leave it off
  so cached slots cannot cross-link between distinct slab
  pools. Reentrancy is guarded by a `__thread` flag (the
  cache struct itself is allocated via the dispatcher, so the
  initial create would otherwise infinite-recurse). The
  `pthread_key` destructor now drains the cache via a hook
  installed by `v8m_api.c` (routing the drain to
  `g_dispatch.slab`) before freeing the cache struct, and
  latches a sticky `t_in_destructor` flag so any tail
  allocation in the thread's exit path bypasses TLC — without
  this the destructor would immediately re-create a cache
  via the freed-cache `free()` call, and the pthread runtime
  would iterate the destructor up to
  `PTHREAD_DESTRUCTOR_ITERATIONS` (4) times. `v8m_purge` and
  `v8m_purge_thread` now drain the calling thread's TLC so
  single-thread workloads (the thread never exits, the
  destructor never fires) can return cached slots on demand.
  Coverage in `tests/test_thread_cache.c` adds
  `check_alloc_free_round_trip` (LIFO order, empty-bin NULL
  return) and `check_overflow_signal` (push at capacity
  reports overflow, value within the documented clamp band);
  `tests/test_thread_churn.c` and `tests/test_soak.c` updated
  to call `v8m_purge()` before checking residual region
  counts so the leak invariant reflects the allocator's
  steady state rather than the cache holding. The remote
  free-queue drain (lines 71 + 77) and L2 batching land with
  subsequent cycles; today bin overflow flushes directly to
  the slab pool.

### Added
- Thread-cache (L1) scaffolding (`src/v8m_thread_cache.{h,c}`, `__thread t_cache`). New module owns the
  `__thread struct v8m_thread_cache *t_cache` slot, the lazy
  first-touch initializer (`v8m_thread_cache_get_or_create`),
  and a `pthread_key_create` destructor that reclaims a
  thread's cache when the thread exits. The cache struct is
  intentionally minimal — just an MPSC remote-free queue head
  and an `initialized` flag — so future cycles can grow it
  with bin_heads / bin_count / bin_capacity arrays without
  changing the surface seen by today's callers. The fast-path
  routing (TLS load → bin pop, page mask → magic check → bin
  push, bin overflow → batch flush) lands in subsequent
  cycles; today the cache is allocated and torn down
  correctly but is otherwise inert — `v8m_dispatch` still
  serves every alloc/free directly from the slab / buddy /
  Large backends. Module init / shutdown is wired into the
  library constructor / destructor; init failure is non-fatal
  (logs to stderr, allocator stays usable, per-thread caches
  leak at thread exit). Coverage in
  `tests/test_thread_cache.c` covers same-thread idempotency,
  distinct-thread distinct-cache (using a pthread_barrier so
  both worker threads stay alive simultaneously while the
  parent inspects each cache pointer — without the barrier
  the OS is free to run the workers serially, in which case
  each worker's destructor frees its cache slot before the
  next worker's malloc fires and the slot gets reused), and
  destructor-fires-on-thread-exit (verified via a cumulative
  `v8m_thread_cache_destructor_calls()` reclamation counter).

- `V8M_OPT_DEBUG` red zones for Large/Huge allocations
  (`src/v8m_large.c`). When DEBUG is on, the alloc path fills the
  first 64 bytes (one cache line) of the tail padding
  between the caller's requested size and the usable_size
  boundary with a `0xCD` canary; the free path walks the
  same span and aborts with
  `v8malloc DEBUG: red-zone corrupted at offset N past
  requested size (...) of allocation P` if any byte
  diverges. Catches the small-overflow class that the
  trailing guard page (which sits one full V8M_PAGE_SIZE
  past the request) cannot reach. Cap of 64 bytes bounds
  the per-allocation memset/memcmp cost so DEBUG mode stays
  usable on the largest Huge requests; overflows beyond the
  cap eventually cross into the trailing guard page and
  trap synchronously regardless. New `requested_size`
  field on `v8m_large_page_meta` records the original
  request so the free path can compute the canary span;
  `guard_bytes != 0` keys whether the canary is meaningful.
  Production builds (DEBUG=0) leave `requested_size = 0`,
  skip the fill, and skip the verify — bit-for-bit
  unchanged. New `tests/test_guard_page.c::check_redzone_traps_small_overflow`
  forks a child that stomps byte 8 past the request and
  verifies the parent observes SIGABRT via waitpid. Also
  bumped the test's `LARGE_REQUEST` from 256 KiB (which
  routes to the buddy pool) to 512 KiB so the
  guard-on-overrun check exercises the actual
  `v8m_large_alloc` guard rather than a coincidental
  unmapped-arena-edge fault.

- `V8M_OPT_DEBUG` trailing guard pages for Large/Huge
  allocations (`src/v8m_large.c`, `src/v8m_large.h`).
  When `V8M_OPT_DEBUG != 0` (env `V8M_DEBUG=1`) the alloc path
  appends one V8M_PAGE_SIZE region at the end of every Large /
  Huge allocation and `mprotect()`s it `PROT_NONE`; an overrun
  past the user-data window raises SIGSEGV synchronously
  instead of silently corrupting the next mapping. The guard
  size is recorded on the meta as `guard_bytes` so
  `malloc_usable_size` reports the accessible window only.
  Off the production hot path entirely — production builds run
  with DEBUG=0, `guard_bytes` stays 0, and `usable_size`
  matches the pre-cycle behaviour bit-for-bit. Coverage in
  `tests/test_guard_page.c` covers both directions: with
  DEBUG off, writing every byte of the reported usable_size
  succeeds (catches accidental production-mode guards); with
  DEBUG on, a fork()'d child writing at offset usable_size
  dies with SIGSEGV (the parent verifies via waitpid).
  Slab + buddy guard pages remain future work — slab metadata
  co-locates with the data area, and buddy arenas are
  multi-tenant so per-allocation guards would need finer
  routing.

### Performance
- Buddy arenas now defer the page-heap munmap when they fully
  drain (`src/v8m_buddy_pool.{h,c}`). When the free
  path empties an arena it applies `madvise(MADV_DONTNEED)` —
  which releases the physical frames immediately — and keeps
  the VMA + buddy bookkeeping in a `drained` state. A revival
  alloc within the next few bg-purge ticks reuses the slot
  without an mmap/munmap round trip; the kernel re-faults zero
  pages on the first touch. Bursty workloads that oscillate
  between holding and releasing the same arena (the MB-04
  mixed-size pattern is the canonical example) save one
  mmap/munmap pair plus the matching region-map insert/remove
  per cycle. The bg purge thread now owns a per-tick callback
  (`v8m_bg_purge_set_tick_hook`) routed through the new
  `v8m_dispatch_bg_tick` → `v8m_buddy_pool_sweep_idle`; arenas
  drained for ≥4 sweep ticks (≈4 s at the default 1 s purge
  interval) are released to the page heap. The explicit
  `v8m_purge()` API now also calls
  `v8m_dispatch_purge_drained()` to give callers immediate VMA
  + RSS relief, and the soft-limit refusal path retries after
  force-purging drained arenas so an OOM handler that frees a
  buddy block to make headroom always sees the limit drop.
  New stats accessor `v8m_buddy_pool_get_arena_stats` reports
  live / drained / total-in-use counts. Coverage in
  `tests/test_buddy_pool.c::check_drain_and_revive` (asserts a
  single free leaves the arena in drained state and the next
  alloc reuses it without a fresh page-heap mmap) and
  `check_sweep_threshold` (asserts the idle-tick threshold is
  honoured); existing tests updated to call
  `v8m_buddy_pool_sweep_idle(0)` / `v8m_purge()` where
  immediate release is required.

### Added
- x86_64 RDTSC time source + lazy frequency calibration
  (`src/v8m_arch.{h,c}`). New helpers `v8m_arch_rdtsc()` and
  `v8m_arch_tsc_frequency_mhz()` give the future EMA refill
  controller a portable monotonic time source. On x86_64
  `v8m_arch_rdtsc` is a single `__rdtsc` and the frequency
  helper measures TSC drift against
  `clock_gettime(CLOCK_MONOTONIC_RAW)` over a 5 ms window the
  first time it is called, caching the result in an atomic;
  scheduler-induced calibration outliers fall back to a
  sensible 3 GHz default rather than poisoning every
  downstream batch-size computation. On every other arch the
  rdtsc helper returns `clock_gettime(MONOTONIC_RAW)`
  nanoseconds and the frequency helper returns 1000 — the
  controller's "ticks per microsecond" formulation works on
  both axes without arch-specific code in the caller.
  Coverage in `tests/test_arch.c::check_tsc` asserts
  monotonicity across two reads, the cache returns the same
  value on a second call, and the frequency lands in the
  plausible 100–10000 MHz band on x86_64 / exactly 1000
  elsewhere.

- AArch64 Tier 1 runtime CPU feature probes
  (`src/v8m_arch.{h,c}`). New helpers `v8m_arch_has_lse()` and
  `v8m_arch_runtime_cache_line_size()` ship a portable surface:
  on aarch64 they read `AT_HWCAP & HWCAP_ATOMICS` via getauxval
  and `CTR_EL0` via `mrs` to report whether the running CPU has
  the Large System Extensions and what its actual L1 dcache
  line width is; on every other arch they return `false` and
  `V8M_CACHE_LINE_SIZE` respectively, so callers can use them
  without `#ifdef`. CMake gets an explicit `aarch64` branch in
  `V8MALLOC_ARCH_FLAGS` that pins `-moutline-atomics` (GCC's
  default since 10, but explicit for documentation and future
  toolchain pinning); the GCC-emitted stubs dispatch each
  atomic op between LL/SC and LSE forms at runtime via the
  `__aarch64_have_lse_atomics` flag the runtime sets from
  HWCAP. TPIDR_EL0-relative TLS is compiler-emitted for
  `__thread` automatically. The compile-time `V8M_CACHELINE_ALIGNED`
  macro continues to drive struct layout — the runtime probe is
  for diagnostics today and for a future cycle that wants to
  dynamically pad hot structures on hosts whose actual line
  width exceeds the build assumption (Apple M1 P-cores at
  128 B vs the 64 B default). Coverage in `tests/test_arch.c`
  asserts the helpers are callable on every arch, return
  power-of-two cache lines in the plausible 16–256 B range,
  and that the non-aarch64 fallback returns the compile-time
  constant verbatim.

### Documented
- Tier 2 RISC-V 64 support is explicit in the build system
  (`CMakeLists.txt`) and architecture header (`src/v8m_arch.h`).
  Added a dedicated `elseif(... riscv64)` branch to
  `V8MALLOC_ARCH_FLAGS` so a future tuning flag has a place to
  drop in; the branch ships empty today because every
  feature lowers from existing toolchain primitives:
  `stdatomic` emits LR.D/SC.D on baseline rv64gc and AMOCAS when
  the target advertises the Zacas extension; `__builtin_clzll` /
  `ctzll` / `popcountll` lower to CLZ.D / CTZ.D / CPOP.D when
  Zbb is on and to the documented software emulation otherwise;
  the FENCE instructions stdatomic emits cover the RVWMO memory
  model. Cache line is 64 B on the two parts most QEMU rootfses
  model (SiFive U74, T-Head C910); a future cycle adds a runtime
  probe via `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` for hosts that
  diverge. The documented runtime hwprobe ISA query is documented as
  out-of-scope for v0 — the codegen choice is locked at compile
  time, so a runtime probe would be purely diagnostic.

- Tier 3 LoongArch 64 support is explicit in the build system
  (`CMakeLists.txt`) and architecture header (`src/v8m_arch.h`).
  entry. Added a dedicated `elseif(... loongarch64)` branch to
  `V8MALLOC_ARCH_FLAGS` so a future tuning flag has a place to
  drop in; the branch ships empty today because every
  feature is compiler-emitted under the existing GCC ≥ 13 /
  Clang ≥ 16 floor: DBAR memory barriers via stdatomic
  memory_order, LL.D/SC.D atomics through C11 _Atomic CAS,
  CLZ.D/CTZ.D/CPOP.D through `__builtin_*`, and tp ($r2)-relative
  TLS through `__thread`. Also expanded the loongarch arm of
  `v8m_arch.h`'s detection cascade with a comment explaining
  the same compiler-coverage chain.

### Changed
- Kernel huge-page size is now per-arch via the new
  `V8M_HUGE_PAGE_SIZE` macro in `src/v8m_arch.h`
  :
  s390x uses 1 MiB to match the kernel default, every other
  Tier 1/2 arch we ship today (x86_64, aarch64, ppc64le,
  riscv64, loongarch64) keeps 2 MiB. The MAP_HUGETLB attempt
  threshold + multiple, the MADV_HUGEPAGE hint threshold
  (`src/v8m_page_heap.c`), and the Huge-allocation alignment
  bump in `src/v8m_large.c::V8M_LARGE_HUGE_ALIGN` all route
  through the new constant. Behaviour on x86_64/aarch64/etc.
  is unchanged (the constant evaluates to the same 2 MiB
  literal); on s390x a Huge allocation of e.g. 4 MiB now
  lands as four 1 MiB huge pages instead of being
  bumped/wasted to 2 MiB alignment that the s390x kernel
  cannot back with one huge page anyway. The s390x GOT TLS
  path is compiler-emitted whenever the toolchain targets
  s390x — `__thread` already does the right thing — so no
  source-level work is required for that part of the row.

### Performance
- `v8m_page_heap_alloc` skips the over-allocate-and-trim path
  when the requested alignment fits within one OS page
  (`src/v8m_page_heap.c`). mmap returns an OS-page-aligned
  address by definition, so for those requests a single direct
  mmap is sufficient — saves one extra `alignment` bytes of VMA
  reservation and the matching trim munmap call(s) per
  allocation. Gated on a cached `sysconf(_SC_PAGESIZE)` result.
  Active on ppc64le (64 KiB kernel pages — Debian / Ubuntu /
  RHEL default) and on aarch64 with 16 KiB or 64 KiB kernel
  pages (Asahi / certain server kernels). Dormant on x86_64,
  where the 4 KiB OS page is always smaller than our
  V8M_PAGE_SIZE-shaped requests, so the existing
  over-allocate-and-trim path runs unchanged. Refactored the
  reservation logic into a `reserve_aligned` helper so
  `v8m_page_heap_alloc` retains a flat top-level structure
  (kept clang-tidy's cognitive-complexity score under threshold).

### Added
- `v8m_get_frag_metrics` now reports aggregate slab utilization
  alongside the page-heap-derived counters
  (`include/v8malloc/v8malloc.h`, `src/v8m_api.c`,
  `src/v8m_slab_pool.{h,c}`). Four new fields land on
  `struct v8m_frag_metrics`: `slab_pages_in_use`,
  `slab_slots_total`, `slab_slots_used`, and
  `slab_utilization_pct`. The numbers are sourced from a new
  `v8m_slab_pool_get_aggregate_stats` walker that traverses
  every Tiny/Small class's `current` + `partials` lists under
  the pool lock, summing each page's `capacity` (slot count)
  and `used_count` (live slot count). Empty pages are
  returned to the page heap on free and full pages are off
  the lists by design, so the snapshot reports the
  actively-partitioned population — the population that
  drives operational utilization decisions
  . Per-class breakdown is a follow-up
  API once the per-class field group warrants its own struct.
  Coverage in `tests/test_api.c::check_huge_and_frag_stats`
  asserts `slab_pages_in_use >= 1` and `slab_slots_used >= 1`
  while a Tiny-class allocation is held live, plus the
  range invariants `slab_slots_used <= slab_slots_total`
  and `slab_utilization_pct <= 100`.

### Changed
- Weekly bench workflow now also runs MB-07 (NUMA
  local-allocation rate) — `.github/workflows/bench-weekly.yml`.
  The job header text changes from "MB-01..06" to "MB-01..07"
  so the run page reads correctly. MB-07 only produces real
  numbers on a multi-NUMA host with `get_mempolicy` enabled
  (the GitHub shared runner is single-node and often
  seccomp-filtered, so it prints the "skipping every row" stub
  for now); the artifact still captures the run for the day a
  CONFIG_NUMA=y host enters the matrix. Knob:
  `V8M_BENCH_ITERS=512` for a longer per-cell sample than the
  CI default.

### Added
- `V8M_DEBUG` double-free detection (`src/v8m_api.c`). `v8m_free` consults a 4096-entry ring of
  recently-freed pointers when `V8M_OPT_DEBUG != 0`; a hit
  aborts with `v8malloc DEBUG: double-free detected at <ptr>`
  before the underlying free runs (so the second slab/buddy
  free can never corrupt internal state). Off the hot path
  entirely when DEBUG is 0 — the ring touch is gated behind
  the config check, so production builds pay nothing. Ring
  size of 4096 is the tradeoff for not paying per-pointer
  hash-table cost; sustained free rates above 4096 ops
  between a duplicate could miss the detection. New
  `tests/test_double_free.c` coverage: a fork()'d child
  intentionally double-frees and the parent verifies the
  child died with SIGABRT, asserting the abort fires
  end-to-end.

- `V8M_DEBUG` leak-summary on exit (`src/v8m_api.c`). When `V8M_OPT_DEBUG != 0` (env `V8M_DEBUG=1`),
  the destructor reads `v8m_collect_live_stats` and emits a
  one-line stderr summary when `live_bytes > 1 MiB` OR
  `live_regions > 4`. The thresholds skip the "one slab page
  residue from internal state" false positive routine clean
  shutdowns produce (the allocator retains a slab page across
  the destructor for late libc / stderr-buffer allocations);
  a real user leak of a few MiB+ surfaces immediately. No
  per-allocation tracking — that's the future cycle's
  guard-pages / red-zones / call-site-aware leak detector
  (the substantive Debug-mode work this row reserves).

### Changed
- `v8m_purge()` is no longer a stub. The public API now invokes
  the bg purge thread's scan pass synchronously on the calling
  thread, so an explicit `v8m_purge()` call gets the
  diagnostics — `V8M_VMA_WARN_THRESHOLD` check, optional
  `V8M_VERBOSE` stats line — immediately rather than waiting up
  to one `V8M_OPT_PURGE_INTERVAL` second for the next bg tick.
  No actual page reclamation today (the slab and buddy pools
  already release empty pages eagerly on free); future per-NUMA
  empty-page sweep / TLC bin shrink hooks into the same scan
  body. New internal `v8m_bg_purge_run_once()` (in
  `src/v8m_bg_purge.h`) is the synchronous entry point.

### Added
- VMA-count threshold warning in the bg purge thread
  (`src/v8m_bg_purge.c`). New
  `V8M_OPT_VMA_WARN_THRESHOLD` (env `V8M_VMA_WARN_THRESHOLD`,
  default 1024) drives the bg purge thread to emit a one-line
  stderr warning when the live VMA count from
  `v8m_count_vmas()` crosses the threshold. Hysteresis tracks
  the last warning state so the line fires once per crossing,
  not every tick — but a later crossing after the count
  recovered will re-warn. Independent of `V8M_VERBOSE` so
  production hosts can keep verbose logging off and still get
  the warning. 0 disables. Closes the auto-warning half of the
  VMA monitoring; the MAP_FIXED anchor-reservation
  refactor remains as separate page-heap-strategy work.

- `malloc_info` XML expanded + `V8M_PROFILE` on-exit dump
  (`src/v8m_api.c`). The XML now covers every counter the
  allocator tracks: `<total>` mmap region count + size,
  `<system>` / `<aspace>` page-heap totals, `<large>` and
  `<huge>` per-class alloc / free / bytes_in_use, `<hugetlb>`
  and `<gigantic>` MAP_HUGETLB attempt + failure counts,
  `<hugepage_advise>` MADV_HUGEPAGE calls, `<mbind>` NUMA-pin
  calls + failures, and `<vma>` live VMA count via
  `v8m_count_vmas`. The whole document is one allocator-wide
  snapshot a maintainer can pipe to `xmlstarlet` or grep.
  `V8M_OPT_PROFILE != 0` (set via the `V8M_PROFILE`
  environment variable) makes the destructor dump one final
  malloc_info XML to stderr right before the bg purge thread
  joins — gives a process-exit summary maintainers can grep
  from logs without instrumenting the application. The
  pprof-format heap profile dump-on-exit (open question #5,
  resolved to pprof) lands once the protobuf encoder cycle
  ships; v0 profile-mode stops at the XML dump. Test
  coverage: the existing `malloc_info` block in `test_api`
  now spot-checks `<large>` / `<huge>` / `<vma>` / `<mbind>`
  rows so a regression that drops one of them fails the
  gate.

- Async-signal-safe emergency allocator
  (`src/v8m_signal_safe.{h,c}`).
  `malloc` / `free` are not async-signal-safe in general — the
  slab pool's `pthread_mutex` would happily deadlock if a
  signal handler fires on a thread that already holds it.
  `v8m_signal_safe_alloc(size_t)` (exported under
  `V8MALLOC_1.0`) is the escape hatch: a 64 KiB BSS bump pool
  with an atomic offset, no locks, no syscalls, safe to call
  from any signal handler. Returns NULL on exhaustion (the
  budget is intentionally tiny — abort would be a worse
  signal-handler outcome than a NULL the handler can check).
  `free()` on a signal-safe pointer is a safe no-op: the
  dispatcher's free path consults `v8m_ptr_is_signal_safe`
  before reaching for page metadata, mirroring the bootstrap
  pointer treatment. The buffer leaks for the process lifetime
  by design — per-pointer reclamation would need a free list
  which would not be async-signal-safe under contention from
  non-handler frees. New `tests/test_signal_safe.c` covers
  cold-buffer alloc, free no-op, alloc(0), exhaustion → NULL,
  `malloc_usable_size` no-crash, and an actual
  `raise(SIGUSR1)` round-trip exercising the alloc-from-handler
  path end-to-end.

- 1 GiB Gigantic-page path
  (`src/v8m_page_heap.c`, `src/v8m_large.c`).
  `v8m_page_heap_alloc` now attempts
  `mmap(MAP_HUGETLB | MAP_HUGE_1GB)` as its first preference
  for requests that are 1 GiB-multiple AND 1 GiB-aligned with
  `V8M_OPT_HUGE_PAGES` on; on failure (the common case —
  kernels without 1 GiB hugepages reserved, every CI runner)
  the path falls through cleanly to the existing 2 MiB
  MAP_HUGETLB attempt and ultimately to mmap + MADV_HUGEPAGE,
  so no behavioural change for hosts that cannot serve 1 GiB
  pages. `v8m_large_alloc` bumps the page-heap alignment to
  1 GiB for size ≥ 1 GiB so the new code path is reachable
  from the standard `malloc(N)` entry point — the
  over-alignment cost is bounded (≤ 1 GiB of virtual address
  space, negligible against a multi-GiB allocation) and the
  win when 1 GiB pages are reserved is one TLB entry per
  gigabyte versus 512 entries with 2 MiB pages. Two new
  counters on `struct v8m_page_heap_stats` —
  `gigantic_alloc_calls` and `gigantic_alloc_failures` — let
  a maintainer verify the path fires on production hardware.
  `MAP_HUGE_1GB` is computed locally (`30 << 26`) to avoid
  pulling in `<linux/mman.h>` which collides with glibc's
  `<sys/mman.h>`.

- Background purge thread (`src/v8m_bg_purge.{h,c}`). Spawned at the end of
  `v8m_constructor` after the dispatcher is fully usable;
  joined at the start of `v8m_destructor` before the init
  state flips to TORN_DOWN. Sleeps via
  `pthread_cond_timedwait` so shutdown is a single futex hop
  regardless of the configured `V8M_OPT_PURGE_INTERVAL` —
  no waiting out the full interval at process exit. The
  period is re-read each tick so a runtime
  `v8m_set_option(V8M_OPT_PURGE_INTERVAL,...)` is picked up
  on the next iteration. v0 scan body is a stub that emits
  one stats line to stderr per tick when `V8M_OPT_VERBOSE`
  is set, otherwise no-op (the slab and buddy pools already
  release empty pages eagerly on free, so there is no actual
  purge work today). Real per-NUMA empty-page sweep, TLC bin
  shrink, and VMA-count threshold warning hook into the
  same loop as those subsystems land. Spawn failure is
  non-fatal: the constructor logs to stderr but the
  allocator stays usable.

### Changed
- Init lifecycle is now a four-state machine
  (`src/v8m_api.c`). `enum v8m_init_state` carries
  `NONE / RUNNING / READY / TORN_DOWN` instead of the binary
  0 / 1 the previous version used. The constructor publishes
  RUNNING with release ordering BEFORE touching the dispatcher
  (so a reentrant alloc during `dlsym` / `pthread_atfork` /
  `v8m_dispatch_init` sees a non-READY state and routes
  through bootstrap), then publishes READY after init completes.
  The destructor swaps to TORN_DOWN, distinct from NONE so a
  future debug build can flag "alloc after destructor"
  separately from "alloc before constructor". `dispatch_ready()`
  now returns true only for READY; all atfork handlers gate on
  the same predicate. No hot-path observable difference today
  — the bootstrap path catches all non-READY states the same
  way — but correctness and a clearer mental model for the
  upcoming TLC and bg-purge cycles. Closes the matching P0 — those describe
  behaviour either already present or now landed.

### Added
- NUMA local-node pin via `mbind(MPOL_BIND)` on every
  freshly-mapped page-heap region (`src/v8m_page_heap.c`). Both
  the MAP_HUGETLB primary path and the regular over-allocate-
  and-trim path now issue a direct `SYS_mbind` syscall
  immediately after a successful mmap — before any page fault
  has materialised a physical frame, so the kernel's
  page-allocator places subsequent fault-ins on the calling
  thread's current NUMA node without needing MPOL_MF_MOVE.
  Skipped when `V8M_OPT_NUMA_AWARE == 0`, the host reports a
  single NUMA node (no benefit), or the resolved current node
  is out of range. Best-effort: failures (typical in
  seccomp-restricted runtimes and kernels without
  CONFIG_NUMA — every WSL2 / dev container) increment a
  failure counter and the allocator continues unchanged with
  the kernel's default policy. Two new fields on
  `struct v8m_page_heap_stats` — `mbind_calls` and
  `mbind_failures` — let a maintainer verify the path is firing
  on production hardware. Resolves the P0 mbind item on
  ; the future per-NUMA pool sharding builds on top of
  this.

### Changed
- Cache-line pad the slab + buddy pool locks
  (`src/v8m_slab_pool.h`, `src/v8m_buddy_pool.h`). Both pools'
  `pthread_mutex_t lock` field now carries
  `V8M_CACHELINE_ALIGNED`. The single `v8m_dispatch` global
  embeds `slab` then `buddy`, so without padding the
  slab.lock futex word and the head of buddy.arenas[0] sat on
  adjacent cache lines on x86_64 (V8M_CACHE_LINE_SIZE == 64).
  Two cores hammering the slab lock and the buddy state
  respectively would ping-pong the line every ownership
  hand-off, contributing to the negative-scaling we already
  see in MB-02 / MB-03. The fix is a one-attribute change with
  no functional impact; it lands the first half of the
  false-sharing audit (core-cache and NUMA-pool padding land
  with their structures).

### Added
- mimalloc-bench wrapper script
  (`scripts/bench-mimalloc.sh`). Drives the external
  mimalloc-bench suite (alloc-test, cfrac, espresso, larsonN,
  mstressN, glibc-bench, redis, rptest, …) under v8malloc via
  LD_PRELOAD so the de-facto malloc-replacement benchmark
  catalogue is one command away. Thin wrapper, not a
  re-implementation: expects a tree the user has already built
  with mimalloc-bench's own `build-bench-env.sh`, then for each
  requested workload runs `LD_PRELOAD=$V8M /usr/bin/time -v
  <workload>` and tags the wall time + peak RSS block by
  workload name. `--reps N` for noise sampling. With no
  workloads named, prints the catalogue of `out/bench/sys/` so
  the maintainer does not have to grep. Resolves the v8malloc
  SO via the same probe-then-env-var pattern as
  `bench-compare.sh` (V8M_V8MALLOC_SO overrides).

- `v8m_count_vmas()` public API (exported under
  `V8MALLOC_1.0`). Returns the process-wide VMA count read live
  from `/proc/self/maps` — useful for surfacing the kernel-side
  fragmentation cost a maintainer cannot see from
  `v8m_get_stats` alone (every Huge allocation that does not
  coalesce with an existing region grows the line count, and a
  runaway count slows mmap / fork / page fault paths in the
  kernel). Implemented with raw `open` + `read` + `close` — no
  fopen, no malloc on the path — so it is safe to call from the
  future background purge thread without re-entering the
  allocator. Returns 0 if /proc/self/maps cannot be opened
  (some seccomp sandboxes), letting the caller treat that as
  "unknown" rather than "zero". `v8m_get_frag_metrics` grows a
  `vma_count` field that carries the same number so a single
  snapshot covers both allocator-side and kernel-side
  fragmentation. The MAP_FIXED anchor-reservation
  refactor + auto-warning-on-threshold land in a follow-up cycle.

- AAL primitives test (`tests/test_arch.c`). Covers the
  architecture-abstraction-layer surface the library actually
  exposes today: V8M_ARCH_* detection (exactly one defined,
  plus a cross-check against the compiler's predefined
  `__x86_64__` / `__aarch64__` / `__riscv` / `__powerpc64__` /
  `__s390x__` / `__loongarch64`), V8M_CACHE_LINE_SIZE, V8M_CACHELINE_ALIGNED +
  V8M_ALIGNED(N) (stack structs land on the requested
  boundary), V8M_LIKELY / V8M_UNLIKELY (hints only — both
  branches produce identical observable outcomes),
  V8M_PAGE_SIZE / V8M_PAGE_SHIFT / V8M_PAGE_MASK
  (power-of-two + cross-consistent + masks correctly), and
  the `__builtin_ctzll / clzll / popcountll` intrinsics the
  slab-bitmap and buddy-level hot paths rely on (hand-built
  inputs to catch a broken cross toolchain). The
  per-primitive split (test_atomics.c / test_bitops.c / test_tls.c /
  test_prefetch.c) lands when the matching v8m_* wrappers
  actually ship — today those operations are consumed
  directly from `<stdatomic.h>` and `__builtin_*`, so there
  is nothing in the AAL surface to split across. (`tests/test_soak.c`). Runs the
  MB-04 mixed-size workload for a configurable budget — 2 s by
  default so every PR run pays for it, env-override
  (`V8M_SOAK_DURATION_MS`) up to the 24 hours — sampling
  `v8m_get_stats.live_regions` every 500 ms against a cap that
  scales with `live_count` (leak-induced growth surfaces within
  seconds, not days). After a full post-run drain the test
  asserts `live_bytes` returns within 1 MiB of baseline and
  `live_regions` matches baseline exactly; both would flag a
  per-cycle leak. Knobs: `V8M_SOAK_DURATION_MS` (default 2000),
  `V8M_SOAK_LIVE_COUNT` (default 2000, capped at 1 M),
  `V8M_SOAK_SEED` (default 0x5041). Ran 10 M ops in the default
  2 s budget with peak live_regions = 97 (cap = 4 064) and
  zero leak on drain.

- MB-07 NUMA local-allocation-rate benchmark
  (`bench/mb_07_numa_local.c`).
  Single-thread; for each allocation, touches the first byte to
  force the kernel to back the page with a local physical
  frame, then issues a direct `get_mempolicy` syscall with
  `MPOL_F_NODE | MPOL_F_ADDR` (bypasses libnuma so the bench
  link line stays clean) to read the actual residency node id
  and compares against `v8m_numa_current_node()`. Reports per
  size-band `local` / `unknown` / `local_%`. Up-front probe
  skips the whole sweep cleanly with a one-line note when the
  kernel returns ENOSYS / EPERM (every WSL2 / no-NUMA / seccomp-
  filtered runtime — the dev container exhibits the EPERM
  case). On a single-NUMA host every row reads 100 % local;
  the bench is the regression gate for the future per-NUMA pool
  work, so a routing mistake cannot silently send a thread's
  allocation to the wrong node.

- Weekly benchmark workflow
  (`.github/workflows/bench-weekly.yml`). Runs every Sunday
  06:00 UTC (plus `workflow_dispatch` for on-demand baseline
  captures). Builds Release + bench, opportunistically installs
  jemalloc / tcmalloc / mimalloc (a missing distro package
  doesn't fail the workflow — the compare script skips any
  allocator it cannot find), drives MB-01..06 through
  `scripts/bench-compare.sh` so each result row is tagged by
  allocator, and uploads the aggregated output as a workflow
  artifact (`bench-results-<run-id>`, 90-day retention) so
  maintainers can diff week-over-week. Bench durations are
  bumped above the interactive defaults (1–2 s per cell) so the
  numbers are not warmup-dominated. The automated >5%
  perf-regression gate needs a persisted baseline file
  (follow-up cycle); this lands the scheduler so the raw data
  stream starts now.

- Comparative baseline script (`scripts/bench-compare.sh`).
  Drives a single benchmark binary against every reference
  allocator installed on the host (glibc, jemalloc, tcmalloc,
  mimalloc, v8malloc), setting `LD_PRELOAD` per run and
  emitting one tagged block of bench output per allocator so
  the numbers are directly comparable without leaving the
  current host-state window. Allocator discovery uses per-lib
  env overrides (`V8M_JEMALLOC_SO` / `V8M_TCMALLOC_SO` /
  `V8M_MIMALLOC_SO`) first, then falls back to `ldconfig -p`
  pattern match; missing allocators are skipped with a note so
  a sparse dev box (only v8malloc + glibc) still gets a two-way
  compare. `--only name,...` filters the allocator list;
  `--reps N` repeats each allocator. Pass-through env vars
  (`V8M_BENCH_DURATION_MS`, etc.) work. Chains cleanly with
  `scripts/bench-run.sh`:
  `sudo scripts/bench-run.sh -- scripts/bench-compare.sh \
  --./build/bench/bench/mb_01_throughput`.

- ST-03 fork-safety stress test
  (`tests/test_fork_stress.c`). Spins 4
  worker threads in the parent doing continuous alloc/free
  across four size buckets (slab-Tiny, slab-Small, buddy,
  Large), then forks 25 times back-to-back with the workers
  still running. Each child frees the two inherited anchor
  allocations (slab + buddy), drives a byte-pattern-verified
  4-bucket × 64-op allocate/write/verify/free workload across
  every backend, and `_exit`s. Parent waits for each child,
  aggregates failures, stops the workers, and runs a post-storm
  allocate/free cycle to confirm it stayed healthy. Complements
  the single-fork happy-path coverage in `tests/test_fork.c` —
  any atfork-ordering or parent/child shared-page-map
  regression surfaces as a deadlock (timeout) or a child exit
  with non-zero status.

- MB-05 fragmentation scenario benchmark
  (`bench/mb_05_fragmentation.c`). Drives
  the fragmentation-maximizing pattern: allocate N
  objects with random sizes, free 50 % at random, re-allocate
  N/2 with different sizes (always fresh distribution draws),
  repeat K iterations. After each iteration the bench samples
  three views of memory consumption — `logical_bytes`
  (`malloc_usable_size` summed across live slots),
  `mapped_bytes` (`v8m_get_stats.live_bytes` — the allocator's
  own VMA reservation), and `rss_bytes` (`/proc/self/statm`
  resident pages × page size) — and reports `frag_ratio =
  mapped / logical`, the fraction of allocator reservation
  beyond the user's asked-for bytes (combined internal +
  external fragmentation). Output is one row per iteration so
  the time series is plot-ready. Default 10 000 live × 20
  iterations keeps interactive runs sub-second; the
  1M × 100 sweep is reachable via env knobs. Knobs:
  `V8M_BENCH_LIVE_COUNT` (default 10000, capped at 1M),
  `V8M_BENCH_ITERS` (default 20, capped at 1000),
  `V8M_BENCH_SEED` (default 0x5a5a). Per-class internal /
  external fragmentation breakdown lands as follow-up.

- MB-03 producer/consumer benchmark
  (`bench/mb_03_producer_consumer.c`). N
  producer threads each `malloc` fixed-size objects and hand
  them to a paired consumer via a bounded 1024-slot SPSC ring;
  the consumer `free`s. Every alloc/free pair crosses a thread
  boundary, so the bench measures the cost of the cross-thread
  free path that the future remote-free MPSC queue will
  optimize. Pair sweep (1, 2, 4, 8, 16), capped at
  `min(nproc/2, 32)` by default (`V8M_BENCH_MAX_PAIRS=N`
  overrides). Size sweep matches the exactly: 64 B / 256 B
  / 1 KiB. Reports per-row `handoffs` / `handoffs_per_sec` /
  `per_pair` so the single-mutex scaling story is legible.
  Also serves as the regression gate for the slab-pool
  partials-list double-insertion bug fixed in the same cycle as
  MB-04: any reintroduction of a stale partials-list entry
  segfaults this bench within seconds of the timed loop.

### Fixed
- CI UBSan build broke on the shared library link
  (`CMakeLists.txt`). Clang's driver does not link the
  sanitizer runtime into a `-shared` target — `-static-libsan`
  and `-shared-libsan` are both no-ops on a `-shared` link
  line, and the static archive carries a `.preinit_array`
  section that ld refuses to put in a DSO. The shared library
  therefore legitimately carries unresolved
  `__ubsan_handle_*_abort` references the host executable's
  sanitizer runtime resolves at LD_PRELOAD time. The previous
  `--no-undefined` linker flag rejected those references at
  link time. Drop `--no-undefined` from the SO link line
  whenever any sanitizer is on; keep it for the standard
  build (it stays the default API-completeness gate). Test
  executables, which link the static library, get the runtime
  through clang's normal `-fsanitize=undefined` handling on
  their own non-`-shared` link line and continue to work
  unchanged. Verified: `cmake -DV8MALLOC_BUILD_UBSAN=ON …`
  builds clean and 29/29 ctests pass under UBSan.

- CI format-check drifted from local because clang-format-18
  (Ubuntu-latest's default `clang-format` package) and
  clang-format-19 (the dev container's installed version) ship
  different defaults for `AlignTrailingComments` (bool vs
  struct). Pinned `AlignTrailingComments: false` in
  `.clang-format` so both versions render the same output.
  Re-formatted touched files; the git diff is one-or-two
  trailing-space tweaks per affected source.

- CI build error on older Clang (`src/v8m_bootstrap.c`). The
  bootstrap buffer was declared as
  `static alignas(V8M_PAGE_SIZE) unsigned char …[]`, which
  GitHub Actions' Ubuntu-latest Clang (< 19) rejects with "an
  attribute list cannot appear here" — the parser tries to
  interpret the `alignas(…)` parenthesised expression as an
  attribute-list in a position it is not allowed. C23's grammar
  is ambiguous enough here that leading the alignment specifier
  works on every supported compiler, so the order is now
  `alignas(V8M_PAGE_SIZE) static unsigned char …[]`. Local
  Clang 19 accepted both forms; the CI lane against 18.x was
  the only place the earlier ordering mattered.

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
  (`bench/mb_04_mixed.c`). Single-thread
  workload driving a bounded working set of `live_count`
  concurrent allocations through alloc/free with sizes drawn
  from the six-band distribution (8 B/32 B at 40 %, …,
  > 64 KiB at 3 %). Each iteration picks a random slot and
  replaces its allocation with a fresh size from the
  distribution, so the working set holds at roughly
  `live_count` live objects — the "concurrently live
  objects" knob. NULL returns are tolerated and counted in
  `alloc_failures` (the > 64 KiB tail occasionally exhausts the
  buddy pool's 16 MiB cap; throughput reflects only successful
  ops). Bringing this up surfaced and unblocked the slab-pool
  partials-list double-insertion bug fixed in this same cycle.
  Multi-thread variants (the "threads: 1, 8, 64") land
  with finer-grained per-pool synchronization. Knobs:
  `V8M_BENCH_DURATION_MS` (default 1000), `V8M_BENCH_WARMUP_MS`
  (default 100), `V8M_BENCH_LIVE_COUNT` (default 10000, capped
  at 10 M), `V8M_BENCH_SEED` (default 0x1234).

- MB-06 large-allocation latency benchmark
  (`bench/mb_06_large_latency.c`).
  Single-thread per-iteration timing of the Large / Huge mmap
  path, reported separately for the three phases the kernel
  charges differently: `alloc` latency (mmap + region-map
  insert), `fault` latency (the first-touch `memset` that pulls
  in physical pages on demand — page-fault dominated), and
  `free` latency (region-map remove + munmap). Each phase
  reports both p50 and p99 across `V8M_BENCH_ITERS` iterations
  per size, so the "Large allocation ≤0.5× glibc"
  median-comparison criterion is directly
  legible. The memset is fenced with a one-byte
  `__asm__ volatile("" : : "r"(ptr) : "memory")` compiler
  barrier — without it gcc dead-code-eliminates the memset
  (its result is only ever consumed by free) and the fault
  numbers come out as zero.

  Sweeps the exact size ladder (256 KiB → 256 MiB).
  256 KiB sits at the buddy/Large boundary so the bench also
  surfaces the routing transition. Multi-thread coverage (the
   "threads: 1, 8") lands once the page-heap region map
  grows finer-grained synchronization. Knobs:
  `V8M_BENCH_ITERS` (default 100, clamped to [10, 10000] so
  taint-flow analysis is happy), `V8M_BENCH_MAX_SIZE_MB`
  (default 256; lower for limited-RAM CI runners).

- Benchmark runner script (`scripts/bench-run.sh`). Pins the
  host into the measurement environment
  before invoking the supplied bench command — switches every
  online CPU's frequency governor to `performance`, disables
  transparent huge pages, and disables ASLR — then restores the
  prior settings on exit (trap on EXIT/INT/TERM). Tuning steps
  need root; without root the script prints a warning and runs
  the bench anyway so dev-laptop iteration still benefits from
  the harness. `--drop-caches` opt-in for fault-heavy workloads.
  Usage: `scripts/bench-run.sh --./build/bench/bench/mb_01_throughput`.

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
  unmap+remap when `mmap(NULL,...)` happens to return the same
  virtual address. Reverted the bench to keep the tree green;
  MB-03 is unblocked once the thread cache lands and empty-page
  reclamation can be made lazy.

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
  Tier 2 architecture on the support
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
  (`bench/mb_02_scalability.c`). Sweeps the
   1 / 2 / 4 / 8 / 16 / 32 / 64 / 128 thread counts at the
  fixed 64 B common-case size, reports total throughput,
  per-thread throughput, and the scalability ratio anchored on
  the 1-thread number. Each per-thread-count run barriers the
  workers in, runs the alloc / write / free loop for the shared
  duration, barriers them out, sums per-thread iters. The thread
  sweep is capped to `min(nproc, 32)` by default so small CI
  boxes do not spend wall time thrashing 128 contended threads;
  `V8M_BENCH_MAX_THREADS=128` opts back into the range.
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
  with documented defaults (verbose 0,
  purge interval 10 s, thread-cache max 256, HugePages 1, NUMA
  aware 1, debug 0, profile 0, compaction threshold 25 %).
  v8m_config_init re-reads the environment on every call so tests
  can reset state, and out-of-range option ids fail the
  set/get bounds check rather than scribbling on memory. Test
  suite covers defaults, env overrides for every option, fallback
  to default on unparseable env values, set/get round-trip, and
  the out-of-range guard.
- MB-01 single-thread throughput benchmark
  (`bench/mb_01_throughput.c`) and the
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
  arch coverage stays in a separate weekly workflow so
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
  . `v8m_page_heap_alloc` now tries
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

      _ZdlPv _ZdaPv _ZdlPvm _ZdaPvm
      _ZdlPvSt11align_val_t _ZdaPvSt11align_val_t
      _ZdlPvmSt11align_val_t _ZdaPvmSt11align_val_t
      _ZnwmRKSt9nothrow_t _ZnamRKSt9nothrow_t
      _ZdlPvRKSt9nothrow_t _ZdaPvRKSt9nothrow_t
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

- Memory-pressure stress test (`tests/test_memory_pressure.c`). Five-phase scenario: build a working
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

- Distance-ordered NUMA fallback. The library
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
  refresh. Two new `__thread` slots cache the
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
  * test_api's `posix_memalign(NULL,...)` check tripped UBSan's
    nonnull-attribute runtime check (the volatile-pointer trick
    only dodges the compile-time diagnostic). Switched the test
    to call `v8m_posix_memalign` directly — our function doesn't
    carry glibc's `__nonnull` attribute, so UBSan no longer
    flags the deliberate NULL pass.

- Thread-churn stress test (`tests/test_thread_churn.c`). 100 batches × 100 threads = 10 000
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

- Resolved every remaining open question in :
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
  `v8m_version` resolves through `dlsym(RTLD_DEFAULT,...)` —
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
  as `v8m_select_allocation_page`.
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
  refresh-on-N-th-call optimization lands later. When sysfs is
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
  for symbols nothing imports.

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
  page-heap region map lands.

- Libc fallback for foreign pointers
  (`v8m_libc_fallback_init/ready/free/malloc`): the library
  constructor now resolves the next free/malloc/calloc/realloc on
  the dynamic search path via `dlsym(RTLD_NEXT,...)` before
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
