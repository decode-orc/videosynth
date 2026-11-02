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
#include "project_templates.h"
#include "section_block_presenters.h"
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
  presets.secam_field_identification = false;

  const CvbsPresets secam = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_EQ(secam.signal_state_preset, "STANDARD_STABLE_UNLOCKED");
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
  presets.ntsc_black_setup_ire = 0.0;
  presets.ntsc_black_setup_ire_specified = true;

  const CvbsPresets ntsc = NormalizeCvbsPresetsForStandard(presets);
  EXPECT_TRUE(ntsc.ntsc_black_setup_ire_specified);

  presets.video_standard_preset = Standard::kPal;
  const CvbsPresets pal = NormalizeCvbsPresetsForStandard(presets);
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
    presets.ntsc_black_setup_ire_specified = true;
    presets.ntsc_black_setup_ire = 7.5;
    project.cvbs_presets = NormalizeCvbsPresetsForStandard(presets);

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);
    for (const std::string& error : result.errors) {
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
  project.laserdisc.enabled = true;
  project.laserdisc.efm_audio.enabled = true;
  project.laserdisc.efm_audio.pair = 3;

  const ProjectSettingsFormState state = BuildProjectSettingsFormState(project);
  EXPECT_TRUE(state.efm_pair_editable);
  EXPECT_EQ(state.efm_status, EfmOutputStatus::kActive);
  // The form's verdict must match the predicate the pipeline uses.
  EXPECT_EQ(ProjectEfmAudioPair(project), 3);
}

TEST(ProjectSettingsPresenterTest, EfmStatusReportsUndeclaredPair) {
  Project project = MakeProjectWithAudioPair(Standard::kPal, 0);
  project.laserdisc.enabled = true;
  project.laserdisc.efm_audio.enabled = true;
  project.laserdisc.efm_audio.pair = 4;

  EXPECT_EQ(BuildProjectSettingsFormState(project).efm_status,
            EfmOutputStatus::kPairNotDeclared);
  EXPECT_EQ(ProjectEfmAudioPair(project), -1);
}

TEST(ProjectSettingsPresenterTest, EfmStatusReportsPairOutOfRange) {
  Project project = MakeProjectWithAudioPair(Standard::kPal, 0);
  project.laserdisc.enabled = true;
  project.laserdisc.efm_audio.enabled = true;
  project.laserdisc.efm_audio.pair = kMaxAudioChannelPairs;

  EXPECT_EQ(BuildProjectSettingsFormState(project).efm_status,
            EfmOutputStatus::kPairOutOfRange);
}

// LaserDisc digital audio exists for PAL (IEC 60856:1986 Amd 2 clause 13) and
// 525-line discs, PAL-M included (IEC 60857:1986 Amd 2 clause 13); SECAM must
// offer no enable control and report why.
TEST(ProjectSettingsPresenterTest, EfmAvailableForPalM) {
  Project project = MakeProjectWithAudioPair(Standard::kPalM, 0);
  EXPECT_TRUE(BuildProjectSettingsFormState(project).efm_output_editable);

  project.laserdisc.enabled = true;
  project.laserdisc.efm_audio.enabled = true;
  EXPECT_EQ(BuildProjectSettingsFormState(project).efm_status,
            EfmOutputStatus::kActive);
}

TEST(ProjectSettingsPresenterTest, EfmUnavailableForSecam) {
  Project project = MakeProjectWithAudioPair(Standard::kSecam, 0);
  EXPECT_FALSE(BuildProjectSettingsFormState(project).efm_output_editable);

  project.laserdisc.enabled = true;
  project.laserdisc.efm_audio.enabled = true;
  const ProjectSettingsFormState state = BuildProjectSettingsFormState(project);
  EXPECT_FALSE(state.efm_pair_editable);
  EXPECT_EQ(state.efm_status, EfmOutputStatus::kUnsupportedStandard);
}

