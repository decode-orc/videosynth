/*
 * File:        test_project_validator.cpp
 * Module:      project_validator_tests
 * Purpose:     Validates project constraint enforcement and error reporting.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "videosynth/project_validator.h"

namespace videosynth {
namespace {

std::string DefaultProgressiveSourcePath() { return "fixture.exr"; }

Project MakeValidProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = "/tmp/videosynth_validator_test.cvbs";
  project.output.metadata_path = "/tmp/videosynth_validator_test.meta";
  project.sections.push_back(Section{.name = "Progressive",
                                     .type = "progressive",
                                     .line_injections = {},
                                     .source = DefaultProgressiveSourcePath(),
                                     .duration_frames = 1});
  return project;
}

std::string MakeSourcePath(const std::string& file_name) { return file_name; }

// A profile matching the bundled BT.601 FFV1 clips for the given standard.
ProgressiveFrameSourceProfile MakeConformantVideoProfile(Standard standard) {
  const bool is_625 = Is625LineStandard(standard);
  ProgressiveFrameSourceProfile profile;
  profile.container = "matroska,webm";
  profile.codec = "ffv1";
  profile.pixel_format = "yuv422p10le";
  profile.field_order = is_625 ? "tb" : "bt";
  profile.color_space = "smpte170m";
  profile.color_primaries = is_625 ? "bt470bg" : "smpte170m";
  profile.color_transfer = "bt709";
  profile.color_range = "tv";
  profile.bit_depth = 10;
  profile.width = 720;
  profile.height = is_625 ? 576 : 486;
  profile.sample_aspect_ratio = is_625 ? (128.0 / 117.0) : (108.0 / 119.0);
  profile.frame_rate_hz = is_625 ? 25.0 : (30000.0 / 1001.0);
  profile.average_frame_rate_hz = profile.frame_rate_hz;
  profile.frame_count = 120;
  return profile;
}

class RecordingLogger final : public ILogger {
 public:
  void Info(const std::string& message) override { info.push_back(message); }
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string& message) override { debug.push_back(message); }
  void Trace(const std::string&) override {}

  std::vector<std::string> info;
  std::vector<std::string> debug;
};

class MockProgressiveFrameSourceProbe final
    : public IProgressiveFrameSourceProbe {
 public:
  bool should_succeed = true;
  ProgressiveFrameSourceProfile profile;
  std::string error_message;

  bool Probe(const Section&, ProgressiveFrameSourceProfile* out_profile,
             std::string* error) override {
    if (!should_succeed) {
      if (error != nullptr) {
        *error = error_message;
      }
      return false;
    }

    if (out_profile != nullptr) {
      *out_profile = profile;
    }
    return true;
  }
};

TEST(ProjectValidatorTest, AcceptsMvpCompliantProject) {
  ProjectValidator validator;
  const ValidationResult result = validator.Validate(MakeValidProject());

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, AcceptsDurationRepeatWithAllSourceFrames) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("test.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;
  project.sections[0].duration_frames_repeat = 3;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
}

TEST(ProjectValidatorTest, RejectsDurationRepeatBelowOne) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;
  project.sections[0].duration_frames_repeat = 0;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
}

TEST(ProjectValidatorTest, WarnsDurationRepeatWithoutAllSourceFrames) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames_all = false;
  project.sections[0].duration_frames = 5;
  project.sections[0].duration_frames_repeat = 2;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_FALSE(result.warnings.empty());
}

TEST(ProjectValidatorTest, AcceptsKnownAssetRootTokenSource) {
  Project project = MakeValidProject();
  project.sections[0].source = "{bundled}/exr/720x576/75_BARS.exr";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsUnknownAssetRootTokenSource) {
  Project project = MakeValidProject();
  project.sections[0].source = "{nope}/bars.exr";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  bool mentions_unknown_root = false;
  for (const std::string& error : result.errors) {
    if (error.find("unknown asset root 'nope'") != std::string::npos) {
      mentions_unknown_root = true;
    }
  }
  EXPECT_TRUE(mentions_unknown_root);
}

TEST(ProjectValidatorTest, AcceptsOutputRootTokenInVideoPath) {
  Project project = MakeValidProject();
  project.output.video_path = "{output}/clip.cvbs";
  project.output.metadata_path = "{output}/clip.meta";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsUnknownAssetRootTokenInVideoPath) {
  Project project = MakeValidProject();
  project.output.video_path = "{outputs}/clip.cvbs";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  bool mentions_unknown_root = false;
  for (const std::string& error : result.errors) {
    if (error.find("unknown asset root 'outputs'") != std::string::npos &&
        error.find("output.video_path") != std::string::npos) {
      mentions_unknown_root = true;
    }
  }
  EXPECT_TRUE(mentions_unknown_root);
}

TEST(ProjectValidatorTest, AcceptsTpg21SampleEncodingPreset) {
  Project project = MakeValidProject();
  project.cvbs_presets.sample_encoding_preset = "CVBS_TPG21_4FSC";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, AcceptsSupportedSampleEncodingPresets) {
  const std::vector<std::string> presets = {"CVBS_U10_4FSC",   "CVBS_U16_4FSC",
                                            "RAW_S16_28M",     "RAW_S16_40M",
                                            "CVBS_TPG21_4FSC", "CVBS_S16_4FSC"};

  for (const std::string& preset : presets) {
    Project project = MakeValidProject();
    project.cvbs_presets.sample_encoding_preset = preset;

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);

    EXPECT_TRUE(result.is_valid) << preset;
    EXPECT_TRUE(result.errors.empty()) << preset;
  }
}

TEST(ProjectValidatorTest, RejectsInvalidStandard) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kUnknown;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsSubcarrierLockDisabled) {
  Project project = MakeValidProject();
  project.cvbs_presets.signal_state_preset = "NONSTANDARD_RAW";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
}

TEST(ProjectValidatorTest, AcceptsSupportedNtscBlackSetupValues) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.ntsc_black_setup_ire = 0.0;
  project.cvbs_presets.ntsc_black_setup_ire_specified = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsUnsupportedNtscBlackSetupValue) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.ntsc_black_setup_ire = 1.0;
  project.cvbs_presets.ntsc_black_setup_ire_specified = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsNtscBlackSetupOnPalProject) {
  Project project = MakeValidProject();
  project.cvbs_presets.ntsc_black_setup_ire = 0.0;
  project.cvbs_presets.ntsc_black_setup_ire_specified = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsPalLaserdiscPilotBurstOnNtscProject) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_EQ(
      result.errors[0],
      "Project configuration error: pal_laserdisc_pilot_burst can only be "
      "enabled for PAL projects.");
}

TEST(ProjectValidatorTest, RejectsNtscLaserdiscVbiBurstOnPalProject) {
  Project project = MakeValidProject();
  project.cvbs_presets.ntsc_laserdisc_vbi_burst = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_EQ(result.errors[0],
            "Project configuration error: ntsc_laserdisc_vbi_burst can only be "
            "enabled for NTSC projects.");
}

Project MakeValidSecamProject() {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kSecam;
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_UNLOCKED";
  return project;
}

// True when any diagnostic in `messages` contains `needle`.
bool ContainsMessage(const std::vector<std::string>& messages,
                     const std::string& needle) {
  for (const std::string& message : messages) {
    if (message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TEST(ProjectValidatorTest, AcceptsSecamProjectWithUnlockedSignalState) {
  ProjectValidator validator;
  const ValidationResult result = validator.Validate(MakeValidSecamProject());

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.warnings.empty());
}

TEST(ProjectValidatorTest, RejectsPhaseLockedSignalStateForSecam) {
  // CVBS File Format Specification v1.7.0, Preset: SECAM: no Sc/H convention,
  // so a SECAM file must not be declared phase locked.
  Project project = MakeValidSecamProject();
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(
      result.errors,
      "Project configuration error: signal_state_preset must be "
      "'STANDARD_STABLE_UNLOCKED' for SECAM projects."));
}

TEST(ProjectValidatorTest, RejectsUnlockedSignalStateForPal) {
  Project project = MakeValidProject();
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_UNLOCKED";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors,
                              "Project configuration error: "
                              "signal_state_preset must be "
                              "'STANDARD_STABLE_LOCKED'."));
}

TEST(ProjectValidatorTest, RejectsPalLaserdiscPilotBurstOnSecamProject) {
  Project project = MakeValidSecamProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors,
                              "Project configuration error: "
                              "pal_laserdisc_pilot_burst can only be enabled "
                              "for PAL projects."));
}

TEST(ProjectValidatorTest, RejectsLaserdiscDiscTypeOnSecamProject) {
  Project project = MakeValidSecamProject();
  project.line_injections.disc_type = "CAV";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(
      result.errors,
      "Project configuration error: laserdisc line injections, "
      "line_injections.disc_type and laserdisc VITS placement are not "
      "available for SECAM projects."));
}

TEST(ProjectValidatorTest, WarnsThatSecamVitsKeepPalDefinitions) {
  Project project = MakeValidSecamProject();
  project.line_injections.vits.push_back(VitsInjection{"vits17", {17}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors[0]);
  EXPECT_TRUE(ContainsMessage(
      result.warnings,
      "SECAM warning: VITS test signals are rendered with their PAL "
      "definitions, including any PAL-modulated chroma packets."));
}

TEST(ProjectValidatorTest, WarnsWhenSecamFieldIdentificationSetWithoutSecam) {
  Project project = MakeValidProject();
  project.cvbs_presets.secam_field_identification = false;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(
      result.warnings,
      "secam_field_identification has no effect unless video_standard_preset "
      "is SECAM."));
}

TEST(ProjectValidatorTest, AcceptsPalLaserdiscPilotBurstOnPalProject) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, WarnsPalPilotBurstClippingWithU10Preset) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  ASSERT_EQ(result.warnings.size(), 1U);
  EXPECT_EQ(result.warnings[0],
            "pal_laserdisc_pilot_burst warning: preset 'CVBS_U10_4FSC' clips "
            "sub-sync excursions below -300 mV; the pilot burst trough reaches "
            "-600 mV. Use CVBS_S16_4FSC or RAW_S16_28M/RAW_S16_40M to preserve "
            "the full burst waveform.");
}

TEST(ProjectValidatorTest, WarnsPalPilotBurstClippingWithU16Preset) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U16_4FSC";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  ASSERT_EQ(result.warnings.size(), 1U);
  EXPECT_EQ(result.warnings[0],
            "pal_laserdisc_pilot_burst warning: preset 'CVBS_U16_4FSC' clips "
            "sub-sync excursions below -300 mV; the pilot burst trough reaches "
            "-600 mV. Use CVBS_S16_4FSC or RAW_S16_28M/RAW_S16_40M to preserve "
            "the full burst waveform.");
}

TEST(ProjectValidatorTest, WarnsPalPilotBurstClippingWithTpg21Preset) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  project.cvbs_presets.sample_encoding_preset = "CVBS_TPG21_4FSC";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  ASSERT_EQ(result.warnings.size(), 1U);
  EXPECT_EQ(result.warnings[0],
            "pal_laserdisc_pilot_burst warning: preset 'CVBS_TPG21_4FSC' clips "
            "sub-sync excursions below -300 mV; the pilot burst trough reaches "
            "-600 mV. Use CVBS_S16_4FSC or RAW_S16_28M/RAW_S16_40M to preserve "
            "the full burst waveform.");
}

TEST(ProjectValidatorTest, NoPilotBurstClippingWarningWithS16FscPreset) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  project.cvbs_presets.sample_encoding_preset = "CVBS_S16_4FSC";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.warnings.empty());
}

TEST(ProjectValidatorTest, NoPilotBurstClippingWarningWithRawS1628MPreset) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  project.cvbs_presets.sample_encoding_preset = "RAW_S16_28M";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.warnings.empty());
}

TEST(ProjectValidatorTest, NoPilotBurstClippingWarningWithRawS1640MPreset) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  project.cvbs_presets.sample_encoding_preset = "RAW_S16_40M";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.warnings.empty());
}

TEST(ProjectValidatorTest, NoPilotBurstClippingWarningWhenBurstDisabled) {
  Project project = MakeValidProject();
  project.cvbs_presets.pal_laserdisc_pilot_burst = false;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.warnings.empty());
}

TEST(ProjectValidatorTest, RejectsNtscLaserdiscVbiBurstAsDeferredFeature) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.ntsc_laserdisc_vbi_burst = true;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_EQ(
      result.errors[0],
      "Project configuration error: ntsc_laserdisc_vbi_burst is parsed but "
      "not implemented in the current runtime.");
}

TEST(ProjectValidatorTest, AcceptsProjectVitsSet) {
  Project project = MakeValidProject();
  project.line_injections.vits.push_back(VitsInjection{"vits17", {17}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsLaserdiscInjectionWithoutProjectDiscType) {
  // A section declaring laserdisc codes requires a project-wide disc_type.
  Project project = MakeValidProject();
  Section::LineInjection injection;
  injection.type = "laserdisc";
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("disc_type"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsProjectVitsWithoutTargetLines) {
  Project project = MakeValidProject();
  project.line_injections.vits.push_back(VitsInjection{"vits17", {}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: VITS injection 'vits17' must "
            "provide at least one target line.");
}

TEST(ProjectValidatorTest, RejectsVitsTypeThatDoesNotMatchProjectStandard) {
  Project project = MakeValidProject();
  project.line_injections.vits.push_back(VitsInjection{"ntc7-composite", {17}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: Unsupported vits_type "
            "'ntc7-composite' for standard 'PAL'.");
}

TEST(ProjectValidatorTest, RejectsVitsTargetLineOutsideStandardRange) {
  Project project = MakeValidProject();
  project.line_injections.vits.push_back(VitsInjection{"vits17", {626}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: VITS target line 626 is outside "
            "the valid frame-line range for PAL.");
}

TEST(ProjectValidatorTest, RejectsVitsTypeOnNonRecommendedLine) {
  Project project = MakeValidProject();
  project.line_injections.vits.push_back(VitsInjection{"vits17", {19}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: vits_type 'vits17' must target "
            "frame line 17 for PAL under standard placement.");
}

TEST(ProjectValidatorTest, RejectsOverlappingTargetLinesAcrossProjectVits) {
  Project project = MakeValidProject();
  project.line_injections.vits.push_back(VitsInjection{"vits17", {17}});
  project.line_injections.vits.push_back(VitsInjection{"vits17", {17}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: overlapping VITS target line 17 "
            "within the project VITS set.");
}

TEST(ProjectValidatorTest, RejectsLaserdiscInjectionWithExplicitTargetLines) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CAV";
  project.sections[0].section_type = SectionType::kProgrammeArea;

  Section::LineInjection injection;
  injection.type = "laserdisc";
  injection.target_lines = {16};
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: target_lines must not be "
            "specified for laserdisc injections.");
}

TEST(ProjectValidatorTest, RejectsLaserdiscAndVitcInSameSection) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CAV";
  project.sections[0].section_type = SectionType::kProgrammeArea;

  Section::LineInjection laserdisc;
  laserdisc.type = "laserdisc";
  project.sections[0].line_injections.push_back(laserdisc);

  Section::LineInjection vitc;
  vitc.type = "vitc";
  vitc.target_lines = {21};
  project.sections[0].line_injections.push_back(vitc);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: vitc and laserdisc injections "
            "cannot appear in the same section.");
}

TEST(ProjectValidatorTest,
     RejectsProjectVitsInLaserdiscReservedRangesWhenLaserdiscIsActive) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CAV";
  project.line_injections.vits.push_back(VitsInjection{"vits17", {17}});
  project.sections[0].section_type = SectionType::kProgrammeArea;

  Section::LineInjection laserdisc;
  laserdisc.type = "laserdisc";
  project.sections[0].line_injections.push_back(laserdisc);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: VITS target line 17 conflicts "
            "with laserdisc reserved VBI ranges for PAL.");
}

TEST(ProjectValidatorTest, LaserdiscPlacementAcceptsLaserdiscVitsLines) {
  Project project = MakeValidProject();
  project.line_injections.placement = VitsPlacement::kLaserdisc;
  project.line_injections.vits.push_back(
      VitsInjection{"uk-national", {19, 332}});
  project.line_injections.vits.push_back(VitsInjection{"vits20", {20, 333}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, LaserdiscPlacementRejectsNonLaserdiscVitsLine) {
  Project project = MakeValidProject();
  project.line_injections.placement = VitsPlacement::kLaserdisc;
  // 17 is vits17's broadcast line but not a laserdisc VITS line.
  project.line_injections.vits.push_back(VitsInjection{"vits17", {17}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: VITS target line 17 is not a "
            "laserdisc VITS line for PAL under laserdisc placement (allowed: "
            "19, 20, 332, 333).");
}

TEST(ProjectValidatorTest, CustomPlacementAcceptsNonRecommendedVitsLine) {
  Project project = MakeValidProject();
  project.line_injections.placement = VitsPlacement::kCustom;
  // vits17's broadcast line is 17; custom placement allows any valid line.
  project.line_injections.vits.push_back(VitsInjection{"vits17", {21}});

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest,
     CustomPlacementRejectsReservedVitsLineOnLaserdiscProject) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CAV";
  project.line_injections.placement = VitsPlacement::kCustom;
  project.line_injections.vits.push_back(VitsInjection{"vits17", {17}});
  project.sections[0].section_type = SectionType::kProgrammeArea;

  Section::LineInjection laserdisc;
  laserdisc.type = "laserdisc";
  project.sections[0].line_injections.push_back(laserdisc);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Line injection validation error: VITS target line 17 conflicts "
            "with laserdisc reserved VBI ranges for PAL.");
}

TEST(ProjectValidatorTest, RejectsUnsupportedSectionType) {
  Project project = MakeValidProject();
  project.sections[0].type = "software_generated";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
}

TEST(ProjectValidatorTest, AcceptsProgressiveExrSectionWithFixedDuration) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("test.exr");
  project.sections[0].duration_frames = 8;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsProgressiveSectionWithoutSource) {
  Project project = MakeValidProject();
  project.sections[0].source.clear();

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
}

TEST(ProjectValidatorTest, RejectsProgressiveRawSourceFamily) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("test.raw");

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("Unsupported progressive source family"),
            std::string::npos);
}

TEST(ProjectValidatorTest, AcceptsProgressiveAllDurationSemantics) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("test.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
}

TEST(ProjectValidatorTest, RejectsMissingDurationFrames) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames = 0;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
}

TEST(ProjectValidatorTest, AcceptsProgressiveMkvWithSupportedFfv1Profile) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.sections[0].source = MakeSourcePath("test.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile.container = "matroska,webm";
  probe.profile.codec = "ffv1";
  probe.profile.pixel_format = "yuv422p10le";
  probe.profile.field_order = "tb";
  probe.profile.color_space = "smpte170m";
  probe.profile.color_primaries = "bt470bg";
  probe.profile.color_transfer = "bt709";
  probe.profile.color_range = "tv";
  probe.profile.bit_depth = 10;
  probe.profile.width = 720;
  probe.profile.height = 576;
  probe.profile.frame_rate_hz = 25.0;
  probe.profile.frame_count = 120;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest,
     WarnsButAcceptsProgressiveMkvWithMismatchedSampleAspectMetadata) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.sections[0].source = MakeSourcePath("test.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kNtsc);
  probe.profile.sample_aspect_ratio = 1.0;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  ASSERT_EQ(result.warnings.size(), 1U);
  EXPECT_NE(result.warnings[0].find("sample aspect ratio"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsProgressiveMkvWithStreamCropMetadata) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.sections[0].source = MakeSourcePath("test.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile.container = "matroska,webm";
  probe.profile.codec = "ffv1";
  probe.profile.pixel_format = "yuv422p10le";
  probe.profile.field_order = "tb";
  probe.profile.color_space = "smpte170m";
  probe.profile.color_primaries = "bt470bg";
  probe.profile.color_transfer = "bt709";
  probe.profile.color_range = "tv";
  probe.profile.bit_depth = 10;
  probe.profile.width = 720;
  probe.profile.height = 576;
  probe.profile.sample_aspect_ratio = 128.0 / 117.0;
  probe.profile.crop_left = 1;
  probe.profile.frame_rate_hz = 25.0;
  probe.profile.frame_count = 120;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("crop metadata"), std::string::npos);
}

TEST(ProjectValidatorTest, AcceptsProgressiveMkvWithAnyDecodableCodecProfile) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.sections[0].source = MakeSourcePath("test.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kNtsc);
  probe.profile.codec = "h264";
  probe.profile.pixel_format = "yuv420p";
  probe.profile.bit_depth = 8;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.warnings.empty());
}

TEST(ProjectValidatorTest, AcceptsProgressiveMp4WithMp4Container) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("capture.MP4");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kPal);
  probe.profile.container = "mov,mp4,m4a,3gp,3g2,mj2";
  probe.profile.codec = "h264";
  probe.profile.pixel_format = "yuv420p";

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

// A 120-frame conformant MKV section (see MakeConformantVideoProfile) with
// the given range; duration "all" unless the test overrides it.
Project MakeRangedVideoProject(int start_frame, int end_frame,
                               bool end_frame_specified) {
  Project project = MakeValidProject();
  Section& section = project.sections[0];
  section.source = MakeSourcePath("capture.mkv");
  section.duration_frames_all = true;
  section.duration_frames = 0;
  section.start_frame = start_frame;
  section.end_frame = end_frame;
  section.end_frame_specified = end_frame_specified;
  return project;
}

ValidationResult ValidateWithConformantVideo(const Project& project) {
  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kPal);
  ProjectValidator validator(&probe);
  return validator.Validate(project);
}

TEST(ProjectValidatorTest, AcceptsVideoRangeInsideTheSource) {
  const ValidationResult result =
      ValidateWithConformantVideo(MakeRangedVideoProject(10, 119, true));
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors[0]);
}

TEST(ProjectValidatorTest, RejectsEndFrameBeforeStartFrame) {
  const ValidationResult result =
      ValidateWithConformantVideo(MakeRangedVideoProject(20, 19, true));
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(
      ContainsMessage(result.errors, "end_frame must be >= start_frame"));
}

TEST(ProjectValidatorTest, RejectsNegativeStartFrame) {
  const ValidationResult result =
      ValidateWithConformantVideo(MakeRangedVideoProject(-1, 0, false));
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors, "start_frame must be >= 0"));
}

TEST(ProjectValidatorTest, RejectsStartFrameBeyondTheSource) {
  const ValidationResult result =
      ValidateWithConformantVideo(MakeRangedVideoProject(120, 0, false));
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors,
                              "start_frame 120 is beyond the end of the "
                              "source (120 frames, last frame 119)"));
}

TEST(ProjectValidatorTest, RejectsEndFrameBeyondTheSource) {
  const ValidationResult result =
      ValidateWithConformantVideo(MakeRangedVideoProject(0, 120, true));
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(
      ContainsMessage(result.errors, "end_frame 120 is beyond the end"));
}

TEST(ProjectValidatorTest, RejectsEndFrameWithIntegerDuration) {
  Project project = MakeRangedVideoProject(10, 19, true);
  project.sections[0].duration_frames_all = false;
  project.sections[0].duration_frames = 50;
  const ValidationResult result = ValidateWithConformantVideo(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors,
                              "end_frame cannot be combined with an integer "
                              "duration_frames"));
}

TEST(ProjectValidatorTest, AcceptsStartFrameWithLoopingIntegerDuration) {
  // 110 frames remain after frame 10, so 500 frames loop the range.
  Project project = MakeRangedVideoProject(10, 0, false);
  project.sections[0].duration_frames_all = false;
  project.sections[0].duration_frames = 500;
  const ValidationResult result = ValidateWithConformantVideo(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors[0]);
}

TEST(ProjectValidatorTest, RejectsFrameRangeOnExrStill) {
  for (const bool end_specified : {false, true}) {
    Project project = MakeValidProject();
    project.sections[0].start_frame = end_specified ? 0 : 5;
    project.sections[0].end_frame = 9;
    project.sections[0].end_frame_specified = end_specified;
    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);
    EXPECT_FALSE(result.is_valid) << "end_specified " << end_specified;
    EXPECT_TRUE(
        ContainsMessage(result.errors, "an EXR still has only one frame"));
  }
}

TEST(ProjectValidatorTest, RejectsAllDurationOnExrStill) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;
  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors,
                              "an EXR still needs an integer duration_frames"));
}

TEST(ProjectValidatorTest, RejectsVideoWhoseContainerDoesNotMatchExtension) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("capture.mp4");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kPal);
  probe.profile.container = "matroska,webm";

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("does not match its file extension"),
            std::string::npos);
}

TEST(ProjectValidatorTest, RejectsVariableFrameRateVideo) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("capture.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kPal);
  probe.profile.average_frame_rate_hz = 23.1;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("variable frame rate"), std::string::npos);
}

TEST(ProjectValidatorTest, AcceptsAverageRateWithinTimestampRounding) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("capture.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kPal);
  probe.profile.average_frame_rate_hz = 25.01;

  ProjectValidator validator(&probe);
  EXPECT_TRUE(validator.Validate(project).is_valid);
}

TEST(ProjectValidatorTest, WarnsWhenFieldOrderOpposesStandardDominance) {
  struct Case {
    Standard standard;
    const char* field_order;
  };
  for (const Case& c :
       {Case{Standard::kPal, "bt"}, Case{Standard::kPal, "bb"},
        Case{Standard::kNtsc, "tb"}, Case{Standard::kNtsc, "tt"}}) {
    Project project = MakeValidProject();
    project.cvbs_presets.video_standard_preset = c.standard;
    project.sections[0].source = MakeSourcePath("capture.mkv");
    project.sections[0].duration_frames_all = true;
    project.sections[0].duration_frames = 0;

    MockProgressiveFrameSourceProbe probe;
    probe.profile = MakeConformantVideoProfile(c.standard);
    probe.profile.field_order = c.field_order;

    ProjectValidator validator(&probe);
    const ValidationResult result = validator.Validate(project);

    EXPECT_TRUE(result.is_valid) << c.field_order;
    ASSERT_EQ(result.warnings.size(), 1U) << c.field_order;
    EXPECT_NE(result.warnings[0].find("field order"), std::string::npos);
  }
}

TEST(ProjectValidatorTest, ProgressiveOrUntaggedFieldOrderDoesNotWarn) {
  for (const char* field_order : {"progressive", "", "unknown"}) {
    Project project = MakeValidProject();
    project.sections[0].source = MakeSourcePath("capture.mkv");
    project.sections[0].duration_frames_all = true;
    project.sections[0].duration_frames = 0;

    MockProgressiveFrameSourceProbe probe;
    probe.profile = MakeConformantVideoProfile(Standard::kPal);
    probe.profile.field_order = field_order;

    ProjectValidator validator(&probe);
    const ValidationResult result = validator.Validate(project);
    EXPECT_TRUE(result.is_valid);
    EXPECT_TRUE(result.warnings.empty()) << field_order;
  }
}

TEST(ProjectValidatorTest, LogsPlannedColourConversionAtInfo) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("capture.mp4");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kPal);
  probe.profile.container = "mov,mp4,m4a,3gp,3g2,mj2";
  probe.profile.pixel_format = "yuv420p";
  probe.profile.color_range = "pc";
  probe.profile.color_space = "bt709";

  RecordingLogger logger;
  ProjectValidator validator(&probe, &logger);
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  const auto conversion_line = std::find_if(
      logger.info.begin(), logger.info.end(), [](const std::string& line) {
        return line.find("colour conversion") != std::string::npos;
      });
  ASSERT_NE(conversion_line, logger.info.end());
  EXPECT_NE(conversion_line->find("full range -> studio swing"),
            std::string::npos);
  EXPECT_NE(conversion_line->find("BT.709 matrix -> BT.601"),
            std::string::npos);
}

TEST(ProjectValidatorTest, ConformantVideoLogsNoColourConversion) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("capture.mkv");
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames = 0;

  MockProgressiveFrameSourceProbe probe;
  probe.profile = MakeConformantVideoProfile(Standard::kPal);

  RecordingLogger logger;
  ProjectValidator validator(&probe, &logger);
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.warnings.empty());
  for (const std::string& line : logger.info) {
    EXPECT_EQ(line.find("colour conversion"), std::string::npos) << line;
  }
}

TEST(ProjectValidatorTest, AcceptsProgressiveExrWithSupportedProfile) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.sections[0].source = MakeSourcePath("test.exr");
  project.sections[0].duration_frames = 1;

  MockProgressiveFrameSourceProbe probe;
  probe.profile.container = "exr";
  probe.profile.codec = "openexr";
  probe.profile.pixel_format = "rgbf";
  probe.profile.bit_depth = 32;
  probe.profile.width = 720;
  probe.profile.height = 576;
  probe.profile.frame_rate_hz = 0.0;
  probe.profile.frame_count = 1;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsProgressiveExrWithUnsupportedPixelProfile) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.sections[0].source = MakeSourcePath("test.exr");
  project.sections[0].duration_frames = 1;

  MockProgressiveFrameSourceProbe probe;
  probe.profile.container = "exr";
  probe.profile.codec = "openexr";
  probe.profile.pixel_format = "rgb24";
  probe.profile.bit_depth = 24;
  probe.profile.width = 720;
  probe.profile.height = 486;
  probe.profile.frame_rate_hz = 0.0;
  probe.profile.frame_count = 1;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest,
     RejectsUnsupportedSourceFamilyWithExpectedErrorMessage) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("test.avi");

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Unsupported progressive source family. Supported source families "
            "are EXR stills and MKV or MP4 video.");
}

TEST(ProjectValidatorTest,
     RejectsPalProjectWhenProgressiveRasterDoesNotMatchStandard) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.sections[0].source = MakeSourcePath("test.exr");
  project.sections[0].duration_frames = 1;

  MockProgressiveFrameSourceProbe probe;
  probe.profile.container = "exr";
  probe.profile.codec = "openexr";
  probe.profile.pixel_format = "rgbf";
  probe.profile.bit_depth = 32;
  probe.profile.width = 720;
  probe.profile.height = 486;
  probe.profile.frame_rate_hz = 0.0;
  probe.profile.frame_count = 1;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Progressive section validation error: source raster must be "
            "720x576 for PAL and 720x486 for NTSC or PAL-M.");
}

TEST(ProjectValidatorTest,
     RejectsPalProjectWhenProgressiveFrameRateDoesNotMatchStandard) {
  Project project = MakeValidProject();
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.sections[0].source = MakeSourcePath("test.exr");
  project.sections[0].duration_frames = 1;

  MockProgressiveFrameSourceProbe probe;
  probe.profile.container = "exr";
  probe.profile.codec = "openexr";
  probe.profile.pixel_format = "rgbf";
  probe.profile.bit_depth = 32;
  probe.profile.width = 720;
  probe.profile.height = 576;
  probe.profile.frame_rate_hz = 24.0;
  probe.profile.frame_count = 1;

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "Progressive section validation error: source frame rate must "
            "match selected video standard.");
}

TEST(ProjectValidatorTest, PropagatesProgressiveProbeErrorMessage) {
  Project project = MakeValidProject();
  project.sections[0].source = MakeSourcePath("test.mkv");

  MockProgressiveFrameSourceProbe probe;
  probe.should_succeed = false;
  probe.error_message = "Phase1 probe failure test message.";

  ProjectValidator validator(&probe);
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0], "Phase1 probe failure test message.");
}

TEST(ProjectValidatorTest, AcceptsLaserdiscInjectionWithValidCavDiscType) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CAV";
  project.sections[0].section_type = SectionType::kProgrammeArea;
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode code;
  code.code_type = "picture_number";
  code.start_value = 1;
  code.start_value_specified = true;
  injection.codes.push_back(code);
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, AcceptsLaserdiscInjectionWithValidClvDiscType) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CLV";
  project.sections[0].section_type = SectionType::kProgrammeArea;
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode code;
  code.code_type = "programme_time_code";
  injection.codes.push_back(code);
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsLaserdiscInjectionWithUnknownDiscType) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "VHD";
  Section::LineInjection injection;
  injection.type = "laserdisc";
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("disc_type"), std::string::npos);
  EXPECT_NE(result.errors[0].find("VHD"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsLaserdiscInjectionWithCodeTypeInvalidForCav) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CAV";
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode code;
  code.code_type = "programme_time_code";
  injection.codes.push_back(code);
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("programme_time_code"), std::string::npos);
  EXPECT_NE(result.errors[0].find("CAV"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsLaserdiscInjectionWithCodeTypeInvalidForClv) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CLV";
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode code;
  code.code_type = "picture_number";
  injection.codes.push_back(code);
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("picture_number"), std::string::npos);
  EXPECT_NE(result.errors[0].find("CLV"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsLaserdiscInjectionWithUnknownCodeType) {
  Project project = MakeValidProject();
  project.line_injections.disc_type = "CAV";
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode code;
  code.code_type = "unknown_code";
  injection.codes.push_back(code);
  project.sections[0].line_injections.push_back(injection);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("unknown_code"), std::string::npos);
}

// ---------------------------------------------------------------------------
// OSD validation tests
// ---------------------------------------------------------------------------

TEST(ProjectValidatorTest, AcceptsValidOsdOverlay) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "PN:{picture_number}";
  overlay.x = 0;
  overlay.y = 0;
  overlay.scale = 2;
  overlay.fg_level = OsdFgLevel::kWhite;
  overlay.bg_level = OsdBgLevel::kBlack;
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_TRUE(result.is_valid);
}

TEST(ProjectValidatorTest, RejectsOsdOverlayScaleTooLow) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "LABEL";
  overlay.scale = 0;
  overlay.fg_level = OsdFgLevel::kWhite;
  overlay.bg_level = OsdBgLevel::kTransparent;
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("scale"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsOsdOverlayScaleTooHigh) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "LABEL";
  overlay.scale = 5;
  overlay.fg_level = OsdFgLevel::kWhite;
  overlay.bg_level = OsdBgLevel::kTransparent;
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("scale"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsOsdOverlayFgLevelUnknown) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "LABEL";
  overlay.scale = 1;
  overlay.fg_level = OsdFgLevel::kUnknown;
  overlay.fg_level_text = "0.5";
  overlay.bg_level = OsdBgLevel::kTransparent;
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("fg_luma"), std::string::npos);
}

TEST(ProjectValidatorTest, RejectsOsdOverlayBgLevelUnknown) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "LABEL";
  overlay.scale = 1;
  overlay.fg_level = OsdFgLevel::kWhite;
  overlay.bg_level = OsdBgLevel::kUnknown;
  overlay.bg_level_text = "-0.5";
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("bg_luma"), std::string::npos);
}

TEST(ProjectValidatorTest, AcceptsOsdOverlayTransparentBackground) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "LABEL";
  overlay.scale = 1;
  overlay.fg_level = OsdFgLevel::kWhite;
  overlay.bg_level = OsdBgLevel::kTransparent;
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_TRUE(result.is_valid);
}

TEST(ProjectValidatorTest, RejectsOsdOverlayWithUnknownToken) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "VALUE:{unknown_token}";
  overlay.scale = 1;
  overlay.fg_level = OsdFgLevel::kWhite;
  overlay.bg_level = OsdBgLevel::kTransparent;
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("unknown_token"), std::string::npos);
}

TEST(ProjectValidatorTest, AcceptsAllFourSupportedTokens) {
  Project project = MakeValidProject();
  OsdOverlay overlay;
  overlay.text = "{picture_number} {biphase_hex} {phase_id} {section_name}";
  overlay.scale = 1;
  overlay.fg_level = OsdFgLevel::kWhite;
  overlay.bg_level = OsdBgLevel::kTransparent;
  project.sections[0].osd.overlays.push_back(overlay);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);
  EXPECT_TRUE(result.is_valid);
}

// Installs a single channel pair (number `pair`) on section 0 and returns a
// mutable reference to its (active) left channel for the audio-validation
// tests.
AudioParameters& InstallLeftChannel(Project& project, int pair = 0) {
  project.sections[0].audio_channel_pairs.clear();
  AudioChannelPair channel_pair;
  channel_pair.pair = pair;
  channel_pair.pair_specified = true;
  channel_pair.left.enabled = true;
  project.sections[0].audio_channel_pairs.push_back(channel_pair);
  return project.sections[0].audio_channel_pairs.back().left;
}

TEST(ProjectValidatorTest, AcceptsValidFixedAudio) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames = 25;
  AudioParameters& audio = InstallLeftChannel(project);
  audio.waveform = AudioWaveform::kSine;
  audio.frequency_hz = 1000.0;
  audio.amplitude = 0.5;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, AcceptsMultipleChannelPairs) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames = 25;
  AudioChannelPair pair0;
  pair0.pair = 0;
  pair0.pair_specified = true;
  pair0.left.enabled = true;
  pair0.right.enabled = true;
  AudioChannelPair pair7;
  pair7.pair = 7;
  pair7.pair_specified = true;
  pair7.left.enabled = true;  // Right stays silent.
  project.sections[0].audio_channel_pairs = {pair0, pair7};

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, AcceptsValidPeriodicRampAudio) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames = 25;  // 1.0 s at 25 fps.
  AudioParameters& audio = InstallLeftChannel(project);
  audio.ramp_enabled = true;
  audio.ramp_start_specified = true;
  audio.ramp_end_specified = true;
  audio.ramp_start_hz = 100.0;
  audio.ramp_end_hz = 2000.0;
  audio.ramp_mode = AudioRampMode::kBounce;
  audio.ramp_period_seconds = 0.5;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsUnspecifiedPairNumber) {
  Project project = MakeValidProject();
  AudioChannelPair channel_pair;  // pair_specified stays false.
  channel_pair.left.enabled = true;
  project.sections[0].audio_channel_pairs.push_back(channel_pair);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsPairNumberOutOfRange) {
  Project project = MakeValidProject();
  InstallLeftChannel(project, /*pair=*/8);  // Only 0–7 permitted.

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsDuplicatePairNumberInSection) {
  Project project = MakeValidProject();
  AudioChannelPair first;
  first.pair = 0;
  first.pair_specified = true;
  first.left.enabled = true;
  AudioChannelPair second;
  second.pair = 0;  // Duplicate within the same section.
  second.pair_specified = true;
  second.left.enabled = true;
  project.sections[0].audio_channel_pairs = {first, second};

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsChannelPairWithNoActiveChannel) {
  Project project = MakeValidProject();
  AudioChannelPair channel_pair;
  channel_pair.pair = 0;
  channel_pair.pair_specified = true;
  // Neither left nor right is enabled.
  project.sections[0].audio_channel_pairs.push_back(channel_pair);

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsUnknownAudioWaveform) {
  Project project = MakeValidProject();
  AudioParameters& audio = InstallLeftChannel(project);
  audio.waveform = AudioWaveform::kUnknown;
  audio.waveform_text = "noise";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsAudioAmplitudeOutOfRange) {
  Project project = MakeValidProject();
  AudioParameters& audio = InstallLeftChannel(project);
  audio.amplitude = 1.5;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsAudioFrequencyOutOfRange) {
  Project project = MakeValidProject();
  AudioParameters& audio = InstallLeftChannel(project);
  audio.frequency_hz = 30000.0;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsRampMissingEndFrequency) {
  Project project = MakeValidProject();
  AudioParameters& audio = InstallLeftChannel(project);
  audio.ramp_enabled = true;
  audio.ramp_start_specified = true;
  audio.ramp_start_hz = 100.0;
  // ramp_end_specified deliberately left false.

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsUnknownRampMode) {
  Project project = MakeValidProject();
  AudioParameters& audio = InstallLeftChannel(project);
  audio.ramp_enabled = true;
  audio.ramp_start_specified = true;
  audio.ramp_end_specified = true;
  audio.ramp_start_hz = 100.0;
  audio.ramp_end_hz = 200.0;
  audio.ramp_mode = AudioRampMode::kUnknown;
  audio.ramp_mode_text = "fast";

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsRampFrequencyOutOfRange) {
  Project project = MakeValidProject();
  AudioParameters& audio = InstallLeftChannel(project);
  audio.ramp_enabled = true;
  audio.ramp_start_specified = true;
  audio.ramp_end_specified = true;
  audio.ramp_start_hz = 100.0;
  audio.ramp_end_hz = 25000.0;
  audio.ramp_mode = AudioRampMode::kUp;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsRampPeriodExceedingSectionDuration) {
  Project project = MakeValidProject();
  project.sections[0].duration_frames = 25;  // 1.0 s at 25 fps.
  AudioParameters& audio = InstallLeftChannel(project);
  audio.ramp_enabled = true;
  audio.ramp_start_specified = true;
  audio.ramp_end_specified = true;
  audio.ramp_start_hz = 100.0;
  audio.ramp_end_hz = 200.0;
  audio.ramp_mode = AudioRampMode::kUp;
  audio.ramp_period_seconds = 2.0;  // Exceeds the 1.0 s section.

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

TEST(ProjectValidatorTest, RejectsNegativeRampPeriod) {
  Project project = MakeValidProject();
  AudioParameters& audio = InstallLeftChannel(project);
  audio.ramp_enabled = true;
  audio.ramp_start_specified = true;
  audio.ramp_end_specified = true;
  audio.ramp_start_hz = 100.0;
  audio.ramp_end_hz = 200.0;
  audio.ramp_mode = AudioRampMode::kUp;
  audio.ramp_period_seconds = -1.0;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
}

Section MakeEfmSection(const std::string& name, SectionType section_type,
                       int duration_frames) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.section_type = section_type;
  section.source = DefaultProgressiveSourcePath();
  section.duration_frames = duration_frames;
  return section;
}

