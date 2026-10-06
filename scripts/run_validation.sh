#!/usr/bin/env bash
# Runs the three scenarios that carry quantitative validation and writes
# their metrics where scripts/validate.py expects them.
set -euo pipefail
QUALITY="${1:-medium}"
OUT="${2:-results/validation}"
BIN="${AQUASPH_BIN:-./build/aquasph}"

mkdir -p "$OUT"
for s in dam_break sloshing_tank controlled_wave_tank; do
  echo "=== $s ($QUALITY) ==="
  "$BIN" --scenario "$s" --quality "$QUALITY" --metrics "$OUT/$s.json" --quiet
done

python3 scripts/validate.py "$OUT"
