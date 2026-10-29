#!/usr/bin/env bash
# Generates MOVING_BARS_75.mkv for both rasters: 75% colour bars (EBU
# 100/0/75/0) scrolling left by 4 pixels per frame, so picture chroma moves
# from frame to frame. A 3D comb filter or a field-phase decision only shows
# its behaviour on moving chroma, which the static stills cannot provide.
#
# The bars fill the 704-sample active image, inside 8 samples of black
# padding on each side of the 720-sample line, and wrap after 176 frames, so
# the clip loops seamlessly. The encoding and metadata follow
# docs/mkv-bt601-compliance-requirements.md (FFV1 10-bit 4:2:2, BT.601 matrix,
# limited range, 704-in-720 placement) and match the other bundled MKV clips
# of each raster.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${VIDEOSYNTH_MOVING_BARS_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  VIDEOSYNTH_MOVING_BARS_IN_SHELL=1 exec nix shell nixpkgs#ffmpeg \
    -c bash "${BASH_SOURCE[0]}" "$@"
fi

# Active image width, the black padding either side of it, the pixels the
# pattern moves per frame, and the frame count of one full wrap.
readonly kActiveWidth=704
readonly kPadding=8
readonly kScrollPixels=4
readonly kFrames=$((kActiveWidth / kScrollPixels))

# width height rate sar field_order field_mode primaries
generate() {
  local width="$1" height="$2" rate="$3" sar="$4" field_order="$5"
  local field_mode="$6" primaries="$7"
  local out_dir="${ROOT_DIR}/assets/mkv/${width}x${height}"
  mkdir -p "${out_dir}"

  # scroll takes a fraction of the width per frame; the bars source is
  # generated in BT.601 limited-range Y'CbCr, so no colour conversion runs,
  # and pad's black is studio black (Y'=64, Cb=Cr=512).
  ffmpeg -v error -f lavfi \
    -i "pal75bars=size=${kActiveWidth}x${height}:rate=${rate}" \
    -vf "scroll=horizontal=${kScrollPixels}/${kActiveWidth},format=yuv422p10le,pad=${width}:${height}:${kPadding}:0:black,setsar=${sar},setparams=field_mode=${field_mode}:range=tv:color_primaries=${primaries}:color_trc=bt709:colorspace=smpte170m" \
    -frames:v "${kFrames}" -c:v ffv1 -level 3 -g 1 -slicecrc 1 \
    -field_order "${field_order}" -color_range tv \
    -color_primaries "${primaries}" -color_trc bt709 \
    -colorspace smpte170m -y "${out_dir}/MOVING_BARS_75.mkv"
  echo "Wrote ${out_dir}/MOVING_BARS_75.mkv"
}

# ITU-R BT.601-5 Part A sample aspect ratios as the other clips carry them;
# SMPTE 170M-2004 (525) and ITU-R BT.470-6 (625) primaries.
generate 720 486 30000/1001 59/65 bt bff smpte170m
generate 720 576 25 93/85 tb tff bt470bg
