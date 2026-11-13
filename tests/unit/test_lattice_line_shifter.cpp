/*
 * File:        test_lattice_line_shifter.cpp
 * Module:      lattice_line_shifter_tests
 * Purpose:     Validates that the lattice line shifter delays each storage
 *              line by its lattice delay without disturbing flat levels, the
 *              colour subcarrier or the shape of a sync edge
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/lattice_line_shifter.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Layout {
  std::vector<int> counts;
  std::vector<int> offsets;
  int frame_samples = 0;
};

Layout FrameLayout(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  Layout layout;
  layout.counts = BuildLineSampleCounts(standard, timing.lines_per_frame,
                                        timing.samples_per_line_4fsc);
  layout.offsets = BuildLineSampleOffsets(layout.counts);
  layout.frame_samples = layout.offsets.back() + layout.counts.back();
  return layout;
}

TEST(LatticeLineShifterTest, DelaysAreTheLatticeDelays) {
  const Layout layout = FrameLayout(Standard::kPal);
  const LatticeLineShifter shifter(Standard::kPal, layout.counts);
  EXPECT_FALSE(shifter.all_delays_zero());
  for (int line : {1, 2, 313, 314, 625}) {
    EXPECT_DOUBLE_EQ(
        shifter.LineDelaySamples(static_cast<std::size_t>(line) - 1U),
        LatticeLineDelay(Standard::kPal, line).Samples())
        << "line " << line;
  }
  EXPECT_TRUE(
      LatticeLineShifter(Standard::kNtsc, FrameLayout(Standard::kNtsc).counts)
          .all_delays_zero());
}

// A frame of constant blanking passes unchanged: the kernel has unit DC gain.
TEST(LatticeLineShifterTest, ConstantBlankingPassesUnchanged) {
  const Layout layout = FrameLayout(Standard::kPal);
  const LatticeLineShifter shifter(Standard::kPal, layout.counts);
  const SampleFixed blanking = MillivoltsToSampleFixed(0.0);
  std::vector<SampleFixed> frame(static_cast<std::size_t>(layout.frame_samples),
                                 blanking);
  LatticeLineShifter::Workspace workspace;
  shifter.Apply(frame.data(), blanking, {}, &workspace);
  for (std::size_t n = 0; n < frame.size(); ++n) {
    ASSERT_EQ(frame[n], blanking) << "sample " << n;
  }
}

// Zero delays and no lead-in leave a frame untouched.
TEST(LatticeLineShifterTest, ZeroDelaysWithoutLeadInDoNothing) {
  const Layout layout = FrameLayout(Standard::kNtsc);
  const LatticeLineShifter shifter(Standard::kNtsc, layout.counts);
  std::vector<SampleFixed> frame(
      static_cast<std::size_t>(layout.frame_samples));
  for (std::size_t n = 0; n < frame.size(); ++n) {
    frame[n] = static_cast<SampleFixed>((n * 7919U) % 104729U);
  }
  const std::vector<SampleFixed> original = frame;
  LatticeLineShifter::Workspace workspace;
  shifter.Apply(frame.data(), SampleFixed{0}, {}, &workspace);
  EXPECT_EQ(frame, original);
}

// A 4.43 MHz (fs / 4) tone rendered 0H-relative on each line, delayed by the
// line's d_k, matches the analytic delayed tone within 0.1 % in amplitude and
// 0.1 degree in phase, whatever the delay.
TEST(LatticeLineShifterTest, SubcarrierToneKeepsAmplitudeAndPhase) {
  const Layout layout = FrameLayout(Standard::kPal);
  const LatticeLineShifter shifter(Standard::kPal, layout.counts);
  constexpr double kAmplitudeMv = 300.0;
  constexpr double kOmega = kPi / 2.0;
  std::vector<SampleFixed> frame(
      static_cast<std::size_t>(layout.frame_samples));
  for (std::size_t line = 0; line < layout.counts.size(); ++line) {
    for (int j = 0; j < layout.counts[line]; ++j) {
      frame[static_cast<std::size_t>(layout.offsets[line]) +
            static_cast<std::size_t>(j)] =
          MillivoltsToSampleFixed(kAmplitudeMv *
                                  std::sin(kOmega * static_cast<double>(j)));
    }
  }
  LatticeLineShifter::Workspace workspace;
  shifter.Apply(frame.data(), SampleFixed{0}, {}, &workspace);

  for (int line : {2, 157, 312, 313, 314, 470, 624}) {
    const auto index = static_cast<std::size_t>(line - 1);
    const double delay = shifter.LineDelaySamples(index);
    // Fit A sin(omega j + phi) over whole cycles in the middle of the line,
    // clear of the tone's ends.
    double in_phase = 0.0;
    double quadrature = 0.0;
    constexpr int kFirst = 100;
    constexpr int kCount = 800;
    for (int j = kFirst; j < kFirst + kCount; ++j) {
      const double sample = SampleFixedToMillivolts(
          frame[static_cast<std::size_t>(layout.offsets[index]) +
                static_cast<std::size_t>(j)]);
      in_phase += sample * std::sin(kOmega * static_cast<double>(j));
      quadrature += sample * std::cos(kOmega * static_cast<double>(j));
    }
    const double amplitude =
        2.0 * std::hypot(in_phase, quadrature) / static_cast<double>(kCount);
    const double phase = std::atan2(quadrature, in_phase);
    EXPECT_NEAR(amplitude / kAmplitudeMv, 1.0, 1e-3) << "line " << line;
    double phase_error = phase + (kOmega * delay);
    phase_error = std::remainder(phase_error, 2.0 * kPi);
    EXPECT_NEAR(phase_error, 0.0, 0.1 * kPi / 180.0) << "line " << line;
  }
}

// A falling edge whose half-amplitude point is at -0.5 in the render, with
// its pre-0H part as the lead-in, has its half-amplitude point moved by d_k
// on every line, and the lead-in lands in the previous storage line.
TEST(LatticeLineShifterTest, SyncEdgeMovesByTheLineDelay) {
  const Layout layout = FrameLayout(Standard::kPal);
  const LatticeLineShifter shifter(Standard::kPal, layout.counts);
  constexpr double kTipMv = -300.0;
  // A raised-cosine edge over 7 samples centred on -0.5, held at the tip for
  // 80 samples.
  auto edge_mv = [&](double t) {
    constexpr double kHalfWidth = 3.0;
    if (t <= -0.5 - kHalfWidth) {
      return 0.0;
    }
    if (t >= -0.5 + kHalfWidth) {
      return kTipMv;
    }
    const double x = (t + 0.5 + kHalfWidth) / (2.0 * kHalfWidth);
    return kTipMv * 0.5 * (1.0 - std::cos(kPi * x));
  };
  std::vector<SampleFixed> lead_in;
  for (int j = -3; j < 0; ++j) {
    lead_in.push_back(MillivoltsToSampleFixed(edge_mv(j)));
  }
  std::vector<SampleFixed> frame(static_cast<std::size_t>(layout.frame_samples),
                                 SampleFixed{0});
  for (std::size_t line = 0; line < layout.counts.size(); ++line) {
    for (int j = 0; j < 80; ++j) {
      frame[static_cast<std::size_t>(layout.offsets[line]) +
            static_cast<std::size_t>(j)] = MillivoltsToSampleFixed(edge_mv(j));
    }
  }
  LatticeLineShifter::Workspace workspace;
  shifter.Apply(frame.data(), SampleFixed{0}, lead_in, &workspace);

  const auto frame_count = static_cast<std::int64_t>(frame.size());
  auto at = [&](std::int64_t n) {
    return SampleFixedToMillivolts(frame[static_cast<std::size_t>(
        ((n % frame_count) + frame_count) % frame_count)]);
  };
  for (std::size_t line = 0; line < layout.counts.size(); ++line) {
    const std::int64_t offset = layout.offsets[line];
    double crossing = 0.0;
    bool found = false;
    for (std::int64_t n = offset - 6; n < offset + 6 && !found; ++n) {
      const double before = at(n);
      const double after = at(n + 1);
      if (before >= kTipMv / 2.0 && after < kTipMv / 2.0) {
        crossing = static_cast<double>(n - offset) +
                   ((before - (kTipMv / 2.0)) / (before - after));
        found = true;
      }
    }
    ASSERT_TRUE(found) << "line " << (line + 1);
    EXPECT_NEAR(crossing, -0.5 + shifter.LineDelaySamples(line), 0.01)
        << "line " << (line + 1);
  }
}

// Updating a lattice frame for a render that differs on a few lines gives
// exactly what delaying the new render whole gives, on PAL and on NTSC.
TEST(LatticeLineShifterTest, UpdateMatchesAFullApply) {
  for (Standard standard : {Standard::kPal, Standard::kNtsc}) {
    const Layout layout = FrameLayout(standard);
    const LatticeLineShifter shifter(standard, layout.counts);
    const std::vector<SampleFixed> lead_in = {MillivoltsToSampleFixed(-20.0),
                                              MillivoltsToSampleFixed(-150.0),
                                              MillivoltsToSampleFixed(-280.0)};
    const SampleFixed blanking = MillivoltsToSampleFixed(0.0);
    std::vector<SampleFixed> old_render(
        static_cast<std::size_t>(layout.frame_samples));
    for (std::size_t n = 0; n < old_render.size(); ++n) {
      old_render[n] = MillivoltsToSampleFixed(
          300.0 * std::sin(0.37 * static_cast<double>(n)));
    }
    std::vector<SampleFixed> new_render = old_render;
    for (int line :
         {1, 16, 313, 314, 500, static_cast<int>(layout.counts.size())}) {
      const auto offset = static_cast<std::size_t>(
          layout.offsets[static_cast<std::size_t>(line - 1)]);
      for (std::size_t j = 0; j < 40U; ++j) {
        new_render[offset + j] += MillivoltsToSampleFixed(55.5);
      }
    }

    LatticeLineShifter::Workspace workspace;
    std::vector<SampleFixed> updated = old_render;
    shifter.Apply(updated.data(), blanking, lead_in, &workspace);
    shifter.Update(old_render.data(), new_render.data(), updated.data(),
                   blanking, lead_in, &workspace);
    std::vector<SampleFixed> applied = new_render;
    shifter.Apply(applied.data(), blanking, lead_in, &workspace);
    EXPECT_EQ(updated, applied) << StandardToString(standard);
  }
}

}  // namespace
}  // namespace videosynth
