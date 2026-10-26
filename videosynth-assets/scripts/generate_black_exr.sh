#!/usr/bin/env bash
# Generates the BLACK.exr still for both rasters: every pixel R=G=B=0.0, which
# videosynth ingests as BT.601 studio black (Y'=64, Cb=Cr=512).
#
# The pixels come from /dev/zero rather than a colour generator so the output
# is exactly 0.0 regardless of ffmpeg's colour conversion. Metadata follows
# docs/exr-bt601-compliance-requirements.md (FLOAT RGB, no compression,
# gamma 1, framesPerSecond and pixelAspectRatio for the raster's standard).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${VIDEOSYNTH_BLACK_EXR_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  VIDEOSYNTH_BLACK_EXR_IN_SHELL=1 exec nix shell nixpkgs#ffmpeg nixpkgs#openexr \
    -c bash "${BASH_SOURCE[0]}" "$@"
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

# width height fps_num fps_den pixel_aspect
generate() {
  local width="$1" height="$2" fps_num="$3" fps_den="$4" pixel_aspect="$5"
  local out_dir="${ROOT_DIR}/assets/exr/${width}x${height}"
  local encoded="${work_dir}/black_${width}x${height}.exr"

  ffmpeg -v error -f rawvideo -pix_fmt gbrpf32le -s:v "${width}x${height}" \
    -i /dev/zero -frames:v 1 -c:v exr -compression none -format float \
    -gamma 1 -y "${encoded}"

  mkdir -p "${out_dir}"
  exrstdattr -framesPerSecond "${fps_num}" "${fps_den}" \
    -pixelAspectRatio "${pixel_aspect}" "${encoded}" "${out_dir}/BLACK.exr"
  echo "Wrote ${out_dir}/BLACK.exr"
}

# ITU-R BT.601-5 Part A: 625-line 128/117 and 525-line 108/119 pixel aspect.
generate 720 576 25 1 1.094017
generate 720 486 30000 1001 0.907563