// Declares channel pair `pair` on `section` so the EFM selection resolves to a
// real audio source.
void AddActiveChannelPair(Section* section, int pair) {
  AudioChannelPair channel_pair;
  channel_pair.pair = pair;
  channel_pair.pair_specified = true;
  channel_pair.left.enabled = true;
  channel_pair.left.frequency_hz = 1000.0;
  section->audio_channel_pairs.push_back(channel_pair);
}

// PAL project (25 frames/s) with a lead-in, one 8 s programme-area track
// carrying channel pair 0, and a lead-out; EFM enabled for pair 0.
Project MakeEfmProject() {
  Project project = MakeValidProject();
  project.sections.clear();
  project.sections.push_back(
      MakeEfmSection("LeadIn", SectionType::kLeadIn, 25));
  Section programme =
      MakeEfmSection("Track1", SectionType::kProgrammeArea, 200);
  AddActiveChannelPair(&programme, 0);
  project.sections.push_back(programme);
  project.sections.push_back(
      MakeEfmSection("LeadOut", SectionType::kLeadOut, 25));
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 0;
  return project;
}

TEST(ProjectValidatorTest, AcceptsEfmAudioWithDeclaredPairAndLeadIn) {
  ProjectValidator validator;
  const ValidationResult result = validator.Validate(MakeEfmProject());

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_FALSE(ContainsMessage(result.warnings, "efm_audio"));
}