TEST(ProjectSettingsPresenterTest, NormalizeLaserdiscClearsEfmForeignToStd) {
  LaserdiscParameters laserdisc;
  laserdisc.enabled = true;
  laserdisc.efm_audio.enabled = true;
  laserdisc.efm_audio.pair = 2;

  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    const LaserdiscParameters kept =
        NormalizeLaserdiscForStandard(laserdisc, standard);
    EXPECT_TRUE(kept.efm_audio.enabled);
    EXPECT_EQ(kept.efm_audio.pair, 2);
  }

  const LaserdiscParameters cleared =
      NormalizeLaserdiscForStandard(laserdisc, Standard::kSecam);
  EXPECT_FALSE(cleared.efm_audio.enabled);
  EXPECT_EQ(cleared.efm_audio.pair, 0);
}

TEST(ProjectSettingsPresenterTest, TruthIsOfferedForQuadratureStandardsOnly) {
  OutputTargets output;
  output.truth = true;
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    EXPECT_TRUE(NormalizeOutputTargetsForStandard(output, standard).truth);
    Project project;
    project.cvbs_presets.video_standard_preset = standard;
    EXPECT_TRUE(BuildProjectSettingsFormState(project).truth_editable);
  }
  EXPECT_FALSE(
      NormalizeOutputTargetsForStandard(output, Standard::kSecam).truth);
  Project secam;
  secam.cvbs_presets.video_standard_preset = Standard::kSecam;
  EXPECT_FALSE(BuildProjectSettingsFormState(secam).truth_editable);
}

// Normalized output targets must never trip the validator's standard rule for
// EFM (the enablement mirrors validation).
TEST(ProjectSettingsPresenterTest, NormalizedOutputPassesValidatorEfmRule) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
    Project project = MakeProjectWithAudioPair(standard, 0);
    project.laserdisc.enabled = true;
    project.laserdisc.efm_audio.enabled = true;
    project.laserdisc =
        NormalizeLaserdiscForStandard(project.laserdisc, standard);

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);
    for (const std::string& error : result.errors) {
      EXPECT_EQ(error.find("laserdisc"), std::string::npos) << error;
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
  project.channel.timing_offset_samples = -0.3;
  EXPECT_TRUE(project.channel.edge_smear.enabled);
  EXPECT_TRUE(IsValid(project));
  EXPECT_EQ(RoundTrip(project), project);

  SetEdgeSmearEnabled(&project.channel, false);
  EXPECT_EQ(project.channel.edge_smear, EdgeSmearParameters{});
  SetChannelEnabled(&project.channel, false);
  EXPECT_EQ(project.channel, ChannelParameters{});
  EXPECT_EQ(YamlProjectEmitter().EmitString(project).find("channel:"),
            std::string::npos);
}

TEST(ProjectSettingsPresenterTest, ChannelSubBlockEnablesTheChannel) {
  ChannelParameters channel;
  SetEdgeSmearEnabled(&channel, true);
  EXPECT_TRUE(channel.enabled);
}

