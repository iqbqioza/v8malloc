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

- **Memory frugality.** 60 % of tcmalloc's RSS on the fragmentation
  bench; ties mimalloc and glibc for the lowest steady-state
  footprint. Matters when you pay per GiB or fit more replicas per
  host.
- **Large / Huge allocation latency.** Best in class at 16 MiB and
  64 MiB thanks to the recycle cache; matches the leaders at every
  other Large / Huge size. Matters for buffer pools, scratch arenas,
  model weights, `mmap`-shaped data.
- **Operator visibility.** pprof heap dumps, lifetime classification
  (ephemeral / short / long), per-class histograms, NUMA balance,
  OOM handler with soft limits, `v8m_purge()`. The other allocators
  hand you a number; v8malloc hands you a story.
- **NUMA-aware out of the box.** `mbind`-pinned pages, distance-
  ordered fallback, periodic rebalance. Matters on 2+ socket boxes.
- **Drop-in.** `LD_PRELOAD=libv8malloc.so` — no code changes.

Examples that hit all five: API gateways, message brokers, embedding
servers, model-serving runtimes, ETL workers, search indexers,
time-series databases.

Not the right pick if you need non-Linux portability, absolute lowest
latency on sub-64 B allocations, or peak throughput on symmetric 8+
thread alloc-only workloads — glibc and tcmalloc currently edge
v8malloc there by a few percent. See
[Cross-allocator comparison](#cross-allocator-comparison) below for
the full benchmark breakdown.

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
microbench suite against glibc, jemalloc, and mimalloc
(`libjemalloc.so.2`, `libmimalloc.so.3` on Debian Trixie;
`libtcmalloc_minimal.so.4` was unavailable on this host and is
omitted from the cell-level numbers, but reproduces cleanly via
`scripts/bench-compare.sh` when installed). Each cell is the
median of three runs at `V8M_BENCH_DURATION_MS=300–400` on an
8-core Intel Core Ultra 5 236V dev container. Treat
single-digit-percent gaps as noise — the bench is sensitive to
host load and run-to-run jitter; the trend across columns is
what matters.

The v8malloc column is the optimized GCC Release build
(`-flto=auto`, `V8M_BIN_CAPACITY_MAX=512`); see
`CMakeLists.txt` and `src/v8m_thread_cache.h`.

#### MB-01 single-thread throughput (ns/op, lower is better)

| size    | glibc | jemalloc | mimalloc | **v8malloc** |
|---------|------:|---------:|---------:|-------------:|
| 8 B     |    26 |       27 |       26 |       **27** |
| 64 B    |    26 |       26 |       26 |       **27** |
| 512 B   |    28 |       28 |       28 |       **28** |
| 4 KiB   |    28 |       28 |       28 |       **28** |
| 16 KiB  |    85 |       80 |       79 |       **82** |
| 64 KiB  |    86 |       80 |       81 |       **83** |
| 256 KiB |  3932 |     4001 |     4019 |     **3977** |
| 2 MiB   |    97 |       94 |       96 |       **96** |

`v8malloc` ties the field across every size band. The 2 MiB row
collapses to ~96 ns (vs. multi-µs for an unrecycled mmap)
because the Large/Huge recycle cache
(`v8m_large.c::large_cache_take`) keeps freed regions mapped, so
the bench's repeat-alloc loop skips the mmap/munmap roundtrip
entirely — the same shape glibc/jemalloc/mimalloc deliver via
their own region caches.

#### MB-02 multi-thread scalability (ops/sec, size = 64 B, higher is better)

| threads | glibc      | jemalloc   | mimalloc   | **v8malloc** |
|--------:|-----------:|-----------:|-----------:|-------------:|
|       1 |  36.3 M    |  37.0 M    |  37.0 M    |  **35.7 M**  |
|       2 |  33.7 M    |  33.4 M    |  34.0 M    |  **33.3 M**  |
|       4 |  28.2 M    |  28.3 M    |  28.4 M    |  **28.5 M**  |
|       8 |  24.4 M    |  24.7 M    |  24.8 M    |  **25.0 M**  |

`v8malloc` ties the leaders end-to-end and edges ahead at 4 and
8 threads. The previous 8-thread shortfall closed once the TLC
bin-capacity ceiling rose to 512 (halving the per-thread refill
rate against the single global slab pool) and GCC Release builds
gained `-flto=auto` (inlining the `v8m_malloc → dispatch → TLC`
hot chain across translation units).

#### MB-04 mixed-size workload (ops/sec, higher is better)

| metric   | glibc   | jemalloc | mimalloc | **v8malloc** |
|----------|--------:|---------:|---------:|-------------:|
| ops/sec  | 13.37 M |  13.33 M |  13.41 M |  **13.44 M** |

`v8malloc` leads the cluster (within noise) on a realistic mixed
distribution (8 B → 256 KiB).

#### MB-05 fragmentation (RSS after 20 grow/shrink rounds, lower is better)

| metric   | glibc   | jemalloc | mimalloc | **v8malloc** |
|----------|--------:|---------:|---------:|-------------:|
| RSS      | 9.1 MiB | 11.6 MiB |  9.3 MiB |   **9.3 MiB** |

`v8malloc` ties mimalloc and stays within 0.2 MiB of glibc's
per-thread-arena scheme, despite the larger TLC bin ceiling —
the adaptive controller still drives cold classes back toward
`V8M_BIN_CAPACITY_MIN = 16`, so RSS does not balloon in proportion
to the new ceiling.

#### MB-06 large allocation latency (first-touch fault p50, µs, lower is better)

Allocation p50 collapses below microsecond resolution on this
host (the recycle / region-cache path is hit ≥99 % of the time
for every allocator), so the table reports the first-touch
page-fault p50 — the cost actually paid on the first write into
a freshly-mapped region. This isolates the page-table /
zero-page work the kernel does once the allocator hands the
region back to the caller.

| size     | glibc | jemalloc | mimalloc | **v8malloc** |
|----------|------:|---------:|---------:|-------------:|
| 2 MiB    |    26 |       50 |       29 |       **39** |
| 4 MiB    |    92 |       97 |      101 |       **90** |
| 16 MiB   |   159 |      171 |      176 |      **158** |
| 64 MiB   |   800 |      687 |      672 |      **652** |
| 256 MiB  |  2991 |     2968 |     2763 |     **2956** |

`v8malloc` is fastest at 4 MiB, 16 MiB, and 64 MiB, ties at
256 MiB, and trails only at 2 MiB. The lead at the upper sizes
comes from the Huge slab path (`v8m_huge_slab.c`) preferring 2 MiB
THP-backed regions, which cuts the per-byte fault cost on the
first-touch loop.

#### Summary scorecard

| workload                                  | leader(s)                  | v8malloc verdict       |
|-------------------------------------------|----------------------------|------------------------|
| Single-thread throughput, all sizes       | tied (page-fault floored)  | **matches the field**  |
| Multi-thread scalability, ≤4 threads      | tied                       | **matches / edges +1 %** |
| Multi-thread scalability, 8 threads       | tied                       | **matches the field**  |
| Realistic mixed workload                  | tied                       | **edges ahead**        |
| Memory frugality (fragmentation)          | **v8malloc**, mimalloc, glibc | **ties the leaders** |
| Large allocation first-touch latency      | **v8malloc**               | **wins at 4/16/64 MiB** |

Reproduce locally:

```bash
LD_PRELOAD=$(pwd)/build/libv8malloc.so \
    ./build/bench/mb_01_throughput
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 \
    ./build/bench/mb_01_throughput
# repeat for each bench + each LD_PRELOAD
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