TEST(ProjectValidatorTest, RejectsEfmAudioPairAboveRange) {
  Project project = MakeEfmProject();
  project.output.efm_audio.pair = 8;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors, "output.efm_audio.pair"));
  EXPECT_TRUE(ContainsMessage(result.errors, "8"));
}

TEST(ProjectValidatorTest, RejectsEfmAudioPairBelowRange) {
  Project project = MakeEfmProject();
  project.output.efm_audio.pair = -1;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors, "output.efm_audio.pair"));
}

TEST(ProjectValidatorTest, RejectsEfmAudioForPalMPreset) {
  Project project = MakeEfmProject();
  project.cvbs_presets.video_standard_preset = Standard::kPalM;
  project.cvbs_presets.ntsc_black_setup_ire = 7.5;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors,
                              "output.efm_audio requires a PAL "
                              "or NTSC video_standard_preset"));
  EXPECT_TRUE(ContainsMessage(result.errors, "PAL_M"));
}

TEST(ProjectValidatorTest, WarnsWhenEfmAudioPairIsNotDeclared) {
  Project project = MakeEfmProject();
  project.output.efm_audio.pair = 4;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.warnings,
                              "output.efm_audio.pair 4 is not declared"));
}

TEST(ProjectValidatorTest, RejectsMoreEfmTracksThanTheDiscLimit) {
  Project project = MakeEfmProject();
  // 80 programme-area sections exceeds the 79-track limit of IEC 60856 Amd 2
  // 13.5.3.3 / IEC 60857 Amd 2 13.6.3.3. Insert before the lead_out so the
  // section ordering stays valid.
  for (int index = 1; index < 80; ++index) {
    project.sections.insert(project.sections.end() - 1,
                            MakeEfmSection("Track" + std::to_string(index + 1),
                                           SectionType::kProgrammeArea, 200));
  }

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.errors, "at most 79 tracks"));
  EXPECT_TRUE(ContainsMessage(result.errors, "80 programme_area section(s)"));
}

