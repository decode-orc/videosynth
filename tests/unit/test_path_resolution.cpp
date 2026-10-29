/*
 * File:        test_path_resolution.cpp
 * Module:      model_tests
 * Purpose:     Unit tests for logical asset-root path resolution
 *              ({name}/path tokens, DefaultAssetRoots, anchor_unset).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include "videosynth/model.h"
#include "videosynth/path_resolution.h"

namespace videosynth {
namespace {

AssetRootMap Roots(std::map<std::string, std::string> roots) {
  AssetRootMap map;
  map.roots = std::move(roots);
  return map;
}

// --- token resolution ------------------------------------------------------

TEST(PathResolutionTest, BundledTokenResolvesToRootBase) {
  const AssetRootMap roots = Roots({{"bundled", "/assets"}});
  EXPECT_EQ(ResolveAssetPath("{bundled}/exr/bars.exr", roots, "/proj",
                             /*anchor_unset=*/true),
            "/assets/exr/bars.exr");
}

TEST(PathResolutionTest, BareTokenResolvesToRootDirectory) {
  const AssetRootMap roots = Roots({{"bundled", "/assets"}});
  EXPECT_EQ(ResolveAssetPath("{bundled}", roots, "/proj", true), "/assets");
}

TEST(PathResolutionTest, ProjectTokenAlwaysMapsToProjectDir) {
  const AssetRootMap roots;  // no explicit "project" entry needed
  EXPECT_EQ(ResolveAssetPath("{project}/clip.mkv", roots, "/proj/dir", false),
            "/proj/dir/clip.mkv");
  EXPECT_EQ(ResolveAssetPath("{project}", roots, "/proj/dir", false),
            "/proj/dir");
}

TEST(PathResolutionTest, UnknownTokenIsLeftUnchanged) {
  const AssetRootMap roots = Roots({{"bundled", "/assets"}});
  EXPECT_EQ(ResolveAssetPath("{nope}/x.exr", roots, "/proj", true),
            "{nope}/x.exr");
}

TEST(PathResolutionTest, RelativeRootBaseIsAnchoredToProjectDir) {
  const AssetRootMap roots = Roots({{"bundled", "shared/assets"}});
  EXPECT_EQ(ResolveAssetPath("{bundled}/bars.exr", roots, "/proj", false),
            "/proj/shared/assets/bars.exr");
}

// --- non-token paths -------------------------------------------------------

TEST(PathResolutionTest, AbsolutePathUnchanged) {
  EXPECT_EQ(ResolveAssetPath("/abs/bars.exr", {}, "/proj", true),
            "/abs/bars.exr");
}

TEST(PathResolutionTest, PlainRelativeAnchoredOnlyWhenRequested) {
  EXPECT_EQ(
      ResolveAssetPath("clips/bars.exr", {}, "/proj", /*anchor_unset=*/true),
      "/proj/clips/bars.exr");
  EXPECT_EQ(
      ResolveAssetPath("clips/bars.exr", {}, "/proj", /*anchor_unset=*/false),
      "clips/bars.exr");
}

TEST(PathResolutionTest, EmptyPathUnchanged) {
  EXPECT_EQ(ResolveAssetPath("", {}, "/proj", true), "");
}

// --- ResolveProjectPaths ---------------------------------------------------

TEST(PathResolutionTest, ResolveProjectPathsAppliesToSourcesAndOutput) {
  Project project;
  Section section;
  section.source = "{bundled}/exr/bars.exr";
  project.sections.push_back(section);
  project.output.video_path = "{project}/out.cvbs";
  project.output.metadata_path = "out.meta";

  const AssetRootMap roots = Roots({{"bundled", "/assets"}});
  const Project resolved = ResolveProjectPaths(project, roots, "/proj",
                                               /*anchor_unset=*/true);
  EXPECT_EQ(resolved.sections[0].source, "/assets/exr/bars.exr");
  EXPECT_EQ(resolved.output.video_path, "/proj/out.cvbs");
  EXPECT_EQ(resolved.output.metadata_path, "/proj/out.meta");  // plain relative
}

