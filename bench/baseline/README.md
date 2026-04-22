# Bench baselines

Per-bench baseline files consumed by `scripts/bench-regression-check.sh`
and the `bench-weekly` GitHub Actions workflow.

## How to establish a baseline

Run the bench in the same configuration the CI workflow uses, then
capture:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DV8MALLOC_BUILD_BENCH=ON
cmake --build build/bench --parallel

V8M_BENCH_DURATION_MS=1000 \
  scripts/bench-regression-check.sh --capture \
  build/bench/bench/mb_01_throughput \
  bench/baseline/mb_01_throughput.txt

git add bench/baseline/mb_01_throughput.txt
git commit -m "bench: Capture baseline for MB-01"
```

The CI workflow's regression-check step will pick up the file on
the next run and start gating against it (currently
`continue-on-error: true` — promote to a hard gate once the
baseline has stabilised across a few weekly runs).

## How regressions are detected

`bench-regression-check.sh` compares each row's `ops_per_sec` (or
`ns_per_op`/`latency_us` fallback) to the matching row in the
baseline. A row regresses when the rate drops by more than the
threshold (`BENCH_REGRESSION_THRESHOLD_PCT`, default 5 %). Improved
and stable rows are reported but do not fail the check.

## Capturing baselines on multiple hosts

Baselines are workload + host specific. The committed baseline
file is the CI runner's baseline; running locally on a different
machine will show large absolute deltas that are not real
regressions. Use the local baseline workflow only for one-off
regression hunts; do not commit dev-machine baselines.
