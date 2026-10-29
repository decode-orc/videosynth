/*
 * File:        path_resolution.h
 * Module:      model
 * Purpose:     Resolves section-source and output paths using logical asset
 *              roots ({name}/path tokens) resolved at runtime.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "videosynth/model.h"

namespace videosynth {

// Thread-safety: all functions in this module are thread-safe pure functions
// over their inputs; AssetRootMap is a plain data container.

// Logical asset roots: name -> base directory. A base may be absolute or, if
// relative, is anchored to the project directory at resolution time. The names
// "project" and "output" are always available and need not be present in the
// map: "project" maps to the project directory, and "output" maps to the
// project directory unless the map (or the environment) overrides it.
struct AssetRootMap {
  std::map<std::string, std::string> roots;
};

// Names of the built-in roots understood by the resolver ("bundled", "user",
// "project", "output"). Used by the validator to flag unknown {name} tokens.
std::vector<std::string> BuiltinRootNames();

// True if `name` is a built-in root name.
bool IsBuiltinRootName(const std::string& name);

// Builds the default root map with no dependency on Qt (uses std::filesystem
// and environment variables only):
//   bundled = $VIDEOSYNTH_ASSET_DIR, else the compile-time
//             VIDEOSYNTH_BUNDLED_ASSET_DIR install/dev default.
//   user    = $XDG_DATA_HOME/videosynth/assets, else
//             $HOME/.local/share/videosynth/assets.
//   output  = $VIDEOSYNTH_OUTPUT_DIR when set; otherwise left unset so that
//             "{output}" falls back to the project directory.
// The GUI starts from this map and may override bundled/user via
// QStandardPaths. Empty when a location cannot be determined.
AssetRootMap DefaultAssetRoots();

// Resolves a single `path` for a section source or output target:
//   - Leading "{name}" (optionally "{name}/rest"): resolved to
//     `roots[name] / rest`. The built-in "project" root maps to `project_dir`,
//     as does "output" when the map carries no entry for it. A root base that
//     is itself relative is anchored to `project_dir`. When `name` is not a
//     known root the path is returned unchanged (the validator surfaces the
//     error).
//   - Absolute path: returned unchanged.
//   - Plain relative path: resolved against `project_dir` when `anchor_unset`
//     is true (the GUI), otherwise returned unchanged (the CLI, preserving the
//     working-directory-relative convention).
//   - Empty path: returned unchanged.
std::string ResolveAssetPath(const std::string& path, const AssetRootMap& roots,
                             const std::string& project_dir, bool anchor_unset);

// Derives the metadata sidecar path colocated with a video output path: strips
// a trailing ".cvbs" or ".cvbsy" (otherwise any final extension in the last
// path component) and appends ".meta". An empty input returns an empty string.
// The metadata file always shares the video output's directory and stem; it is
// never configured independently.
std::string DeriveMetadataPath(const std::string& video_path);

// Resolves a single relative `path` against `base_dir`. Absolute and empty
// paths are returned unchanged; an empty `base_dir` returns `path` unchanged.
// Otherwise `base_dir / path` is returned, lexically normalised.
std::string ResolvePathAgainstBase(const std::string& base_dir,
                                   const std::string& path);

// Returns a copy of `project` with every section source, every audio-file
// channel-pair path, and both output targets resolved via ResolveAssetPath.
//
// `anchor_unset` distinguishes the callers: the GUI passes true so a saved
// project's plain-relative paths anchor to its file's directory (probe,
// preview, and generation stay consistent); the CLI passes false so plain
// relative paths keep the working-directory-relative behaviour.
Project ResolveProjectPaths(Project project, const AssetRootMap& roots,
                            const std::string& project_dir, bool anchor_unset);

// Creates the directories that will hold `file_path`. A bare file name lives
// in the working directory, so it has no parent to create and succeeds. On
// failure returns false and sets `ec`.
bool CreateParentDirectories(const std::string& file_path, std::error_code* ec);

}  // namespace videosynth
