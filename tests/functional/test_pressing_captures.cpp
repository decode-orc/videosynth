/*
 * File:        test_pressing_captures.cpp
 * Module:      channel_tests
 * Purpose:     End-to-end checks of the generated two-pressing stacking sets:
 *              captures of one pressing share their pressing impairments on
 *              matching picture numbers, and the pressings' clean renders
 *              differ by the configured sub-sample timing offset.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "fixture_paths.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/timing_constants.h"
#include "videosynth/timing_offset_filter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

const char* const kStandards[] = {"pal", "ntsc", "pal_m"};

std::filesystem::path OutputRoot() {
  return std::filesystem::temp_directory_path() /
         "videosynth_pressing_captures";
}

// Loads a generated project with its paths resolved like the CLI, sending
// "{output}/..." targets to the scratch output root.
bool LoadGenerated(const std::string& relative, Project* project) {
  const std::filesystem::path path =
      test_support::GeneratedProjectRoot() / relative;
  const ParseResult parsed = YamlProjectParser().ParseFile(path.string());
  if (!parsed.ok) {
    ADD_FAILURE() << relative << ": "
                  << (parsed.errors.empty() ? "parse failed"
                                            : parsed.errors[0]);
    return false;
  }
  AssetRootMap roots = DefaultAssetRoots();
  roots.roots["output"] = OutputRoot().string();
  *project =
      ResolveProjectPaths(parsed.project, roots, path.parent_path().string(),
                          /*anchor_unset=*/true);
  return true;
}

// Renders a project and returns its 16-bit sample codes, one vector per
// output frame.
std::vector<std::vector<std::uint16_t>> Render(const Project& project) {
  NullLogger logger;
  YamlProjectParser parser(&logger);
  ProgressiveFrameSourceProbe probe;
  ProjectValidator validator(&probe, &logger);
  GenerationStage generation(&logger);
  NoiseInjectionStage noise(&logger);
  DropoutInjectionStage dropouts(&logger);
  OutputStage output(&logger);
  VideoSynthPipeline pipeline(&parser, &validator, &generation, &noise,
                              &dropouts, &output, &logger);
  std::filesystem::create_directories(
      std::filesystem::path(project.output.video_path).parent_path());
  RunOptions options;
  options.threads = 4;
  if (!pipeline.RunProject(project, options)) {
    ADD_FAILURE() << "render failed: " << project.output.video_path;
    return {};
  }

  std::ifstream stream(project.output.video_path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
  std::filesystem::remove(project.output.video_path);
  std::filesystem::remove(project.output.metadata_path);

  const std::size_t spf = SamplesPerFrameForEncodingPreset(
      project.cvbs_presets.video_standard_preset,
      project.cvbs_presets.sample_encoding_preset);
  std::vector<std::vector<std::uint16_t>> frames(bytes.size() / (2U * spf));
  for (std::size_t f = 0; f < frames.size(); ++f) {
    frames[f].resize(spf);
    for (std::size_t s = 0; s < spf; ++s) {
      const std::size_t at = 2U * ((f * spf) + s);
      frames[f][s] = static_cast<std::uint16_t>(
          static_cast<unsigned char>(bytes[at]) |
          (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1U]))
           << 8U));
    }
  }
  return frames;
}

// Picture number of every output frame, from each section's picture_number
// start value.
std::vector<int> PictureNumbers(const Project& project) {
  std::vector<int> numbers;
  for (const Section& section : project.sections) {
    int start = -1;
    for (const Section::LineInjection& injection : section.line_injections) {
      for (const Section::LineInjectionCode& code : injection.codes) {
        if (code.code_type == "picture_number") {
          start = code.start_value;
        }
      }
    }
    for (int i = 0; i < section.duration_frames; ++i) {
      numbers.push_back(start + i);
    }
  }
  return numbers;
}

// The same project with the capture noise layer removed: what is left is the
// pressing (signal, channel and shared noise).
Project PressingOnly(Project project) {
  for (Section& section : project.sections) {
    section.noise.enabled = false;
  }
  const std::filesystem::path video(project.output.video_path);
  const std::string stem = video.stem().string() + "_pressing_only";
  project.output.video_path =
      (video.parent_path() / (stem + video.extension().string())).string();
  project.output.metadata_path =
      (video.parent_path() / (stem + ".meta")).string();
  return project;
}

