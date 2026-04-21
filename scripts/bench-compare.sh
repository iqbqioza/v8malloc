#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run a single v8malloc benchmark against the reference allocator
# zoo (glibc / jemalloc / tcmalloc / mimalloc / v8malloc) and emit a
# per-allocator block of output, so the numbers are directly
# comparable across a single host's current load and kernel state.
# Every allocator the script knows about that is installed on the
# system is exercised; the rest are skipped with a note so a sparse
# dev box (only v8malloc + glibc) still gets a two-way compare.
#
# Usage:
#   scripts/bench-compare.sh -- ./build/bench/bench/mb_01_throughput
#   scripts/bench-compare.sh --only jemalloc,v8malloc \
#       -- ./build/bench/bench/mb_02_scalability
#   scripts/bench-compare.sh --reps 3 \
#       -- ./build/bench/bench/mb_06_large_latency
#
# Environment variables the wrapped bench uses
# (V8M_BENCH_DURATION_MS etc.) pass through unchanged — the script
# simply sets LD_PRELOAD per run and executes the command. For
# stable numbers combine with `scripts/bench-run.sh`:
#
#   sudo scripts/bench-run.sh \
#       -- scripts/bench-compare.sh -- ./build/bench/bench/mb_01_throughput
#
# Discovery: the script looks for each allocator's shared library in
# the standard library search paths (/usr/lib, /usr/local/lib, etc.)
# using `ldconfig -p`. If a distro has a non-standard path, pass it
# via the per-allocator env var:
#
#   V8M_JEMALLOC_SO=/opt/jemalloc/lib/libjemalloc.so ...
#   V8M_TCMALLOC_SO=...
#   V8M_MIMALLOC_SO=...
#   V8M_V8MALLOC_SO=... (defaults to ./build/bench/libv8malloc.so,
#                       with a fallback to ./build/dev/libv8malloc.so)

set -euo pipefail

REPS=1
ONLY=""

while (( "$#" )); do
	case "$1" in
		--reps)
			REPS="$2"
			shift 2
			;;
		--only)
			ONLY="$2"
			shift 2
			;;
		--)
			shift
			break
			;;
		-h|--help)
			sed -n '4,36p' "$0"
			exit 0
			;;
		*)
			break
			;;
	esac
done

if (( $# == 0 )); then
	echo "bench-compare: missing benchmark command (use '-- <cmd>')" >&2
	exit 2
fi

BENCH_CMD=( "$@" )
if [[ ! -x "${BENCH_CMD[0]}" ]]; then
	echo "bench-compare: '${BENCH_CMD[0]}' is not an executable file" >&2
	exit 2
fi

# Per-allocator lib discovery. Each entry: name | env-override | ldconfig-pattern.
# The glibc row has no preload (empty LIB) — it runs the bench
# against the system allocator.
discover_lib() {
	local name="$1"
	local override="$2"
	local pattern="$3"
	if [[ -n "$override" ]]; then
		echo "$override"
		return 0
	fi
	if [[ -z "$pattern" ]]; then
		echo ""
		return 0
	fi
	# ldconfig -p prints "libfoo.so.N (arch, os) => /path/to/libfoo.so.N".
	# We take the first match's path.
	ldconfig -p 2>/dev/null \
		| awk -v pat="$pattern" '$1 ~ pat { print $NF; exit }'
}

# Resolve v8malloc's own shared library. Tries the env override
# first, then the two common build trees in this repo.
resolve_v8malloc() {
	if [[ -n "${V8M_V8MALLOC_SO:-}" && -f "${V8M_V8MALLOC_SO}" ]]; then
		echo "${V8M_V8MALLOC_SO}"
		return 0
	fi
	local candidates=(
		"$(pwd)/build/bench/libv8malloc.so"
		"$(pwd)/build/dev/libv8malloc.so"
		"$(pwd)/build/release/libv8malloc.so"
	)
	for cand in "${candidates[@]}"; do
		if [[ -f "$cand" ]]; then
			echo "$cand"
			return 0
		fi
	done
	echo ""
}

GLIBC_LIB=""
JEMALLOC_LIB=$(discover_lib jemalloc "${V8M_JEMALLOC_SO:-}" "^libjemalloc\.so")
TCMALLOC_LIB=$(discover_lib tcmalloc "${V8M_TCMALLOC_SO:-}" "^libtcmalloc\.so")
MIMALLOC_LIB=$(discover_lib mimalloc "${V8M_MIMALLOC_SO:-}" "^libmimalloc\.so")
V8MALLOC_LIB=$(resolve_v8malloc)

# Filter via --only if provided.
want() {
	local name="$1"
	if [[ -z "$ONLY" ]]; then
		return 0
	fi
	local entry
	for entry in ${ONLY//,/ }; do
		if [[ "$entry" == "$name" ]]; then
			return 0
		fi
	done
	return 1
}

run_one() {
	local name="$1"
	local lib="$2"
	if ! want "$name"; then
		return 0
	fi
	if [[ "$name" != "glibc" && -z "$lib" ]]; then
		printf '\n=== %s: skipped (library not found) ===\n' "$name"
		return 0
	fi
	if [[ -n "$lib" ]]; then
		printf '\n=== %s (LD_PRELOAD=%s) ===\n' "$name" "$lib"
	else
		printf '\n=== %s (system malloc) ===\n' "$name"
	fi
	for (( rep=1; rep <= REPS; rep++ )); do
		if (( REPS > 1 )); then
			printf -- '--- run %d of %d ---\n' "$rep" "$REPS"
		fi
		# Each rep's env is scoped so LD_PRELOAD does not leak
		# into the next allocator's run.
		if [[ -n "$lib" ]]; then
			LD_PRELOAD="$lib" "${BENCH_CMD[@]}"
		else
			"${BENCH_CMD[@]}"
		fi
	done
}

printf 'bench-compare: %s (reps=%d)\n' "${BENCH_CMD[*]}" "$REPS"

run_one glibc    "$GLIBC_LIB"
run_one jemalloc "$JEMALLOC_LIB"
run_one tcmalloc "$TCMALLOC_LIB"
run_one mimalloc "$MIMALLOC_LIB"
run_one v8malloc "$V8MALLOC_LIB"
