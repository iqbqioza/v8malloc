#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Persisted bench baseline regression check (resolution of the
# `>5% perf-regression gate` follow-on noted in TODO.md §Phase 5
# CI row 205). Runs a v8malloc bench binary, extracts per-row
# rate metrics, compares each row to the matching row in a stored
# baseline file, and fails (exit 1) if any row's metric dropped
# by more than the regression threshold.
#
# Usage:
#   bench-regression-check.sh <bench-binary> <baseline-file> [threshold_pct]
#   bench-regression-check.sh --capture <bench-binary> <baseline-file>
#
# `--capture` runs the bench and writes the output to the baseline
# file verbatim. Use this for the first run of a new bench to
# establish the baseline; commit the result.
#
# Threshold defaults to 5 (= 5 % regression). Override via the
# `BENCH_REGRESSION_THRESHOLD_PCT` env or the third positional arg.
#
# Exit codes:
#   0 — no regression OR --capture mode succeeded
#   1 — at least one row regressed past the threshold
#   2 — usage error / bench failure / baseline missing
#
# Output format expectation (matches `bench/mb_01_throughput`
# etc.): comment lines start with `#`; the first non-comment line
# is a whitespace-separated header; the rate column must be one
# of `ops_per_sec`, `handoffs_per_sec` (higher-is-better) or
# `ns_per_op`, `latency_us` (lower-is-better fallback). The first
# column of every data row is the row key.

set -euo pipefail

usage() {
	cat >&2 <<'EOF'
usage:
  bench-regression-check.sh <bench-binary> <baseline-file> [threshold_pct]
  bench-regression-check.sh --capture <bench-binary> <baseline-file>

Threshold defaults to 5 (%). Override via $BENCH_REGRESSION_THRESHOLD_PCT.
EOF
	exit 2
}

CAPTURE=0
if [ "${1-}" = "--capture" ]; then
	CAPTURE=1
	shift
fi

if [ $# -lt 2 ]; then
	usage
fi

BENCH_BIN="$1"
BASELINE="$2"
THRESHOLD_PCT="${3-${BENCH_REGRESSION_THRESHOLD_PCT-5}}"

if [ ! -x "$BENCH_BIN" ]; then
	echo "bench-regression-check: bench binary not found or not executable: $BENCH_BIN" >&2
	exit 2
fi

if [ "$CAPTURE" = "1" ]; then
	mkdir -p "$(dirname "$BASELINE")"
	tmp_out=$(mktemp)
	trap 'rm -f "$tmp_out"' EXIT
	if ! "$BENCH_BIN" >"$tmp_out" 2>&1; then
		echo "bench-regression-check: bench run failed" >&2
		cat "$tmp_out" >&2
		exit 2
	fi
	cp "$tmp_out" "$BASELINE"
	echo "bench-regression-check: baseline captured at $BASELINE"
	exit 0
fi

if [ ! -f "$BASELINE" ]; then
	echo "bench-regression-check: baseline file missing: $BASELINE" >&2
	echo "  re-run with --capture to establish the baseline first." >&2
	exit 2
fi

current_out=$(mktemp)
trap 'rm -f "$current_out"' EXIT
if ! "$BENCH_BIN" >"$current_out" 2>&1; then
	echo "bench-regression-check: bench run failed" >&2
	cat "$current_out" >&2
	exit 2
fi

# extract_pairs <input-file> — emits "kind<TAB>key<TAB>value" per
# data row to stdout. `kind` is "higher" or "lower" depending on
# which rate column type the header advertised. Mawk-compatible —
# no gawk extensions, no array-in-array, no third-arg split flags.
extract_pairs() {
	awk '
		BEGIN { col = 0; kind = "" }
		/^#/ { next }
		NF == 0 { next }
		col == 0 {
			for (i = 1; i <= NF; i++) {
				if ($i == "ops_per_sec" || $i == "handoffs_per_sec") {
					col = i
					kind = "higher"
				}
			}
			if (col == 0) {
				for (i = 1; i <= NF; i++) {
					if ($i == "ns_per_op" || $i == "latency_us") {
						col = i
						kind = "lower"
					}
				}
			}
			next
		}
		col > 0 && NF >= col {
			val = $col + 0
			if (val > 0) {
				printf "%s\t%s\t%s\n", kind, $1, $col
			}
		}
	' "$1"
}

baseline_pairs=$(extract_pairs "$BASELINE")
current_pairs=$(extract_pairs "$current_out")

baseline_kind=$(printf '%s\n' "$baseline_pairs" | awk -F'\t' 'NR == 1 { print $1 }')
current_kind=$(printf '%s\n' "$current_pairs" | awk -F'\t' 'NR == 1 { print $1 }')

if [ -z "$baseline_kind" ]; then
	echo "bench-regression-check: baseline lacks a rate column" >&2
	exit 2
fi
if [ -z "$current_kind" ]; then
	echo "bench-regression-check: current run lacks a rate column" >&2
	exit 2
fi
if [ "$baseline_kind" != "$current_kind" ]; then
	echo "bench-regression-check: rate kind mismatch baseline=$baseline_kind current=$current_kind" >&2
	exit 2
fi

regressions=0
improvements=0
stable=0
missing=0

# Build an associative lookup from key -> current value via the
# helper file because bash 3 doesn't have `declare -A` semantics
# we can rely on cross-host. Use a tmp tab-separated file and
# `awk` for the lookup.
current_keys_file=$(mktemp)
printf '%s\n' "$current_pairs" | awk -F'\t' 'NF == 3 { print $2 "\t" $3 }' >"$current_keys_file"

lookup_current() {
	awk -F'\t' -v k="$1" '$1 == k { print $2; exit }' "$current_keys_file"
}

while IFS=$'\t' read -r kind key base; do
	[ -z "$key" ] && continue
	cur=$(lookup_current "$key")
	if [ -z "$cur" ]; then
		printf "  MISSING %-12s baseline=%s\n" "$key" "$base"
		missing=$((missing + 1))
		continue
	fi
	if [ "$baseline_kind" = "higher" ]; then
		delta=$(awk -v a="$cur" -v b="$base" 'BEGIN { printf "%.4f", (a - b) * 100.0 / b }')
	else
		delta=$(awk -v a="$cur" -v b="$base" 'BEGIN { printf "%.4f", (b - a) * 100.0 / b }')
	fi
	verdict=$(awk -v d="$delta" -v t="$THRESHOLD_PCT" 'BEGIN {
		if (d < -t) { print "regressed" }
		else if (d > t) { print "improved" }
		else { print "stable" }
	}')
	case "$verdict" in
	regressed)
		printf "  REGRESSED %-12s baseline=%s current=%s delta=%+.2f%%\n" \
			"$key" "$base" "$cur" "$delta"
		regressions=$((regressions + 1))
		;;
	improved)
		printf "  IMPROVED  %-12s baseline=%s current=%s delta=%+.2f%%\n" \
			"$key" "$base" "$cur" "$delta"
		improvements=$((improvements + 1))
		;;
	stable)
		stable=$((stable + 1))
		;;
	esac
done <<EOF
$baseline_pairs
EOF

rm -f "$current_keys_file"

printf "bench-regression-check: %d regressed, %d improved, %d stable, %d missing (threshold %s%%)\n" \
	"$regressions" "$improvements" "$stable" "$missing" "$THRESHOLD_PCT"

if [ "$regressions" -gt 0 ]; then
	exit 1
fi
exit 0
