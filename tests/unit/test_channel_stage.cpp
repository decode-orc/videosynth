/*
 * File:        test_channel_stage.cpp
 * Module:      channel_tests
 * Purpose:     Validates the `channel:` block (parsing and validation) and
 *              the ChannelStage FM beat, edge smear and timing offset.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "videosynth/channel_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/project_validator.h"
#include "videosynth/sample_clamp.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr double kPiForTests = 3.14159265358979323846;

const std::string kYamlPrefix =
    "cvbs_presets:\n"
    "  video_standard_preset: PAL\n"
    "  sample_encoding_preset: CVBS_U10_4FSC\n"
    "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
    "output:\n"
    "  video_path: out.cvbs\n";

const std::string kYamlSections =
    "sections:\n"
    "  - name: S\n"
    "    type: progressive\n"
    "    source: f.exr\n"
    "    duration_frames: 8\n";

ParseResult ParseWithChannel(const std::string& channel_block) {
  return YamlProjectParser().ParseString(kYamlPrefix + channel_block +
                                         kYamlSections);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

TEST(ChannelStageTest, ParserLeavesChannelDisabledWhenAbsent) {
  const ParseResult result = ParseWithChannel("");
  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.project.channel.enabled);
}

TEST(ChannelStageTest, ParserReadsTimingOffset) {
  const ParseResult result =
      ParseWithChannel("channel:\n  timing_offset_samples: -0.3\n");
  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.project.channel.enabled);
  EXPECT_DOUBLE_EQ(result.project.channel.timing_offset_samples, -0.3);
}

TEST(ChannelStageTest, ParserRejectsUnknownKey) {
  const ParseResult result =
      ParseWithChannel("channel:\n  timing_offset_samples: 0.5\n  jitter: 1\n");
  EXPECT_FALSE(result.ok);
}

TEST(ChannelStageTest, ParserRejectsEmptyBlock) {
  const ParseResult result = ParseWithChannel("channel: {}\n");
  ASSERT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "channel block must set at least one of: timing_offset_samples, "
            "edge_smear, fm_beat.");
}

TEST(ChannelStageTest, ParserReadsEdgeSmearAndFmBeat) {
  const ParseResult result = ParseWithChannel(
      "channel:\n"
      "  edge_smear: { fraction: 0.15, tau_samples: 4.0 }\n"
      "  fm_beat: { carrier_black_hz: 7.08e6, level_db: -20 }\n");
  ASSERT_TRUE(result.ok) << result.errors[0];
  const ChannelParameters& channel = result.project.channel;
  EXPECT_TRUE(channel.enabled);
  EXPECT_DOUBLE_EQ(channel.timing_offset_samples, 0.0);
  EXPECT_TRUE(channel.edge_smear.enabled);
  EXPECT_DOUBLE_EQ(channel.edge_smear.fraction, 0.15);
  EXPECT_DOUBLE_EQ(channel.edge_smear.tau_samples, 4.0);
  EXPECT_TRUE(channel.fm_beat.enabled);
  EXPECT_DOUBLE_EQ(channel.fm_beat.carrier_black_hz, 7.08e6);
  EXPECT_DOUBLE_EQ(channel.fm_beat.level_db, -20.0);
}

TEST(ChannelStageTest, ParserDefaultsFmBeatCarrier) {
  const ParseResult result =
      ParseWithChannel("channel:\n  fm_beat: { level_db: -30 }\n");
  ASSERT_TRUE(result.ok);
  EXPECT_DOUBLE_EQ(result.project.channel.fm_beat.carrier_black_hz, 7.088e6);
}

TEST(ChannelStageTest, ParserRequiresEdgeSmearAndFmBeatFields) {
  const struct {
    const char* block;
    const char* error;
  } cases[] = {
      {"channel:\n  edge_smear: { fraction: 0.1 }\n",
       "channel.edge_smear is missing required field: 'tau_samples'."},
      {"channel:\n  edge_smear: { tau_samples: 2 }\n",
       "channel.edge_smear is missing required field: 'fraction'."},
      {"channel:\n  fm_beat: { carrier_black_hz: 7.1e6 }\n",
       "channel.fm_beat is missing required field: 'level_db'."},
      {"channel:\n  fm_beat: { level_db: loud }\n",
       "channel.fm_beat.level_db must be a number."},
      {"channel:\n  edge_smear: 0.1\n",
       "channel.edge_smear must be a map/object."},
  };
  for (const auto& test_case : cases) {
    const ParseResult result = ParseWithChannel(test_case.block);
    ASSERT_FALSE(result.ok) << test_case.block;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0], test_case.error);
  }
  EXPECT_FALSE(
      ParseWithChannel("channel:\n  fm_beat: { level_db: -20, phase: 1 }\n")
          .ok);
}

TEST(ChannelStageTest, ParserRejectsNonNumericOffset) {
  const ParseResult result =
      ParseWithChannel("channel:\n  timing_offset_samples: late\n");
  ASSERT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "channel.timing_offset_samples must be a number.");
}

// ---------------------------------------------------------------------------
// Validator
// ---------------------------------------------------------------------------

Project MakeChannelProject(double offset_samples) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = "/tmp/channel_test.cvbs";
  project.output.metadata_path = "/tmp/channel_test.meta";
  project.channel.enabled = true;
  project.channel.timing_offset_samples = offset_samples;

  Section section;
  section.name = "S";
  section.type = "progressive";
  section.source = "f.exr";
  section.duration_frames = 8;
  project.sections.push_back(section);
  return project;
}

TEST(ChannelStageTest, ValidatorAcceptsOffsetsWithinRange) {
  for (double offset : {-8.0, -0.3, 0.0, 2.5, 8.0}) {
    EXPECT_TRUE(
        ProjectValidator().Validate(MakeChannelProject(offset)).is_valid)
        << offset;
  }
}

TEST(ChannelStageTest, ValidatorRejectsOffsetsOutOfRange) {
  for (double offset : {-8.01, 9.0, std::numeric_limits<double>::quiet_NaN()}) {
    const ValidationResult result =
        ProjectValidator().Validate(MakeChannelProject(offset));
    ASSERT_FALSE(result.is_valid) << offset;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].rfind(
                  "Channel validation error: timing_offset_samples must be in "
                  "the range [-8.0, 8.0]",
                  0),
              0U)
        << result.errors[0];
  }
}

TEST(ChannelStageTest, ValidatorChecksEdgeSmearRanges) {
  Project project = MakeChannelProject(0.0);
  project.channel.edge_smear.enabled = true;
  for (const auto& [fraction, tau] :
       {std::pair{0.0, 1.0}, std::pair{0.5, 100.0}, std::pair{0.2, 0.01}}) {
    project.channel.edge_smear.fraction = fraction;
    project.channel.edge_smear.tau_samples = tau;
    EXPECT_TRUE(ProjectValidator().Validate(project).is_valid)
        << fraction << " " << tau;
  }
  const struct {
    double fraction;
    double tau;
    const char* error;
  } cases[] = {
      {0.51, 2.0,
       "Channel validation error: edge_smear.fraction must be in the range "
       "[0.0, 0.5]"},
      {-0.1, 2.0,
       "Channel validation error: edge_smear.fraction must be in the range "
       "[0.0, 0.5]"},
      {0.1, 0.0,
       "Channel validation error: edge_smear.tau_samples must be in the range "
       "(0.0, 100.0]"},
      {0.1, 100.5,
       "Channel validation error: edge_smear.tau_samples must be in the range "
       "(0.0, 100.0]"},
  };
  for (const auto& test_case : cases) {
    project.channel.edge_smear.fraction = test_case.fraction;
    project.channel.edge_smear.tau_samples = test_case.tau;
    const ValidationResult result = ProjectValidator().Validate(project);
    ASSERT_FALSE(result.is_valid);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].rfind(test_case.error, 0), 0U)
        << result.errors[0];
  }
}

TEST(ChannelStageTest, ValidatorChecksFmBeatRangesAndStandard) {
  Project project = MakeChannelProject(0.0);
  project.channel.fm_beat.enabled = true;
  project.channel.fm_beat.level_db = -20.0;
  EXPECT_TRUE(ProjectValidator().Validate(project).is_valid);

  const struct {
    double carrier;
    double level;
    const char* error;
  } cases[] = {
      {6.8e6, -20.0,
       "Channel validation error: fm_beat.carrier_black_hz must be in the "
       "range [6900000, 7300000]"},
      {7.35e6, -20.0,
       "Channel validation error: fm_beat.carrier_black_hz must be in the "
       "range [6900000, 7300000]"},
      {7.088e6, 1.0,
       "Channel validation error: fm_beat.level_db must be in the range "
       "[-60.0, 0.0]"},
      {7.088e6, -61.0,
       "Channel validation error: fm_beat.level_db must be in the range "
       "[-60.0, 0.0]"},
  };
  for (const auto& test_case : cases) {
    project.channel.fm_beat.carrier_black_hz = test_case.carrier;
    project.channel.fm_beat.level_db = test_case.level;
    const ValidationResult result = ProjectValidator().Validate(project);
    ASSERT_FALSE(result.is_valid);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].rfind(test_case.error, 0), 0U)
        << result.errors[0];
  }

  project.channel.fm_beat.carrier_black_hz = 7.088e6;
  project.channel.fm_beat.level_db = -20.0;
  for (Standard standard :
       {Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
    project.cvbs_presets.video_standard_preset = standard;
    if (standard == Standard::kSecam) {
      project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_UNLOCKED";
    }
    const ValidationResult result = ProjectValidator().Validate(project);
    ASSERT_FALSE(result.is_valid) << StandardToString(standard);
    bool found = false;
    for (const std::string& error : result.errors) {
      found = found || error ==
                           "Channel validation error: fm_beat is supported for "
                           "625-line PAL only; the beat model is fitted to PAL "
                           "LaserDisc pressings.";
    }
    EXPECT_TRUE(found) << StandardToString(standard);
  }
}

// ---------------------------------------------------------------------------
// Stage
// ---------------------------------------------------------------------------

std::size_t PalSamplesPerFrame() {
  return SamplesPerFrameForEncodingPreset(Standard::kPal, "CVBS_U10_4FSC");
}

// A frame pattern that changes on every sample and never leaves the legal
// range, so any shift is visible and no clamp applies.
std::vector<SampleFixed> PatternFrames(std::size_t frames, SampleFixed bias) {
  const std::size_t spf = PalSamplesPerFrame();
  std::vector<SampleFixed> buffer(frames * spf);
  for (std::size_t n = 0; n < buffer.size(); ++n) {
    buffer[n] = MillivoltsToSampleFixed(
                    static_cast<double>((n * 7919U) % 997U) * 0.5 - 200.0) +
                bias;
  }
  return buffer;
}

TEST(ChannelStageTest, AbsentOrZeroOffsetLeavesBuffersBitIdentical) {
  Project project = MakeChannelProject(0.0);
  // Out-of-range samples prove the zero-offset path does not even clamp.
  std::vector<SampleFixed> y = PatternFrames(1, 0);
  y[10] = MillivoltsToSampleFixed(5000.0);
  std::vector<SampleFixed> c = PatternFrames(1, 3);
  const std::vector<SampleFixed> y_before = y;
  const std::vector<SampleFixed> c_before = c;

  const ChannelStage stage;
  stage.ApplyChannel(project, {}, 0U, 1U, &y, &c);
  EXPECT_EQ(y, y_before);
  EXPECT_EQ(c, c_before);

  project.channel.enabled = false;
  project.channel.timing_offset_samples = 1.5;
  stage.ApplyChannel(project, {}, 0U, 1U, &y, &c);
  EXPECT_EQ(y, y_before);
  EXPECT_EQ(c, c_before);
}

TEST(ChannelStageTest, IntegerOffsetShiftsEachFrameCircularlyInYAndC) {
  const Project project = MakeChannelProject(3.0);
  const std::size_t spf = PalSamplesPerFrame();
  std::vector<SampleFixed> y = PatternFrames(2, 0);
  std::vector<SampleFixed> c = PatternFrames(2, 11);
  const std::vector<SampleFixed> y_before = y;
  const std::vector<SampleFixed> c_before = c;

  ChannelStage().ApplyChannel(project, {}, 0U, 2U, &y, &c);

  for (std::size_t frame = 0; frame < 2U; ++frame) {
    const std::size_t start = frame * spf;
    for (std::size_t s : {std::size_t{0}, std::size_t{2}, std::size_t{3},
                          std::size_t{1000}, spf - 1U}) {
      const std::size_t source = start + ((s + spf - 3U) % spf);
      EXPECT_EQ(y[start + s], y_before[source]) << frame << "/" << s;
      EXPECT_EQ(c[start + s], c_before[source]) << frame << "/" << s;
    }
  }
}

TEST(ChannelStageTest, FractionalOffsetClampsLumaButNotChroma) {
  const Project project = MakeChannelProject(0.5);
  const SampleClampBounds bounds = SampleClampBoundsFor(Standard::kPal);
  std::vector<SampleFixed> y(PalSamplesPerFrame(), bounds.max);
  // A full-scale step overshoots on both sides of the edge.
  for (std::size_t s = 0; s < 1000U; ++s) {
    y[s] = bounds.min;
  }
  // Saturated chroma swings below the composite floor and must survive.
  std::vector<SampleFixed> c(y.size(), MillivoltsToSampleFixed(-440.0));

  ChannelStage().ApplyChannel(project, {}, 0U, 1U, &y, &c);
  for (SampleFixed sample : y) {
    ASSERT_GE(sample, bounds.min);
    ASSERT_LE(sample, bounds.max);
  }
  // The edge moved half a sample: the old edge sample now sits half way.
  const double mid = SampleFixedToMillivolts(y[1000]);
  const double half_way = 0.5 * (SampleFixedToMillivolts(bounds.min) +
                                 SampleFixedToMillivolts(bounds.max));
  EXPECT_NEAR(mid, half_way, 1.0);
  for (SampleFixed sample : c) {
    ASSERT_NEAR(SampleFixedToMillivolts(sample), -440.0, 1e-3);
  }
}

// A flat 50 IRE PAL colour: Y at 350 mV and a constant subcarrier on every
// sample.
void FlatColourFrame(double chroma_mv, std::vector<SampleFixed>* y,
                     std::vector<SampleFixed>* c) {
  const std::size_t spf = PalSamplesPerFrame();
  y->assign(spf, MillivoltsToSampleFixed(350.0));
  c->resize(spf);
  for (std::size_t n = 0; n < spf; ++n) {
    (*c)[n] = MillivoltsToSampleFixed(
        chroma_mv *
        std::cos((0.5 * kPiForTests * static_cast<double>(n)) + 0.3));
  }
}

TEST(ChannelStageTest, FmBeatLevelIsRelativeToChromaAt50Ire) {
  constexpr double kChromaMv = 150.0;
  for (double level_db : {-40.0, -20.0, -6.0}) {
    Project project = MakeChannelProject(0.0);
    project.channel.fm_beat.enabled = true;
    project.channel.fm_beat.level_db = level_db;
    std::vector<SampleFixed> y;
    std::vector<SampleFixed> c;
    FlatColourFrame(kChromaMv, &y, &c);
    const std::vector<SampleFixed> y_before = y;
    const std::vector<SampleFixed> c_before = c;

    ChannelStage().ApplyChannel(project, {}, 0U, 1U, &y, &c);
    EXPECT_EQ(c, c_before);

    // Mean beat power over the middle of every picture line, clear of the
    // window edges.
    const std::vector<PictureLineWindow> windows =
        BuildPictureLineWindows(project, Standard::kPal);
    ASSERT_FALSE(windows.empty());
    double power = 0.0;
    std::size_t samples = 0;
    for (const PictureLineWindow& window : windows) {
      for (std::size_t s = window.start + 200U;
           s < window.start + window.count - 200U; ++s) {
        const double beat = SampleFixedToMillivolts(y[s]) -
                            SampleFixedToMillivolts(y_before[s]);
        power += beat * beat;
        ++samples;
      }
    }
    const double amplitude =
        std::sqrt(2.0 * power / static_cast<double>(samples));
    EXPECT_NEAR(20.0 * std::log10(amplitude / kChromaMv), level_db, 0.5)
        << level_db;

    // Outside the picture windows Y is untouched.
    std::vector<bool> in_window(y.size(), false);
    for (const PictureLineWindow& window : windows) {
      for (std::size_t s = window.start; s < window.start + window.count; ++s) {
        in_window[s] = true;
      }
    }
    for (std::size_t s = 0; s < y.size(); ++s) {
      if (!in_window[s]) {
        ASSERT_EQ(y[s], y_before[s]) << s;
      }
    }
  }
}

TEST(ChannelStageTest, FmBeatWithoutPictureChromaIsBitIdentical) {
  Project project = MakeChannelProject(0.0);
  project.channel.fm_beat.enabled = true;
  project.channel.fm_beat.level_db = 0.0;
  std::vector<SampleFixed> y = PatternFrames(1, 0);
  // Chroma outside the picture windows (a burst, say) carries no beat.
  std::vector<SampleFixed> c(y.size(), 0);
  const std::vector<PictureLineWindow> windows =
      BuildPictureLineWindows(project, Standard::kPal);
  for (std::size_t s = 0; s < windows.front().start; ++s) {
    c[s] = MillivoltsToSampleFixed((s % 4U) < 2U ? 150.0 : -150.0);
  }
  const std::vector<SampleFixed> y_before = y;
  const std::vector<SampleFixed> c_before = c;
  ChannelStage().ApplyChannel(project, {}, 0U, 1U, &y, &c);
  EXPECT_EQ(y, y_before);
  EXPECT_EQ(c, c_before);
}

TEST(ChannelStageTest, EdgeSmearIsIndependentOfFrameOrder) {
  Project project = MakeChannelProject(0.0);
  project.channel.edge_smear.enabled = true;
  project.channel.edge_smear.fraction = 0.25;
  project.channel.edge_smear.tau_samples = 7.0;
  const std::size_t spf = PalSamplesPerFrame();
  const std::vector<SampleFixed> a = PatternFrames(1, 0);
  const std::vector<SampleFixed> b = PatternFrames(1, 12345);

  std::vector<SampleFixed> forward = a;
  forward.insert(forward.end(), b.begin(), b.end());
  std::vector<SampleFixed> reversed = b;
  reversed.insert(reversed.end(), a.begin(), a.end());
  std::vector<SampleFixed> forward_c = forward;
  std::vector<SampleFixed> reversed_c = reversed;

  const ChannelStage stage;
  stage.ApplyChannel(project, {}, 0U, 2U, &forward, &forward_c);
  stage.ApplyChannel(project, {}, 0U, 2U, &reversed, &reversed_c);
  for (std::size_t s = 0; s < spf; ++s) {
    ASSERT_EQ(forward[s], reversed[spf + s]) << s;
    ASSERT_EQ(forward[spf + s], reversed[s]) << s;
    ASSERT_EQ(forward_c[s], reversed_c[spf + s]) << s;
  }
  EXPECT_NE(std::vector<SampleFixed>(forward.begin(), forward.begin() + spf),
            a);
}

TEST(ChannelStageTest, StepsRunBeatThenSmearThenOffset) {
  Project project = MakeChannelProject(2.0);
  project.channel.edge_smear.enabled = true;
  project.channel.edge_smear.fraction = 0.2;
  project.channel.edge_smear.tau_samples = 3.0;
  project.channel.fm_beat.enabled = true;
  project.channel.fm_beat.level_db = -10.0;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  FlatColourFrame(150.0, &y, &c);
  std::vector<SampleFixed> expected_y = y;
  std::vector<SampleFixed> expected_c = c;

  ChannelStage().ApplyChannel(project, {}, 0U, 1U, &y, &c);

  // The same three steps, one at a time.
  Project step = project;
  step.channel.edge_smear.enabled = false;
  step.channel.timing_offset_samples = 0.0;
  ChannelStage().ApplyChannel(step, {}, 0U, 1U, &expected_y, &expected_c);
  step = project;
  step.channel.fm_beat.enabled = false;
  step.channel.timing_offset_samples = 0.0;
  ChannelStage().ApplyChannel(step, {}, 0U, 1U, &expected_y, &expected_c);
  step = project;
  step.channel.fm_beat.enabled = false;
  step.channel.edge_smear.enabled = false;
  ChannelStage().ApplyChannel(step, {}, 0U, 1U, &expected_y, &expected_c);
  EXPECT_EQ(y, expected_y);
  EXPECT_EQ(c, expected_c);
}

}  // namespace
}  // namespace videosynth
