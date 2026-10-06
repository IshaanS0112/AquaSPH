#!/usr/bin/env bash
# Assembles a PNG frame sequence written by `aquasph_view --record` into an MP4 and a looping
# GIF. Needs ffmpeg (apt install ffmpeg / brew install ffmpeg).
# Usage: scripts/make_video.sh <frame-dir> [fps] [output-basename]
set -euo pipefail

DIR="${1:?usage: make_video.sh <frame-dir> [fps] [output-basename]}"
FPS="${2:-30}"
OUT="${3:-$DIR/clip}"

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg not found. Install it (apt install ffmpeg / brew install ffmpeg)" >&2
  exit 2
fi
if ! ls "$DIR"/frame_*.png >/dev/null 2>&1; then
  echo "No frame_*.png in '$DIR'." >&2
  exit 2
fi

# yuv420p and the even-dimension scale filter: some players (QuickTime, most browsers,
# PowerPoint) refuse H.264 with an odd width or height or a non-4:2:0 pixel format, and the
# failure is a black frame rather than an error.
ffmpeg -y -loglevel error \
  -framerate "$FPS" -i "$DIR/frame_%05d.png" \
  -vf "scale=trunc(iw/2)*2:trunc(ih/2)*2" \
  -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p \
  "${OUT}.mp4"
echo "wrote ${OUT}.mp4"

# Two-pass GIF via a generated palette.
ffmpeg -y -loglevel error \
  -framerate "$FPS" -i "$DIR/frame_%05d.png" \
  -vf "fps=15,scale=640:-1:flags=lanczos,palettegen=stats_mode=diff" \
  "$DIR/palette.png"
ffmpeg -y -loglevel error \
  -framerate "$FPS" -i "$DIR/frame_%05d.png" -i "$DIR/palette.png" \
  -lavfi "fps=15,scale=640:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3" \
  -loop 0 "${OUT}.gif"
rm -f "$DIR/palette.png"
echo "wrote ${OUT}.gif"
