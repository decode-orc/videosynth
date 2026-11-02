#!/usr/bin/env bash
# Generates the synthetic still test patterns rendered by
# generate_test_patterns.py: PAL_STRESS (720x576), and LUMA_ZONE_PLATE,
# CHROMA_ZONE_PLATE and GREEN_MAGENTA_EDGES for both rasters.
#
# The Python renderer writes planar float32 R'G'B' frames; write_exr.sh wraps
# them as EXR stills with the metadata required by
# docs/exr-bt601-compliance-requirements.md.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${ROOT_DIR}/scripts"
# shellcheck source=write_exr.sh
source "${SCRIPT_DIR}/write_exr.sh"

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
  write_exr "${raw}" "${ROOT_DIR}/assets/exr/${raster}/${name}.exr" \
    "${raster%x*}" "${raster#*x}"
done