TEST(ProjectSettingsPresenterTest, LaserdiscSubBlockEnablesItsParents) {
  LaserdiscParameters laserdisc;
  SetFmBeatEnabled(&laserdisc, true);
  EXPECT_TRUE(laserdisc.enabled);
  EXPECT_TRUE(laserdisc.channel.enabled);
  EXPECT_EQ(laserdisc.channel.fm_beat.carrier_black_hz,
            kFmBeatCarrierBlackDefaultHz);

  for (const auto enable :
       {+[](LaserdiscParameters* l) {
          SetGroupDelayEnabled(l, true, Standard::kPal);
        },
        +[](LaserdiscParameters* l) { SetField2OffsetEnabled(l, true); },
        +[](LaserdiscParameters* l) {
          SetReplayResponseMode(l, ReplayResponseMode::kFixed);
        }}) {
    LaserdiscParameters fresh;
    enable(&fresh);
    EXPECT_TRUE(fresh.enabled);
    EXPECT_TRUE(fresh.channel.enabled);
  }
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

TEST(ProjectSettingsPresenterTest, NormalizeLaserdiscClearsForeignFeatures) {
  LaserdiscParameters laserdisc = MakeLaserdiscParameters();
  laserdisc.pilot_burst = true;
  laserdisc.vbi_burst = true;
  SetFmBeatEnabled(&laserdisc, true);
  SetGroupDelayEnabled(&laserdisc, true, Standard::kPal);
  SetField2OffsetEnabled(&laserdisc, true);

  const LaserdiscParameters pal =
      NormalizeLaserdiscForStandard(laserdisc, Standard::kPal);
  EXPECT_TRUE(pal.pilot_burst);
  EXPECT_FALSE(pal.vbi_burst);
  EXPECT_TRUE(pal.channel.fm_beat.enabled);
  EXPECT_EQ(pal.channel.group_delay.preset, GroupDelayPreset::kIec60856Nominal);

  for (const Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    SCOPED_TRACE(StandardToString(standard));
    const LaserdiscParameters normalized =
        NormalizeLaserdiscForStandard(laserdisc, standard);
    EXPECT_TRUE(normalized.enabled);
    EXPECT_FALSE(normalized.pilot_burst);
    EXPECT_EQ(normalized.vbi_burst, standard == Standard::kNtsc);
    EXPECT_FALSE(normalized.channel.fm_beat.enabled);
    // The nominal preset is PAL-only; its curve carries over as a custom one.
    EXPECT_TRUE(normalized.channel.group_delay.enabled);
    EXPECT_EQ(normalized.channel.group_delay.preset, GroupDelayPreset::kCustom);
    EXPECT_EQ(normalized.channel.group_delay.points_ns,
              Iec60856NominalGroupDelay());
    EXPECT_TRUE(normalized.channel.field2_offset.enabled);
    Project project = MakeProject(standard);
    project.laserdisc = normalized;
    project.line_injections = MakeLaserdiscLineInjections(standard);
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_TRUE(result.is_valid)
        << (result.errors.empty() ? "" : result.errors.front());
  }

  EXPECT_EQ(NormalizeLaserdiscForStandard(laserdisc, Standard::kSecam),
            LaserdiscParameters{});
}

TEST(ProjectSettingsPresenterTest, NormalizeClearsChromaErrorForSecam) {
  ChromaErrorParameters chroma_error;
  SetChromaErrorEnabled(&chroma_error, true);
  SetCrosstalkEnabled(&chroma_error, true);
  EXPECT_EQ(NormalizeChromaErrorForStandard(chroma_error, Standard::kNtsc),
            chroma_error);
  EXPECT_EQ(NormalizeChromaErrorForStandard(chroma_error, Standard::kSecam),
            ChromaErrorParameters{});
}

TEST(ProjectSettingsPresenterTest, ChannelLimitsMirrorValidator) {
  namespace limits = project_editor_limits;
  Project project = MakeProject(Standard::kPal);
  SetEdgeSmearEnabled(&project.channel, true);
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

TEST(ProjectSettingsPresenterTest, LaserdiscIsNotOfferedForSecam) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    EXPECT_TRUE(
        BuildProjectSettingsFormState(MakeProject(standard)).laserdisc_editable)
        << StandardToString(standard);
  }
  EXPECT_FALSE(BuildProjectSettingsFormState(MakeProject(Standard::kSecam))
                   .laserdisc_editable);
}

