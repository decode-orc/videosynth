#!/usr/bin/env bash
# Generates the synthetic still test patterns rendered by
# generate_test_patterns.py: PAL_STRESS (720x576), and LUMA_ZONE_PLATE and
# CHROMA_ZONE_PLATE for both rasters.
#
# The Python renderer writes planar float32 R'G'B' frames; ffmpeg wraps them as
# EXR without a colour conversion (gbrpf32le is the EXR encoder's native
# format), and exrstdattr adds the metadata required by
# docs/exr-bt601-compliance-requirements.md (FLOAT RGB, no compression, gamma 1,
# framesPerSecond and pixelAspectRatio for the raster's standard).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${ROOT_DIR}/scripts"

if [[ "${VIDEOSYNTH_PATTERN_EXR_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  VIDEOSYNTH_PATTERN_EXR_IN_SHELL=1 exec nix shell nixpkgs#ffmpeg \
    nixpkgs#openexr nixpkgs#python3 -c bash "${BASH_SOURCE[0]}" "$@"
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

python3 -I "${SCRIPT_DIR}/generate_test_patterns.py" "${work_dir}" >/dev/null

for raw in "${work_dir}"/*.gbrpf32le; do
  stem="$(basename "${raw}" .gbrpf32le)"
  name="${stem%_*}"
  raster="${stem##*_}"
  width="${raster%x*}"
  height="${raster#*x}"
  # ITU-R BT.601-5 Part A: 625-line 128/117 and 525-line 108/119 pixel aspect.
  if [[ "${height}" == "576" ]]; then
    fps=(25 1)
    pixel_aspect=1.094017
  else
    fps=(30000 1001)
    pixel_aspect=0.907563
  fi
  encoded="${work_dir}/${stem}.exr"
  ffmpeg -v error -f rawvideo -pix_fmt gbrpf32le -s:v "${width}x${height}" \
    -i "${raw}" -frames:v 1 -c:v exr -compression none -format float \
    -gamma 1 -y "${encoded}"

  out_dir="${ROOT_DIR}/assets/exr/${raster}"
  mkdir -p "${out_dir}"
  exrstdattr -framesPerSecond "${fps[0]}" "${fps[1]}" \
    -pixelAspectRatio "${pixel_aspect}" "${encoded}" "${out_dir}/${name}.exr"
  echo "Wrote ${out_dir}/${name}.exr"
done
