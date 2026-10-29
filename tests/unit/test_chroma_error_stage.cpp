/*
 * File:        test_chroma_error_stage.cpp
 * Module:      chroma_error_tests
 * Purpose:     Validates the `chroma_error:` block (parsing and validation),
 *              the Hilbert transformer and the ChromaErrorStage gain, phase
 *              and per-line jitter on picture chroma.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "videosynth/chroma_error_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"
#include "videosynth/sample_clamp.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

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

ParseResult ParseWithChromaError(const std::string& block) {
  return YamlProjectParser().ParseString(kYamlPrefix + block + kYamlSections);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

TEST(ChromaErrorStageTest, ParserLeavesChromaErrorDisabledWhenAbsent) {
  const ParseResult result = ParseWithChromaError("");
  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.project.chroma_error.enabled);
}

TEST(ChromaErrorStageTest, ParserReadsFullBlock) {
  const ParseResult result = ParseWithChromaError(
      "chroma_error:\n"
      "  gain: 1.05\n"
      "  phase_deg: 2.0\n"
      "  line_jitter: { phase_deg: 0.5, gain: 0.01, seed: 5 }\n");
  ASSERT_TRUE(result.ok) << result.errors.front();
  const ChromaErrorParameters& chroma_error = result.project.chroma_error;
  EXPECT_TRUE(chroma_error.enabled);
  EXPECT_DOUBLE_EQ(chroma_error.gain, 1.05);
  EXPECT_DOUBLE_EQ(chroma_error.phase_deg, 2.0);
  EXPECT_TRUE(chroma_error.line_jitter.enabled);
  EXPECT_DOUBLE_EQ(chroma_error.line_jitter.phase_deg, 0.5);
  EXPECT_DOUBLE_EQ(chroma_error.line_jitter.gain, 0.01);
  EXPECT_TRUE(chroma_error.line_jitter.seed_specified);
  EXPECT_EQ(chroma_error.line_jitter.seed, 5);
}

TEST(ChromaErrorStageTest, ParserDefaultsOmittedKeys) {
  const ParseResult result =
      ParseWithChromaError("chroma_error:\n  phase_deg: -4.0\n");
  ASSERT_TRUE(result.ok);
  EXPECT_DOUBLE_EQ(result.project.chroma_error.gain, 1.0);
  EXPECT_FALSE(result.project.chroma_error.line_jitter.enabled);
}

TEST(ChromaErrorStageTest, ParserRejectsEmptyBlocks) {
  ParseResult result = ParseWithChromaError("chroma_error: {}\n");
  ASSERT_FALSE(result.ok);
  EXPECT_EQ(result.errors[0],
            "chroma_error block must set at least one of: gain, phase_deg, "
            "line_jitter.");

  result = ParseWithChromaError("chroma_error:\n  line_jitter: { seed: 3 }\n");
  ASSERT_FALSE(result.ok);
  EXPECT_EQ(result.errors[0],
            "chroma_error.line_jitter must set at least one of: phase_deg, "
            "gain.");
}

TEST(ChromaErrorStageTest, ParserRejectsUnknownKeysAndNonNumbers) {
  EXPECT_FALSE(ParseWithChromaError("chroma_error:\n  hue: 1.0\n").ok);
  EXPECT_FALSE(ParseWithChromaError(
                   "chroma_error:\n  line_jitter: { phase_deg: 1, rate: 2 }\n")
                   .ok);

  const ParseResult result =
      ParseWithChromaError("chroma_error:\n  gain: lots\n");
  ASSERT_FALSE(result.ok);
  EXPECT_EQ(result.errors[0], "chroma_error.gain must be a number.");
}

// ---------------------------------------------------------------------------
// Validator
// ---------------------------------------------------------------------------

Project MakeChromaProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset =
      GeneratedSignalStatePreset(standard);
  project.output.video_path = "/tmp/chroma_error_test.cvbs";
  project.output.metadata_path = "/tmp/chroma_error_test.meta";
  project.chroma_error.enabled = true;

  Section section;
  section.name = "S";
  section.type = "progressive";
  section.source = "f.exr";
  section.duration_frames = 8;
  project.sections.push_back(section);
  return project;
}

TEST(ChromaErrorStageTest, ValidatorAcceptsRangeLimits) {
  Project project = MakeChromaProject(Standard::kNtsc);
  project.chroma_error.gain = 1.5;
  project.chroma_error.phase_deg = -180.0;
  project.chroma_error.line_jitter.enabled = true;
  project.chroma_error.line_jitter.phase_deg = 10.0;
  project.chroma_error.line_jitter.gain = 0.1;
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors.front());
}

TEST(ChromaErrorStageTest, ValidatorRejectsOutOfRangeValues) {
  struct Case {
    double gain;
    double phase_deg;
    double jitter_phase_deg;
    double jitter_gain;
    std::string message_prefix;
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Case cases[] = {
      {0.49, 0.0, 0.0, 0.0, "gain must be in the range [0.5, 1.5]"},
      {nan, 0.0, 0.0, 0.0, "gain must be in the range [0.5, 1.5]"},
      {1.0, 180.5, 0.0, 0.0, "phase_deg must be in the range [-180.0, 180.0]"},
      {1.0, 0.0, -0.1, 0.0,
       "line_jitter.phase_deg must be in the range [0.0, 10.0]"},
      {1.0, 0.0, 0.0, 0.11, "line_jitter.gain must be in the range [0.0, 0.1]"},
  };
  for (const Case& c : cases) {
    Project project = MakeChromaProject(Standard::kPal);
    project.chroma_error.gain = c.gain;
    project.chroma_error.phase_deg = c.phase_deg;
    project.chroma_error.line_jitter.enabled = true;
    project.chroma_error.line_jitter.phase_deg = c.jitter_phase_deg;
    project.chroma_error.line_jitter.gain = c.jitter_gain;
    const ValidationResult result = ProjectValidator().Validate(project);
    ASSERT_FALSE(result.is_valid) << c.message_prefix;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].rfind(
                  "Chroma error validation error: " + c.message_prefix, 0),
              0U)
        << result.errors[0];
  }
}

TEST(ChromaErrorStageTest, ValidatorRejectsSecam) {
  Project project = MakeChromaProject(Standard::kSecam);
  project.chroma_error.gain = 1.1;
  const ValidationResult result = ProjectValidator().Validate(project);
  ASSERT_FALSE(result.is_valid);
  bool found = false;
  for (const std::string& error : result.errors) {
    found = found || error.rfind(
                         "Chroma error validation error: chroma_error is not "
                         "supported for SECAM",
                         0) == 0U;
  }
  EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// Hilbert transformer
// ---------------------------------------------------------------------------

TEST(ChromaErrorStageTest, HilbertKernelIsAntisymmetricWithZeroEvenTaps) {
  const std::vector<double> kernel = DesignChromaHilbertKernel();
  constexpr int kHalf = kChromaHilbertHalfTaps;
  ASSERT_EQ(kernel.size(), 2U * kHalf + 1U);
  const auto half = static_cast<std::size_t>(kHalf);
  for (std::size_t k = 0; k <= half; ++k) {
    const double positive = kernel[half + k];
    const double negative = kernel[half - k];
    EXPECT_DOUBLE_EQ(positive, -negative) << k;
    if (k % 2 == 0) {
      EXPECT_EQ(positive, 0.0) << k;
    }
  }
}

TEST(ChromaErrorStageTest, HilbertResponseIsMinusJAcrossChromaBand) {
  const std::vector<double> kernel = DesignChromaHilbertKernel();
  constexpr int kHalf = kChromaHilbertHalfTaps;
  for (Standard standard : {Standard::kPal, Standard::kNtsc}) {
    const double fs = GetTimingConstants(standard).sample_rate_4fsc_hz;
    const double low_hz = 2.0e6;
    const double high_hz = (fs / 2.0) - 2.0e6;
    for (double f = low_hz; f <= high_hz; f += 0.05e6) {
      std::complex<double> response = 0.0;
      for (std::size_t i = 0; i < kernel.size(); ++i) {
        const double k = static_cast<double>(i) - kHalf;
        response += kernel[i] * std::polar(1.0, -2.0 * kPi * f / fs * k);
      }
      EXPECT_LT(std::abs(response - std::complex<double>(0.0, -1.0)), 1e-4)
          << StandardToString(standard) << " " << f / 1e6 << " MHz";
    }
  }
}

// ---------------------------------------------------------------------------
// Stage on synthetic chroma frames
// ---------------------------------------------------------------------------

// A line-relative sample range [start, end).
struct SampleRange {
  int start = 0;
  int end = 0;
};

// Burst gate as the generation stage places it (ITU-R BT.470-6 Table 2 item
// 2.14g/h).
SampleRange BurstRange(Standard standard, double fs) {
  double start_s = 5.6e-6;
  double end_s = 8.0e-6;
  if (standard == Standard::kNtsc) {
    start_s = 5.3e-6;
    end_s = 7.97e-6;
  } else if (standard == Standard::kPalM) {
    start_s = 5.8e-6;
    end_s = 8.32e-6;
  }
  return SampleRange{static_cast<int>(std::lround(fs * start_s)),
                     static_cast<int>(std::lround(fs * end_s))};
}

// The frame line layout a synthetic chroma frame is built on.
struct ChromaLayout {
  Standard standard = Standard::kUnknown;
  std::size_t samples_per_frame = 0;
  std::vector<int> line_samples;
  std::vector<int> line_offsets;
  std::vector<LineTimingPrimitive> lines;
  ActiveRasterGeometry geometry;
  SampleRange burst;
};

ChromaLayout MakeLayout(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  ChromaLayout layout;
  layout.standard = standard;
  layout.samples_per_frame =
      static_cast<std::size_t>(SamplesPerFrame4fsc(standard));
  layout.line_samples = BuildLineSampleCounts(standard, timing.lines_per_frame,
                                              timing.samples_per_line_4fsc);
  layout.line_offsets = BuildLineSampleOffsets(layout.line_samples);
  layout.lines = BuildFrameTimingPrimitives(standard);
  layout.geometry =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  layout.burst = BurstRange(standard, timing.sample_rate_4fsc_hz);
  return layout;
}

SampleRange ActiveRange(const ChromaLayout& layout) {
  return SampleRange{layout.geometry.active_window_start_samples,
                     layout.geometry.active_window_end_samples};
}

bool IsPictureLine(const ChromaLayout& layout, int line_1based,
                   const std::set<int>& vits_lines) {
  const LineTimingPrimitive& line =
      layout.lines[static_cast<std::size_t>(line_1based - 1)];
  return line.sync_pulse_kind == SyncPulseKind::kHorizontal &&
         line.content_kind == LineContentKind::kActivePicture &&
         ActiveFrameLineIndex(layout.geometry, line_1based) >= 0 &&
         vits_lines.count(line_1based) == 0U;
}

constexpr double kBurstMv = 150.0;
constexpr double kChromaMv = 200.0;

double BurstPhase(int line_1based) { return 1.1 * line_1based; }
double ChromaPhase(int line_1based) { return 0.3 + (0.7 * line_1based); }

// Chroma frames on the real line layout: a burst on every line, a flat
// subcarrier (one hue per line) across the active window of every line, VBI
// and VITS lines included, and a small pattern in the blanking between, so
// every sample the stage must leave alone carries a value.
std::vector<SampleFixed> MakeChromaFrames(const ChromaLayout& layout,
                                          std::size_t frames) {
  std::vector<SampleFixed> buffer(frames * layout.samples_per_frame);
  const SampleRange active = ActiveRange(layout);
  for (std::size_t f = 0; f < frames; ++f) {
    SampleFixed* frame = buffer.data() + (f * layout.samples_per_frame);
    for (std::size_t li = 0; li < layout.line_samples.size(); ++li) {
      const int line_1based = static_cast<int>(li) + 1;
      const int base = layout.line_offsets[li];
      for (int s = 0; s < layout.line_samples[li]; ++s) {
        // fsc = fs / 4 on every 4fsc standard.
        const double carrier = kPi / 2.0 * (base + s);
        double mv = 2.0 * std::sin(0.37 * (base + s));
        if (s >= layout.burst.start && s < layout.burst.end) {
          mv = kBurstMv * std::cos(carrier + BurstPhase(line_1based));
        } else if (s >= active.start && s < active.end) {
          mv = kChromaMv * std::cos(carrier + ChromaPhase(line_1based));
        }
        frame[base + s] = MillivoltsToSampleFixed(mv);
      }
    }
  }
  return buffer;
}

// Complex subcarrier phasor of samples [start, end) of one line, relative to
// the frame-wide carrier; an even sample count cancels the 2fsc image.
std::complex<double> Phasor(const SampleFixed* frame,
                            const ChromaLayout& layout, int line_1based,
                            int start, int end) {
  const int base =
      layout.line_offsets[static_cast<std::size_t>(line_1based - 1)];
  std::complex<double> sum = 0.0;
  for (int s = start; s < end; ++s) {
    sum += SampleFixedToMillivolts(frame[base + s]) *
           std::polar(1.0, -kPi / 2.0 * (base + s));
  }
  return sum * (2.0 / static_cast<double>(end - start));
}

// Hue and saturation of one line's picture chroma, demodulated against the
// line's burst: (phase of picture - phase of burst, picture amplitude).
struct Demodulated {
  double phase_rad = 0.0;
  double amplitude_mv = 0.0;
};

// Margin, in samples, kept clear of the active window edges, where the
// window-local Hilbert transform is truncated.
constexpr int kEdgeMargin = 2 * kChromaHilbertHalfTaps;

Demodulated DemodulateAgainstBurst(const SampleFixed* frame,
                                   const ChromaLayout& layout,
                                   int line_1based) {
  const SampleRange active = ActiveRange(layout);
  int start = active.start + kEdgeMargin;
  int end = active.end - kEdgeMargin;
  end -= (end - start) % 4;
  int burst_start = layout.burst.start + 4;
  int burst_end = layout.burst.end - 4;
  burst_end -= (burst_end - burst_start) % 2;
  const std::complex<double> picture =
      Phasor(frame, layout, line_1based, start, end);
  const std::complex<double> burst =
      Phasor(frame, layout, line_1based, burst_start, burst_end);
  return Demodulated{std::arg(picture / burst), std::abs(picture)};
}

double WrapRad(double value) { return std::remainder(value, 2.0 * kPi); }

std::vector<IGenerationStage::FrameScheduleItem> MakeSchedule(
    const Project& project, std::size_t frames) {
  std::vector<IGenerationStage::FrameScheduleItem> schedule(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    schedule[i].section = &project.sections[0];
    schedule[i].section_frame_index = static_cast<int>(i);
    schedule[i].source_frame_index = static_cast<int>(i);
  }
  return schedule;
}

TEST(ChromaErrorStageTest, PhaseAndGainShiftDecodedHueAndSaturation) {
  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    for (const auto& [gain, phase_deg] :
         {std::pair{1.05, 2.0}, std::pair{0.8, -30.0}, std::pair{1.3, 135.0}}) {
      const ChromaLayout layout = MakeLayout(standard);
      Project project = MakeChromaProject(standard);
      project.chroma_error.gain = gain;
      project.chroma_error.phase_deg = phase_deg;
      const auto schedule = MakeSchedule(project, 1U);

      const std::vector<SampleFixed> input = MakeChromaFrames(layout, 1U);
      std::vector<SampleFixed> output = input;
      ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &output);

      int picture_lines = 0;
      for (int line = 1; line <= static_cast<int>(layout.lines.size());
           ++line) {
        if (!IsPictureLine(layout, line, {})) {
          continue;
        }
        ++picture_lines;
        const Demodulated before =
            DemodulateAgainstBurst(input.data(), layout, line);
        const Demodulated after =
            DemodulateAgainstBurst(output.data(), layout, line);
        const double hue_shift_deg =
            WrapRad(after.phase_rad - before.phase_rad) * 180.0 / kPi;
        ASSERT_NEAR(hue_shift_deg, phase_deg, 0.2)
            << StandardToString(standard) << " line " << line;
        ASSERT_NEAR(after.amplitude_mv / before.amplitude_mv, gain,
                    0.005 * gain)
            << StandardToString(standard) << " line " << line;
      }
      EXPECT_EQ(picture_lines, 2 * layout.geometry.active_lines_per_field)
          << StandardToString(standard);
    }
  }
}

TEST(ChromaErrorStageTest, BurstVitsAndBlankingAreBitIdentical) {
  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    const ChromaLayout layout = MakeLayout(standard);
    Project project = MakeChromaProject(standard);
    project.chroma_error.gain = 1.2;
    project.chroma_error.phase_deg = 40.0;
    project.chroma_error.line_jitter.enabled = true;
    project.chroma_error.line_jitter.phase_deg = 3.0;
    project.chroma_error.line_jitter.seed = 9;
    project.chroma_error.line_jitter.seed_specified = true;
    // A custom-placed VITS on an active picture line keeps its chroma.
    const int vits_line = layout.geometry.first_active_line_field1 + 40;
    project.line_injections.placement = VitsPlacement::kCustom;
    project.line_injections.vits.push_back(
        VitsInjection{.vits_type = "multiburst", .target_lines = {vits_line}});
    const std::set<int> vits_lines = {vits_line};
    const auto schedule = MakeSchedule(project, 2U);

    const std::vector<SampleFixed> input = MakeChromaFrames(layout, 2U);
    std::vector<SampleFixed> output = input;
    ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 2U, &output);

    const SampleRange active = ActiveRange(layout);
    std::size_t changed_picture_samples = 0;
    for (std::size_t f = 0; f < 2U; ++f) {
      for (std::size_t li = 0; li < layout.line_samples.size(); ++li) {
        const int line_1based = static_cast<int>(li) + 1;
        const bool picture = IsPictureLine(layout, line_1based, vits_lines);
        const std::size_t base =
            (f * layout.samples_per_frame) +
            static_cast<std::size_t>(layout.line_offsets[li]);
        for (int s = 0; s < layout.line_samples[li]; ++s) {
          const std::size_t idx = base + static_cast<std::size_t>(s);
          if (picture && s >= active.start && s < active.end) {
            changed_picture_samples += output[idx] != input[idx] ? 1U : 0U;
            continue;
          }
          ASSERT_EQ(output[idx], input[idx])
              << StandardToString(standard) << " frame " << f << " line "
              << line_1based << " sample " << s;
        }
      }
    }
    EXPECT_GT(changed_picture_samples, 0U);
  }
}

TEST(ChromaErrorStageTest, LineJitterSigmaMatchesConfiguration) {
  constexpr double kPhaseSigmaDeg = 2.0;
  constexpr double kGainSigma = 0.05;
  constexpr double kGain = 1.1;
  constexpr double kPhaseDeg = 5.0;
  constexpr std::size_t kFrames = 4;

  const ChromaLayout layout = MakeLayout(Standard::kPal);
  Project project = MakeChromaProject(Standard::kPal);
  project.chroma_error.gain = kGain;
  project.chroma_error.phase_deg = kPhaseDeg;
  project.chroma_error.line_jitter.enabled = true;
  project.chroma_error.line_jitter.phase_deg = kPhaseSigmaDeg;
  project.chroma_error.line_jitter.gain = kGainSigma;
  project.chroma_error.line_jitter.seed = 5;
  project.chroma_error.line_jitter.seed_specified = true;
  const auto schedule = MakeSchedule(project, kFrames);

  const std::vector<SampleFixed> input = MakeChromaFrames(layout, kFrames);
  std::vector<SampleFixed> output = input;
  ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, kFrames,
                                        &output);

  std::vector<double> phases_deg;
  std::vector<double> gains;
  for (std::size_t f = 0; f < kFrames; ++f) {
    const std::size_t frame_start = f * layout.samples_per_frame;
    for (int line = 1; line <= static_cast<int>(layout.lines.size()); ++line) {
      if (!IsPictureLine(layout, line, {})) {
        continue;
      }
      const Demodulated before =
          DemodulateAgainstBurst(input.data() + frame_start, layout, line);
      const Demodulated after =
          DemodulateAgainstBurst(output.data() + frame_start, layout, line);
      phases_deg.push_back(WrapRad(after.phase_rad - before.phase_rad) * 180.0 /
                           kPi);
      gains.push_back(after.amplitude_mv / before.amplitude_mv);
    }
  }

  auto MeanAndSigma = [](const std::vector<double>& values) {
    double mean = 0.0;
    for (double v : values) {
      mean += v;
    }
    mean /= static_cast<double>(values.size());
    double variance = 0.0;
    for (double v : values) {
      variance += (v - mean) * (v - mean);
    }
    variance /= static_cast<double>(values.size() - 1U);
    return std::pair{mean, std::sqrt(variance)};
  };
  const auto [phase_mean, phase_sigma] = MeanAndSigma(phases_deg);
  const auto [gain_mean, gain_sigma] = MeanAndSigma(gains);
  EXPECT_NEAR(phase_sigma, kPhaseSigmaDeg, 0.1 * kPhaseSigmaDeg);
  EXPECT_NEAR(gain_sigma, kGainSigma, 0.1 * kGainSigma);
  EXPECT_NEAR(phase_mean, kPhaseDeg, 0.2);
  EXPECT_NEAR(gain_mean, kGain, 0.005);
}

TEST(ChromaErrorStageTest, JitterIsKeyedBySeedAndOutputFrame) {
  const ChromaLayout layout = MakeLayout(Standard::kNtsc);
  Project project = MakeChromaProject(Standard::kNtsc);
  project.chroma_error.line_jitter.enabled = true;
  project.chroma_error.line_jitter.phase_deg = 1.0;
  project.chroma_error.line_jitter.gain = 0.02;
  constexpr std::size_t kFrames = 3;
  const auto schedule = MakeSchedule(project, kFrames);
  const std::vector<SampleFixed> input = MakeChromaFrames(layout, kFrames);
  const std::size_t spf = layout.samples_per_frame;

  auto Run = [&](const ChromaErrorStage& stage) {
    std::vector<SampleFixed> output = input;
    stage.ApplyChromaError(project, schedule, 0U, kFrames, &output);
    return output;
  };

  // Unseeded: the run base seed decides.
  EXPECT_EQ(Run(ChromaErrorStage(11U)), Run(ChromaErrorStage(11U)));
  EXPECT_NE(Run(ChromaErrorStage(11U)), Run(ChromaErrorStage(12U)));

  // Seeded: the run base seed no longer matters.
  project.chroma_error.line_jitter.seed = 5;
  project.chroma_error.line_jitter.seed_specified = true;
  const std::vector<SampleFixed> batch = Run(ChromaErrorStage(11U));
  EXPECT_EQ(batch, Run(ChromaErrorStage(12U)));

  // One frame at a time, as the synthesis pool runs it, matches the batch.
  for (std::size_t f = 0; f < kFrames; ++f) {
    const auto first = input.begin() + static_cast<std::ptrdiff_t>(f * spf);
    std::vector<SampleFixed> single(first,
                                    first + static_cast<std::ptrdiff_t>(spf));
    ChromaErrorStage(13U).ApplyChromaError(project, schedule, f, 1U, &single);
    EXPECT_TRUE(
        std::equal(single.begin(), single.end(),
                   batch.begin() + static_cast<std::ptrdiff_t>(f * spf)))
        << f;
  }

  // Every frame draws its own jitter: identical input frames differ.
  EXPECT_FALSE(std::equal(batch.begin(),
                          batch.begin() + static_cast<std::ptrdiff_t>(spf),
                          batch.begin() + static_cast<std::ptrdiff_t>(spf)));
}

TEST(ChromaErrorStageTest, IdentityOrAbsentBlockLeavesBufferBitIdentical) {
  const ChromaLayout layout = MakeLayout(Standard::kPal);
  // Out-of-range samples prove the identity path does not even clamp.
  std::vector<SampleFixed> input = MakeChromaFrames(layout, 1U);
  for (std::size_t s = 0; s < input.size(); s += 101U) {
    input[s] = MillivoltsToSampleFixed(5000.0);
  }

  Project project = MakeChromaProject(Standard::kPal);
  const auto schedule = MakeSchedule(project, 1U);
  project.chroma_error.line_jitter.enabled = true;
  project.chroma_error.line_jitter.seed_specified = true;
  EXPECT_FALSE(ChromaErrorStage::IsActive(project.chroma_error));

  std::vector<SampleFixed> output = input;
  ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &output);
  EXPECT_EQ(output, input);

  project.chroma_error.enabled = false;
  project.chroma_error.gain = 1.4;
  EXPECT_FALSE(ChromaErrorStage::IsActive(project.chroma_error));
  ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &output);
  EXPECT_EQ(output, input);
}

TEST(ChromaErrorStageTest, AmplifiedChromaIsNotClampedToCompositeBounds) {
  // 100% red chroma already swings to about -440 mV; the composite bounds
  // apply to Y + C at output, not to C alone.
  const ChromaLayout layout = MakeLayout(Standard::kPal);
  Project project = MakeChromaProject(Standard::kPal);
  project.chroma_error.gain = 1.5;
  const auto schedule = MakeSchedule(project, 1U);

  const int line = layout.geometry.first_active_line_field1 + 10;
  const SampleRange active = ActiveRange(layout);
  std::vector<SampleFixed> buffer(layout.samples_per_frame, 0);
  const std::size_t start =
      static_cast<std::size_t>(
          layout.line_offsets[static_cast<std::size_t>(line - 1)]) +
      static_cast<std::size_t>(active.start);
  for (std::size_t s = 0;
       s < static_cast<std::size_t>(active.end - active.start); ++s) {
    buffer[start + s] = MillivoltsToSampleFixed(
        440.0 * std::cos(kPi / 2.0 * static_cast<double>(start + s)));
  }

  ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &buffer);
  const SampleClampBounds bounds = SampleClampBoundsFor(Standard::kPal);
  const auto [low, high] = std::minmax_element(buffer.begin(), buffer.end());
  EXPECT_LT(*low, bounds.min);
  EXPECT_NEAR(SampleFixedToMillivolts(*low), -660.0, 1.0);
  EXPECT_NEAR(SampleFixedToMillivolts(*high), 660.0, 1.0);
}

}  // namespace
}  // namespace videosynth
