#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Drive the mimalloc-bench external suite under v8malloc via
# LD_PRELOAD. mimalloc-bench (https://github.com/daanx/mimalloc-bench)
# is the de-facto external benchmark suite for malloc replacements —
# alloc-test, cfrac, espresso, larsonN, mstressN, glibc-bench, redis,
# rptest, etc. It builds each workload as a standalone binary that
# resolves malloc/free dynamically, so LD_PRELOAD substitution is the
# right hand-off mechanism.
#
# This script is a thin wrapper, not a re-implementation of
# mimalloc-bench's own driver:
#
#   - It expects a built mimalloc-bench tree (run their
#     `build-bench-env.sh` once first to populate `out/bench/sys/`)
#   - It runs each requested workload with LD_PRELOAD set to our SO
#   - It measures wall time + peak RSS via `/usr/bin/time -v`, so
#     numbers are comparable across allocators
#   - One block of output per workload, allocator-tagged so a
#     run-and-diff loop is straightforward
#
# Usage:
#   scripts/bench-mimalloc.sh --bench-dir /path/to/mimalloc-bench \
#                             -- alloc-test cfrac espresso
#   scripts/bench-mimalloc.sh --bench-dir /path/to/mimalloc-bench \
#                             --reps 3 -- larsonN
#
# The `--` separator terminates flag parsing; everything after it is
# a workload name. With no workloads listed, the script lists what
# `out/bench/sys/` contains and exits.
#
# Set V8M_V8MALLOC_SO to override the path to libv8malloc.so;
# otherwise the script looks for ./build/release/libv8malloc.so,
# then ./build/bench/libv8malloc.so, then ./build/dev/libv8malloc.so.

set -euo pipefail

BENCH_DIR=""
REPS=1

while (( "$#" )); do
	case "$1" in
		--bench-dir)
			BENCH_DIR="$2"
			shift 2
			;;
		--reps)
			REPS="$2"
			shift 2
			;;
		--)
			shift
			break
			;;
		-h|--help)
			sed -n '4,33p' "$0"
			exit 0
			;;
		*)
			break
			;;
	esac
done

if [[ -z "$BENCH_DIR" ]]; then
	echo "bench-mimalloc: --bench-dir <path> is required" >&2
	echo "  e.g.: bench-mimalloc.sh --bench-dir /opt/mimalloc-bench -- cfrac" >&2
	exit 2
fi

if [[ ! -d "$BENCH_DIR" ]]; then
	echo "bench-mimalloc: '$BENCH_DIR' is not a directory" >&2
	exit 2
fi

WORKLOAD_DIR="$BENCH_DIR/out/bench/sys"
if [[ ! -d "$WORKLOAD_DIR" ]]; then
	echo "bench-mimalloc: '$WORKLOAD_DIR' missing — run mimalloc-bench's" >&2
	echo "                ./build-bench-env.sh first to build the workloads." >&2
	exit 2
fi

# Resolve v8malloc's SO. Same probe order as bench-compare.sh.
resolve_v8malloc() {
	if [[ -n "${V8M_V8MALLOC_SO:-}" && -f "${V8M_V8MALLOC_SO}" ]]; then
		echo "${V8M_V8MALLOC_SO}"
		return 0
	fi
	for cand in \
		"$(pwd)/build/release/libv8malloc.so" \
		"$(pwd)/build/bench/libv8malloc.so" \
		"$(pwd)/build/dev/libv8malloc.so"; do
		if [[ -f "$cand" ]]; then
			echo "$cand"
			return 0
		fi
	done
	echo ""
}

V8MALLOC_SO=$(resolve_v8malloc)
if [[ -z "$V8MALLOC_SO" ]]; then
	echo "bench-mimalloc: could not find libv8malloc.so" >&2
	echo "                set V8M_V8MALLOC_SO or build the library first" >&2
	exit 2
fi

# With no workloads listed, print the catalogue and stop. Saves a
# trip to the docs when a maintainer is not sure what they have.
if (( $# == 0 )); then
	echo "bench-mimalloc: no workloads passed — listing what '$WORKLOAD_DIR' has:"
	find "$WORKLOAD_DIR" -maxdepth 1 -type f -executable \
		-printf '  %f\n' | sort
	echo ""
	echo "Pass workload names after '--' to run them, e.g.:"
	echo "  $0 --bench-dir $BENCH_DIR -- cfrac alloc-test"
	exit 0
fi

# /usr/bin/time -v emits the "Maximum resident set size (kbytes)"
# line GNU coreutils' bash builtin doesn't, so we need the binary.
TIME_BIN="/usr/bin/time"
if [[ ! -x "$TIME_BIN" ]]; then
	echo "bench-mimalloc: /usr/bin/time not found (apt-get install time)" >&2
	exit 2
fi

run_workload() {
	local workload_name="$1"
	local workload_path="$WORKLOAD_DIR/$workload_name"
	if [[ ! -x "$workload_path" ]]; then
		printf '\n=== %s: not found at %s ===\n' \
			"$workload_name" "$workload_path"
		return 0
	fi
	printf '\n=== %s (LD_PRELOAD=%s) ===\n' \
		"$workload_name" "$V8MALLOC_SO"
	for (( rep=1; rep <= REPS; rep++ )); do
		if (( REPS > 1 )); then
			printf -- '--- run %d of %d ---\n' "$rep" "$REPS"
		fi
		# Each rep gets its own scoped env so LD_PRELOAD does
		# not leak across allocator switches if the caller is
		# chaining this with bench-compare.sh.
		LD_PRELOAD="$V8MALLOC_SO" "$TIME_BIN" -v \
			"$workload_path" 2>&1 | \
			grep -E "^(	|Maximum|Elapsed|Major|Minor|Voluntary|Involuntary)"
	done
}

printf 'bench-mimalloc: bench-dir=%s reps=%d\n' "$BENCH_DIR" "$REPS"
printf 'bench-mimalloc: SO=%s\n' "$V8MALLOC_SO"

for workload in "$@"; do
	run_workload "$workload"
done