TEST(ProjectSettingsPresenterTest, NominalGroupDelayIsOfferedForPalOnly) {
  EXPECT_TRUE(BuildProjectSettingsFormState(MakeProject(Standard::kPal))
                  .group_delay_nominal_editable);
  for (const Standard standard :
       {Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
    EXPECT_FALSE(BuildProjectSettingsFormState(MakeProject(standard))
                     .group_delay_nominal_editable)
        << StandardToString(standard);
  }
}

TEST(ProjectSettingsPresenterTest, CrosstalkAlternationFollowsPalSwitch) {
  for (const Standard standard : {Standard::kPal, Standard::kPalM}) {
    EXPECT_TRUE(BuildProjectSettingsFormState(MakeProject(standard))
                    .crosstalk_alternate_editable)
        << StandardToString(standard);
  }
  EXPECT_FALSE(BuildProjectSettingsFormState(MakeProject(Standard::kNtsc))
                   .crosstalk_alternate_editable);

  ChromaErrorParameters chroma_error;
  SetCrosstalkEnabled(&chroma_error, true);
  chroma_error.crosstalk.alternate_with_switch = true;
  EXPECT_TRUE(NormalizeChromaErrorForStandard(chroma_error, Standard::kPalM)
                  .crosstalk.alternate_with_switch);
  const ChromaErrorParameters ntsc =
      NormalizeChromaErrorForStandard(chroma_error, Standard::kNtsc);
  EXPECT_FALSE(ntsc.crosstalk.alternate_with_switch);
  EXPECT_TRUE(ntsc.crosstalk.enabled);
  Project project = MakeProject(Standard::kNtsc);
  project.chroma_error = ntsc;
  EXPECT_TRUE(IsValid(project));
}

TEST(ProjectSettingsPresenterTest, LaserdiscEnableHelpersSeedValidBlocks) {
  Project project = MakeProject(Standard::kPal);
  SetLaserdiscEnabled(&project.laserdisc, true);
  EXPECT_TRUE(project.laserdisc.enabled);
  EXPECT_TRUE(IsValid(project));
  EXPECT_EQ(RoundTrip(project), project);

  SetLaserdiscChannelEnabled(&project.laserdisc, true);
  SetFmBeatEnabled(&project.laserdisc, true);
  SetGroupDelayEnabled(&project.laserdisc, true, Standard::kPal);
  SetField2OffsetEnabled(&project.laserdisc, true);
  SetReplayResponseMode(&project.laserdisc, ReplayResponseMode::kFixed);
  EXPECT_EQ(project.laserdisc.channel.group_delay.preset,
            GroupDelayPreset::kIec60856Nominal);
  EXPECT_TRUE(IsValid(project));
  EXPECT_EQ(RoundTrip(project), project);

  SetReplayResponseMode(&project.laserdisc, ReplayResponseMode::kByPicture);
  EXPECT_TRUE(IsValid(project));
  EXPECT_EQ(RoundTrip(project), project);

  SetGroupDelayPreset(&project.laserdisc, GroupDelayPreset::kCustom);
  EXPECT_EQ(project.laserdisc.channel.group_delay.points_ns,
            Iec60856NominalGroupDelay());
  EXPECT_TRUE(IsValid(project));
  EXPECT_EQ(RoundTrip(project), project);

  SetLaserdiscChannelEnabled(&project.laserdisc, false);
  EXPECT_EQ(project.laserdisc.channel, LaserdiscChannelParameters{});
  EXPECT_TRUE(project.laserdisc.enabled);
  SetLaserdiscEnabled(&project.laserdisc, false);
  EXPECT_EQ(project.laserdisc, LaserdiscParameters{});
  EXPECT_EQ(YamlProjectEmitter().EmitString(project).find("laserdisc:"),
            std::string::npos);
}

TEST(ProjectSettingsPresenterTest, GroupDelayOutsidePalStartsAsCustomCurve) {
  for (const Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    SCOPED_TRACE(StandardToString(standard));
    Project project = MakeProject(standard);
    SetGroupDelayEnabled(&project.laserdisc, true, standard);
    const GroupDelayParameters& group_delay =
        project.laserdisc.channel.group_delay;
    EXPECT_EQ(group_delay.preset, GroupDelayPreset::kCustom);
    EXPECT_EQ(group_delay.points_ns, Iec60856NominalGroupDelay());
    EXPECT_EQ(group_delay.scale, 1.0);
    EXPECT_TRUE(IsValid(project));
    EXPECT_EQ(RoundTrip(project), project);
  }
}

TEST(ProjectSettingsPresenterTest, GroupDelayPresetSwitchKeepsOneCurve) {
  LaserdiscParameters laserdisc;
  SetGroupDelayEnabled(&laserdisc, true, Standard::kNtsc);
  laserdisc.channel.group_delay.points_ns = {{0.5, 0.0}, {3.0, -40.0}};
  // Back to the preset drops the custom curve (it would not emit)...
  SetGroupDelayPreset(&laserdisc, GroupDelayPreset::kIec60856Nominal);
  EXPECT_TRUE(laserdisc.channel.group_delay.points_ns.empty());
  // ...and a fresh custom curve starts from the nominal one.
  SetGroupDelayPreset(&laserdisc, GroupDelayPreset::kCustom);
  EXPECT_EQ(laserdisc.channel.group_delay.points_ns,
            Iec60856NominalGroupDelay());
}

TEST(ProjectSettingsPresenterTest, ReplayResponseModesNeverCoexist) {
  LaserdiscParameters laserdisc;
  EXPECT_EQ(ReplayResponseModeOf(laserdisc.channel), ReplayResponseMode::kNone);

  SetReplayResponseMode(&laserdisc, ReplayResponseMode::kFixed);
  EXPECT_EQ(ReplayResponseModeOf(laserdisc.channel),
            ReplayResponseMode::kFixed);
  ASSERT_FALSE(laserdisc.channel.response_db.empty());
  laserdisc.channel.response_db = {{1.0, -2.0}, {4.0, -6.0}};

  // By picture carries the fixed response to both seeded positions.
  SetReplayResponseMode(&laserdisc, ReplayResponseMode::kByPicture);
  EXPECT_EQ(ReplayResponseModeOf(laserdisc.channel),
            ReplayResponseMode::kByPicture);
  EXPECT_TRUE(laserdisc.channel.response_db.empty());
  ASSERT_EQ(laserdisc.channel.response_by_picture.size(), 2U);
  EXPECT_LT(laserdisc.channel.response_by_picture[0].picture,
            laserdisc.channel.response_by_picture[1].picture);
  for (const ResponseByPictureEntry& entry :
       laserdisc.channel.response_by_picture) {
    EXPECT_EQ(entry.response_db,
              (std::vector<NoiseSpectrumPoint>{{1.0, -2.0}, {4.0, -6.0}}));
  }

  // Back to fixed takes the first position's response.
  laserdisc.channel.response_by_picture[0].response_db = {{2.0, -1.0}};
  SetReplayResponseMode(&laserdisc, ReplayResponseMode::kFixed);
  EXPECT_TRUE(laserdisc.channel.response_by_picture.empty());
  EXPECT_EQ(laserdisc.channel.response_db,
            (std::vector<NoiseSpectrumPoint>{{2.0, -1.0}}));

  SetReplayResponseMode(&laserdisc, ReplayResponseMode::kNone);
  EXPECT_TRUE(laserdisc.channel.response_db.empty());
  EXPECT_TRUE(laserdisc.channel.response_by_picture.empty());
}

TEST(ProjectSettingsPresenterTest, ResponseByPictureEntriesAscend) {
  namespace limits = project_editor_limits;
  std::vector<ResponseByPictureEntry> entries;
  AddResponseByPictureEntry(&entries);
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].picture, limits::kResponsePictureMin);
  EXPECT_FALSE(entries[0].response_db.empty());
  entries[0].response_db = {{1.0, -4.0}};

  AddResponseByPictureEntry(&entries);
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_GT(entries[1].picture, entries[0].picture);
  EXPECT_EQ(entries[1].response_db, entries[0].response_db);

  // Near the editor's limit the new entry lands on it, then stops.
  entries[1].picture = limits::kResponsePictureMax - 1;
  AddResponseByPictureEntry(&entries);
  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[2].picture, limits::kResponsePictureMax);
  AddResponseByPictureEntry(&entries);
  EXPECT_EQ(entries.size(), 3U);

  RemoveResponseByPictureEntry(&entries, 5);
  EXPECT_EQ(entries.size(), 3U);
  RemoveResponseByPictureEntry(&entries, 0);
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].picture, limits::kResponsePictureMax - 1);
}

