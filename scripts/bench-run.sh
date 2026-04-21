#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Benchmark runner. Pins the system into the configuration the
# benchmarks.md §1.3 measurement environment calls for, runs the
# requested benchmark, and restores prior settings on exit so the
# host stays usable. The setup steps need root (CPU governor +
# /proc/sys writes); without root the script prints a warning and
# runs the benchmark anyway, since dev iteration on a workstation
# without sudo still benefits from the harness.
#
# What it does:
#   - Switches every online CPU's frequency governor to `performance`
#     (so a turbo-down + turbo-up jitter does not contaminate latency)
#   - Disables transparent huge pages (so v8malloc's own MAP_HUGETLB
#     path is the only HugePage code under test, no kernel-side
#     interference)
#   - Disables ASLR (`/proc/sys/kernel/randomize_va_space = 0`) so
#     repeated runs map the same virtual addresses — important for
#     comparing mmap-driven costs across runs
#
# What it does NOT do:
#   - Pin the bench to specific cores (use `taskset -c ...` ahead of
#     `bench-run.sh` if you want that)
#   - Drop the page cache (only matters for fault-heavy workloads;
#     opt in via `--drop-caches`)
#   - Restore tuning if the script is killed with SIGKILL — only the
#     normal exit / SIGINT / SIGTERM paths trigger the trap
#
# Usage:
#   scripts/bench-run.sh [--drop-caches] -- ./build/bench/bench/mb_01_throughput
#   scripts/bench-run.sh -- ./build/bench/bench/mb_02_scalability
#
# Pass-through env vars (V8M_BENCH_DURATION_MS etc.) work as expected
# because the script execs the bench in the current environment.

set -euo pipefail

DROP_CACHES=0

while (( "$#" )); do
	case "$1" in
		--drop-caches)
			DROP_CACHES=1
			shift
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

if (( $# == 0 )); then
	echo "bench-run: missing benchmark command (use '-- <cmd>')" >&2
	echo "  e.g.: bench-run.sh -- ./build/bench/bench/mb_01_throughput" >&2
	exit 2
fi

if [[ ! -x "$1" && "$1" != /* && "$1" != ./* ]]; then
	# Not directly executable and not an absolute or relative
	# path — most likely a typo. The bench binary is part of
	# the build tree, so a bare command is almost never right.
	echo "bench-run: '$1' is not an executable file" >&2
	exit 2
fi

is_root() {
	[[ $EUID -eq 0 ]]
}

# Save / restore state. Each setting captured before any change so
# the trap can put it back even if some operations fail mid-flight.
ORIG_THP=""
ORIG_ASLR=""
declare -a ORIG_GOVERNORS=()
declare -a GOVERNOR_PATHS=()

restore() {
	local rc=$?
	if [[ -n "$ORIG_THP" ]]; then
		echo "$ORIG_THP" \
			> /sys/kernel/mm/transparent_hugepage/enabled \
			2>/dev/null || true
	fi
	if [[ -n "$ORIG_ASLR" ]]; then
		echo "$ORIG_ASLR" \
			> /proc/sys/kernel/randomize_va_space \
			2>/dev/null || true
	fi
	if (( ${#ORIG_GOVERNORS[@]} > 0 )); then
		for i in "${!GOVERNOR_PATHS[@]}"; do
			echo "${ORIG_GOVERNORS[$i]}" \
				> "${GOVERNOR_PATHS[$i]}" 2>/dev/null || true
		done
	fi
	exit "$rc"
}
trap restore EXIT INT TERM

if ! is_root; then
	echo "bench-run: not running as root — tuning steps will be skipped." >&2
	echo "bench-run: numbers will be noisier; rerun under sudo for stable comparison." >&2
fi

# THP
if is_root; then
	if [[ -r /sys/kernel/mm/transparent_hugepage/enabled ]]; then
		# /sys/kernel/mm/transparent_hugepage/enabled looks like
		# "always madvise [never]" — the bracketed token is the
		# active value. Capture it for restore, then disable.
		ORIG_THP=$(awk '{
			for (i=1;i<=NF;i++) {
				if ($i ~ /^\[/) {
					gsub(/[][]/, "", $i)
					print $i
				}
			}
		}' /sys/kernel/mm/transparent_hugepage/enabled)
		echo never > /sys/kernel/mm/transparent_hugepage/enabled
		echo "bench-run: THP disabled (was: $ORIG_THP)" >&2
	fi
fi

# ASLR
if is_root; then
	if [[ -r /proc/sys/kernel/randomize_va_space ]]; then
		ORIG_ASLR=$(cat /proc/sys/kernel/randomize_va_space)
		echo 0 > /proc/sys/kernel/randomize_va_space
		echo "bench-run: ASLR disabled (was: $ORIG_ASLR)" >&2
	fi
fi

# CPU governor — per-cpu file under cpufreq/. Some VMs do not expose
# cpufreq at all (the file just doesn't exist); skip cleanly in that
# case.
if is_root; then
	shopt -s nullglob
	for governor_file in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
		ORIG_GOVERNORS+=("$(cat "$governor_file")")
		GOVERNOR_PATHS+=("$governor_file")
		echo performance > "$governor_file" 2>/dev/null || true
	done
	shopt -u nullglob
	if (( ${#GOVERNOR_PATHS[@]} > 0 )); then
		echo "bench-run: ${#GOVERNOR_PATHS[@]} CPU governors set to performance" >&2
	fi
fi

# Optional: drop the page cache. The bench's own warmup loop usually
# absorbs cold-cache effects, so leave this off by default.
if (( DROP_CACHES == 1 )); then
	if is_root; then
		sync
		echo 3 > /proc/sys/vm/drop_caches
		echo "bench-run: page caches dropped" >&2
	else
		echo "bench-run: --drop-caches requires root, ignored" >&2
	fi
fi

# Hand off to the benchmark. The trap above runs after the bench
# exits (or if it gets a signal forwarded by bash) and restores the
# host configuration. We deliberately do NOT exec — exec would
# replace this shell and the trap would never fire.
"$@"
