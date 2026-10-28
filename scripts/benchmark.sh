#!/usr/bin/env bash
# Throughput and scaling sweep. Every number in benchmarks/scaling_results.md comes from this
# script -- measured, never estimated.
# Usage: scripts/benchmark.sh [steps] [scenario]
set -u
STEPS="${1:-40}"
SCENARIO="${2:-dam_break}"
BIN="${AQUASPH_BIN:-./build/aquasph}"

if [ ! -x "$BIN" ]; then echo "aquasph not found at $BIN" >&2; exit 2; fi

echo "scenario=$SCENARIO steps=$STEPS  (nproc=$(nproc))"
printf '%-8s %8s %9s %9s %12s %10s\n' QUALITY FLUID BOUNDARY THREADS MS_PER_STEP STEPS_PER_S

for q in low medium high; do
  for t in 1 2 4 8; do
    out=$("$BIN" --scenario "$SCENARIO" --quality "$q" --steps "$STEPS" \
                  --threads "$t" --profile --quiet 2>/dev/null)
    fluid=$(echo "$out"  | grep -m1 'Fluid particles' | awk '{print $4}')
    bound=$(echo "$out"  | grep -m1 'Boundary particles' | awk '{print $3}')
    ms=$(echo "$out"     | grep -m1 'TOTAL (measured)' | awk '{print $3}')
    sps=$(awk -v m="$ms" 'BEGIN{ if (m>0) printf "%.2f", 1000.0/m; else print "-" }')
    printf '%-8s %8s %9s %9s %12s %10s\n' "$q" "$fluid" "$bound" "$t" "$ms" "$sps"
  done
done