struct Capture {
  std::vector<int> picture_numbers;
  std::vector<std::vector<std::uint16_t>> full;
  std::vector<std::vector<std::uint16_t>> pressing_only;
};

TEST(PressingCapturesTest, CapturesOfOnePressingDifferOnlyByCaptureNoise) {
  if (test_support::GeneratedProjectRoot().empty()) {
    GTEST_SKIP() << "generated projects unavailable (no Python at build time)";
  }

  for (const char* standard : kStandards) {
    // cap1 plays PNs 1-16, cap2 starts at PN 5, cap3 replays PNs 9-12.
    std::map<std::string, Capture> captures;
    for (const char* name : {"cap1", "cap2", "cap3"}) {
      Project project;
      ASSERT_TRUE(LoadGenerated(std::string("stacking-captures/") + standard +
                                    "_pressing_A_" + name + ".yaml",
                                &project));
      Capture& capture = captures[name];
      capture.picture_numbers = PictureNumbers(project);
      capture.full = Render(project);
      capture.pressing_only = Render(PressingOnly(project));
      ASSERT_EQ(capture.full.size(), capture.picture_numbers.size());
      ASSERT_EQ(capture.pressing_only.size(), capture.picture_numbers.size());
    }

    // Reference: cap1's frame for every picture number.
    const Capture& reference = captures["cap1"];
    std::map<int, std::size_t> reference_frame;
    for (std::size_t f = 0; f < reference.picture_numbers.size(); ++f) {
      reference_frame[reference.picture_numbers[f]] = f;
    }

    for (const char* name : {"cap2", "cap3"}) {
      const Capture& capture = captures[name];
      for (std::size_t f = 0; f < capture.picture_numbers.size(); ++f) {
        const int pn = capture.picture_numbers[f];
        ASSERT_EQ(reference_frame.count(pn), 1U);
        const std::size_t r = reference_frame[pn];
        // With the capture layer removed, the two captures are the same
        // pressing: byte-identical on the same picture, wherever it falls in
        // the capture's timeline.
        EXPECT_EQ(capture.pressing_only[f], reference.pressing_only[r])
            << standard << " " << name << " PN " << pn;
        // With it, they differ: the capture noise is fresh on every play.
        EXPECT_NE(capture.full[f], reference.full[r])
            << standard << " " << name << " PN " << pn;
      }
    }

    // The replayed range is a fresh play of the same pressing.
    const Capture& replay = captures["cap3"];
    for (std::size_t f = 0; f < 4U; ++f) {
      EXPECT_EQ(replay.pressing_only[8U + f], replay.pressing_only[12U + f])
          << standard << " replay frame " << f;
      EXPECT_NE(replay.full[8U + f], replay.full[12U + f])
          << standard << " replay frame " << f;
    }
  }
}

// The timing offset is specified within its passband (to ~6 MHz); content
// near Nyquist, such as the sharp multi-burst packet edges, is attenuated by
// the band-limited delay and would bias a full-band correlation. Both renders
// are therefore low-passed identically (Hann-windowed sinc at kLowPassMhz,
// circular like the delay) before they are correlated.
constexpr double kLowPassMhz = 4.5;
constexpr int kLowPassHalfTaps = 64;

std::vector<double> LowPass(const std::vector<std::uint16_t>& frame,
                            double sample_rate_hz) {
  constexpr double kPi = 3.14159265358979323846;
  const double cutoff = kLowPassMhz * 1.0e6 / sample_rate_hz;
  std::vector<double> taps;
  double gain = 0.0;
  for (int m = -kLowPassHalfTaps; m <= kLowPassHalfTaps; ++m) {
    const double sinc = m == 0 ? 2.0 * cutoff
                               : std::sin(2.0 * kPi * cutoff * m) /
                                     (kPi * static_cast<double>(m));
    const double window =
        0.5 + (0.5 * std::cos(kPi * m / (kLowPassHalfTaps + 1.0)));
    taps.push_back(sinc * window);
    gain += taps.back();
  }
  const auto count = static_cast<std::int64_t>(frame.size());
  std::vector<double> filtered(frame.size(), 0.0);
  for (std::int64_t n = 0; n < count; ++n) {
    double acc = 0.0;
    for (std::size_t t = 0; t < taps.size(); ++t) {
      const std::int64_t m = static_cast<std::int64_t>(t) - kLowPassHalfTaps;
      const std::int64_t at = (((n - m) % count) + count) % count;
      acc += taps[t] * static_cast<double>(frame[static_cast<std::size_t>(at)]);
    }
    filtered[static_cast<std::size_t>(n)] = acc / gain;
  }
  return filtered;
}

