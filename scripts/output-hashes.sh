#!/usr/bin/env bash
set -euo pipefail

# Records and compares SHA-256 manifests of generated media, so an optimisation
# can be asserted byte-identical to a recorded baseline.
#
# Hashed artefacts per suite output tree: .cvbs, .cvbsy, .cvbsc composite/Y/C
# samples, _audio_<pair>.wav audio, the .truth picture truth, and the .meta
# SQLite sidecars (.truth.meta included). The sidecar is
# hashed as its canonical `sqlite3 .dump` text rather than as raw bytes: page
# layout can differ between SQLite builds while the recorded rows are identical,
# and the rows are what the format specifies.
#
# Typical use around a change:
#   scripts/output-hashes.sh --record          # on the known-good build
#   ...apply the optimisation and rebuild...
#   scripts/output-hashes.sh                   # fails if any artefact differs
#
# Usage: scripts/output-hashes.sh [options] [suite ...]

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${VIDEOSYNTH_BUILD_DIR:-$repo_root/build}"
output_root="$build_dir/project-output"
baseline_dir="${VIDEOSYNTH_HASH_BASELINE_DIR:-$build_dir/output-hashes}"

all_suites=(general stacking)
mode="check"
run_projects=1

usage() {
  cat >&2 <<EOF
Usage: $(basename "$0") [options] [suite ...]

Suites: ${all_suites[*]} (default: all)

Options:
  --record             Write the manifests as the new baseline
  --skip-run           Hash the existing outputs instead of regenerating them
  --baseline-dir <dir> Where manifests are stored (default: $baseline_dir)
  --list               List the suites and their manifest paths
  --help, -h           Show this help

Exits non-zero when a manifest differs from its baseline.
EOF
}

suites=()
while (($#)); do
  case "$1" in
    --record)
      mode="record"
      shift
      ;;
    --skip-run)
      run_projects=0
      shift
      ;;
    --baseline-dir)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      baseline_dir="$2"
      shift 2
      ;;
    --list)
      for suite in "${all_suites[@]}"; do
        echo "$suite: $baseline_dir/$suite.sha256"
      done
      exit 0
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    -*)
      usage
      exit 2
      ;;
    *)
      known=0
      for suite in "${all_suites[@]}"; do
        [[ "$1" == "$suite" ]] && known=1
      done
      if ((known == 0)); then
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

# The dev shell supplies sqlite3 and the ffprobe needed by the MKV fixtures.
if [[ -z "${IN_NIX_SHELL:-}" ]] || ! command -v sqlite3 >/dev/null 2>&1; then
  exec nix develop "path:$repo_root" --command "${BASH_SOURCE[0]}" "$@"
fi

cd "$repo_root"

mkdir -p "$baseline_dir"

if ((run_projects == 1)); then
  run_log="$baseline_dir/run-projects.log"
  echo "Regenerating suite outputs (log: $run_log)"
  if ! scripts/run-projects.sh "${suites[@]}" >"$run_log" 2>&1; then
    echo "Error: generation failed; see $run_log" >&2
    tail -n 20 "$run_log" >&2
    exit 1
  fi
fi

# Writes a "<sha256>  <relative path>" manifest for one suite output tree.
write_manifest() {
  local suite_output="$1"
  local manifest="$2"

  : >"$manifest"
  : >"$manifest.unsorted"
  local file rel digest
  while IFS= read -r -d '' file; do
    rel="${file#"$suite_output"/}"
    case "$file" in
      *.meta)
        # Canonical row dump: stable across SQLite page-layout differences.
        digest="$(sqlite3 "$file" .dump | sha256sum | cut -d' ' -f1)"
        ;;
      *)
        digest="$(sha256sum "$file" | cut -d' ' -f1)"
        ;;
    esac
    printf '%s  %s\n' "$digest" "$rel" >>"$manifest.unsorted"
  done < <(find "$suite_output" -type f \
    \( -name '*.cvbs' -o -name '*.cvbsy' -o -name '*.cvbsc' \
    -o -name '*.meta' -o -name '*.wav' -o -name '*.truth' \) -print0)

  LC_ALL=C sort -k2 "$manifest.unsorted" >"$manifest"
  rm -f "$manifest.unsorted"
}

differences=0
for suite in "${suites[@]}"; do
  suite_output="$output_root/$suite"
  if [[ ! -d "$suite_output" ]]; then
    echo "Error: no outputs for suite '$suite' at $suite_output" >&2
    echo "Run without --skip-run, or run scripts/run-projects.sh first." >&2
    exit 1
  fi

  baseline="$baseline_dir/$suite.sha256"
  current="$baseline_dir/$suite.sha256.current"
  write_manifest "$suite_output" "$current"

  file_count="$(wc -l <"$current")"
  if [[ "$mode" == "record" ]]; then
    mv "$current" "$baseline"
    echo "Recorded $file_count artefact hash(es) for suite '$suite' -> $baseline"
    continue
  fi

  if [[ ! -f "$baseline" ]]; then
    rm -f "$current"
    echo "Error: no baseline for suite '$suite' at $baseline" >&2
    echo "Record one first: scripts/output-hashes.sh --record $suite" >&2
    exit 1
  fi

  if cmp -s "$baseline" "$current"; then
    echo "Suite '$suite': $file_count artefact(s) match the baseline."
    rm -f "$current"
  else
    echo "Suite '$suite': artefacts differ from the baseline." >&2
    # Report the artefact names rather than the hash noise.
    LC_ALL=C comm -3 \
      <(LC_ALL=C sort "$baseline") <(LC_ALL=C sort "$current") |
      awk '{ print "  changed: " $NF }' | LC_ALL=C sort -u >&2
    echo "  full diff: diff -u $baseline $current" >&2
    differences=$((differences + 1))
  fi
done

if ((differences > 0)); then
  echo "$differences suite(s) differ from the recorded baseline." >&2
  exit 1
fi

if [[ "$mode" == "check" ]]; then
  echo "All checked suites match their baselines."
fi
