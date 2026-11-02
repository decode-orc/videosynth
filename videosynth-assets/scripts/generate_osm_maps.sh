#!/usr/bin/env bash
# Generates the OSM_TOWN and OSM_RURAL map stills (720x576) by rendering the
# committed OpenStreetMap extract sources/osm/bradford_on_avon.json with
# render_osm_map.py, then wrapping the frames as EXR with write_exr.sh.
#
# Runs in an ad-hoc Nix shell with ffmpeg, openexr, python3 (numpy, pillow)
# and the Liberation fonts (SIL Open Font License 1.1) for the lettering.
# The maps carry the OSM attribution; see the licence record in README.md.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${ROOT_DIR}/scripts"

if [[ "${VIDEOSYNTH_OSM_MAPS_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  VIDEOSYNTH_FONT_DIR="$(nix build --no-link --print-out-paths \
    nixpkgs#liberation_ttf)/share/fonts/truetype"
  export VIDEOSYNTH_FONT_DIR
  VIDEOSYNTH_OSM_MAPS_IN_SHELL=1 exec nix shell --impure --expr '
    let pkgs = (builtins.getFlake "nixpkgs").legacyPackages.${builtins.currentSystem};
    in pkgs.symlinkJoin {
      name = "videosynth-osm-maps";
      paths = [ pkgs.ffmpeg (pkgs.lib.getBin pkgs.openexr)
        (pkgs.python3.withPackages (ps: [ ps.numpy ps.pillow ])) ];
    }' -c bash "${BASH_SOURCE[0]}" "$@"
fi

# shellcheck source=write_exr.sh
source "${SCRIPT_DIR}/write_exr.sh"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

python3 -I "${SCRIPT_DIR}/render_osm_map.py" \
  "${ROOT_DIR}/sources/osm/bradford_on_avon.json" "${VIDEOSYNTH_FONT_DIR}" \
  "${work_dir}" >/dev/null

for raw in "${work_dir}"/*.gbrpf32le; do
  stem="$(basename "${raw}" .gbrpf32le)"
  write_exr "${raw}" "${ROOT_DIR}/assets/exr/720x576/${stem%_*}.exr" 720 576
done