// Sum over the frame of a[n] * b(n - delay), with b delayed by the
// band-limited fractional delay: the cross-correlation at a fractional lag.
double CrossCorrelation(const std::vector<double>& a,
                        const std::vector<SampleFixed>& b, double delay) {
  std::vector<SampleFixed> delayed(b.size());
  TimingOffsetFilter(delay).Apply(b.data(), b.size(), delayed.data());
  double sum = 0.0;
  for (std::size_t n = 0; n < a.size(); ++n) {
    sum += a[n] * static_cast<double>(delayed[n]);
  }
  return sum;
}

// Delay, in samples, that best aligns b with a: the cross-correlation peak of
// the low-passed frames, found by golden-section search over [-1, 1].
double AligningDelay(const std::vector<std::uint16_t>& a,
                     const std::vector<std::uint16_t>& b,
                     double sample_rate_hz) {
  const std::vector<double> a_low = LowPass(a, sample_rate_hz);
  const std::vector<double> b_low = LowPass(b, sample_rate_hz);
  std::vector<SampleFixed> b_fixed(b_low.size());
  for (std::size_t n = 0; n < b_low.size(); ++n) {
    b_fixed[n] = MillivoltsToSampleFixed(b_low[n]);
  }
  constexpr double kInverseGolden = 0.6180339887498949;
  double lo = -1.0;
  double hi = 1.0;
  double x1 = hi - (kInverseGolden * (hi - lo));
  double x2 = lo + (kInverseGolden * (hi - lo));
  double f1 = CrossCorrelation(a_low, b_fixed, x1);
  double f2 = CrossCorrelation(a_low, b_fixed, x2);
  while (hi - lo > 1e-4) {
    if (f1 > f2) {
      hi = x2;
      x2 = x1;
      f2 = f1;
      x1 = hi - (kInverseGolden * (hi - lo));
      f1 = CrossCorrelation(a_low, b_fixed, x1);
    } else {
      lo = x1;
      x1 = x2;
      f1 = f2;
      x2 = lo + (kInverseGolden * (hi - lo));
      f2 = CrossCorrelation(a_low, b_fixed, x2);
    }
  }
  return 0.5 * (lo + hi);
}

TEST(PressingCapturesTest, PressingCleanRendersDifferByConfiguredOffset) {
  if (test_support::GeneratedProjectRoot().empty()) {
    GTEST_SKIP() << "generated projects unavailable (no Python at build time)";
  }

  for (const char* standard : kStandards) {
    Project pressing_a;
    Project pressing_b;
    ASSERT_TRUE(LoadGenerated(std::string("stacking-clean/") + standard +
                                  "_pressing_A_cap1_clean.yaml",
                              &pressing_a));
    ASSERT_TRUE(LoadGenerated(std::string("stacking-clean/") + standard +
                                  "_pressing_B_cap1_clean.yaml",
                              &pressing_b));
    const double expected = pressing_b.channel.timing_offset_samples -
                            pressing_a.channel.timing_offset_samples;
    ASSERT_NE(expected, 0.0) << standard;

    const auto frames_a = Render(pressing_a);
    const auto frames_b = Render(pressing_b);
    ASSERT_EQ(frames_a.size(), frames_b.size());
    ASSERT_FALSE(frames_a.empty());

    const double sample_rate_hz = SampleRateHzForEncodingPreset(
        pressing_a.cvbs_presets.video_standard_preset,
        pressing_a.cvbs_presets.sample_encoding_preset);
    // One frame per section's content: bars, test card, multi-burst, ramps.
    for (std::size_t f : {0U, 4U, 8U, 12U}) {
      // Delaying B by -offset_b + offset_a lines it up with A.
      const double measured =
          -AligningDelay(frames_a[f], frames_b[f], sample_rate_hz);
      EXPECT_NEAR(measured, expected, 0.02) << standard << " frame " << f;
    }
  }
}

}  // namespace
}  // namespace videosynth
