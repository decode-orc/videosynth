#!/usr/bin/env bash
# Generates the TEXT_CARD still (720x576) with generate_text_card.py: hard-
# edged text in three sizes and nine colour pairs, plus a band keyed over the
# PHOTO_GALTSTROMS photograph, which must already exist (see
# generate_photo_exrs.sh). write_exr.sh wraps the frame as EXR. Runs in an
# ad-hoc Nix shell with ffmpeg, openexr and python3.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${ROOT_DIR}/scripts"
PHOTO="${ROOT_DIR}/assets/exr/720x576/PHOTO_GALTSTROMS.exr"

if [[ "${VIDEOSYNTH_TEXT_CARD_IN_SHELL:-}" != "1" ]]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "Error: nix is required but not found in PATH." >&2
    exit 1
  fi
  VIDEOSYNTH_TEXT_CARD_IN_SHELL=1 exec nix shell nixpkgs#ffmpeg \
    nixpkgs#openexr nixpkgs#python3 -c bash "${BASH_SOURCE[0]}" "$@"
fi

# shellcheck source=write_exr.sh
source "${SCRIPT_DIR}/write_exr.sh"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

read_exr "${PHOTO}" "${work_dir}/photo.gbrpf32le"
python3 -I "${SCRIPT_DIR}/generate_text_card.py" \
  "${work_dir}/photo.gbrpf32le" "${work_dir}/TEXT_CARD.gbrpf32le"
write_exr "${work_dir}/TEXT_CARD.gbrpf32le" \
  "${ROOT_DIR}/assets/exr/720x576/TEXT_CARD.exr" 720 576
