/*
 * File:        test_section_block_presenters.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for the optional-block mapping layer (audio,
 *              noise, dropouts, luma bandwidth, OSD)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "section_block_presenters.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth::gui {
namespace {

Project MakeValidProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = "out/video.cvbs";
  project.output.metadata_path = "out/video.meta";
  Section section;
  section.name = "Bars";
  section.type = "progressive";
  section.source = "assets/bars.exr";
  section.duration_frames = 200;
  project.sections.push_back(section);
  return project;
}

std::string EmitYaml(const Project& project) {
  const YamlProjectEmitter emitter;
  return emitter.EmitString(project);
}

bool ProjectIsValid(const Project& project, std::string* first_error) {
  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  if (!result.is_valid && first_error != nullptr && !result.errors.empty()) {
    *first_error = result.errors.front();
  }
  return result.is_valid;
}

TEST(SectionBlockPresenterTest, NoiseEnableEmitsBlockDisableRemovesIt) {
  Project project = MakeValidProject();
  EXPECT_EQ(EmitYaml(project).find("noise:"), std::string::npos);

  SetNoiseBlockEnabled(&project.sections[0], true);
  EXPECT_NE(EmitYaml(project).find("noise:"), std::string::npos);

  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;

  SetNoiseBlockEnabled(&project.sections[0], false);
  EXPECT_EQ(EmitYaml(project).find("noise:"), std::string::npos);
  EXPECT_EQ(project.sections[0].noise, NoiseParameters{});
}

TEST(SectionBlockPresenterTest, NoiseToggleKeepsSharedPressingLayer) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  section.noise.shared.enabled = true;
  section.noise.shared.noise_db = 44.0;
  section.noise.shared.noise_seed = 77;
  section.noise.shared.noise_seed_specified = true;
  section.noise.shared.spectrum = {{0.0, 0.0}, {6.0, 10.0}};
  const SharedNoiseParameters shared = section.noise.shared;

  SetNoiseBlockEnabled(&section, true);
  EXPECT_TRUE(section.noise.enabled);
  EXPECT_EQ(section.noise.shared, shared);

  SetNoiseBlockEnabled(&section, false);
  EXPECT_FALSE(section.noise.enabled);
  EXPECT_EQ(section.noise.shared, shared);
  EXPECT_NE(EmitYaml(project).find("shared:"), std::string::npos);
}

TEST(SectionBlockPresenterTest, DropoutEnableEmitsBlockDisableRemovesIt) {
  Project project = MakeValidProject();
  EXPECT_EQ(EmitYaml(project).find("dropouts:"), std::string::npos);

  SetRandomDropoutsEnabled(&project.sections[0], true);
  EXPECT_NE(EmitYaml(project).find("random:"), std::string::npos);
  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;

  SetScratchDropoutsEnabled(&project.sections[0], true);
  EXPECT_NE(EmitYaml(project).find("scratch:"), std::string::npos);
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;

  SetRandomDropoutsEnabled(&project.sections[0], false);
  SetScratchDropoutsEnabled(&project.sections[0], false);
  EXPECT_EQ(EmitYaml(project).find("dropouts:"), std::string::npos);
}

// Emits the project and parses it back, as a save and reload would.
Project RoundTrip(const Project& project) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(EmitYaml(project));
  EXPECT_TRUE(parsed.ok) << (parsed.errors.empty() ? "" : parsed.errors[0]);
  return parsed.project;
}

TEST(SectionBlockPresenterTest, SharedNoiseEnableSeedsAValidSeededLayer) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  SetSharedNoiseEnabled(&section, true);
  EXPECT_TRUE(section.noise.shared.enabled);
  EXPECT_TRUE(section.noise.shared.noise_seed_specified);
  EXPECT_FALSE(section.noise.enabled);
  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;
  EXPECT_EQ(RoundTrip(project), project);

  SetSharedNoiseEnabled(&section, false);
  EXPECT_EQ(section.noise, NoiseParameters{});
  EXPECT_EQ(EmitYaml(project).find("shared:"), std::string::npos);
}

TEST(SectionBlockPresenterTest, SharedNoiseEnableKeepsAnExistingLayer) {
  Section section;
  SetSharedNoiseEnabled(&section, true);
  section.noise.shared.noise_db = 40.0;
  section.noise.shared.noise_seed = 77;
  SetSharedNoiseEnabled(&section, true);
  EXPECT_EQ(section.noise.shared.noise_db, 40.0);
  EXPECT_EQ(section.noise.shared.noise_seed, 77);
}

TEST(SectionBlockPresenterTest, SharedLevelTermEnableIsValidAndRoundTrips) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  SetSharedNoiseEnabled(&section, true);
  SetSharedNoiseLevelTermEnabled(&section, true);
  EXPECT_TRUE(section.noise.shared.level_term.enabled);
  ASSERT_EQ(section.noise.shared.level_term.level_db.size(), 1U);
  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;
  EXPECT_EQ(RoundTrip(project), project);

  // Enabling again keeps an edited term; disabling leaves the layer.
  section.noise.shared.level_term.noise_db = 36.0;
  SetSharedNoiseLevelTermEnabled(&section, true);
  EXPECT_EQ(section.noise.shared.level_term.noise_db, 36.0);
  SetSharedNoiseLevelTermEnabled(&section, false);
  EXPECT_EQ(section.noise.shared.level_term, SharedNoiseLevelTerm{});
  EXPECT_TRUE(section.noise.shared.enabled);
  EXPECT_EQ(EmitYaml(project).find("level_term:"), std::string::npos);
}

TEST(SectionBlockPresenterTest, LevelPointsStepUpAndStopAtTheMaximum) {
  std::vector<NoiseLevelPoint> curve;
  AddNoiseLevelPoint(&curve);
  ASSERT_EQ(curve.size(), 1U);
  EXPECT_EQ(curve[0].level_mv, 0.0);
  EXPECT_EQ(curve[0].relative_db, 0.0);

  curve[0].relative_db = -6.0;
  AddNoiseLevelPoint(&curve);
  ASSERT_EQ(curve.size(), 2U);
  EXPECT_EQ(curve[1].level_mv, 50.0);
  EXPECT_EQ(curve[1].relative_db, -6.0);

  // Closer to the maximum than a full step: halfway instead.
  curve[1].level_mv = 780.0;
  AddNoiseLevelPoint(&curve);
  ASSERT_EQ(curve.size(), 3U);
  EXPECT_EQ(curve[2].level_mv, 790.0);

  curve[2].level_mv = editor_limits::kNoiseLevelMvMax;
  AddNoiseLevelPoint(&curve);
  EXPECT_EQ(curve.size(), 3U);

  RemoveNoiseLevelPoint(&curve, 3);
  RemoveNoiseLevelPoint(&curve, -1);
  EXPECT_EQ(curve.size(), 3U);
  RemoveNoiseLevelPoint(&curve, 1);
  ASSERT_EQ(curve.size(), 2U);
  EXPECT_EQ(curve[1].level_mv, editor_limits::kNoiseLevelMvMax);
}

TEST(SectionBlockPresenterTest, SpectrumPointsStepUpAndStopAtTheMaximum) {
  std::vector<NoiseSpectrumPoint> spectrum;
  AddNoiseSpectrumPoint(&spectrum, 3.0);
  ASSERT_EQ(spectrum.size(), 1U);
  EXPECT_EQ(spectrum[0].frequency_mhz, 0.0);
  EXPECT_EQ(spectrum[0].relative_db, 0.0);

  spectrum[0].relative_db = -6.0;
  AddNoiseSpectrumPoint(&spectrum, 3.0);
  ASSERT_EQ(spectrum.size(), 2U);
  EXPECT_EQ(spectrum[1].frequency_mhz, 1.0);
  EXPECT_EQ(spectrum[1].relative_db, -6.0);

  // Closer to the maximum than a full step: halfway instead.
  spectrum[1].frequency_mhz = 2.5;
  AddNoiseSpectrumPoint(&spectrum, 3.0);
  ASSERT_EQ(spectrum.size(), 3U);
  EXPECT_EQ(spectrum[2].frequency_mhz, 2.75);

  spectrum[2].frequency_mhz = 3.0;
  AddNoiseSpectrumPoint(&spectrum, 3.0);
  EXPECT_EQ(spectrum.size(), 3U);
}

TEST(SectionBlockPresenterTest, RemoveSpectrumPointIgnoresOutOfRangeRows) {
  std::vector<NoiseSpectrumPoint> spectrum = {
      {0.0, 0.0}, {1.0, -3.0}, {2.0, -6.0}};
  RemoveNoiseSpectrumPoint(&spectrum, 3);
  RemoveNoiseSpectrumPoint(&spectrum, -1);
  EXPECT_EQ(spectrum.size(), 3U);
  RemoveNoiseSpectrumPoint(&spectrum, 1);
  ASSERT_EQ(spectrum.size(), 2U);
  EXPECT_EQ(spectrum[1].frequency_mhz, 2.0);
}

TEST(SectionBlockPresenterTest, AddedSpectrumPointsValidateOnEveryStandard) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeValidProject();
    project.cvbs_presets.video_standard_preset = standard;
    Section& section = project.sections[0];
    SetNoiseBlockEnabled(&section, true);
    SetSharedNoiseEnabled(&section, true);
    const double max_mhz = NoiseSpectrumMaxFrequencyMhz(standard);
    // Enough presses to run into the maximum.
    for (int i = 0; i < 40; ++i) {
      AddNoiseSpectrumPoint(&section.noise.spectrum, max_mhz);
      AddNoiseSpectrumPoint(&section.noise.shared.spectrum, max_mhz);
    }
    section.noise.spectrum.back().frequency_mhz = max_mhz;
    std::string error;
    EXPECT_TRUE(ProjectIsValid(project, &error))
        << StandardToString(standard) << ": " << error;

    section.noise.spectrum.back().frequency_mhz = max_mhz + 0.01;
    EXPECT_FALSE(ProjectIsValid(project, nullptr));
  }
}

TEST(SectionBlockPresenterTest, SpectrumEditsRoundTripToTheModel) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  SetNoiseBlockEnabled(&section, true);
  section.noise.spectrum = {{0.0, 0.0}, {2.0, -12.5}, {4.0, -30.0}};
  SetSharedNoiseEnabled(&section, true);
  section.noise.shared.spectrum = {{1.0, 6.0}, {3.5, -3.0}};
  std::string error;
  ASSERT_TRUE(ProjectIsValid(project, &error)) << error;
  EXPECT_EQ(RoundTrip(project), project);
}

TEST(SectionBlockPresenterTest, SpectrumLevelLimitsMirrorValidator) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  SetNoiseBlockEnabled(&section, true);
  section.noise.spectrum = {{0.0, editor_limits::kNoiseSpectrumRelativeDbMin},
                            {1.0, editor_limits::kNoiseSpectrumRelativeDbMax}};
  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;
  section.noise.spectrum[0].relative_db -= 0.1;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
  section.noise.spectrum[0].relative_db += 0.1;
  section.noise.spectrum[1].relative_db += 0.1;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
}

TEST(SectionBlockPresenterTest, DiscAnchorFixesASeedWhenNoneIsSet) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  SetRandomDropoutsEnabled(&section, true);
  SetScratchDropoutsEnabled(&section, true);
  SetRandomDropoutAnchor(&section, DropoutAnchor::kDisc);
  SetScratchDropoutAnchor(&section, DropoutAnchor::kDisc);
  EXPECT_EQ(section.dropouts.random.anchor, DropoutAnchor::kDisc);
  EXPECT_TRUE(section.dropouts.random.seed_specified);
  EXPECT_EQ(section.dropouts.scratch.anchor, DropoutAnchor::kDisc);
  EXPECT_TRUE(section.dropouts.scratch.seed_specified);
  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;

  // Without the seed the validator rejects a disc anchor.
  section.dropouts.random.seed_specified = false;
  section.dropouts.random.seed = 0;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
}

TEST(SectionBlockPresenterTest, AnchorChangesKeepAnExistingSeed) {
  Section section;
  SetRandomDropoutsEnabled(&section, true);
  section.dropouts.random.seed_specified = true;
  section.dropouts.random.seed = 4242;
  SetRandomDropoutAnchor(&section, DropoutAnchor::kDisc);
  EXPECT_EQ(section.dropouts.random.seed, 4242);
  SetRandomDropoutAnchor(&section, DropoutAnchor::kSection);
  EXPECT_EQ(section.dropouts.random.anchor, DropoutAnchor::kSection);
  EXPECT_TRUE(section.dropouts.random.seed_specified);
  EXPECT_EQ(section.dropouts.random.seed, 4242);
}

TEST(SectionBlockPresenterTest, FlaggedAndAnchorEditsRoundTripToTheModel) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  SetRandomDropoutsEnabled(&section, true);
  SetScratchDropoutsEnabled(&section, true);
  section.dropouts.random.flagged = false;
  SetRandomDropoutAnchor(&section, DropoutAnchor::kDisc);
  section.dropouts.scratch.flagged = false;
  std::string error;
  ASSERT_TRUE(ProjectIsValid(project, &error)) << error;
  EXPECT_EQ(RoundTrip(project), project);
}

TEST(SectionBlockPresenterTest, LumaBandwidthEnableSeedsAValidLimit) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeValidProject();
    project.cvbs_presets.video_standard_preset = standard;
    Section& section = project.sections[0];
    SetLumaBandwidthEnabled(&section, true);
    EXPECT_TRUE(section.luma_bandwidth.enabled);
    EXPECT_EQ(section.luma_bandwidth.transition_mhz,
              kLumaBandwidthTransitionDefaultMhz);
    std::string error;
    EXPECT_TRUE(ProjectIsValid(project, &error)) << error;
    EXPECT_EQ(RoundTrip(project), project);

    SetLumaBandwidthEnabled(&section, false);
    EXPECT_EQ(section.luma_bandwidth, LumaBandwidthParameters{});
    EXPECT_EQ(EmitYaml(project).find("luma_bandwidth"), std::string::npos);
  }
}

TEST(SectionBlockPresenterTest, LumaBandwidthLimitsMirrorValidator) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    SCOPED_TRACE(StandardToString(standard));
    Project project = MakeValidProject();
    project.cvbs_presets.video_standard_preset = standard;
    Section& section = project.sections[0];
    SetLumaBandwidthEnabled(&section, true);
    LumaBandwidthParameters& bandwidth = section.luma_bandwidth;
    for (const double transition :
         {editor_limits::kLumaBandwidthTransitionMinMhz,
          kLumaBandwidthTransitionDefaultMhz,
          editor_limits::kLumaBandwidthTransitionMaxMhz}) {
      bandwidth.transition_mhz = transition;
      const double max_cutoff = LumaBandwidthMaxCutoffMhz(standard, transition);
      bandwidth.cutoff_mhz = max_cutoff;
      EXPECT_TRUE(ProjectIsValid(project, nullptr)) << transition;
      bandwidth.cutoff_mhz = max_cutoff + 0.01;
      EXPECT_FALSE(ProjectIsValid(project, nullptr)) << transition;
      bandwidth.cutoff_mhz = editor_limits::kLumaBandwidthCutoffMinMhz;
      EXPECT_TRUE(ProjectIsValid(project, nullptr)) << transition;
      bandwidth.cutoff_mhz -= 0.01;
      EXPECT_FALSE(ProjectIsValid(project, nullptr)) << transition;
    }
    bandwidth.cutoff_mhz = 3.0;
    bandwidth.transition_mhz =
        editor_limits::kLumaBandwidthTransitionMinMhz - 0.01;
    EXPECT_FALSE(ProjectIsValid(project, nullptr));
    bandwidth.transition_mhz =
        editor_limits::kLumaBandwidthTransitionMaxMhz + 0.01;
    EXPECT_FALSE(ProjectIsValid(project, nullptr));
  }
}

TEST(SectionBlockPresenterTest, ApplySectionEditDeltaMirrorsNoiseLayersApart) {
  Section before;
  before.name = "Edited";
  Section target;
  target.name = "Target";
  SetNoiseBlockEnabled(&target, true);
  target.noise.noise_db = 30.0;

  // Adding the shared layer leaves the target's own capture layer alone.
  Section after = before;
  SetSharedNoiseEnabled(&after, true);
  after.noise.shared.noise_seed = 99;
  Section mirrored = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(mirrored.noise.shared, after.noise.shared);
  EXPECT_TRUE(mirrored.noise.enabled);
  EXPECT_EQ(mirrored.noise.noise_db, 30.0);

  // A capture-layer edit leaves the target's shared layer alone.
  Section shared_target = target;
  SetSharedNoiseEnabled(&shared_target, true);
  shared_target.noise.shared.noise_seed = 5;
  Section capture_after = before;
  SetNoiseBlockEnabled(&capture_after, true);
  capture_after.noise.spectrum = {{0.0, 0.0}, {2.0, -10.0}};
  mirrored = ApplySectionEditDelta(before, capture_after, shared_target);
  EXPECT_EQ(mirrored.noise.spectrum, capture_after.noise.spectrum);
  EXPECT_EQ(mirrored.noise.noise_db, capture_after.noise.noise_db);
  EXPECT_EQ(mirrored.noise.shared.noise_seed, 5);
}

TEST(SectionBlockPresenterTest, ApplySectionEditDeltaMirrorsLumaBandwidth) {
  Section before;
  Section after = before;
  SetLumaBandwidthEnabled(&after, true);
  after.luma_bandwidth.cutoff_mhz = 2.5;
  Section target;
  target.name = "Target";
  const Section mirrored = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(mirrored.luma_bandwidth, after.luma_bandwidth);
  EXPECT_EQ(mirrored.name, "Target");

  // An unrelated edit never overwrites the target's own limit.
  Section limited = target;
  SetLumaBandwidthEnabled(&limited, true);
  limited.luma_bandwidth.cutoff_mhz = 4.0;
  Section renamed = before;
  renamed.source = "assets/other.exr";
  EXPECT_EQ(ApplySectionEditDelta(before, renamed, limited).luma_bandwidth,
            limited.luma_bandwidth);
}

TEST(SectionBlockPresenterTest, ApplySectionEditDeltaMirrorsFlaggedAndAnchor) {
  Section before;
  SetRandomDropoutsEnabled(&before, true);
  Section after = before;
  after.dropouts.random.flagged = false;
  SetRandomDropoutAnchor(&after, DropoutAnchor::kDisc);
  Section target;
  SetScratchDropoutsEnabled(&target, true);
  const Section mirrored = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(mirrored.dropouts.random, after.dropouts.random);
  EXPECT_EQ(mirrored.dropouts.scratch, target.dropouts.scratch);
}

TEST(SectionBlockPresenterTest, AddChannelPairEmitsBlockClearRemovesIt) {
  Project project = MakeValidProject();
  EXPECT_EQ(EmitYaml(project).find("audio:"), std::string::npos);

  project.sections[0].audio_channel_pairs.push_back(
      MakeDefaultAudioChannelPair(0));
  EXPECT_NE(EmitYaml(project).find("audio:"), std::string::npos);
  EXPECT_NE(EmitYaml(project).find("channel_pairs:"), std::string::npos);
  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;

  project.sections[0].audio_channel_pairs.clear();
  EXPECT_EQ(EmitYaml(project).find("audio:"), std::string::npos);
}

TEST(SectionBlockPresenterTest, DefaultChannelPairHasIdenticalStereoChannels) {
  const AudioChannelPair channel_pair = MakeDefaultAudioChannelPair(4);
  EXPECT_EQ(channel_pair.pair, 4);
  EXPECT_TRUE(channel_pair.pair_specified);
  EXPECT_TRUE(channel_pair.left.enabled);
  EXPECT_EQ(channel_pair.left.waveform, AudioWaveform::kSine);
  // Identical channels are what the editor infers as its linked
  // "same tone on both channels" mode.
  EXPECT_TRUE(channel_pair.right == channel_pair.left);
}

TEST(SectionBlockPresenterTest, ChannelRampEnableSeedsValidRamp) {
  Project project = MakeValidProject();
  AudioChannelPair channel_pair = MakeDefaultAudioChannelPair(0);
  SetAudioChannelRampEnabled(&channel_pair.left, true);
  project.sections[0].audio_channel_pairs.push_back(channel_pair);

  const AudioParameters& audio =
      project.sections[0].audio_channel_pairs[0].left;
  EXPECT_TRUE(audio.ramp_enabled);
  EXPECT_TRUE(audio.ramp_start_specified);
  EXPECT_TRUE(audio.ramp_end_specified);
  EXPECT_EQ(audio.ramp_mode, AudioRampMode::kUp);
  EXPECT_NE(EmitYaml(project).find("ramp:"), std::string::npos);

  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;

  SetAudioChannelRampEnabled(&project.sections[0].audio_channel_pairs[0].left,
                             false);
  EXPECT_EQ(EmitYaml(project).find("ramp:"), std::string::npos);
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;
}

TEST(SectionBlockPresenterTest, SetChannelWaveformKeepsTextAndEnumConsistent) {
  AudioParameters channel = MakeDefaultAudioChannel();
  SetAudioChannelWaveform(&channel, "triangle");
  EXPECT_EQ(channel.waveform, AudioWaveform::kTriangle);
  EXPECT_EQ(channel.waveform_text, "triangle");
}

TEST(SectionBlockPresenterTest, NextFreeChannelPairSkipsUsedNumbers) {
  std::vector<AudioChannelPair> pairs;
  EXPECT_EQ(NextFreeAudioChannelPair(pairs), 0);
  pairs.push_back(MakeDefaultAudioChannelPair(0));
  pairs.push_back(MakeDefaultAudioChannelPair(1));
  EXPECT_EQ(NextFreeAudioChannelPair(pairs), 2);
  for (int pair = 2; pair < kMaxAudioChannelPairs; ++pair) {
    pairs.push_back(MakeDefaultAudioChannelPair(pair));
  }
  EXPECT_EQ(NextFreeAudioChannelPair(pairs), -1);  // All eight used.
}

TEST(SectionBlockPresenterTest, OsdBlockFollowsOverlayList) {
  Project project = MakeValidProject();
  EXPECT_FALSE(OsdBlockEnabled(project.sections[0]));
  EXPECT_EQ(EmitYaml(project).find("osd:"), std::string::npos);

  project.sections[0].osd.overlays.push_back(MakeDefaultOsdOverlay());
  EXPECT_TRUE(OsdBlockEnabled(project.sections[0]));
  EXPECT_NE(EmitYaml(project).find("osd:"), std::string::npos);

  std::string error;
  EXPECT_TRUE(ProjectIsValid(project, &error)) << error;
}

// The editor limits must accept exactly what the validator accepts: values
// at both bounds pass, values one step outside fail.
TEST(SectionBlockPresenterTest, NoiseLimitsMirrorValidator) {
  Project project = MakeValidProject();
  SetNoiseBlockEnabled(&project.sections[0], true);

  project.sections[0].noise.noise_db = editor_limits::kNoiseDbMin;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  project.sections[0].noise.noise_db = editor_limits::kNoiseDbMax;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  project.sections[0].noise.noise_db = editor_limits::kNoiseDbMin - 0.1;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
  project.sections[0].noise.noise_db = editor_limits::kNoiseDbMax + 0.1;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
}

TEST(SectionBlockPresenterTest, DropoutScaleLimitsMirrorValidator) {
  Project project = MakeValidProject();
  SetRandomDropoutsEnabled(&project.sections[0], true);

  project.sections[0].dropouts.random.scale = editor_limits::kDropoutScaleMin;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  project.sections[0].dropouts.random.scale = editor_limits::kDropoutScaleMax;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  project.sections[0].dropouts.random.scale =
      editor_limits::kDropoutScaleMax + 1;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
}

TEST(SectionBlockPresenterTest, OsdScaleLimitsMirrorValidator) {
  Project project = MakeValidProject();
  project.sections[0].osd.overlays.push_back(MakeDefaultOsdOverlay());

  project.sections[0].osd.overlays[0].scale = editor_limits::kOsdScaleMin;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  project.sections[0].osd.overlays[0].scale = editor_limits::kOsdScaleMax;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  project.sections[0].osd.overlays[0].scale = editor_limits::kOsdScaleMax + 1;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
}

TEST(SectionBlockPresenterTest, AudioLimitsMirrorValidator) {
  Project project = MakeValidProject();
  project.sections[0].audio_channel_pairs.push_back(
      MakeDefaultAudioChannelPair(0));
  AudioParameters& left = project.sections[0].audio_channel_pairs[0].left;

  left.frequency_hz = editor_limits::kAudioFrequencyMaxHz;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  left.frequency_hz = editor_limits::kAudioFrequencyMaxHz + 1.0;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));

  left.frequency_hz = 1000.0;
  left.amplitude = editor_limits::kAudioAmplitudeMax;
  EXPECT_TRUE(ProjectIsValid(project, nullptr));
  left.amplitude = editor_limits::kAudioAmplitudeMax + 0.1;
  EXPECT_FALSE(ProjectIsValid(project, nullptr));
}

TEST(SectionBlockPresenterTest, ApplySectionEditDeltaMirrorsOnlyChangedFields) {
  Section before;
  before.name = "Primary";
  before.source = "assets/bars.exr";
  before.duration_frames = 200;
  before.section_type = SectionType::kUnknown;

  // The edit: section type changed, nothing else.
  Section after = before;
  after.section_type = SectionType::kProgrammeArea;

  Section target;
  target.name = "Other";
  target.source = "assets/sweep.mkv";
  target.duration_frames = 50;
  SetNoiseBlockEnabled(&target, true);

  const Section result = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(result.section_type, SectionType::kProgrammeArea);
  // Untouched fields keep the target's own values.
  EXPECT_EQ(result.name, "Other");
  EXPECT_EQ(result.source, "assets/sweep.mkv");
  EXPECT_EQ(result.duration_frames, 50);
  EXPECT_TRUE(result.noise.enabled);
}

TEST(SectionBlockPresenterTest, ApplySectionEditDeltaMirrorsSourceRange) {
  Section before;
  before.source = "assets/capture.mp4";
  before.duration_frames_all = true;
  Section after = before;
  after.start_frame = 10;
  after.end_frame = 19;
  after.end_frame_specified = true;

  Section target;
  target.source = "assets/other.mkv";
  target.duration_frames_all = true;
  const Section result = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(result.start_frame, 10);
  EXPECT_EQ(result.end_frame, 19);
  EXPECT_TRUE(result.end_frame_specified);
  EXPECT_EQ(result.source, "assets/other.mkv");
}

TEST(SectionBlockPresenterTest, NewVideoSourceDefaultsToAllOfItsFrames) {
  Section section;
  section.source = "assets/new.mp4";
  section.duration_frames = 250;
  section.start_frame = 40;
  section.end_frame = 90;
  section.end_frame_specified = true;
  ApplySourceSelectionDefaults("assets/old.mkv", 250, &section);
  EXPECT_TRUE(section.duration_frames_all);
  EXPECT_EQ(section.start_frame, 0);
  EXPECT_FALSE(section.end_frame_specified);
}

TEST(SectionBlockPresenterTest, NewStillSourceClearsRangeAndAllDuration) {
  Section section;
  section.source = "assets/bars.exr";
  section.duration_frames_all = true;
  section.duration_frames_repeat = 3;
  section.start_frame = 40;
  section.end_frame = 90;
  section.end_frame_specified = true;
  ApplySourceSelectionDefaults("assets/old.mkv", 125, &section);
  EXPECT_FALSE(section.duration_frames_all);
  EXPECT_EQ(section.duration_frames, 125);
  EXPECT_EQ(section.duration_frames_repeat, 1);
  EXPECT_EQ(section.start_frame, 0);
  EXPECT_FALSE(section.end_frame_specified);
}

TEST(SectionBlockPresenterTest, NewStillSourceKeepsAFixedDuration) {
  Section section;
  section.source = "assets/bars.exr";
  section.duration_frames = 75;
  ApplySourceSelectionDefaults("assets/grey.exr", 125, &section);
  EXPECT_FALSE(section.duration_frames_all);
  EXPECT_EQ(section.duration_frames, 75);
}

TEST(SectionBlockPresenterTest, UnchangedSourceNeverRewritesTheDuration) {
  Section section;
  section.source = "assets/capture.mp4";
  section.duration_frames = 75;
  section.start_frame = 12;
  const Section original = section;
  ApplySourceSelectionDefaults("assets/capture.mp4", 125, &section);
  EXPECT_EQ(section, original);
}

// Emits and re-parses `project`, as saving and reopening it would.
Project SaveAndReload(const Project& project) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(EmitYaml(project));
  EXPECT_TRUE(parsed.ok) << (parsed.errors.empty() ? "" : parsed.errors[0]);
  return parsed.project;
}

TEST(SectionBlockPresenterTest, SwitchingPairContentKeepsOnlyThatKind) {
  AudioChannelPair channel_pair = MakeDefaultAudioChannelPair(2);
  channel_pair.description = "Narration";

  SetAudioPairContent(&channel_pair, AudioPairContent::kFile);
  EXPECT_EQ(channel_pair.content, AudioPairContent::kFile);
  EXPECT_FALSE(channel_pair.left.enabled);
  EXPECT_FALSE(channel_pair.right.enabled);
  EXPECT_TRUE(channel_pair.file.path.empty());
  EXPECT_EQ(channel_pair.pair, 2);
  EXPECT_EQ(channel_pair.description, "Narration");

  channel_pair.file = {"{project}/narration.flac", true};
  SetAudioPairContent(&channel_pair, AudioPairContent::kVideo);
  EXPECT_EQ(channel_pair.content, AudioPairContent::kVideo);
  EXPECT_EQ(channel_pair.file, AudioFileInput{});
  EXPECT_EQ(channel_pair.video_audio.stream, 0);

  channel_pair.video_audio.stream = 1;
  SetAudioPairContent(&channel_pair, AudioPairContent::kTones);
  EXPECT_EQ(channel_pair.video_audio, VideoAudioInput{});
  EXPECT_EQ(channel_pair.left, MakeDefaultAudioChannel());
  EXPECT_EQ(channel_pair.right, channel_pair.left);
}

TEST(SectionBlockPresenterTest, ReselectingTheSameContentChangesNothing) {
  AudioChannelPair channel_pair;
  channel_pair.content = AudioPairContent::kFile;
  channel_pair.file = {"music.wav", true};
  const AudioChannelPair original = channel_pair;
  SetAudioPairContent(&channel_pair, AudioPairContent::kFile);
  EXPECT_EQ(channel_pair, original);
}

TEST(SectionBlockPresenterTest, EveryPairContentRoundTripsThroughTheSection) {
  Project project = MakeValidProject();
  project.sections[0].source = "{project}/capture.mp4";
  project.sections[0].duration_frames_all = true;
  for (const AudioPairContent content :
       {AudioPairContent::kTones, AudioPairContent::kFile,
        AudioPairContent::kVideo}) {
    AudioChannelPair channel_pair = MakeDefaultAudioChannelPair(0);
    SetAudioPairContent(&channel_pair, content);
    if (content == AudioPairContent::kFile) {
      channel_pair.file = {"{project}/narration.flac", true};
    } else if (content == AudioPairContent::kVideo) {
      channel_pair.video_audio.stream = 1;
    }
    project.sections[0].audio_channel_pairs = {channel_pair};
    EXPECT_EQ(SaveAndReload(project).sections[0].audio_channel_pairs,
              project.sections[0].audio_channel_pairs)
        << "content " << static_cast<int>(content);
  }
}

TEST(SectionBlockPresenterTest, SoundtrackIsNotOfferedForStills) {
  const VideoSoundtrackEditorState state = VideoSoundtrackEditorStateFor(
      "assets/bars.exr", true, ProgressiveFrameSourceProfile{}, 0);
  EXPECT_FALSE(state.selectable);
  EXPECT_TRUE(state.stream_labels.empty());
}

TEST(SectionBlockPresenterTest, SoundtrackListsTheProbedStreams) {
  ProgressiveFrameSourceProfile profile;
  ProgressiveAudioStreamProfile stereo;
  stereo.codec = "aac";
  stereo.channels = 2;
  stereo.sample_rate_hz = 48000;
  ProgressiveAudioStreamProfile surround;
  surround.codec = "ac3";
  surround.channels = 6;
  surround.sample_rate_hz = 44100;
  profile.audio_streams = {stereo, surround};

  const VideoSoundtrackEditorState state =
      VideoSoundtrackEditorStateFor("capture.MP4", true, profile, 0);
  EXPECT_TRUE(state.selectable);
  EXPECT_EQ(state.stream_labels,
            (std::vector<std::string>{"Stream 0: aac, 2 ch, 48000 Hz",
                                      "Stream 1: ac3, 6 ch, 44100 Hz"}));
}

TEST(SectionBlockPresenterTest, SoundtrackIsDisabledForAVideoWithoutAudio) {
  const VideoSoundtrackEditorState state = VideoSoundtrackEditorStateFor(
      "capture.mkv", true, ProgressiveFrameSourceProfile{}, 0);
  EXPECT_FALSE(state.selectable);
}

TEST(SectionBlockPresenterTest, SoundtrackNeverDropsTheStoredStream) {
  // Before a probe the stored stream is offered by number.
  const VideoSoundtrackEditorState pending = VideoSoundtrackEditorStateFor(
      "capture.mkv", false, ProgressiveFrameSourceProfile{}, 2);
  EXPECT_TRUE(pending.selectable);
  EXPECT_EQ(pending.stream_labels,
            (std::vector<std::string>{"Stream 0", "Stream 1", "Stream 2"}));

  // A stored stream the probe did not find stays listed, marked as missing.
  ProgressiveFrameSourceProfile profile;
  profile.audio_streams.resize(1);
  profile.audio_streams[0].channels = 2;
  profile.audio_streams[0].sample_rate_hz = 48000;
  const VideoSoundtrackEditorState probed =
      VideoSoundtrackEditorStateFor("capture.mkv", true, profile, 1);
  ASSERT_EQ(probed.stream_labels.size(), 2U);
  EXPECT_EQ(probed.stream_labels[1], "Stream 1 (not in source)");
}

TEST(SectionBlockPresenterTest, RangeWidgetsAreDisabledForStills) {
  Section section;
  section.source = "assets/bars.exr";
  section.duration_frames = 50;
  EXPECT_FALSE(SourceRangeEditorStateFor(section, 1, 1000).enabled);
}

TEST(SectionBlockPresenterTest, RangeWidgetsAreCappedAtTheProbedLastFrame) {
  Section section;
  section.source = "assets/capture.mp4";
  section.duration_frames_all = true;
  const SourceRangeEditorState probed =
      SourceRangeEditorStateFor(section, 120, 1000);
  EXPECT_TRUE(probed.enabled);
  EXPECT_EQ(probed.maximum, 119);
  EXPECT_EQ(SourceRangeEditorStateFor(section, -1, 1000).maximum, 1000);
}

TEST(SectionBlockPresenterTest, RangeWidgetCapNeverClampsStoredValues) {
  Section section;
  section.source = "assets/capture.mp4";
  section.duration_frames_all = true;
  section.start_frame = 150;
  section.end_frame = 200;
  section.end_frame_specified = true;
  EXPECT_EQ(SourceRangeEditorStateFor(section, 120, 1000).maximum, 200);
}

TEST(SectionBlockPresenterTest, ApplySectionEditDeltaNeverMirrorsName) {
  Section before;
  before.name = "Primary";
  Section after = before;
  after.name = "Primary renamed";

  Section target;
  target.name = "Other";
  const Section result = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(result.name, "Other");
}

TEST(SectionBlockPresenterTest,
     ApplySectionEditDeltaDurationFieldsPropagateAsUnit) {
  Section before;
  before.duration_frames = 200;
  Section after = before;
  after.duration_frames = 0;
  after.duration_frames_all = true;
  after.duration_frames_repeat = 3;

  Section target;
  target.duration_frames = 50;
  const Section result = ApplySectionEditDelta(before, after, target);
  EXPECT_TRUE(result.duration_frames_all);
  EXPECT_EQ(result.duration_frames, 0);
  EXPECT_EQ(result.duration_frames_repeat, 3);
}

TEST(SectionBlockPresenterTest,
     ApplySectionEditDeltaDropoutBlocksDiffIndependently) {
  Section before;
  SetScratchDropoutsEnabled(&before, true);
  before.dropouts.scratch.scale = 5;

  // The edit: enable random dropouts; scratch untouched.
  Section after = before;
  SetRandomDropoutsEnabled(&after, true);
  after.dropouts.random.scale = 7;

  // The target has its own scratch configuration that must survive.
  Section target;
  SetScratchDropoutsEnabled(&target, true);
  target.dropouts.scratch.scale = 12;

  const Section result = ApplySectionEditDelta(before, after, target);
  EXPECT_TRUE(result.dropouts.random.enabled);
  EXPECT_EQ(result.dropouts.random.scale, 7);
  EXPECT_TRUE(result.dropouts.scratch.enabled);
  EXPECT_EQ(result.dropouts.scratch.scale, 12);
}

TEST(SectionBlockPresenterTest,
     ApplySectionEditDeltaOptionalBlocksMirrorOnChange) {
  Section before;
  Section after = before;
  SetNoiseBlockEnabled(&after, true);
  after.noise.noise_db = 30.0;
  after.osd.overlays.push_back(MakeDefaultOsdOverlay());
  after.audio_channel_pairs.push_back(MakeDefaultAudioChannelPair(0));

  Section target;
  const Section result = ApplySectionEditDelta(before, after, target);
  EXPECT_TRUE(result.noise.enabled);
  EXPECT_DOUBLE_EQ(result.noise.noise_db, 30.0);
  ASSERT_EQ(result.osd.overlays.size(), 1u);
  ASSERT_EQ(result.audio_channel_pairs.size(), 1u);
  EXPECT_EQ(result.audio_channel_pairs[0].pair, 0);
}

namespace {

// A single laserdisc injection carrying the listed code types (no values).
std::vector<Section::LineInjection> MakeInjection(
    const std::vector<std::string>& code_types) {
  Section::LineInjection injection;
  injection.type = "laserdisc";
  for (const std::string& code_type : code_types) {
    Section::LineInjectionCode code;
    code.code_type = code_type;
    injection.codes.push_back(std::move(code));
  }
  return {std::move(injection)};
}

std::vector<std::string> CodeTypesOf(
    const std::vector<Section::LineInjection>& injections) {
  std::vector<std::string> types;
  if (!injections.empty()) {
    for (const Section::LineInjectionCode& code : injections.front().codes) {
      types.push_back(code.code_type);
    }
  }
  return types;
}

}  // namespace

TEST(SectionBlockPresenterTest,
     LineInjectionDeltaSkipsCodesInvalidForTargetType) {
  // The reported bug: users_code ticked on a lead-out primary must not land
  // on a programme_area target, whose type forbids it.
  const auto before = MakeInjection({"lead_out"});
  const auto after = MakeInjection({"lead_out", "users_code"});
  const auto target = MakeInjection({"picture_number", "chapter_number"});

  const auto result = ApplyLineInjectionEditDelta(before, after, target,
                                                  SectionType::kProgrammeArea);
  EXPECT_EQ(CodeTypesOf(result),
            (std::vector<std::string>{"picture_number", "chapter_number"}));
}

TEST(SectionBlockPresenterTest,
     LineInjectionDeltaUpsertsValidCodesAndPreservesTargetCodes) {
  // The same edit onto a lead-in target adds users_code (valid there) while
  // keeping the target's own codes; the primary's lead_out marker — untouched
  // by the edit — is never mirrored.
  const auto before = MakeInjection({"lead_out"});
  const auto after = MakeInjection({"lead_out", "users_code"});
  const auto target = MakeInjection({"lead_in"});

  const auto result =
      ApplyLineInjectionEditDelta(before, after, target, SectionType::kLeadIn);
  EXPECT_EQ(CodeTypesOf(result),
            (std::vector<std::string>{"lead_in", "users_code"}));
}

TEST(SectionBlockPresenterTest, LineInjectionDeltaRemovesUntickedCodes) {
  const auto before = MakeInjection({"lead_out", "users_code"});
  const auto after = MakeInjection({"lead_out"});
  const auto target = MakeInjection({"lead_in", "users_code"});

  const auto result =
      ApplyLineInjectionEditDelta(before, after, target, SectionType::kLeadIn);
  EXPECT_EQ(CodeTypesOf(result), (std::vector<std::string>{"lead_in"}));
}

TEST(SectionBlockPresenterTest, LineInjectionDeltaMirrorsRevaluedCodes) {
  auto before = MakeInjection({"chapter_number"});
  auto after = MakeInjection({"chapter_number"});
  after.front().codes.front().chapter = 5;
  after.front().codes.front().chapter_specified = true;
  const auto target = MakeInjection({"picture_number", "chapter_number"});

  const auto result = ApplyLineInjectionEditDelta(before, after, target,
                                                  SectionType::kProgrammeArea);
  ASSERT_EQ(result.size(), 1u);
  ASSERT_EQ(result.front().codes.size(), 2u);
  EXPECT_EQ(result.front().codes[1].code_type, "chapter_number");
  EXPECT_TRUE(result.front().codes[1].chapter_specified);
  EXPECT_EQ(result.front().codes[1].chapter, 5);
}

TEST(SectionBlockPresenterTest, LineInjectionDeltaMirrorsBlockDisable) {
  const auto before = MakeInjection({"lead_out"});
  const std::vector<Section::LineInjection> after;
  const auto target = MakeInjection({"picture_number"});

  EXPECT_TRUE(ApplyLineInjectionEditDelta(before, after, target,
                                          SectionType::kProgrammeArea)
                  .empty());
}

TEST(SectionBlockPresenterTest, LineInjectionDeltaCreatesNoEmptyBlockOnTarget) {
  // Enabling the block on a lead-out primary seeds its lead_out marker; a
  // programme_area target with no injections gains nothing — not an empty
  // injection block.
  const std::vector<Section::LineInjection> before;
  const auto after = MakeInjection({"lead_out"});
  const std::vector<Section::LineInjection> target;

  EXPECT_TRUE(ApplyLineInjectionEditDelta(before, after, target,
                                          SectionType::kProgrammeArea)
                  .empty());
}

TEST(SectionBlockPresenterTest,
     ApplySectionEditDeltaFiltersInjectionsByMirroredSectionType) {
  // The end-to-end path of the reported bug: a lead-out primary gains
  // users_code while a programme_area section is also selected. The target
  // must keep its own codes and must not receive users_code.
  Section before;
  before.section_type = SectionType::kLeadOut;
  before.line_injections = MakeInjection({"lead_out"});
  Section after = before;
  after.line_injections = MakeInjection({"lead_out", "users_code"});

  Section target;
  target.name = "Programme";
  target.section_type = SectionType::kProgrammeArea;
  target.line_injections = MakeInjection({"picture_number"});

  const Section result = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(result.section_type, SectionType::kProgrammeArea);
  EXPECT_EQ(CodeTypesOf(result.line_injections),
            (std::vector<std::string>{"picture_number"}));
}

TEST(SectionBlockPresenterTest,
     SectionTypeBatchAssignmentRejectsSingleInstanceTypes) {
  // ValidateSectionOrdering permits at most one lead_in and one lead_out, so
  // neither may be mirrored onto a multi-section selection.
  EXPECT_FALSE(SectionTypeAllowsBatchAssignment(SectionType::kLeadIn));
  EXPECT_FALSE(SectionTypeAllowsBatchAssignment(SectionType::kLeadOut));
}

TEST(SectionBlockPresenterTest,
     SectionTypeBatchAssignmentAcceptsRepeatableTypes) {
  EXPECT_TRUE(SectionTypeAllowsBatchAssignment(SectionType::kProgrammeArea));
  EXPECT_TRUE(SectionTypeAllowsBatchAssignment(SectionType::kUnknown));
}

TEST(SectionBlockPresenterTest, OsdTokenCatalogueMatchesResolverTokens) {
  Project project = MakeValidProject();
  // Every documented token must be accepted by OSD validation when used in
  // an overlay.
  for (const OsdTokenHelp& help : OsdTokenCatalogue()) {
    project.sections[0].osd.overlays.clear();
    OsdOverlay overlay = MakeDefaultOsdOverlay();
    overlay.text = help.token;
    project.sections[0].osd.overlays.push_back(overlay);
    std::string error;
    EXPECT_TRUE(ProjectIsValid(project, &error)) << help.token << ": " << error;
  }
}

TEST(SectionBlockPresenterTest,
     DurationFramesToSecondsDividesByRateWhenRatePositive) {
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 3: PAL is 25 frames/s.
  EXPECT_DOUBLE_EQ(DurationFramesToSeconds(250, 25.0), 10.0);
  // SMPTE 170M-2004 Section 11.3: NTSC is 30000/1001 frames/s.
  EXPECT_NEAR(DurationFramesToSeconds(30, 30000.0 / 1001.0), 1.001, 1e-9);
}

TEST(SectionBlockPresenterTest,
     DurationFramesToSecondsReturnsZeroWhenRateNotPositive) {
  EXPECT_DOUBLE_EQ(DurationFramesToSeconds(250, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(DurationFramesToSeconds(250, -25.0), 0.0);
}

TEST(SectionBlockPresenterTest, DurationSecondsToFramesRoundsToNearestFrame) {
  EXPECT_EQ(DurationSecondsToFrames(10.0, 25.0, 1000000), 250);
  // 1 s of NTSC is 29.97 frames -> rounds to 30.
  EXPECT_EQ(DurationSecondsToFrames(1.0, 30000.0 / 1001.0, 1000000), 30);
  // 0.02 s of PAL is exactly half a frame -> rounds away from zero to 1.
  EXPECT_EQ(DurationSecondsToFrames(0.02, 25.0, 1000000), 1);
}

TEST(SectionBlockPresenterTest,
     DurationSecondsToFramesClampsToValidFrameRange) {
  // Below one frame (and negative input) clamps up to the 1-frame minimum.
  EXPECT_EQ(DurationSecondsToFrames(0.0, 25.0, 1000000), 1);
  EXPECT_EQ(DurationSecondsToFrames(-5.0, 25.0, 1000000), 1);
  // Above the editor maximum clamps down to it.
  EXPECT_EQ(DurationSecondsToFrames(1000.0, 25.0, 100), 100);
  // A non-positive rate cannot convert; the safe minimum is returned.
  EXPECT_EQ(DurationSecondsToFrames(10.0, 0.0, 100), 1);
}

}  // namespace
}  // namespace videosynth::gui