TEST(PathResolutionTest, ResolveProjectPathsAppliesToAudioFilePairs) {
  Project project;
  Section section;
  section.source = "{project}/capture.mp4";
  AudioChannelPair narration;
  narration.content = AudioPairContent::kFile;
  narration.file.path = "{project}/narration.flac";
  AudioChannelPair soundtrack;
  soundtrack.content = AudioPairContent::kVideo;
  section.audio_channel_pairs = {narration, soundtrack};
  project.sections.push_back(section);

  const Project resolved =
      ResolveProjectPaths(project, {}, "/proj", /*anchor_unset=*/true);
  EXPECT_EQ(resolved.sections[0].audio_channel_pairs[0].file.path,
            "/proj/narration.flac");
  EXPECT_TRUE(resolved.sections[0].audio_channel_pairs[1].file.path.empty());
}

// --- built-in names --------------------------------------------------------

TEST(PathResolutionTest, BuiltinRootNames) {
  EXPECT_TRUE(IsBuiltinRootName("bundled"));
  EXPECT_TRUE(IsBuiltinRootName("user"));
  EXPECT_TRUE(IsBuiltinRootName("project"));
  EXPECT_TRUE(IsBuiltinRootName("output"));
  EXPECT_FALSE(IsBuiltinRootName("nope"));
}

// --- output root -----------------------------------------------------------

TEST(PathResolutionTest, OutputTokenFallsBackToProjectDirWhenUnmapped) {
  AssetRootMap roots;
  roots.roots["bundled"] = "/assets";

  EXPECT_EQ(ResolveAssetPath("{output}/clip.cvbs", roots, "/proj", false),
            "/proj/clip.cvbs");
  EXPECT_EQ(ResolveAssetPath("{output}", roots, "/proj", false), "/proj");
}

TEST(PathResolutionTest, OutputTokenUsesMappedRootWhenPresent) {
  AssetRootMap roots;
  roots.roots["output"] = "/scratch/run";

  EXPECT_EQ(ResolveAssetPath("{output}/clip.cvbs", roots, "/proj", false),
            "/scratch/run/clip.cvbs");
}

TEST(PathResolutionTest, OutputRootBaseIsAnchoredToProjectDirWhenRelative) {
  AssetRootMap roots;
  roots.roots["output"] = "out";

  EXPECT_EQ(ResolveAssetPath("{output}/clip.cvbs", roots, "/proj", false),
            "/proj/out/clip.cvbs");
}

// --- DefaultAssetRoots (environment) ---------------------------------------

TEST(PathResolutionTest, DefaultAssetRootsHonoursEnvironment) {
  ::setenv("VIDEOSYNTH_ASSET_DIR", "/opt/videosynth/assets", 1);
  ::setenv("XDG_DATA_HOME", "/home/tester/.data", 1);

  const AssetRootMap map = DefaultAssetRoots();
  EXPECT_EQ(map.roots.at("bundled"), "/opt/videosynth/assets");
  EXPECT_EQ(map.roots.at("user"), "/home/tester/.data/videosynth/assets");

  ::unsetenv("VIDEOSYNTH_ASSET_DIR");
  ::unsetenv("XDG_DATA_HOME");
}

TEST(PathResolutionTest, DefaultAssetRootsMapsOutputOnlyWhenEnvironmentSets) {
  ::unsetenv("VIDEOSYNTH_OUTPUT_DIR");
  EXPECT_EQ(DefaultAssetRoots().roots.count("output"), 0U);

  ::setenv("VIDEOSYNTH_OUTPUT_DIR", "/scratch/videosynth", 1);
  EXPECT_EQ(DefaultAssetRoots().roots.at("output"), "/scratch/videosynth");

  ::unsetenv("VIDEOSYNTH_OUTPUT_DIR");
}

// --- ResolvePathAgainstBase primitive --------------------------------------

TEST(PathResolutionTest, ResolvePathAgainstBaseNormalises) {
  EXPECT_EQ(ResolvePathAgainstBase("/a/b", "c/d.exr"), "/a/b/c/d.exr");
  EXPECT_EQ(ResolvePathAgainstBase("/a/b", "/abs.exr"), "/abs.exr");
  EXPECT_EQ(ResolvePathAgainstBase("", "c/d.exr"), "c/d.exr");
  EXPECT_EQ(ResolvePathAgainstBase("/a/b", ""), "");
}

TEST(PathResolutionTest, CreateParentDirectoriesAcceptsBareFileName) {
  // A bare name has an empty parent path; it lives in the working directory,
  // so there is nothing to create and no filesystem call is made.
  std::error_code ec = std::make_error_code(std::errc::io_error);
  EXPECT_TRUE(CreateParentDirectories("capture.cvbs", &ec));
  EXPECT_FALSE(ec);
}

}  // namespace
}  // namespace videosynth