TEST(ProjectSettingsPresenterTest, CurvePointsAppendAboveTheLast) {
  std::vector<CurvePoint> points;
  AddCurvePoint(&points, 1.0, 5.0, CurvePoint{0.5, 0.0});
  ASSERT_EQ(points.size(), 1U);
  EXPECT_EQ(points[0], (CurvePoint{0.5, 0.0}));
  points[0].y = -3.0;
  AddCurvePoint(&points, 1.0, 5.0, CurvePoint{0.5, 0.0});
  ASSERT_EQ(points.size(), 2U);
  EXPECT_EQ(points[1], (CurvePoint{1.5, -3.0}));
  // Closer to the maximum than a step: halfway there.
  points[1].x = 4.5;
  AddCurvePoint(&points, 1.0, 5.0, CurvePoint{0.5, 0.0});
  ASSERT_EQ(points.size(), 3U);
  EXPECT_DOUBLE_EQ(points[2].x, 4.75);
  points[2].x = 5.0;
  AddCurvePoint(&points, 1.0, 5.0, CurvePoint{0.5, 0.0});
  EXPECT_EQ(points.size(), 3U);

  RemoveCurvePoint(&points, -1);
  EXPECT_EQ(points.size(), 3U);
  RemoveCurvePoint(&points, 1);
  ASSERT_EQ(points.size(), 2U);
  EXPECT_EQ(points[1].x, 5.0);
}

