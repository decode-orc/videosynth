/*
 * File:        biphase_encoder.cpp
 * Module:      biphase_encoder
 * Purpose:     24-bit biphase (Manchester) signal generation for LaserDisc
 *              VBI injection per IEC 60856 (PAL) and IEC 60857 (NTSC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/biphase_encoder.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "videosynth/biphase_utils.h"
#include "videosynth/fixed_point.h"
#include "videosynth/signal_shaping.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// A pulse width large enough that ShapedPulseLevel's leading edge covers the
// full ramp region before any trailing-edge fallback begins.  ramp_samples_ is
// at most a few dozen samples, so 10000 is safely larger than ramp/2.
constexpr int kLargePulseWidth = 10000;

}  // namespace

BiphaseEncoder::BiphaseEncoder(double sample_rate_hz,
                               double bit_cell_duration_us,
                               double transition_duration_ns)
    : ramp_samples_(TransitionTimeToRampSamples(transition_duration_ns * 1.0e-9,
                                                sample_rate_hz, 0.1, 0.9)),
      bit_cell_exact_(bit_cell_duration_us * 1.0e-6 * sample_rate_hz),
      bit_cell_samples_(static_cast<int>(std::round(bit_cell_exact_))) {
  if (bit_cell_samples_ <= 0) {
    throw std::invalid_argument(
        "BiphaseEncoder: bit cell duration yields zero or negative samples");
  }
  // Cells on the exact grid are bit_cell_samples_ ± 1 samples long, so the
  // ramp must fit the shortest half cell.
  if (ramp_samples_ >= (bit_cell_samples_ - 1) / 2) {
    throw std::invalid_argument(
        "BiphaseEncoder: transition ramp is too wide to fit within a half bit "
        "cell — check sample rate and timing parameters");
  }
}

void BiphaseEncoder::ApplyStepTransition(std::vector<double>& samples,
                                         int center_sample, bool rising,
                                         double baseline_level_mv,
                                         double peak_level_mv) const {
  const int ramp_half = ramp_samples_ / 2;
  const int ramp_start = center_sample - ramp_half;
  const int ramp_end = ramp_start + ramp_samples_;
  const int buf_size = static_cast<int>(samples.size());

  for (int i = std::max(0, ramp_start); i < std::min(buf_size, ramp_end); ++i) {
    const int rel = i - ramp_start;
    if (rising) {
      // Leading edge of a very wide pulse: baseline → peak.
      samples[i] = ShapedPulseLevel(rel, kLargePulseWidth, ramp_samples_,
                                    baseline_level_mv, peak_level_mv);
    } else {
      // Trailing edge of a very wide pulse: peak → baseline.
      samples[i] = ShapedPulseLevel(kLargePulseWidth - ramp_samples_ + rel,
                                    kLargePulseWidth, ramp_samples_,
                                    baseline_level_mv, peak_level_mv);
    }
  }
}

std::vector<SampleFixed> BiphaseEncoder::GenerateBit(
    bool bit_value, double baseline_level_mv, double peak_level_mv) const {
  std::vector<double> samples(static_cast<std::size_t>(bit_cell_samples_));

  const int half_cell = bit_cell_samples_ / 2;
  const int center = half_cell;
  const int ramp_half = ramp_samples_ / 2;
  const int post_start = center + (ramp_samples_ - ramp_half);

  // For '1': low (baseline) in first half, high (peak) in second half.
  // For '0': high (peak) in first half, low (baseline) in second half.
  const double start_level = bit_value ? baseline_level_mv : peak_level_mv;
  const double end_level = bit_value ? peak_level_mv : baseline_level_mv;

  for (int i = 0; i < center - ramp_half; ++i) {
    samples[static_cast<std::size_t>(i)] = start_level;
  }
  for (int i = post_start; i < bit_cell_samples_; ++i) {
    samples[static_cast<std::size_t>(i)] = end_level;
  }

  ApplyStepTransition(samples, center, bit_value, baseline_level_mv,
                      peak_level_mv);

  std::vector<SampleFixed> result;
  result.reserve(static_cast<std::size_t>(bit_cell_samples_));
  for (const double level : samples) {
    result.push_back(MillivoltsToSampleFixed(level));
  }
  return result;
}

int BiphaseEncoder::BitCellStart(int bit_index) const {
  return static_cast<int>(std::lround(bit_index * bit_cell_exact_));
}

int BiphaseEncoder::BitCellCentre(int bit_index) const {
  return static_cast<int>(std::lround((bit_index + 0.5) * bit_cell_exact_));
}

std::vector<SampleFixed> BiphaseEncoder::Generate24BitCode(
    uint32_t code_value, double baseline_level_mv, double peak_level_mv) const {
  const int total = BitCellStart(24);
  std::vector<double> samples(static_cast<std::size_t>(total));

  const int ramp_half = ramp_samples_ / 2;

  // --- Pass 1: fill each bit cell with constant regions and center
  //     transitions ---
  for (int bit_idx = 0; bit_idx < 24; ++bit_idx) {
    // Bits are transmitted MSB first (bit 23 is transmitted first).
    const bool bit = ((code_value >> (23 - bit_idx)) & 1u) != 0u;
    const int bit_start = BitCellStart(bit_idx);
    const int bit_end = BitCellStart(bit_idx + 1);
    const int center = BitCellCentre(bit_idx);
    const int post_start = center + (ramp_samples_ - ramp_half);

    const double start_level = bit ? baseline_level_mv : peak_level_mv;
    const double end_level = bit ? peak_level_mv : baseline_level_mv;

    // Constant region before the centre transition.
    for (int i = bit_start; i < center - ramp_half; ++i) {
      samples[static_cast<std::size_t>(i)] = start_level;
    }
    // Constant region after the centre transition.
    for (int i = post_start; i < bit_end; ++i) {
      samples[static_cast<std::size_t>(i)] = end_level;
    }

    // Centre transition (always present for every bit).
    ApplyStepTransition(samples, center, bit, baseline_level_mv, peak_level_mv);
  }

  // --- Pass 2: apply inter-bit transitions where consecutive bits are equal
  //     ---
  // Manchester encoding: same consecutive bit values require a transition at
  // the bit boundary so that each bit starts at the correct level.
  //
  // After a '1' bit the signal is at peak; the next '1' must start at
  // baseline → falling boundary transition.
  // After a '0' bit the signal is at baseline; the next '0' must start at
  // peak → rising boundary transition.
  for (int bit_idx = 1; bit_idx < 24; ++bit_idx) {
    const bool prev_bit = ((code_value >> (24 - bit_idx)) & 1u) != 0u;
    const bool curr_bit = ((code_value >> (23 - bit_idx)) & 1u) != 0u;

    if (prev_bit == curr_bit) {
      const int boundary = BitCellStart(bit_idx);
      // prev '0'→'0': prev ends at baseline, curr starts at peak → rising.
      // prev '1'→'1': prev ends at peak,     curr starts at baseline → falling.
      const bool rising = !prev_bit;
      ApplyStepTransition(samples, boundary, rising, baseline_level_mv,
                          peak_level_mv);
    }
  }

  // Convert double working buffer to fixed-point output.
  std::vector<SampleFixed> result;
  result.reserve(static_cast<std::size_t>(total));
  for (const double level : samples) {
    result.push_back(MillivoltsToSampleFixed(level));
  }
  return result;
}

std::vector<SampleFixed> BiphaseEncoder::GenerateLine(
    const std::string& hex_code, Standard standard, double baseline_level_mv,
    double peak_level_mv) const {
  const auto maybe_code = ParseBiphaseHexCode(hex_code);
  if (!maybe_code) {
    throw std::invalid_argument(
        "BiphaseEncoder::GenerateLine: invalid hex code '" + hex_code + "'");
  }

  const TimingConstants timing = GetTimingConstants(standard);
  const int line_samples = timing.samples_per_line_4fsc;

  // Initialise the full line buffer at baseline.
  std::vector<SampleFixed> line(static_cast<std::size_t>(line_samples),
                                MillivoltsToSampleFixed(baseline_level_mv));

  // Embed the 24-bit biphase waveform starting at sample 0.
  const auto code_samples =
      Generate24BitCode(*maybe_code, baseline_level_mv, peak_level_mv);
  const int copy_count =
      std::min(static_cast<int>(code_samples.size()), line_samples);
  for (int i = 0; i < copy_count; ++i) {
    line[static_cast<std::size_t>(i)] =
        code_samples[static_cast<std::size_t>(i)];
  }

  return line;
}

}  // namespace videosynth