TEST(ProjectValidatorTest, AcceptsExactlyTheEfmTrackLimit) {
  Project project = MakeEfmProject();
  // Insert before the lead_out so the section ordering stays valid.
  for (int index = 1; index < 79; ++index) {
    project.sections.insert(project.sections.end() - 1,
                            MakeEfmSection("Track" + std::to_string(index + 1),
                                           SectionType::kProgrammeArea, 200));
  }

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_FALSE(ContainsMessage(result.errors, "at most 79 tracks"));
}

TEST(ProjectValidatorTest, WarnsWhenFirstEfmTrackIsShorterThanPausePlusTrack) {
  Project project = MakeEfmProject();
  // 125 PAL frames = 5 s: longer than the 4 s track minimum but shorter than
  // the 2 s pause plus 4 s track required of the first track.
  project.sections[1].duration_frames = 125;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(
      result.warnings,
      "section 'Track1' is shorter than the 6 s minimum length"));
}

TEST(ProjectValidatorTest, WarnsWhenLaterEfmTrackIsShorterThanFourSeconds) {
  Project project = MakeEfmProject();
  // 75 PAL frames = 3 s, below the 4 s minimum for a non-first track.
  project.sections.insert(
      project.sections.begin() + 2,
      MakeEfmSection("Track2", SectionType::kProgrammeArea, 75));

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(
      result.warnings,
      "section 'Track2' is shorter than the 4 s minimum length"));
  EXPECT_FALSE(ContainsMessage(result.warnings, "'Track1' is shorter"));
}

TEST(ProjectValidatorTest, WarnsWhenEfmProjectHasNoLeadInSection) {
  Project project = MakeEfmProject();
  project.sections.erase(project.sections.begin());

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(ContainsMessage(result.warnings, "no lead_in section"));
}

TEST(ProjectValidatorTest, IgnoresEfmTrackLayoutWhenEfmAudioIsDisabled) {
  Project project = MakeEfmProject();
  project.output.efm_audio.enabled = false;
  project.output.efm_audio.pair = 4;
  project.sections[1].duration_frames = 1;

  ProjectValidator validator;
  const ValidationResult result = validator.Validate(project);

  EXPECT_TRUE(result.is_valid);
  EXPECT_FALSE(ContainsMessage(result.warnings, "efm_audio"));
}

}  // namespace
}  // namespace videosynth
