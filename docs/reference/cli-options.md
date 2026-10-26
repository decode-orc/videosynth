# CLI options

```text
Usage:
  videosynth --project <path> [options]
  videosynth --project <path> --validate [options]
```

The output path is not a command-line option — it comes from `output.video_path` in the project, and the metadata sidecar and audio tracks are colocated with it.

## Options

### `--project <path>`

The YAML project file. Required for every run except `--version`.

### `--validate`

Parse, validate and probe the sources, then exit without generating anything. Errors go to stderr and the exit code is non-zero; warnings are reported but do not fail the run.

### `--version`

Print the build version — the git commit hash the binary was built from — and exit.

### `--threads <n>`

Frame synthesis worker threads. `auto` (the default) uses the available hardware threads; a positive integer fixes the count; `1` selects the pure sequential path.

Output is **byte-identical regardless of the thread count**. This trades CPU for wall-clock and nothing else.

### `--template-cache-mb <n>`

Frame template cache capacity in MiB. Default `512`; `0` disables the cache.

The cache holds the parts of a frame that do not change between frames — sync structure, burst, static VBI content — and patches per frame with what does. Output is byte-identical either way; reduce it on a memory-constrained machine.

### `--log-level <level>`

`info` (default), `debug` or `trace`.

| Level | Contents |
|-------|----------|
| `info` | Stage transitions, validation summary, frame progress |
| `debug` | Configuration details, branch decisions, resolved paths |
| `trace` | Per-frame, per-line and per-sample state |

`trace` produces an enormous volume on anything but a very short project. Pair it with `--log-file`.

### `--log-file <path>`

Write logs to a file as well as to stderr.

### `--asset-root <name>=<path>`

Map the `{name}/…` logical asset root to a directory. Repeatable, and can register new roots as well as override the built-in `bundled` and `user` roots.

```bash
videosynth --project my.yaml \
    --asset-root bundled=/opt/videosynth/assets \
    --asset-root archive=/mnt/media/masters
```

### `--output-root <path>`

The directory `{output}/…` resolves to. Defaults to the project file's own directory. Shorthand for `--asset-root output=<path>`.

## Environment variables

| Variable | Effect |
|----------|--------|
| `VIDEOSYNTH_ASSET_DIR` | Where `{bundled}` resolves |
| `XDG_DATA_HOME` | Base for `{user}` (`$XDG_DATA_HOME/videosynth/assets`) |
| `VIDEOSYNTH_OUTPUT_DIR` | Where `{output}` resolves |

Command-line options take precedence over the environment. The GUI also looks for an installed asset library on the XDG data dirs, which takes precedence over `VIDEOSYNTH_ASSET_DIR` there (see [Asset roots](asset-roots.md)).

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success — validated cleanly, or generated successfully |
| non-zero | Validation errors, or a generation failure |

## The GUI's options

`videosynth-gui` accepts `--log-level` and `--log-file`, which override the persisted preferences for that session without changing the saved values, plus `--help` and `--version`. It takes no project path on the command line.

## Examples

```bash
# Validate without generating
videosynth --project projects/general/ntsc_vits.yaml --validate

# Sequential generation with debug logging to a file
videosynth --project projects/general/pal_audio.yaml \
    --threads 1 --log-level debug --log-file out/videosynth.log

# Send all generated media to a scratch directory
videosynth --project projects/stacking/pal_discsim_A.yaml \
    --output-root /mnt/scratch/videosynth
```
