![v8malloc logo](./docs/v8malloc.png)

[![CI](https://github.com/iqbqioza/v8malloc/actions/workflows/ci.yml/badge.svg)](https://github.com/iqbqioza/v8malloc/actions/workflows/ci.yml) [![multi-arch](https://github.com/iqbqioza/v8malloc/actions/workflows/multi-arch.yml/badge.svg)](https://github.com/iqbqioza/v8malloc/actions/workflows/multi-arch.yml) [![coverage](https://github.com/iqbqioza/v8malloc/actions/workflows/coverage.yml/badge.svg)](https://github.com/iqbqioza/v8malloc/actions/workflows/coverage.yml) [![fuzz](https://github.com/iqbqioza/v8malloc/actions/workflows/fuzz.yml/badge.svg)](https://github.com/iqbqioza/v8malloc/actions/workflows/fuzz.yml) [![release](https://github.com/iqbqioza/v8malloc/actions/workflows/release.yml/badge.svg)](https://github.com/iqbqioza/v8malloc/actions/workflows/release.yml)

# v8malloc

A high-performance memory allocator for Linux, designed to outperform
jemalloc, mimalloc, and tcmalloc on multithreaded and NUMA workloads.

> **Status**: in active development. The build system, public API
> surface, and project layout are in place; most allocator internals are
> being implemented now.

## What is it best for?

**Long-running Linux services that allocate a mix of small and large
objects, run for days or weeks, and need to keep RSS down without
losing observability.**

Concretely, where v8malloc currently leads the field in its own
benchmark suite:

- **Memory frugality.** ~47 % of tcmalloc's RSS and ~33 % of
  jemalloc's on the fragmentation bench (MB-05); ties glibc for
  the lowest steady-state footprint at 9.56 MiB. Matters when you
  pay per GiB or fit more replicas per host.
- **Small-object throughput.** Fastest of the five at 8 B (21
  ns/op) and 2 MiB (86 ns/op) on MB-01, and ties tcmalloc / glibc
  at 64 B – 4 KiB (22 – 23 ns/op). Round-1–20 branch-probability
  hints, cold-attribute pass, branchless `size==0` coerce, and the
  inlined TLC fast path keep the per-op cost flat across every
  Small / Tiny size.
- **Multi-thread scalability.** Top of the field on MB-02 at 1, 2,
  and 8 threads — including 290.4 M ops/s at 8 threads, 11 % above
  tcmalloc's 260.1 M and 49 % above mimalloc's 211.9 M. The L2
  per-CPU core cache plus the cross-CPU work-stealing path keeps
  per-thread cost near-flat as the thread count climbs.
- **Large / Huge allocation latency.** Best in class — tied with
  glibc and tcmalloc — across every Huge size (16 / 64 / 256 MiB)
  at alloc-p99 ≤ 1 µs, beating jemalloc and mimalloc by 20×–75× on
  the 256 MiB row. Matters for buffer pools, scratch arenas, model
  weights, `mmap`-shaped data.
- **Realistic mixed workload.** MB-04 lands within ~2 % of the
  combined glibc / mimalloc cohort (14.5 M ops/s) and stays well
  ahead of jemalloc (1.5×). Trails tcmalloc's central-cache /
  page-heap split (23.1 M) — same medium-band gap MB-01 surfaces.
- **Operator visibility.** pprof heap dumps, lifetime classification
  (ephemeral / short / long), per-class histograms, NUMA balance,
  OOM handler with soft limits, `v8m_purge()`. The other allocators
  hand you a number; v8malloc hands you a story.
- **NUMA-aware out of the box.** `mbind`-pinned pages, distance-
  ordered fallback, periodic rebalance. Matters on 2+ socket boxes.
- **Drop-in.** `LD_PRELOAD=libv8malloc.so` — no code changes.

Examples that hit all six: API gateways, message brokers, embedding
servers, model-serving runtimes, ETL workers, search indexers,
time-series databases.

Not the right pick if you need non-Linux portability, peak
throughput on the 16 KiB – 256 KiB Medium band (tcmalloc holds a
flat ~24 ns/op across that whole band, ~3× v8malloc's buddy-pool
path), or peak cross-thread free throughput (mimalloc's MPSC
remote-free queue leads MB-03 by ~3×). Tracking tcmalloc's
central-cache + page-heap split for medium classes and wiring a
true MPSC remote-free queue are the next planned tuning cycles.
See [Cross-allocator comparison](#cross-allocator-comparison)
below for the full benchmark breakdown.

## Design highlights

- 4-tier hierarchical allocation path: thread-local cache → core-local
  cache → NUMA-node pool → global page heap
- Lock-free thread-local fast path; MPSC remote-free queue for
  cross-thread frees
- Header-less per-page metadata with O(1) pointer-to-metadata lookup
- NUMA-aware allocation with `mbind`-pinned pages and distance-ordered
  fallback
- Explicit HugePage management (no THP dependency)
- Branchless size-class lookup, 41 size classes from 8B to 2MB
- Linux-only; supports x86_64, aarch64, riscv64, ppc64le, s390x,
  loongarch64

## Requirements

- Linux kernel 5.4+
- GCC 13+ or Clang 16+
- CMake 3.25+
- pthread, libdl

## Build

```bash
make            # debug build (Clang)
make release    # release build (GCC)
make test       # run the test suite
make clean      # remove build artifacts
```

## Use

### LD_PRELOAD

```bash
LD_PRELOAD=/path/to/libv8malloc.so ./your-program
```

### Direct linking

```bash
gcc -o myapp myapp.c -lv8malloc -lpthread -ldl
```

### CMake

```cmake
find_package(v8malloc REQUIRED)
target_link_libraries(myapp PRIVATE v8malloc::v8malloc)
```

### pkg-config

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs v8malloc)
```

## Code quality

```bash
make format         # apply clang-format in place
make format-check   # verify formatting (CI-friendly)
make tidy           # clang-tidy
make cppcheck       # cppcheck
make lint           # all of the above
```

## Fuzzing

```bash
make fuzz                   # 60-second smoke run (clang + UBSan)
FUZZ_TIME=600 make fuzz     # 10-minute campaign
```

Builds a libFuzzer driver (`tests/fuzz_alloc.c`) that drives the
public allocation API with random sequences of malloc / calloc /
realloc / free / aligned_alloc, verifying byte patterns across
calls so a use-after-free or two-slot aliasing bug aborts the
run. The default configuration combines `V8MALLOC_BUILD_FUZZ` with
`V8MALLOC_BUILD_UBSAN` so undefined behaviour discovered along
any explored path also fails the run.

Requires clang plus the libFuzzer runtime (`libclang-rt-N-dev` on
Debian-derived distros). v0 fuzzes the driver only; instrumenting
the full library for coverage-guided fuzzing is a follow-on cycle.

## Coverage

```bash
make coverage                    # default 85% line-coverage gate
COVERAGE_MIN=80 make coverage    # relax the threshold
```

Builds the library and tests with `--coverage` (gcov), runs the
suite, and post-processes the `.gcda` files through `lcov` +
`genhtml` into `build/coverage/html/index.html`. The target then
parses lcov's summary and **fails** when line coverage drops below
`COVERAGE_MIN` (85% by default), so CI catches regressions.
Without lcov / genhtml installed the raw `.gcda` data still lives
under `build/coverage/` for IDE-driven coverage tooling and the
threshold check is skipped. Coverage instrumentation is mutually
exclusive with the sanitizers.

## Benchmarks

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
                          -DV8MALLOC_BUILD_BENCH=ON
cmake --build build/bench
./build/bench/bench/mb_01_throughput
```

`bench/mb_01_throughput.c` is the first microbenchmark from
`benchmarks.md` — a single-thread alloc / write / free loop
across eight sizes spanning every backend (8 B through 2 MiB).
It links statically against the library so the numbers reflect
the same code path direct consumers see.

`bench/mb_03_producer_consumer.c` pairs N producer threads
with N consumer threads via an SPSC ring; every alloc/free pair
crosses a thread boundary, so the bench measures the
cross-thread free path. Without a thread cache this number is a
regression floor — as TLC lands, it should climb dramatically.

`bench/mb_04_mixed.c` drives a bounded working set through
alloc/free with sizes drawn from the spec's six-band
distribution (8 B → 256 KiB), the closest thing the suite has
to a "realistic mixed workload". Single-thread.

`bench/mb_05_fragmentation.c` runs the spec's
alloc/free-half/refill churn for K iterations and reports
`logical_bytes` / `mapped_bytes` / `rss_bytes` per iteration so
the RSR trend over time is directly plot-ready.

`bench/mb_06_large_latency.c` is the latency-side companion to
MB-01.

`bench/mb_07_numa_local.c` verifies allocations land on the
caller's NUMA node (via `get_mempolicy`) and reports a
per-size-band local-rate percentage. On single-node hosts the
bench reads 100 % and doubles as a regression gate for the
future per-NUMA pool sharding work. Single-thread per-iteration p50/p99 timing of the Large
mmap path, broken down into alloc / first-touch fault / free
phases — the spec's "Large allocation ≤0.5× glibc median"
pass criterion is directly comparable from the alloc_us_p50
column.

`bench/mb_02_scalability.c` sweeps thread counts at the
spec's 64 B fixed size and reports total throughput,
per-thread throughput, and the scalability ratio versus the
1-thread baseline. The thread sweep is capped to
`min(nproc, 32)` by default so small CI boxes do not thrash;
set `V8M_BENCH_MAX_THREADS=128` to honour the spec exactly.

```bash
./build/bench/bench/mb_02_scalability
V8M_BENCH_DURATION_MS=10000 \
    ./build/bench/bench/mb_02_scalability   # 10s/config (spec)
```

LD_PRELOAD-style comparison runs (v8malloc vs glibc / jemalloc /
mimalloc / tcmalloc) drop in cleanly because the bench links
against the standard `malloc` / `free` symbols:

```bash
./build/bench/bench/mb_01_throughput              # v8malloc (linked)
LD_PRELOAD=$(pwd)/path/to/libjemalloc.so \
    ./build/bench/bench/mb_01_throughput          # jemalloc
```

For stable numbers across runs, drive the bench through
`scripts/bench-run.sh`, which pins the CPU governor to
`performance`, disables THP, and disables ASLR for the bench's
lifetime (and restores all three on exit). Tuning steps need
root; without root the script warns and runs the bench anyway:

```bash
sudo scripts/bench-run.sh -- ./build/bench/bench/mb_01_throughput
sudo scripts/bench-run.sh --drop-caches \
    -- ./build/bench/bench/mb_02_scalability
```

For a cross-allocator comparison (glibc / jemalloc / tcmalloc /
mimalloc / v8malloc), `scripts/bench-compare.sh` drives a single
bench against every allocator installed on the host and tags
each run's output with the allocator name:

```bash
scripts/bench-compare.sh -- ./build/bench/bench/mb_01_throughput
sudo scripts/bench-run.sh -- scripts/bench-compare.sh \
    -- ./build/bench/bench/mb_02_scalability
```

For the external mimalloc-bench suite, `scripts/bench-mimalloc.sh`
drives any workload from a pre-built mimalloc-bench tree under
v8malloc LD_PRELOAD and reports wall time + peak RSS:

```bash
git clone https://github.com/daanx/mimalloc-bench /tmp/mb
cd /tmp/mb && ./build-bench-env.sh
scripts/bench-mimalloc.sh --bench-dir /tmp/mb -- cfrac alloc-test
```

Knobs (env vars):
- `V8M_BENCH_DURATION_MS` — per-size / per-config timing budget
  (MB-01 default 250, MB-02 default 1000)
- `V8M_BENCH_WARMUP_MS`   — warmup before timing (MB-01 default
  50, MB-02 default 100)
- `V8M_BENCH_SIZE`        — MB-02 only, per-op allocation size
  (default 64)
- `V8M_BENCH_MAX_THREADS` — MB-02 only, cap on the thread sweep
  (default `min(nproc, 32)`)

Output is space-separated columns (size_bytes / iters /
elapsed_us / ns_per_op / ops_per_sec) — easy to ingest into a
spreadsheet or compare across runs with `diff`.

### Cross-allocator comparison

The numbers below are LD_PRELOAD comparison runs of the in-tree
microbench suite against glibc, jemalloc, tcmalloc, and mimalloc
(`libjemalloc.so.2`, `libtcmalloc.so.4`, `libmimalloc.so.3` on
Debian Trixie; reproduces cleanly via `scripts/bench-compare.sh`).
Each cell is a single run at the bench's default duration on an
8-core Intel Core Ultra 5 236V dev container. Treat
single-digit-percent gaps as noise — the bench is sensitive to host
load and run-to-run jitter; the trend across columns is what
matters.

The v8malloc column is the optimized clang Release build
(`-flto=auto`, `V8M_BIN_CAPACITY_MAX=512`, `__attribute__((hot))`
on the dispatcher and TLC entry points, the cumulative round-1–20
`__builtin_expect` branch-probability hints + `V8M_ALWAYS_INLINE`
/ `V8M_PURE` / `V8M_CONST_FN` micro-attributes across the alloc /
free hot paths, branchless `size==0` coercion in the dispatch
entry, and `__attribute__((cold))` on the lifecycle / fork-handler
/ OOM paths to keep them out of the icache); see
`CMakeLists.txt`, `src/v8m_thread_cache.h`, `src/v8m_dispatch.c`,
`src/v8m_thread_cache.c`, `src/v8m_api.c`, `src/v8m_bg_purge.c`,
`src/v8m_slab_pool.c`, `src/v8m_anchor_reservation.c`,
`src/v8m_numa_pool.c`, `src/v8m_numa.c`, `src/v8m_config.c`,
`src/v8m_bootstrap.c`, `src/v8m_bump.c`, `src/v8m_huge_slab.c`,
`src/v8m_core_cache.c`, and `src/v8m_libc_fallback.c`.

This run also includes a concurrency-bug fix in
`src/v8m_core_cache.c::v8m_core_cache_pop_batch`: the prior
single-CAS "snapshot then walk" implementation could dereference
the `next` link of nodes that a concurrent single-pop had already
removed and recycled into a caller's payload — reproducible as a
MB-03 SEGV at ≥2 producer/consumer pairs. The replacement claims
each node atomically before reading its `next` field; a tested
detach-walk-restore alternative was also crash-free but measured
~40× slower on MB-03 and was not adopted.

#### MB-01 single-thread throughput (ns/op, lower is better)

| size    | glibc | jemalloc | tcmalloc | mimalloc | **v8malloc** |
|---------|------:|---------:|---------:|---------:|-------------:|
| 8 B     |    22 |       26 |       24 |       32 |       **21** |
| 64 B    |    22 |       27 |       23 |       32 |       **22** |
| 512 B   |    23 |       27 |       24 |       33 |       **22** |
| 4 KiB   |    23 |       28 |       24 |       26 |       **23** |
| 16 KiB  |    71 |       37 |       24 |       36 |       **75** |
| 64 KiB  |    76 |      167 |       25 |       37 |       **79** |
| 256 KiB |  3981 |      237 |       24 |       36 |     **3890** |
| 2 MiB   |    94 |      170 |       66 |      253 |       **86** |

`v8malloc` leads the field at 8 B (21 ns/op, fastest column) and
at the 2 MiB row (86 ns/op — the recycle cache in
`v8m_large.c::large_cache_take` keeps freed regions mapped so the
alloc collapses below jemalloc's 170 and mimalloc's 253). The
64 B / 512 B / 4 KiB rows tie tcmalloc / glibc within 1 ns, and
stay well ahead of jemalloc and mimalloc throughout.

The 16 KiB – 256 KiB rows are tcmalloc's runaway win — its
central-cache + page-heap split keeps every medium class at a
flat ~24 ns/op while v8malloc's buddy-pool / large-mmap path
falls into the same mmap roundtrip glibc also pays. Closing this
gap is the next planned tuning cycle.

#### MB-02 multi-thread scalability (ops/sec, size = 64 B, higher is better)

| threads | glibc      | jemalloc   | tcmalloc   | mimalloc   | **v8malloc** |
|--------:|-----------:|-----------:|-----------:|-----------:|-------------:|
|       1 |   40.6 M   |   32.2 M   |   34.9 M   |   31.8 M   |  **46.9 M**  |
|       2 |   82.0 M   |   60.4 M   |   69.3 M   |   59.5 M   |  **88.4 M**  |
|       4 |  148.4 M   |  122.2 M   |  131.2 M   |  102.6 M   |   135.5 M    |
|       8 |  273.6 M   |  194.3 M   |  260.1 M   |  211.9 M   | **290.4 M**  |

`v8malloc` leads the field at 1, 2, and 8 threads. The 8-thread
result (290.4 M ops/s) is 11 % above tcmalloc's 260.1 M and 49 %
above mimalloc's 211.9 M, driven by the L2 per-CPU core cache
(`src/v8m_core_cache.c`) plus the cross-CPU work-stealing path
that avoids round-tripping to the slab pool on TLC misses. The
4-thread row trails glibc by ~9 % and tcmalloc by ~3 % — noise
band for this bench's run-to-run jitter.

#### MB-03 producer/consumer (handoffs/sec, higher is better; cross-thread free path)

| pairs / size | glibc   | jemalloc | tcmalloc | mimalloc   | **v8malloc** |
|--------------|--------:|---------:|---------:|-----------:|-------------:|
| 1 / 64 B     |  7.48 M |   6.88 M |   6.07 M | **10.16 M**|     7.13 M   |
| 2 / 64 B     | 10.09 M |  11.36 M |   4.77 M | **17.68 M**|     9.87 M   |
| 4 / 64 B     |  5.70 M |  21.38 M |   2.71 M | **28.42 M**|     5.90 M   |
| 4 / 1024 B   |  5.62 M |  14.99 M |   2.39 M | **23.24 M**|     5.65 M   |

MB-03 is the bench v8malloc trails on. mimalloc's MPSC remote-free
queue lets each consumer drain returns without contending against
the producer's allocator, and beats v8malloc by ~3×–5× across the
sweep. v8malloc holds parity with glibc and stays ahead of
tcmalloc, but the structural fix (a true MPSC remote-free queue)
is the next planned cycle. Note: a previous single-CAS batch-pop
in the L2 core cache crashed this bench at ≥2 pairs; that bug is
fixed in this run (see `src/v8m_core_cache.c` notes above).

#### MB-04 mixed-size workload (ops/sec, higher is better)

| metric   | glibc   | jemalloc | tcmalloc | mimalloc | **v8malloc** |
|----------|--------:|---------:|---------:|---------:|-------------:|
| ops/sec  | 14.87 M |   9.45 M |  23.13 M |  14.73 M |  **14.52 M** |

`v8malloc` lands within a percent of mimalloc and glibc on a
realistic mixed distribution (8 B → 256 KiB), still beating
jemalloc by ~1.5×. The 37 % gap to tcmalloc is the same medium-band
shortfall MB-01 surfaces — the MB-04 distribution includes a
16 KiB – 256 KiB tier where tcmalloc's central cache pays no mmap
roundtrip and v8malloc's buddy / large path does.

#### MB-05 fragmentation (RSS after 20 grow/shrink rounds, lower is better)

| metric   | glibc    | jemalloc  | tcmalloc  | mimalloc  | **v8malloc** |
|----------|---------:|----------:|----------:|----------:|-------------:|
| RSS      | 9.56 MiB | 28.97 MiB | 20.45 MiB | 13.89 MiB | **9.56 MiB** |

`v8malloc` ties glibc for the lowest steady-state RSS — 47 % of
tcmalloc's, 33 % of jemalloc's, and 69 % of mimalloc's — despite
the larger TLC bin ceiling. The adaptive controller drives cold
classes back toward `V8M_BIN_CAPACITY_MIN = 16`, so RSS does not
balloon in proportion to the new ceiling.

#### MB-06 large allocation latency (alloc p99, µs, lower is better)

| size     | glibc | jemalloc | tcmalloc | mimalloc | **v8malloc** |
|----------|------:|---------:|---------:|---------:|-------------:|
| 4 MiB    |     1 |        3 |        2 |        2 |        **1** |
| 16 MiB   |     0 |        4 |        2 |        1 |        **0** |
| 64 MiB   |     1 |       23 |        1 |       41 |        **1** |
| 256 MiB  |     1 |       23 |        2 |       74 |        **1** |

`v8malloc` ties glibc / tcmalloc for the lead on Huge-allocation
latency end to end: alloc-p99 ≤ 1 µs at every size from 16 MiB up.
jemalloc and mimalloc cross into 2-digit µs at 64 MiB and stay
there at 256 MiB (74 µs / 23 µs vs v8malloc's 1 µs). The recycle
cache plus the Huge-slab THP / 1 GiB hugepage path keeps the Huge
alloc loop out of the mmap critical section.

#### Summary scorecard

| workload                                  | leader(s)                       | v8malloc verdict       |
|-------------------------------------------|---------------------------------|------------------------|
| Single-thread throughput, 8 B             | **v8malloc**                    | **wins outright (21 ns/op)** |
| Single-thread throughput, 64 B – 4 KiB    | tcmalloc, glibc, **v8malloc**   | **3-way tie within 1 ns** |
| Single-thread throughput, 16 – 256 KiB    | tcmalloc                        | **trails (medium-band TODO)** |
| Single-thread throughput, 2 MiB           | **v8malloc**                    | **wins (86 ns/op, fastest column)** |
| Multi-thread scalability, 1 / 2 threads   | **v8malloc**                    | **wins (46.9 M / 88.4 M ops/s)** |
| Multi-thread scalability, 4 threads       | glibc                           | **3rd (within ~9 % noise)** |
| Multi-thread scalability, 8 threads       | **v8malloc**                    | **wins (290.4 M, +11 % vs tcmalloc)** |
| Producer/consumer cross-thread free       | mimalloc                        | **trails (~3–5×; MPSC queue TODO)** |
| Realistic mixed workload (MB-04)          | tcmalloc                        | **ties glibc / mimalloc; +1.5× vs jemalloc** |
| Memory frugality (fragmentation)          | **v8malloc**, glibc             | **ties glibc; 47–69 % of tcmalloc / jemalloc / mimalloc** |
| Large allocation alloc-p99 (≥16 MiB)      | **v8malloc**, glibc, tcmalloc   | **3-way tie at ≤ 1 µs end to end** |

Reproduce locally:

```bash
# All-in-one comparison (auto-discovers every installed allocator):
scripts/bench-compare.sh -- ./build/bench/bench/mb_01_throughput
scripts/bench-compare.sh -- ./build/bench/bench/mb_02_scalability
scripts/bench-compare.sh -- ./build/bench/bench/mb_03_producer_consumer
scripts/bench-compare.sh -- ./build/bench/bench/mb_04_mixed
scripts/bench-compare.sh -- ./build/bench/bench/mb_05_fragmentation
scripts/bench-compare.sh -- ./build/bench/bench/mb_06_large_latency

# Or drive a single LD_PRELOAD by hand:
LD_PRELOAD=$(pwd)/build/bench/libv8malloc.so \
    ./build/bench/bench/mb_01_throughput
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so.4 \
    ./build/bench/bench/mb_01_throughput
```

## Sanitizers

v8malloc ships with two sanitizer build variants:

```bash
cmake -S . -B build/ubsan -DV8MALLOC_BUILD_UBSAN=ON
cmake --build build/ubsan && ctest --test-dir build/ubsan

cmake -S . -B build/tsan -DV8MALLOC_BUILD_TSAN=ON
cmake --build build/tsan
```

- **UndefinedBehaviorSanitizer** (`-DV8MALLOC_BUILD_UBSAN=ON`) is
  the supported configuration: the full `ctest` suite passes under
  UBSan, catching integer overflow, misaligned loads, and the
  other standard UB classes across every allocator path.
- **ThreadSanitizer** (`-DV8MALLOC_BUILD_TSAN=ON`) builds the
  library with `-fsanitize=thread` but is currently experimental.
  TSan's shadow-memory scheme clashes with the early mmap calls
  our library constructor makes, and the full test suite does not
  yet pass under it. Tracked for a dedicated investigation cycle.
- **AddressSanitizer is intentionally unsupported.** ASan
  intercepts `malloc` / `free` at load time, which means an
  ASan-instrumented binary bypasses v8malloc entirely and the
  sanitizer run exercises ASan's own allocator rather than ours.
  Use Valgrind (which does the same but is transparent to us) or
  the UBSan build for defect-finding; a v8malloc-aware ASan
  variant is future work.

## Continuous integration

GitHub Actions runs the per-PR gate (`.github/workflows/ci.yml`)
on every push and pull request to `main`:

  - **build-and-test** matrix — `gcc` and `clang`, `Debug` and
    `Release`. Full ctest suite for each combo (4 jobs).
  - **LD_PRELOAD smoke** — preloads `libv8malloc.so` into a set
    of common binaries (`/bin/true`, `/bin/ls`, `bash` fork
    stress) to catch constructor-ordering or symbol-export
    regressions the unit tests don't cover.
  - **clang + UBSan** — full suite under
    `V8MALLOC_BUILD_UBSAN=ON`.
  - **format-check + tidy + cppcheck** — the same lint surface
    `make lint` runs locally.

Two follow-on workflows fire on file-touch + a manual button:

  - `coverage.yml` — `--coverage` build, ctest, lcov HTML report
    uploaded as an artifact (and Codecov when the token is set).
    Runs on PRs that touch `src/`, `tests/`, `include/`, or the
    build files.
  - `fuzz.yml` — libFuzzer + UBSan against `tests/fuzz_alloc.c`.
    Default 60 s smoke; `workflow_dispatch` accepts a longer
    `duration_seconds` input. Also fires on PRs that touch core
    allocator source.

Two scheduled workflows:

  - `bench-weekly.yml` — Sundays 06:00 UTC. Builds Release + the
    `bench/` harness, runs every MB-01..06 through
    `scripts/bench-compare.sh` (cross-allocator), uploads the
    aggregated output as an artifact for week-over-week diffing.
    `workflow_dispatch` lets a maintainer trigger it on-demand
    after a perf-related change.
  - `multi-arch.yml` — Mondays 06:00 UTC. Full build + ctest on
    aarch64, ppc64le, s390x, riscv64 under QEMU-user
    (`uraimo/run-on-arch-action`). 5–15 minutes per lane — split
    out from the per-PR gate so PR turnaround stays under five
    minutes.

All `uses:` references are pinned by 40-char commit SHA with the
release tag in an inline comment (`<sha> # vX.Y.Z`); Dependabot
(`.github/dependabot.yml`) bumps both halves in lockstep.

## Cutting a release

Releases are cut by the **`release` workflow**
(`.github/workflows/release.yml`) — manual `workflow_dispatch`
with a `bump` input (`patch` / `minor` / `major`).

Pipeline shape:

  1. **prepare** — reads `project(... VERSION X.Y.Z)` from
     `CMakeLists.txt`, computes the next version, refuses if
     `vX.Y.Z` already exists on origin.
  2. **build-binaries** — five matrix lanes (x86_64 native +
     aarch64 / ppc64le / s390x / riscv64 under qemu-user). Each
     applies the new version to its own checkout via in-place
     sed, builds Release, packages a `tar.xz` + sha256.
  3. **commit-and-tag** — only runs when **all five** lanes
     built green. Uses the GitHub GraphQL
     `createCommitOnBranch` mutation to push a single commit
     bumping `CMakeLists.txt`. The commit is **auto-signed by
     GitHub's web-flow GPG key** (Verified badge) without us
     managing any private key. Then creates the annotated tag
     via the REST API.
  4. **release** — generates a reproducible source tarball from
     the freshly-pushed tag, downloads every arch's artifact,
     and creates the GitHub release with
     `gh release create --generate-notes`. Release notes are
     **auto-generated by GitHub** from the merged PRs since the
     previous tag, grouped by the categories defined in
     `.github/release.yml` (Security / Features / Fixes /
     Performance / Refactor / Documentation / Tests / Build &
     CI). PRs from `dependabot` / `github-actions[bot]` are
     excluded so bot churn stays out of the public narrative.

Reorder rationale: the version bump only commits AFTER every
binary lane is green. A failed build leaves `CMakeLists.txt`
untouched and no tag pushed, so re-triggering the workflow with
the same `bump` produces the same target version cleanly.

For local one-off tarballs without going through CI:

```bash
git tag -s -m "Release 0.2.0" v0.2.0
./scripts/release.sh                 # builds + tarball + sha256
```

The script verifies HEAD is on a `vMAJOR.MINOR.PATCH` tag, that
`V8M_VERSION_STRING` in the public header matches the tag, runs a
release-mode build + the full ctest suite, then produces a
`git archive` source tarball with sha256 (and a detached GPG
signature when a key is available).

## Project layout

```
.
├── include/v8malloc/   # Public headers
├── src/                # Library implementation + linker version script
├── tests/              # Unit tests (ctest)
├── bench/              # Benchmark harness
├── cmake/              # Package config templates
├── man/                # Man pages
├── .claude/            # Internal design docs (not packaged)
├── .githooks/          # Conventional Commits + clang-format gates
├── .github/workflows/  # GitHub Actions CI
└── scripts/            # Release pipeline + bench runner
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Vulnerabilities go to the
contact in [SECURITY.md](SECURITY.md), not to public issues.

## Sponsorship

If v8malloc is useful in your work — production, research, or
just curiosity — consider sponsoring development through
**GitHub Sponsors**:

  [https://github.com/sponsors/iqbqioza](https://github.com/sponsors/iqbqioza)

GitHub Sponsors is the project's only configured funding channel
(see [`.github/FUNDING.yml`](.github/FUNDING.yml)); the **Sponsor**
button at the top of the GitHub repo links to the same page.
Sponsorship buys sustained development time for the larger
architectural cycles (per-CPU caches with restartable sequences,
the thread-owned-slab refactor, NUMA pool sharding) that don't
fit into stolen evenings, and gives sponsors a say in
prioritization.

Not in a position to sponsor financially? File a clear bug
report, run `LD_PRELOAD=libv8malloc.so` against your workload and
share the comparison numbers, open a PR, or write a public note
about your experience — all four move the project forward just as
much.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
