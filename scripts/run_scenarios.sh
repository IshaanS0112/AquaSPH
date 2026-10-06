#!/usr/bin/env bash
# Runs every scenario in configs/scenarios and reports STABLE/UNSTABLE plus the key metrics,
# writing one JSON file per scenario.
# Usage: scripts/run_scenarios.sh [quality] [outdir] [extra aquasph args...]
set -u

QUALITY="${1:-low}"
OUTDIR="${2:-results/${QUALITY}}"
shift 2 2>/dev/null || shift $# 

BIN="${AQUASPH_BIN:-./build/aquasph}"
if [ ! -x "$BIN" ]; then
  echo "aquasph binary not found at $BIN (set AQUASPH_BIN or build first)" >&2
  exit 2
fi

mkdir -p "$OUTDIR"
fail=0
printf '%-22s %-10s %9s %9s %8s %10s %s\n' SCENARIO STATUS FLUID BOUNDARY STEPS WALL_S NOTE

for path in configs/scenarios/*.json; do
  name="$(basename "$path" .json)"
  log="$OUTDIR/$name.log"
  if "$BIN" --scenario "$name" --quality "$QUALITY" \
        --metrics "$OUTDIR/$name.json" "$@" >"$log" 2>&1; then
    status=STABLE
  else
    status=UNSTABLE
    fail=$((fail + 1))
  fi
  fluid=$(grep -m1 'Fluid particles' "$log" | awk '{print $4}')
  bound=$(grep -m1 'Boundary particles' "$log" | awk '{print $3}')
  steps=$(grep -m1 '^Steps:' "$log" | awk '{print $2}')
  wall=$(grep -m1 'Wall time' "$log" | awk '{print $3}')
  note=$(grep -m1 'NOTE:' "$log" | cut -c1-40)
  printf '%-22s %-10s %9s %9s %8s %10s %s\n' \
    "$name" "$status" "${fluid:-?}" "${bound:-?}" "${steps:-?}" "${wall:-?}" "$note"
done

echo
if [ "$fail" -gt 0 ]; then
  echo "$fail scenario(s) reported UNSTABLE. Logs are in $OUTDIR."
  exit 1
fi
echo "All scenarios STABLE. Metrics and logs in $OUTDIR."
