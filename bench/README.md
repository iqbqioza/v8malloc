# Benchmark harness

Microbenchmarks and macrobenchmarks for v8malloc, specified in
[`../.claude/docs/benchmarks.md`](../.claude/docs/benchmarks.md).

The harness binaries will be added here as the corresponding allocator
paths come online. Build them with:

```bash
cmake -S . -B build/bench -DV8MALLOC_BUILD_BENCH=ON
cmake --build build/bench
```

When running benchmarks, pin the CPU governor to `performance`, disable
transparent huge pages (so v8malloc's explicit HugePage path is the one
under test), and disable ASLR for stable numbers — the runner script
committed here handles all three.
