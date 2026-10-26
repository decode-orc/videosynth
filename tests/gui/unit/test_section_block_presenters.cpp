/*
 * File:        test_section_block_presenters.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for the optional-block mapping layer (audio,
 *              noise, dropouts, OSD)
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
