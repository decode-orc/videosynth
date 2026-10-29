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
#include <string>
#include <vector>

#include "project_settings_presenter.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

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

TEST(ProjectSettingsPresenterTest, EnablementFollowsStandardSecam) {
  const ProjectSettingsFormState state =
      BuildProjectSettingsFormState(MakeProject(Standard::kSecam));
  EXPECT_FALSE(state.pilot_burst_editable);
  EXPECT_FALSE(state.vbi_burst_editable);
  EXPECT_FALSE(state.setup_ire_editable);
  EXPECT_TRUE(state.secam_field_identification_editable);
  EXPECT_FALSE(state.laserdisc_editable);
  EXPECT_FALSE(state.efm_output_editable);
  EXPECT_EQ(state.signal_state_options,
            std::vector<std::string>{"STANDARD_STABLE_UNLOCKED"});
}

TEST(ProjectSettingsPresenterTest, SecamOptionsOnlyApplyToSecam) {
  const std::vector<std::string> standards =
      BuildProjectSettingsFormState(MakeProject(Standard::kPal))
          .standard_options;
  EXPECT_NE(std::find(standards.begin(), standards.end(), "SECAM"),
            standards.end());
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    const ProjectSettingsFormState state =
        BuildProjectSettingsFormState(MakeProject(standard));
    EXPECT_FALSE(state.secam_field_identification_editable);
    EXPECT_TRUE(state.laserdisc_editable);
    EXPECT_EQ(state.signal_state_options,
              std::vector<std::string>{"STANDARD_STABLE_LOCKED"});
  }
}

TEST(ProjectSettingsPresenterTest, NormalizeAppliesSecamSignalStateAndFlags) {
  CvbsPresets presets;
  presets.video_standard_preset = Standard::kSecam;
  presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  presets.pal_laserdisc_pilot_burst = true;
  presets.secam_field_identification = false;

  const CvbsPresets secam = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_EQ(secam.signal_state_preset, "STANDARD_STABLE_UNLOCKED");
  EXPECT_FALSE(secam.pal_laserdisc_pilot_burst);
  EXPECT_FALSE(secam.secam_field_identification);

  presets.video_standard_preset = Standard::kPal;
  presets.signal_state_preset = "STANDARD_STABLE_UNLOCKED";
  const CvbsPresets pal = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_EQ(pal.signal_state_preset, "STANDARD_STABLE_LOCKED");
  EXPECT_TRUE(pal.secam_field_identification);
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
       {Standard::kPal, Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
    Project project = MakeProject(standard);
    CvbsPresets presets = project.cvbs_presets;
    presets.signal_state_preset = "NOT_A_STATE";
    presets.secam_field_identification = false;
    presets.pal_laserdisc_pilot_burst = true;
    presets.ntsc_black_setup_ire_specified = true;
    presets.ntsc_black_setup_ire = 7.5;
    project.cvbs_presets = NormalizeCvbsPresetsForStandard(presets);

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);
    for (const std::string& error : result.errors) {
      EXPECT_EQ(error.find("pal_laserdisc_pilot_burst"), std::string::npos)
          << error;
      EXPECT_EQ(error.find("ntsc_black_setup_ire"), std::string::npos) << error;
      EXPECT_EQ(error.find("signal_state_preset"), std::string::npos) << error;
    }
    for (const std::string& warning : result.warnings) {
      EXPECT_EQ(warning.find("secam_field_identification"), std::string::npos)
          << warning;
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

// LaserDisc digital audio exists for PAL (IEC 60856:1986 Amd 2 clause 13) and
// 525-line discs, PAL-M included (IEC 60857:1986 Amd 2 clause 13); SECAM must
// offer no enable control and report why.
TEST(ProjectSettingsPresenterTest, EfmAvailableForPalM) {
  Project project = MakeProjectWithAudioPair(Standard::kPalM, 0);
  EXPECT_TRUE(BuildProjectSettingsFormState(project).efm_output_editable);

  project.output.efm_audio.enabled = true;
  EXPECT_EQ(BuildProjectSettingsFormState(project).efm_status,
            EfmOutputStatus::kActive);
}

TEST(ProjectSettingsPresenterTest, EfmUnavailableForSecam) {
  Project project = MakeProjectWithAudioPair(Standard::kSecam, 0);
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

  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    const OutputTargets kept =
        NormalizeOutputTargetsForStandard(output, standard);
    EXPECT_TRUE(kept.efm_audio.enabled);
    EXPECT_EQ(kept.efm_audio.pair, 2);
  }

  const OutputTargets cleared =
      NormalizeOutputTargetsForStandard(output, Standard::kSecam);
  EXPECT_FALSE(cleared.efm_audio.enabled);
  EXPECT_EQ(cleared.efm_audio.pair, 0);
}

// Normalized output targets must never trip the validator's standard rule for
// EFM (the enablement mirrors validation).
TEST(ProjectSettingsPresenterTest, NormalizedOutputPassesValidatorEfmRule) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
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

bool IsValid(const Project& project) {
  return ProjectValidator().Validate(project).is_valid;
}

Project RoundTrip(const Project& project) {
  YamlProjectParser parser;
  const ParseResult parsed =
      parser.ParseString(YamlProjectEmitter().EmitString(project));
  EXPECT_TRUE(parsed.ok) << (parsed.errors.empty() ? "" : parsed.errors[0]);
  return parsed.project;
}

TEST(ProjectSettingsPresenterTest, FmBeatIsOfferedForPalOnly) {
  EXPECT_TRUE(BuildProjectSettingsFormState(MakeProject(Standard::kPal))
                  .fm_beat_editable);
  for (const Standard standard :
       {Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
    EXPECT_FALSE(
        BuildProjectSettingsFormState(MakeProject(standard)).fm_beat_editable)
        << StandardToString(standard);
  }
}

TEST(ProjectSettingsPresenterTest, ChromaErrorIsNotOfferedForSecam) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    EXPECT_TRUE(BuildProjectSettingsFormState(MakeProject(standard))
                    .chroma_error_editable)
        << StandardToString(standard);
  }
  EXPECT_FALSE(BuildProjectSettingsFormState(MakeProject(Standard::kSecam))
                   .chroma_error_editable);
}

