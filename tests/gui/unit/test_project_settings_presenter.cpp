/*
 * File:        test_project_settings_presenter.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for the project settings mapping layer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "project_settings_presenter.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"

namespace videosynth::gui {
namespace {

Project MakeProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.output.video_path = "out/video.cvbs";
  project.output.metadata_path = "out/video.meta";
  Section section;
  section.name = "Bars";
  section.type = "progressive";
  section.source = "assets/bars.exr";
  section.duration_frames = 10;
  project.sections.push_back(section);
  return project;
}

TEST(ProjectSettingsPresenterTest, EnablementFollowsStandardPal) {
  const ProjectSettingsFormState state =
      BuildProjectSettingsFormState(MakeProject(Standard::kPal));
  EXPECT_TRUE(state.pilot_burst_editable);
  EXPECT_FALSE(state.vbi_burst_editable);
  EXPECT_FALSE(state.setup_ire_editable);
}

TEST(ProjectSettingsPresenterTest, EnablementFollowsStandardNtsc) {
  const ProjectSettingsFormState state =
      BuildProjectSettingsFormState(MakeProject(Standard::kNtsc));
  EXPECT_FALSE(state.pilot_burst_editable);
  EXPECT_TRUE(state.vbi_burst_editable);
  EXPECT_TRUE(state.setup_ire_editable);
}

TEST(ProjectSettingsPresenterTest, EnablementFollowsStandardPalM) {
  const ProjectSettingsFormState state =
      BuildProjectSettingsFormState(MakeProject(Standard::kPalM));
  EXPECT_FALSE(state.pilot_burst_editable);
  EXPECT_FALSE(state.vbi_burst_editable);
  EXPECT_TRUE(state.setup_ire_editable);
}

TEST(ProjectSettingsPresenterTest, SecamOfferedForPalOnly) {
  EXPECT_TRUE(BuildProjectSettingsFormState(MakeProject(Standard::kPal))
                  .secam_editable);
  EXPECT_FALSE(BuildProjectSettingsFormState(MakeProject(Standard::kNtsc))
                   .secam_editable);
  EXPECT_FALSE(BuildProjectSettingsFormState(MakeProject(Standard::kPalM))
                   .secam_editable);
}

TEST(ProjectSettingsPresenterTest, SecamAndPilotBurstAreMutuallyExclusive) {
  Project project = MakeProject(Standard::kPal);
  project.cvbs_presets.chroma_encoding = ChromaEncoding::kSecam;
  ProjectSettingsFormState state = BuildProjectSettingsFormState(project);
  EXPECT_FALSE(state.pilot_burst_editable);
  EXPECT_TRUE(state.secam_editable);
  EXPECT_TRUE(state.secam_field_identification_editable);

  project.cvbs_presets.chroma_encoding = ChromaEncoding::kStandard;
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  state = BuildProjectSettingsFormState(project);
  EXPECT_TRUE(state.pilot_burst_editable);
  EXPECT_FALSE(state.secam_editable);
  EXPECT_FALSE(state.secam_field_identification_editable);
}

TEST(ProjectSettingsPresenterTest, NormalizeClearsSecamOutsidePal) {
  CvbsPresets presets;
  presets.video_standard_preset = Standard::kNtsc;
  presets.chroma_encoding = ChromaEncoding::kSecam;
  presets.secam_field_identification = false;

  const CvbsPresets ntsc = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_EQ(ntsc.chroma_encoding, ChromaEncoding::kStandard);
  EXPECT_TRUE(ntsc.secam_field_identification);

  presets.video_standard_preset = Standard::kPal;
  presets.pal_laserdisc_pilot_burst = true;
  const CvbsPresets pal = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_EQ(pal.chroma_encoding, ChromaEncoding::kSecam);
  EXPECT_FALSE(pal.secam_field_identification);
  EXPECT_FALSE(pal.pal_laserdisc_pilot_burst);
}

TEST(ProjectSettingsPresenterTest, YSuffixFlagFollowsSignalType) {
  Project project = MakeProject(Standard::kPal);
  EXPECT_FALSE(
      BuildProjectSettingsFormState(project).video_path_requires_luma_suffix);
  project.output.signal_type = "yc";
  EXPECT_TRUE(
      BuildProjectSettingsFormState(project).video_path_requires_luma_suffix);
}

// Every offered sample-encoding option must be accepted by the model's
// supported-preset predicate the validator uses.
TEST(ProjectSettingsPresenterTest, SampleEncodingOptionsMirrorSupportedSet) {
  const ProjectSettingsFormState state =
      BuildProjectSettingsFormState(MakeProject(Standard::kPal));
  ASSERT_FALSE(state.sample_encoding_options.empty());
  for (const std::string& preset : state.sample_encoding_options) {
    EXPECT_TRUE(IsSupportedSampleEncodingPreset(preset)) << preset;
  }
}

TEST(ProjectSettingsPresenterTest, NormalizeClearsFlagsForeignToStandard) {
  CvbsPresets presets;
  presets.video_standard_preset = Standard::kNtsc;
  presets.pal_laserdisc_pilot_burst = true;  // PAL-only flag.
  presets.ntsc_laserdisc_vbi_burst = true;
  presets.ntsc_black_setup_ire = 0.0;
  presets.ntsc_black_setup_ire_specified = true;

  const CvbsPresets ntsc = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_FALSE(ntsc.pal_laserdisc_pilot_burst);
  EXPECT_TRUE(ntsc.ntsc_laserdisc_vbi_burst);
  EXPECT_TRUE(ntsc.ntsc_black_setup_ire_specified);

  presets.video_standard_preset = Standard::kPal;
  const CvbsPresets pal = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_TRUE(pal.pal_laserdisc_pilot_burst);
  EXPECT_FALSE(pal.ntsc_laserdisc_vbi_burst);
  EXPECT_FALSE(pal.ntsc_black_setup_ire_specified);
  EXPECT_EQ(pal.ntsc_black_setup_ire, 7.5);
}

// Normalized presets must never trip the validator's standard-dependent
// preset rules (the enablement mirrors validation).
TEST(ProjectSettingsPresenterTest, NormalizedPresetsPassValidatorFlagRules) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeProject(standard);
    CvbsPresets presets = project.cvbs_presets;
    presets.pal_laserdisc_pilot_burst = true;
    presets.ntsc_black_setup_ire_specified = true;
    presets.ntsc_black_setup_ire = 7.5;
    presets.chroma_encoding = ChromaEncoding::kSecam;
    project.cvbs_presets = NormalizeCvbsPresetsForStandard(presets);

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);
    for (const std::string& error : result.errors) {
      EXPECT_EQ(error.find("pal_laserdisc_pilot_burst"), std::string::npos)
          << error;
      EXPECT_EQ(error.find("ntsc_black_setup_ire"), std::string::npos) << error;
      EXPECT_EQ(error.find("chroma_encoding"), std::string::npos) << error;
    }
  }
}

TEST(ProjectSettingsPresenterTest, DeriveMetadataPathStripsOutputSuffixes) {
  EXPECT_EQ(DeriveMetadataPath("out/video.cvbs"), "out/video.meta");
  EXPECT_EQ(DeriveMetadataPath("out/video.cvbsy"), "out/video.meta");
  EXPECT_EQ(DeriveMetadataPath("out/video"), "out/video.meta");
  EXPECT_EQ(DeriveMetadataPath("out/video.raw"), "out/video.meta");
  // A dot in a directory component is not an extension.
  EXPECT_EQ(DeriveMetadataPath("out.v2/video"), "out.v2/video.meta");
  EXPECT_EQ(DeriveMetadataPath(""), "");
}

TEST(ProjectSettingsPresenterTest, EnforceSignalTypeRewritesVideoPath) {
  EXPECT_EQ(EnforceSignalTypeVideoPath("out/video.cvbs", "yc"),
            "out/video.cvbsy");
  EXPECT_EQ(EnforceSignalTypeVideoPath("out/video.cvbsy", "yc"),
            "out/video.cvbsy");
  EXPECT_EQ(EnforceSignalTypeVideoPath("out/video", "yc"), "out/video.cvbsy");
  EXPECT_EQ(EnforceSignalTypeVideoPath("out/video.cvbsy", "composite"),
            "out/video.cvbs");
  EXPECT_EQ(EnforceSignalTypeVideoPath("out/video.cvbs", "composite"),
            "out/video.cvbs");
  EXPECT_EQ(EnforceSignalTypeVideoPath("", "yc"), "");
}

// The enforced yc path must satisfy the validator's ".cvbsy" suffix rule.
TEST(ProjectSettingsPresenterTest, EnforcedYcPathPassesValidator) {
  Project project = MakeProject(Standard::kPal);
  project.output.signal_type = "yc";
  project.output.video_path =
      EnforceSignalTypeVideoPath("out/video.cvbs", "yc");
  project.output.metadata_path = DeriveMetadataPath(project.output.video_path);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_TRUE(result.is_valid);
}

// A project whose single section declares audio channel pair `pair`, so the
// EFM selection has something to encode.
Project MakeProjectWithAudioPair(Standard standard, int pair) {
  Project project = MakeProject(standard);
  AudioChannelPair channel_pair;
  channel_pair.pair = pair;
  channel_pair.pair_specified = true;
  channel_pair.left.enabled = true;
  channel_pair.right.enabled = true;
  project.sections[0].audio_channel_pairs.push_back(channel_pair);
  return project;
}

TEST(ProjectSettingsPresenterTest, EfmControlsIdleWhenSelectionDisabled) {
  const ProjectSettingsFormState state = BuildProjectSettingsFormState(
      MakeProjectWithAudioPair(Standard::kPal, 0));
  EXPECT_TRUE(state.efm_output_editable);
  EXPECT_FALSE(state.efm_pair_editable);
  EXPECT_EQ(state.efm_status, EfmOutputStatus::kDisabled);
}

TEST(ProjectSettingsPresenterTest, EfmPairOptionsCoverEveryChannelPair) {
  const ProjectSettingsFormState state =
      BuildProjectSettingsFormState(MakeProject(Standard::kPal));
  ASSERT_EQ(static_cast<int>(state.efm_pair_options.size()),
            kMaxAudioChannelPairs);
  for (int pair = 0; pair < kMaxAudioChannelPairs; ++pair) {
    EXPECT_EQ(state.efm_pair_options[static_cast<std::size_t>(pair)], pair);
  }
}

TEST(ProjectSettingsPresenterTest, EfmStatusActiveWhenPairDeclared) {
  Project project = MakeProjectWithAudioPair(Standard::kNtsc, 3);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 3;

  const ProjectSettingsFormState state = BuildProjectSettingsFormState(project);
  EXPECT_TRUE(state.efm_pair_editable);
  EXPECT_EQ(state.efm_status, EfmOutputStatus::kActive);
  // The form's verdict must match the predicate the pipeline uses.
  EXPECT_EQ(ProjectEfmAudioPair(project), 3);
}

TEST(ProjectSettingsPresenterTest, EfmStatusReportsUndeclaredPair) {
  Project project = MakeProjectWithAudioPair(Standard::kPal, 0);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 4;

  EXPECT_EQ(BuildProjectSettingsFormState(project).efm_status,
            EfmOutputStatus::kPairNotDeclared);
  EXPECT_EQ(ProjectEfmAudioPair(project), -1);
}

TEST(ProjectSettingsPresenterTest, EfmStatusReportsPairOutOfRange) {
  Project project = MakeProjectWithAudioPair(Standard::kPal, 0);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = kMaxAudioChannelPairs;

  EXPECT_EQ(BuildProjectSettingsFormState(project).efm_status,
            EfmOutputStatus::kPairOutOfRange);
}

// LaserDisc digital audio exists only for PAL (IEC 60856:1986 Amd 2 clause 13)
// and NTSC (IEC 60857:1986 Amd 2 clause 13); every other standard must offer no
// enable control and report why.
TEST(ProjectSettingsPresenterTest, EfmUnavailableOutsidePalAndNtsc) {
  Project project = MakeProjectWithAudioPair(Standard::kPalM, 0);
  EXPECT_FALSE(BuildProjectSettingsFormState(project).efm_output_editable);

  project.output.efm_audio.enabled = true;
  const ProjectSettingsFormState state = BuildProjectSettingsFormState(project);
  EXPECT_FALSE(state.efm_pair_editable);
  EXPECT_EQ(state.efm_status, EfmOutputStatus::kUnsupportedStandard);
}

TEST(ProjectSettingsPresenterTest, NormalizeOutputClearsEfmForeignToStandard) {
  OutputTargets output;
  output.efm_audio.enabled = true;
  output.efm_audio.pair = 2;

  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    const OutputTargets kept =
        NormalizeOutputTargetsForStandard(output, standard);
    EXPECT_TRUE(kept.efm_audio.enabled);
    EXPECT_EQ(kept.efm_audio.pair, 2);
  }

  const OutputTargets cleared =
      NormalizeOutputTargetsForStandard(output, Standard::kPalM);
  EXPECT_FALSE(cleared.efm_audio.enabled);
  EXPECT_EQ(cleared.efm_audio.pair, 0);
}

// Normalized output targets must never trip the validator's standard rule for
// EFM (the enablement mirrors validation).
TEST(ProjectSettingsPresenterTest, NormalizedOutputPassesValidatorEfmRule) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeProjectWithAudioPair(standard, 0);
    project.output.efm_audio.enabled = true;
    project.output =
        NormalizeOutputTargetsForStandard(project.output, standard);

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);
    for (const std::string& error : result.errors) {
      EXPECT_EQ(error.find("output.efm_audio"), std::string::npos) << error;
    }
  }
}

}  // namespace
}  // namespace videosynth::gui
