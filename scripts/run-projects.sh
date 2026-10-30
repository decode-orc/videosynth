#!/usr/bin/env bash
set -euo pipefail

# Runs project fixtures through the CLI end to end.
#
# Suites combine the committed projects under projects/ with the variants
# derived into the build tree by scripts/generate_test_projects.py. Every run
# writes into build/project-output/<suite>/ via the {output} logical root, so
# nothing lands in the source tree.
#
# Usage: scripts/run-projects.sh [suite ...] [-- <extra videosynth args>]
#        scripts/run-projects.sh --list

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${VIDEOSYNTH_BUILD_DIR:-$repo_root/build}"
binary="$build_dir/videosynth"
generated_dir="$build_dir/generated-projects"
output_root="$build_dir/project-output"

# suite name -> project directories, relative to the repo root or build dir.
suite_dirs() {
  case "$1" in
    general)  echo "projects/general projects/general-yc @generated-projects/film-clv @generated-projects/film-pal-m" ;;
    stacking) echo "projects/stacking @generated-projects/stacking-captures @generated-projects/stacking-clean @generated-projects/stacking-yc" ;;
    *)        return 1 ;;
  esac
}

all_suites=(general stacking)

usage() {
  echo "Usage: $(basename "$0") [suite ...] [-- <extra videosynth args>]" >&2
  echo "Suites: ${all_suites[*]} (default: all)" >&2
}

suites=()
extra_args=()
while (($#)); do
  case "$1" in
    --list)
      for suite in "${all_suites[@]}"; do
        echo "$suite: $(suite_dirs "$suite")"
      done
      exit 0
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      extra_args=("$@")
      break
      ;;
    -*)
      usage
      exit 2
      ;;
    *)
      if ! suite_dirs "$1" >/dev/null; then
        echo "Error: unknown suite '$1'. Known suites: ${all_suites[*]}" >&2
        exit 2
      fi
      suites+=("$1")
      shift
      ;;
  esac
done

if ((${#suites[@]} == 0)); then
  suites=("${all_suites[@]}")
fi

# The dev shell supplies ffmpeg/ffprobe for the MKV-sourced fixtures.
if [[ -z "${IN_NIX_SHELL:-}" ]] || ! command -v ffprobe >/dev/null 2>&1; then
  exec nix develop "path:$repo_root" --command "${BASH_SOURCE[0]}" "$@"
fi

if [[ ! -x "$binary" ]]; then
  echo "Error: expected executable not found: $binary" >&2
  echo "Build first with: nix develop \"path:$repo_root\" --command cmake --build \"$build_dir\"" >&2
  exit 1
fi

cd "$repo_root"

failures=0
ran=0
for suite in "${suites[@]}"; do
  suite_output="$output_root/$suite"
  mkdir -p "$suite_output"

  projects=()
  for dir in $(suite_dirs "$suite"); do
    # A leading '@' marks a directory generated into the build tree.
    if [[ "$dir" == @* ]]; then
      resolved="$build_dir/${dir#@}"
    else
      resolved="$repo_root/$dir"
    fi
    if [[ ! -d "$resolved" ]]; then
      echo "Error: project directory not found: $resolved" >&2
      echo "Generated variants come from the build; run cmake --build first." >&2
      exit 1
    fi
    shopt -s nullglob
    projects+=("$resolved"/*.yaml)
    shopt -u nullglob
  done

  if ((${#projects[@]} == 0)); then
    echo "Error: suite '$suite' matched no projects" >&2
    exit 1
  fi

  echo "== suite $suite (${#projects[@]} projects) -> $suite_output"
  for project in "${projects[@]}"; do
    rel_project="${project#"$repo_root"/}"
    echo "Running $rel_project"
    ran=$((ran + 1))
    if "$binary" --project "$project" --output-root "$suite_output" \
        --log-level debug "${extra_args[@]+"${extra_args[@]}"}"; then
      echo "PASS  $rel_project"
    else
      echo "FAIL  $rel_project" >&2
      failures=$((failures + 1))
    fi
  done
done

if ((failures > 0)); then
  echo "$failures of $ran project run(s) failed." >&2
  exit 1
fi

echo "All $ran project run(s) succeeded. Outputs are under $output_root/."
