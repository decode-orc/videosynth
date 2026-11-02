#!/usr/bin/env bash
# Generates the PHOTO_BUTTERFLY and PHOTO_GALTSTROMS stills for both rasters
# from the photographer's original camera JPEGs, which are not committed:
#
#   generate_photo_exrs.sh <broken_butterfly.jpg> <galtstroms_bruk.jpg>
#
#   broken_butterfly.jpg  4703x3135, Canon EOS 70D, (c) 2015 Simon Inns. A
#                         child holding a butterfly: skin tones and smooth
#                         gradients next to striped fabric and fine hair.
#   galtstroms_bruk.jpg   4000x3000, GoPro HERO3+ Black, (c) Simon Inns. An
#                         aerial view of Galtstrom ironworks, Sweden:
#                         brickwork, stonework, railings and conifers,
#                         luma detail next to saturated grass and foliage.
#
# convert_photo.py crops each to 4:3 and downscales it with a non-negative
# filter (see its docstring); write_exr.sh wraps the frames as EXR. Both
# photographs are licensed CC BY 4.0; see the licence record in README.md.
# Runs in an ad-hoc Nix shell with ffmpeg, openexr and python3 (numpy,
# pillow).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${ROOT_DIR}/scripts"

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <broken_butterfly.jpg> <galtstroms_bruk.jpg>" >&2
  exit 1
fi

if [[ "${VIDEOSYNTH_PHOTO_EXR_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  VIDEOSYNTH_PHOTO_EXR_IN_SHELL=1 exec nix shell --impure --expr '
    let pkgs = (builtins.getFlake "nixpkgs").legacyPackages.${builtins.currentSystem};
    in pkgs.symlinkJoin {
      name = "videosynth-photo-exrs";
      paths = [ pkgs.ffmpeg (pkgs.lib.getBin pkgs.openexr)
        (pkgs.python3.withPackages (ps: [ ps.numpy ps.pillow ])) ];
    }' -c bash "${BASH_SOURCE[0]}" "$@"
fi

# shellcheck source=write_exr.sh
source "${SCRIPT_DIR}/write_exr.sh"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

# name source crop_centre_x crop_centre_y
photos=(
  "PHOTO_BUTTERFLY $1 0.5 0.5"
  "PHOTO_GALTSTROMS $2 0.5 0.5"
)

for entry in "${photos[@]}"; do
  read -r name source crop_x crop_y <<<"${entry}"
  for raster in 720x576 720x486; do
    width="${raster%x*}"
    height="${raster#*x}"
    raw="${work_dir}/${name}_${raster}.gbrpf32le"
    python3 -I "${SCRIPT_DIR}/convert_photo.py" "${source}" "${crop_x}" \
      "${crop_y}" "${width}" "${height}" "${raw}"
    write_exr "${raw}" "${ROOT_DIR}/assets/exr/${raster}/${name}.exr" \
      "${width}" "${height}"
  done
done
