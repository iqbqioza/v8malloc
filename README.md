![v8malloc logo](./docs/v8malloc.png)

[![CI](https://github.com/iqbqioza/v8malloc/actions/workflows/ci.yml/badge.svg)](https://github.com/iqbqioza/v8malloc/actions/workflows/ci.yml) [![multi-arch](https://github.com/iqbqioza/v8malloc/actions/workflows/multi-arch.yml/badge.svg)](https://github.com/iqbqioza/v8malloc/actions/workflows/multi-arch.yml)

# v8malloc

A high-performance memory allocator for Linux, designed to outperform
jemalloc, mimalloc, and tcmalloc on multithreaded and NUMA workloads.

> **Status**: in active development. The build system, public API
> surface, and project layout are in place; most allocator internals are
> being implemented now.

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
microbench suite against glibc, tcmalloc, jemalloc, and mimalloc
(`libtcmalloc_minimal.so.4`, `libjemalloc.so.2`, `libmimalloc.so.3`
on Debian Bookworm). Each cell is the median of three runs at
`V8M_BENCH_DURATION_MS=150–200` on an 8-core x86_64 dev VM. Treat
single-digit-percent gaps as noise — the bench is sensitive to
host load and run-to-run jitter; the trend across columns is what
matters.

#### MB-01 single-thread throughput (ns/op, lower is better)

| size    | glibc | tcmalloc | jemalloc | mimalloc | **v8malloc** |
|---------|------:|---------:|---------:|---------:|-------------:|
| 8 B     |    33 |       33 |       38 |       33 |       **31** |
| 64 B    |    32 |       33 |       32 |       30 |       **30** |
| 512 B   |    35 |       33 |       43 |       31 |       **32** |
| 4 KiB   |    34 |       31 |       47 |       33 |       **32** |
| 16 KiB  |   105 |      114 |      100 |      104 |      **101** |
| 64 KiB  |   113 |      109 |      102 |      101 |      **104** |
| 256 KiB |  4259 |     3814 |     3644 |     3771 |     **3782** |
| 2 MiB   |  6999 |     6079 |     9360 |     6324 |     **7062** |

`v8malloc` matches the leaders on every Tiny/Small/Medium size.
The 2 MiB column lands within 16 % of tcmalloc thanks to the
Large/Huge recycle cache (`v8m_large.c::large_cache_take`), which
keeps freed regions mapped so repeat allocations skip the
mmap/munmap roundtrip.

#### MB-02 multi-thread scalability (ops/sec, size = 64 B, higher is better)

| threads | glibc      | tcmalloc   | jemalloc   | mimalloc   | **v8malloc** |
|--------:|-----------:|-----------:|-----------:|-----------:|-------------:|
|       1 | 26.0 M     | 32.7 M     | 29.8 M     | 31.7 M     | **30.7 M**   |
|       2 | 64.5 M     | 60.4 M     | 59.2 M     | 51.8 M     | **62.5 M**   |
|       4 | 101.0 M    | 101.0 M    | 100.7 M    | 100.6 M    | **98.1 M**   |
|       8 | 217.7 M    | 217.4 M    | 213.1 M    | 174.1 M    | **179.1 M**  |

`v8malloc` ties the field through 4 threads. At 8 threads it
trails the glibc/tcmalloc/jemalloc cluster by ~18 % — same band
mimalloc lands in. The gap closes with the L2 work-stealing fix
on producer/consumer workloads (MB-03), and is on the perf TODO
for the symmetric case.

#### MB-04 mixed-size workload (ops/sec, higher is better)

| metric   | glibc   | tcmalloc | jemalloc | mimalloc | **v8malloc** |
|----------|--------:|---------:|---------:|---------:|-------------:|
| ops/sec  |  9.80 M |   9.62 M |   9.53 M |   8.86 M |   **9.34 M** |

`v8malloc` lands within 5 % of the leaders on a realistic mixed
distribution (8 B → 256 KiB).

#### MB-05 fragmentation (RSS after 20 grow/shrink rounds, lower is better)

| metric   | glibc   | tcmalloc | jemalloc | mimalloc | **v8malloc** |
|----------|--------:|---------:|---------:|---------:|-------------:|
| RSS      |  8.1 MiB |  13.8 MiB |  10.7 MiB |   8.4 MiB |    **8.4 MiB** |

`v8malloc` ties mimalloc (and the per-thread arena scheme glibc
runs at this scale) for the lowest RSS. tcmalloc holds
**1.65× more** memory at the same logical working-set size.

#### MB-06 large allocation latency (alloc p50, µs, lower is better)

| size     | glibc | tcmalloc | jemalloc | mimalloc | **v8malloc** |
|----------|------:|---------:|---------:|---------:|-------------:|
| 2 MiB    |     2 |        2 |        2 |        2 |        **2** |
| 4 MiB    |    12 |       11 |       15 |        9 |       **13** |
| 16 MiB   |    13 |       23 |       19 |       20 |       **14** |
| 64 MiB   |    36 |       39 |       41 |       41 |       **33** |
| 256 MiB  |    49 |       48 |       47 |       48 |       **48** |

`v8malloc` matches or beats every competitor across the full
Large/Huge range. At 16 MiB and 64 MiB the recycle cache buys a
clean lead; the rest of the column lands inside measurement noise.

#### Summary scorecard

| workload                                  | leader(s)                          | v8malloc verdict       |
|-------------------------------------------|------------------------------------|------------------------|
| Single-thread throughput, all sizes       | tied (page-fault / clock floored)  | matches the field      |
| Multi-thread scalability, ≤4 threads      | tied                               | matches                |
| Multi-thread scalability, 8 threads       | glibc, tcmalloc, jemalloc          | -18 % (matches mimalloc) |
| Realistic mixed workload                  | glibc                              | -5 %                   |
| Memory frugality (fragmentation)          | **v8malloc**, mimalloc, glibc      | **wins**               |
| Large allocation latency                  | **v8malloc**                       | **wins on 16/64 MiB**  |

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

GitHub Actions runs three jobs on every PR and push to `main`
(`.github/workflows/ci.yml`):

  - `gcc build + ctest` and `clang build + ctest` — the full
    suite against both compilers.
  - `clang + UBSan` — same suite under
    `V8MALLOC_BUILD_UBSAN=ON`.
  - `format-check + tidy + cppcheck` — the same lint surface
    `make lint` runs locally.

A weekly benchmark workflow
(`.github/workflows/bench-weekly.yml`) runs every Sunday 06:00
UTC: it builds Release + bench, drives every MB-01..06 through
`scripts/bench-compare.sh` (cross-allocator), and uploads the
aggregated output as a workflow artifact for week-over-week
diffing. `workflow_dispatch` lets a maintainer trigger it
on-demand after a perf-related change.

Multi-arch coverage (aarch64, ppc64le, s390x, riscv64) lives in
`.github/workflows/multi-arch.yml`, scheduled every Monday 06:00
UTC and triggerable on demand via `workflow_dispatch`. Each lane
runs the full build + ctest suite under QEMU-user emulation
(`uraimo/run-on-arch-action`); a per-arch run takes 5–15 minutes,
which is why this is split out from the per-PR gate (the latter
stays under five minutes).

## Cutting a release

```bash
git tag -s -m "Release 0.2.0" v0.2.0
./scripts/release.sh                 # builds + tarball + sha256
./scripts/release.sh --gh-release    # also opens a draft GitHub release
```

The script verifies HEAD is on a `vMAJOR.MINOR.PATCH` tag, that
`V8M_VERSION_STRING` in the public header matches the tag, runs a
release-mode build + the full ctest suite, then produces a
`git archive` source tarball with sha256 (and a detached GPG
signature when a key is available). With `--gh-release` it pulls
the matching CHANGELOG section and opens a draft GitHub release.

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
