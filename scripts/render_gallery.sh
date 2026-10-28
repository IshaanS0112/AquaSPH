#!/usr/bin/env bash
# Simulates every scenario to the moment in render/blender/shots.json and renders it in Blender
# Cycles. Output: web/public/renders/<scenario>.jpg, used by the console and the landing page.
#
#   BLENDER_PY=/path/to/python-with-bpy scripts/render_gallery.sh [scenario ...]
#   BLENDER="blender -b -P" scripts/render_gallery.sh            # with a Blender install
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOLVER="${SOLVER:-$ROOT/build/aquasph}"
OUT="${OUT:-$ROOT/web/public/renders}"
WORK="${WORK:-$(mktemp -d)}"
WIDTH="${WIDTH:-1600}"; HEIGHT="${HEIGHT:-900}"; SAMPLES="${SAMPLES:-64}"
mkdir -p "$OUT" "$WORK"

render() {
  if [ -n "${BLENDER:-}" ]; then
    $BLENDER "$ROOT/render/blender/render_scene.py" -- "$@"
  else
    "${BLENDER_PY:-python3}" "$ROOT/render/blender/render_scene.py" "$@"
  fi
}

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
  mapfile -t names < <(python3 -c "import json; print('\n'.join(k for k in json.load(open('$ROOT/render/blender/shots.json')) if not k.startswith('_')))")
fi

for name in "${names[@]}"; do
  read -r t q < <(python3 -c "import json; s=json.load(open('$ROOT/render/blender/shots.json'))['$name']; print(s['time'], s['quality'])")
  echo "== $name: simulating to t=$t s at $q"
  start=$(date +%s)
  "$SOLVER" --scenario "$name" --quality "$q" --time "$t" --snapshots "$WORK/$name.aqsnap" \
      --snapshot-frames 1 --metrics "$WORK/$name.json" --quiet > "$WORK/$name.log" 2>&1 || {
    echo "   solver exited $? (see $WORK/$name.log)"; }
  echo "   simulated in $(( $(date +%s) - start )) s; rendering ${WIDTH}x${HEIGHT} at $SAMPLES samples"
  render --snapshot "$WORK/$name.aqsnap" --scenario "$ROOT/configs/scenarios/$name.json" \
      --out "$OUT/$name.jpg" --width "$WIDTH" --height "$HEIGHT" --samples "$SAMPLES" | grep -E "^rendered|Error" || true
done