TEST(ProjectSettingsPresenterTest, CurvePointConversionsRoundTrip) {
  const std::vector<NoiseSpectrumPoint> response = {{0.5, 0.0}, {4.0, -6.0}};
  EXPECT_EQ(ToResponsePoints(ToCurvePoints(response)), response);
  const std::vector<GroupDelayPoint> delay = Iec60856NominalGroupDelay();
  EXPECT_EQ(ToGroupDelayPoints(ToCurvePoints(delay)), delay);
  const std::vector<LevelCurvePoint> level = {{0.0, 1.0}, {100.0, 0.9}};
  EXPECT_EQ(ToLevelCurvePoints(ToCurvePoints(level)), level);
  EXPECT_EQ(ToCurvePoints(level)[1], (CurvePoint{100.0, 0.9}));
}

TEST(ProjectSettingsPresenterTest, DifferentialAndCrosstalkSeedValidBlocks) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    SCOPED_TRACE(StandardToString(standard));
    Project project = MakeProject(standard);
    SetDifferentialEnabled(&project.chroma_error, true);
    SetCrosstalkEnabled(&project.chroma_error, true);
    EXPECT_TRUE(project.chroma_error.enabled);
    EXPECT_FALSE(project.chroma_error.differential.gain.empty());
    EXPECT_FALSE(project.chroma_error.differential.phase_deg.empty());
    EXPECT_NE(project.chroma_error.crosstalk.v_to_u, 0.0);
    EXPECT_TRUE(IsValid(project));
    EXPECT_EQ(RoundTrip(project), project);

    // Re-enabling keeps edited values; disabling resets the sub-block only.
    project.chroma_error.crosstalk.u_to_v = 0.2;
    SetCrosstalkEnabled(&project.chroma_error, true);
    EXPECT_EQ(project.chroma_error.crosstalk.u_to_v, 0.2);
    SetDifferentialEnabled(&project.chroma_error, false);
    EXPECT_EQ(project.chroma_error.differential, DifferentialParameters{});
    EXPECT_TRUE(project.chroma_error.crosstalk.enabled);
  }
}

