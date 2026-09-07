#!/usr/bin/env bash
# Renders one clip per scenario plus a contact sheet spanning all of them.
#
# Requires a display. On a headless Linux machine, run the whole script
# under a virtual one:
#     xvfb-run -s "-screen 0 1600x900x24" scripts/make_gallery.sh medium
#
# Usage: scripts/make_gallery.sh [quality] [outdir] [width] [height]
set -euo pipefail

QUALITY="${1:-medium}"
OUTDIR="${2:-gallery/$QUALITY}"
WIDTH="${3:-1280}"
HEIGHT="${4:-720}"

BIN="${AQUASPH_VIEW:-./build/aquasph_view}"
if [ ! -x "$BIN" ]; then
  echo "aquasph_view not found at $BIN." >&2
  echo "Build it with -DAQUASPH_BUILD_VISUALIZATION=ON." >&2
  exit 2
fi

mkdir -p "$OUTDIR"
sheet_inputs=()

for path in configs/scenarios/*.json; do
  name="$(basename "$path" .json)"
  echo "=== $name ==="
  frames="$OUTDIR/$name"
  rm -rf "$frames"
  "$BIN" --scenario "$name" --quality "$QUALITY" --size "$WIDTH" "$HEIGHT" \
         --record-headless --record "$frames" 2>&1 | tail -n 12

  if command -v ffmpeg >/dev/null 2>&1; then
    scripts/make_video.sh "$frames" 30 "$OUTDIR/$name" || true
  fi

  # The contact-sheet still is taken from ~75% through each run: far
  # enough in that the phenomenon has developed, before things settle.
  last=$(ls "$frames"/frame_*.png 2>/dev/null | wc -l)
  if [ "$last" -gt 0 ]; then
    idx=$(( last * 3 / 4 ))
    still=$(printf "%s/frame_%05d.png" "$frames" "$idx")
    [ -f "$still" ] || still=$(ls "$frames"/frame_*.png | tail -1)
    cp "$still" "$OUTDIR/still_$name.png"
    sheet_inputs+=("$OUTDIR/still_$name.png")
  fi
done

if command -v ffmpeg >/dev/null 2>&1 && [ "${#sheet_inputs[@]}" -gt 0 ]; then
  # One row per four scenarios. The contact sheet exists so that visibly
  # different phenomena can be compared at a glance -- which only works
  # because every scenario uses the same camera framing convention and the
  # same palette, so what differs between tiles is the physics.
  ffmpeg -y -loglevel error -pattern_type glob -i "$OUTDIR/still_*.png" \
    -filter_complex "scale=640:-1,tile=4x4:margin=8:padding=8:color=0x111318" \
    -frames:v 1 "$OUTDIR/contact_sheet.png"
  echo "wrote $OUTDIR/contact_sheet.png"
fi

echo "Gallery written to $OUTDIR"