TEST(ProjectSettingsPresenterTest, ChannelEnableHelpersSeedValidBlocks) {
  Project project = MakeProject(Standard::kPal);
  SetChannelEnabled(&project.channel, true);
  EXPECT_TRUE(project.channel.enabled);
  EXPECT_TRUE(IsValid(project));
  EXPECT_EQ(RoundTrip(project), project);

  SetEdgeSmearEnabled(&project.channel, true);
  SetFmBeatEnabled(&project.channel, true);
  project.channel.timing_offset_samples = -0.3;
  EXPECT_TRUE(project.channel.edge_smear.enabled);
  EXPECT_TRUE(project.channel.fm_beat.enabled);
  EXPECT_TRUE(IsValid(project));
  EXPECT_EQ(RoundTrip(project), project);

  SetFmBeatEnabled(&project.channel, false);
  EXPECT_EQ(project.channel.fm_beat, FmBeatParameters{});
  EXPECT_TRUE(project.channel.edge_smear.enabled);
  SetChannelEnabled(&project.channel, false);
  EXPECT_EQ(project.channel, ChannelParameters{});
  EXPECT_EQ(YamlProjectEmitter().EmitString(project).find("channel:"),
            std::string::npos);
}

TEST(ProjectSettingsPresenterTest, ChannelSubBlockEnablesTheChannel) {
  ChannelParameters channel;
  SetEdgeSmearEnabled(&channel, true);
  EXPECT_TRUE(channel.enabled);
  channel = ChannelParameters{};
  SetFmBeatEnabled(&channel, true);
  EXPECT_TRUE(channel.enabled);
  EXPECT_EQ(channel.fm_beat.carrier_black_hz, kFmBeatCarrierBlackDefaultHz);
}

TEST(ProjectSettingsPresenterTest, ReEnablingKeepsEditedValues) {
  ChannelParameters channel;
  SetEdgeSmearEnabled(&channel, true);
  channel.edge_smear.fraction = 0.3;
  SetEdgeSmearEnabled(&channel, true);
  EXPECT_EQ(channel.edge_smear.fraction, 0.3);

  ChromaErrorParameters chroma_error;
  SetChromaJitterEnabled(&chroma_error, true);
  chroma_error.line_jitter.gain = 0.05;
  SetChromaJitterEnabled(&chroma_error, true);
  EXPECT_EQ(chroma_error.line_jitter.gain, 0.05);
}

TEST(ProjectSettingsPresenterTest, ChromaErrorEnableHelpersSeedValidBlocks) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    SCOPED_TRACE(StandardToString(standard));
    Project project = MakeProject(standard);
    SetChromaErrorEnabled(&project.chroma_error, true);
    EXPECT_TRUE(IsValid(project));
    EXPECT_EQ(RoundTrip(project), project);

    SetChromaJitterEnabled(&project.chroma_error, true);
    project.chroma_error.line_jitter.seed_specified = true;
    project.chroma_error.line_jitter.seed = 12;
    project.chroma_error.gain = 1.05;
    project.chroma_error.phase_deg = 2.0;
    EXPECT_TRUE(IsValid(project));
    EXPECT_EQ(RoundTrip(project), project);

    SetChromaErrorEnabled(&project.chroma_error, false);
    EXPECT_EQ(project.chroma_error, ChromaErrorParameters{});
  }
}