TEST(ProjectSettingsPresenterTest, DifferentialAndCrosstalkLimitsMirrorValid) {
  namespace limits = project_editor_limits;
  Project project = MakeProject(Standard::kPal);
  SetDifferentialEnabled(&project.chroma_error, true);
  SetCrosstalkEnabled(&project.chroma_error, true);
  ASSERT_TRUE(IsValid(project));
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
  DifferentialParameters& differential = project.chroma_error.differential;
  differential.gain = {{0.0, 1.0}};
  differential.phase_deg = {{0.0, 0.0}};
  check(&differential.gain[0].level_ire, limits::kDifferentialLevelMinIre,
        limits::kDifferentialLevelMaxIre);
  check(&differential.gain[0].value, limits::kDifferentialGainMin,
        limits::kDifferentialGainMax);
  check(&differential.phase_deg[0].value, -limits::kDifferentialPhaseMaxDeg,
        limits::kDifferentialPhaseMaxDeg);
  check(&project.chroma_error.crosstalk.v_to_u, -limits::kCrosstalkMax,
        limits::kCrosstalkMax);
  check(&project.chroma_error.crosstalk.u_to_v, -limits::kCrosstalkMax,
        limits::kCrosstalkMax);
}

TEST(ProjectSettingsPresenterTest, LaserdiscChannelLimitsMirrorValidator) {
  namespace limits = project_editor_limits;
  Project project = MakeProject(Standard::kPal);
  SetFmBeatEnabled(&project.laserdisc, true);
  SetGroupDelayEnabled(&project.laserdisc, true, Standard::kPal);
  SetGroupDelayPreset(&project.laserdisc, GroupDelayPreset::kCustom);
  SetField2OffsetEnabled(&project.laserdisc, true);
  SetReplayResponseMode(&project.laserdisc, ReplayResponseMode::kFixed);
  LaserdiscChannelParameters& channel = project.laserdisc.channel;
  channel.group_delay.points_ns = {{0.5, 0.0}};
  channel.response_db = {{0.5, 0.0}};
  ASSERT_TRUE(IsValid(project));
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
  check(&channel.fm_beat.carrier_black_hz, limits::kFmBeatCarrierMinHz,
        limits::kFmBeatCarrierMaxHz);
  check(&channel.fm_beat.level_db, limits::kFmBeatLevelMinDb,
        limits::kFmBeatLevelMaxDb);
  check(&channel.group_delay.scale, -limits::kGroupDelayScaleMax,
        limits::kGroupDelayScaleMax);
  check(&channel.group_delay.points_ns[0].delay_ns, -limits::kGroupDelayMaxNs,
        limits::kGroupDelayMaxNs);
  check(&channel.group_delay.points_ns[0].frequency_mhz, 0.0,
        NoiseSpectrumMaxFrequencyMhz(Standard::kPal));
  check(&channel.response_db[0].relative_db, limits::kReplayResponseMinDb,
        limits::kReplayResponseMaxDb);
  check(&channel.field2_offset.x_samples, -limits::kField2OffsetMaxSamples,
        limits::kField2OffsetMaxSamples);
  check(&channel.field2_offset.y_lines, -limits::kField2OffsetMaxLines,
        limits::kField2OffsetMaxLines);

  SetReplayResponseMode(&project.laserdisc, ReplayResponseMode::kByPicture);
  ASSERT_TRUE(IsValid(project));
  channel.response_by_picture[0].picture = limits::kResponsePictureMin - 1;
  EXPECT_FALSE(IsValid(project));
}

}  // namespace
}  // namespace videosynth::gui
