# Asset roots and path resolution

Source and output paths use `{name}/…` brace tokens rather than machine-specific paths, so a project references *what* an asset is rather than *where* it happens to live. Projects then survive reinstalls, repackaging and moving between machines, and behave identically in the CLI and the GUI.

```yaml
output:
  video_path: "{output}/pal_disc.cvbs"

sections:
  - source: "{bundled}/exr/720x576/100_BARS.exr"
```

## Built-in roots

| Root | Meaning | Resolves to | Override |
|------|---------|-------------|----------|
| `{bundled}` | Read-only assets shipped with the application | **CLI**: `$VIDEOSYNTH_ASSET_DIR`, else the directory compiled into the build (the development `videosynth-assets/assets` by default; packaged builds set the installed location). **GUI**: an installed `videosynth/assets` found on the XDG data dirs (includes Flatpak `/app/share`) first, then as for the CLI | `--asset-root bundled=<path>` (CLI) |
| `{user}` | The user's own writable asset library | `$XDG_DATA_HOME/videosynth/assets`, else `~/.local/share/videosynth/assets` | `--asset-root user=<path>` |
| `{project}` | The project file's own directory | The directory containing the `.yaml` | — |
| `{output}` | Where this run's generated media is written | `$VIDEOSYNTH_OUTPUT_DIR` or `--output-root <path>`, else the project file's directory | `--output-root <path>` |

## Resolution rules

For each source or output path:

| Form | Resolution |
|------|------------|
| `{name}/rest` | `rootDir(name) / rest`. A bare `{name}` resolves to the root directory itself. A root directory that is itself relative is anchored to the project file's directory. An **unknown** root name is a validation error |
| Absolute, or empty | Unchanged |
| Plain relative (no token) | **CLI**: relative to the working directory. **GUI**: relative to the saved project file's directory |

!!! warning "Avoid bare relative paths"
    A plain relative path resolves differently in the CLI and the GUI, so a project that works in one will fail in the other. Use `{project}/rest` for a path that must resolve against the project file under both.

## Why `{output}` is separate

`{output}` separates *what a project's artefacts are called* from *where a particular run puts them*. The project names its files relative to `{output}`; the caller decides the directory.

- Run by hand with no `--output-root`, `{output}` is the project's own directory, so the project is self-contained.
- A batch run redirects every project at once — the repository's `scripts/run-projects.sh` sends each suite to `build/project-output/<suite>/`.
- The functional test suites redirect it to a scratch directory, so a test run never writes into the checked-in project tree.

## Overriding roots on the command line

`--asset-root` is repeatable and can register new named roots as well as override the built-ins:

```bash
videosynth --project my.yaml \
    --asset-root bundled=/opt/videosynth/assets \
    --asset-root archive=/mnt/media/masters
```

The project can then reference `{archive}/clip.mkv`.

`--output-root <path>` is shorthand for `--asset-root output=<path>`.

## In the GUI

The GUI does not expose the roots directly. The section editor presents a source as a two-way choice mapped onto the tokens:

- **Built-in asset** composes `{bundled}/<type>/<raster>/<file>`, where `<type>` is the asset kind (`exr` still or `mkv` video), `<raster>` is derived from the project's video standard, and only `<file>` is chosen from a dropdown. Because the path is always recomposed from the *current* raster, changing the video standard self-heals the source to the matching asset.
- **Local file** is a browsed path, stored as `{project}/…` when **Relative to project** is ticked, or as an absolute path otherwise. A source already carrying another logical token — `{user}/…`, say — is preserved verbatim.

New GUI projects are saved to disk as part of creation, so the `{project}` anchor always exists, and their default source uses `{bundled}` so a fresh project previews immediately.

## Bundled asset layout

```text
{bundled}/exr/720x576/<file>.exr     PAL stills
{bundled}/exr/720x486/<file>.exr     NTSC / PAL-M stills
{bundled}/mkv/720x576/<file>.mkv     PAL clips
{bundled}/mkv/720x486/<file>.mkv     NTSC / PAL-M clips
```

The catalogue is listed in [Picture sources](../user-manual/sources.md#bundled-assets).
