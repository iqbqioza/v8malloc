![v8malloc logo](./docs/v8malloc.png)

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

## Coverage

```bash
make coverage   # configure + build + ctest + lcov + genhtml
```

Builds the library and tests with `--coverage` (gcov) and runs
the suite. If `lcov` and `genhtml` are installed, the script
post-processes the `.gcda` files into an HTML report under
`build/coverage/html/index.html`. Without lcov / genhtml the raw
`.gcda` data lives under `build/coverage/` for IDE-driven coverage
tooling. Coverage instrumentation is mutually exclusive with the
sanitizers.

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
└── .githooks/          # Conventional Commits + clang-format gates
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Vulnerabilities go to the
contact in [SECURITY.md](SECURITY.md), not to public issues.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
