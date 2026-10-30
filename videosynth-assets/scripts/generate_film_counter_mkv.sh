#!/usr/bin/env bash
# Generates FILM_COUNTER_24P.mkv: a 720x486 progressive film clip at
# 24000/1001 frames/s for testing 2:3 pulldown. Each film frame carries its
# 0-based frame number in large digits, a white bar that moves 8 pixels per
# frame, and a strip of 75% colour bars that scrolls 8 pixels per frame. Every
# film frame is therefore different, so after pulldown a frame that weaves
# fields from two film frames shows combing on the bar and the bars strip, and
# the counter names the film frame in each field.
#
# The picture fills the 704-sample active image, inside 8 samples of black
# padding on each side of the 720-sample line. The encoding and metadata
# follow docs/mkv-bt601-compliance-requirements.md (FFV1 10-bit 4:2:2, BT.601
# matrix, limited range, 704-in-720 placement) for the film frame rate, with
# the field order tagged progressive.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${VIDEOSYNTH_FILM_COUNTER_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  font_dir="$(nix build nixpkgs#dejavu_fonts --no-link --print-out-paths)"
  VIDEOSYNTH_FILM_COUNTER_IN_SHELL=1 \
    VIDEOSYNTH_FILM_COUNTER_FONT="${font_dir}/share/fonts/truetype/DejaVuSansMono-Bold.ttf" \
    exec nix shell nixpkgs#ffmpeg -c bash "${BASH_SOURCE[0]}" "$@"
fi

readonly kFont="${VIDEOSYNTH_FILM_COUNTER_FONT}"
# Active image width and height, the black padding either side of it, the
# height of the colour-bar strip along the bottom, the moving bar's width, the
# pixels the bar and the strip move per film frame, and the clip length in
# film frames (240 frames, 10.01 s).
readonly kActiveWidth=704
readonly kHeight=486
readonly kPadding=8
readonly kStripHeight=162
readonly kBarWidth=24
readonly kStepPixels=8
readonly kFrames=240
readonly kRate=24000/1001

readonly kUpperHeight=$((kHeight - kStripHeight))
out_dir="${ROOT_DIR}/assets/mkv/720x${kHeight}"
mkdir -p "${out_dir}"

# The grey field and the bar are RGB sources converted with swscale's default
# BT.601 limited-range matrix; the bars strip is generated in BT.601 Y'CbCr,
# and pad's black is studio black (Y'=64, Cb=Cr=512). The counter is drawn
# last so the bar passes behind it.
ffmpeg -v error \
  -f lavfi -i "color=c=0x404040:size=${kActiveWidth}x${kHeight}:rate=${kRate}" \
  -f lavfi -i "pal75bars=size=${kActiveWidth}x${kStripHeight}:rate=${kRate}" \
  -f lavfi -i "color=c=white:size=${kBarWidth}x${kUpperHeight}:rate=${kRate}" \
  -filter_complex "\
[1:v]scroll=horizontal=${kStepPixels}/${kActiveWidth}[strip];\
[0:v][strip]overlay=x=0:y=${kUpperHeight}[base];\
[base][2:v]overlay=x='mod(n*${kStepPixels},${kActiveWidth}-${kBarWidth})':y=0,\
drawtext=fontfile=${kFont}:text='%{eif\:n\:d\:4}':fontsize=200:fontcolor=white:borderw=6:bordercolor=black:x=(w-text_w)/2:y=(${kUpperHeight}-text_h)/2,\
drawtext=fontfile=${kFont}:text='FILM 24000/1001':fontsize=28:fontcolor=white:x=16:y=16,\
format=yuv422p10le,pad=720:${kHeight}:${kPadding}:0:black,setsar=59/65,\
setparams=field_mode=prog:range=tv:color_primaries=smpte170m:color_trc=bt709:colorspace=smpte170m[out]" \
  -map "[out]" -frames:v "${kFrames}" -c:v ffv1 -level 3 -g 1 -slicecrc 1 \
  -field_order progressive -color_range tv -color_primaries smpte170m \
  -color_trc bt709 -colorspace smpte170m -y "${out_dir}/FILM_COUNTER_24P.mkv"
echo "Wrote ${out_dir}/FILM_COUNTER_24P.mkv"