TEST(ProjectSettingsPresenterTest, NormalizeClearsFmBeatOutsidePal) {
  ChannelParameters channel;
  SetEdgeSmearEnabled(&channel, true);
  SetFmBeatEnabled(&channel, true);
  EXPECT_EQ(NormalizeChannelForStandard(channel, Standard::kPal), channel);
  for (const Standard standard :
       {Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
    const ChannelParameters normalized =
        NormalizeChannelForStandard(channel, standard);
    EXPECT_FALSE(normalized.fm_beat.enabled);
    EXPECT_TRUE(normalized.edge_smear.enabled);
    EXPECT_TRUE(normalized.enabled);
    Project project = MakeProject(standard);
    project.cvbs_presets =
        NormalizeCvbsPresetsForStandard(project.cvbs_presets);
    project.channel = normalized;
    EXPECT_TRUE(IsValid(project)) << StandardToString(standard);
  }
}

TEST(ProjectSettingsPresenterTest, NormalizeClearsChromaErrorForSecam) {
  ChromaErrorParameters chroma_error;
  SetChromaErrorEnabled(&chroma_error, true);
  EXPECT_EQ(NormalizeChromaErrorForStandard(chroma_error, Standard::kNtsc),
            chroma_error);
  EXPECT_EQ(NormalizeChromaErrorForStandard(chroma_error, Standard::kSecam),
            ChromaErrorParameters{});
}

TEST(ProjectSettingsPresenterTest, ChannelLimitsMirrorValidator) {
  namespace limits = project_editor_limits;
  Project project = MakeProject(Standard::kPal);
  SetEdgeSmearEnabled(&project.channel, true);
  SetFmBeatEnabled(&project.channel, true);
  ChannelParameters& channel = project.channel;
  const auto check = [&](double* field, double low, double high,
                         bool low_inclusive) {
    const double saved = *field;
    *field = high;
    EXPECT_TRUE(IsValid(project)) << high;
    *field = high + 0.001;
    EXPECT_FALSE(IsValid(project)) << high;
    *field = low;
    EXPECT_EQ(IsValid(project), low_inclusive) << low;
    *field = low - 0.001;
    EXPECT_FALSE(IsValid(project)) << low;
    *field = saved;
  };
  check(&channel.timing_offset_samples, -limits::kTimingOffsetMaxSamples,
        limits::kTimingOffsetMaxSamples, true);
  check(&channel.edge_smear.fraction, 0.0, limits::kEdgeSmearFractionMax, true);
  // tau_samples has an open lower bound of 0; the editor's minimum is valid.
  check(&channel.edge_smear.tau_samples, 0.0, limits::kEdgeSmearTauMaxSamples,
        false);
  channel.edge_smear.tau_samples = limits::kEdgeSmearTauMinSamples;
  EXPECT_TRUE(IsValid(project));
  channel.edge_smear.tau_samples = 3.0;
  check(&channel.fm_beat.carrier_black_hz, limits::kFmBeatCarrierMinHz,
        limits::kFmBeatCarrierMaxHz, true);
  check(&channel.fm_beat.level_db, limits::kFmBeatLevelMinDb,
        limits::kFmBeatLevelMaxDb, true);
}

TEST(ProjectSettingsPresenterTest, ChromaErrorLimitsMirrorValidator) {
  namespace limits = project_editor_limits;
  Project project = MakeProject(Standard::kPal);
  SetChromaJitterEnabled(&project.chroma_error, true);
  ChromaErrorParameters& chroma_error = project.chroma_error;
  const auto check = [&](double* field, double low, double high) {
    const double saved = *field;
    for (const double value : {low, high}) {
      *field = value;
      EXPECT_TRUE(IsValid(project)) << value;
    }
    *field = high + 0.001;
    EXPECT_FALSE(IsValid(project)) << high;
    *field = low - 0.001;
    EXPECT_FALSE(IsValid(project)) << low;
    *field = saved;
  };
  check(&chroma_error.gain, limits::kChromaGainMin, limits::kChromaGainMax);
  check(&chroma_error.phase_deg, -limits::kChromaPhaseMaxDeg,
        limits::kChromaPhaseMaxDeg);
  check(&chroma_error.line_jitter.phase_deg, 0.0,
        limits::kChromaJitterPhaseMaxDeg);
  check(&chroma_error.line_jitter.gain, 0.0, limits::kChromaJitterGainMax);
}

}  // namespace
}  // namespace videosynth::gui
