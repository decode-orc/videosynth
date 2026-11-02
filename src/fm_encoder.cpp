/*
 * File:        fm_encoder.cpp
 * Module:      fm_encoder
 * Purpose:     40-bit FM coded signal generation for NTSC LaserDisc VBI
 *              injection per IEC 60857 §10.2, Figure 13.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/fm_encoder.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "videosynth/fixed_point.h"
#include "videosynth/signal_shaping.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// Large pulse width for ShapedPulseLevel: ensures the leading-edge path is
// always used within the ramp region (ramp_samples_ << kLargePulseWidth).
constexpr int kLargePulseWidth = 10000;

// White flag pulse length as a fraction of H (IEC 60857 Figure 12).
constexpr double kWhiteFlagLengthH = 0.790;

}  // namespace

FmEncoder::FmEncoder(double sample_rate_hz, double bit_cell_duration_us,
                     double transition_duration_ns)
    : ramp_samples_(TransitionTimeToRampSamples(transition_duration_ns * 1.0e-9,
                                                sample_rate_hz, 0.1, 0.9)),
      bit_cell_exact_(bit_cell_duration_us * 1.0e-6 * sample_rate_hz),
      bit_cell_samples_(static_cast<int>(std::round(bit_cell_exact_))) {
  if (bit_cell_samples_ <= 0) {
    throw std::invalid_argument(
        "FmEncoder: bit cell duration yields zero or negative samples");
  }
  // Cells on the exact grid are bit_cell_samples_ ± 1 samples long, so the
  // ramp must fit the shortest half cell.
  if (ramp_samples_ >= (bit_cell_samples_ - 1) / 2) {
    throw std::invalid_argument(
        "FmEncoder: transition ramp is too wide to fit within a half bit "
        "cell — check sample rate and timing parameters");
  }
}

void FmEncoder::ApplyStepTransition(std::vector<double>& samples,
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
      samples[i] = ShapedPulseLevel(rel, kLargePulseWidth, ramp_samples_,
                                    baseline_level_mv, peak_level_mv);
    } else {
      samples[i] = ShapedPulseLevel(kLargePulseWidth - ramp_samples_ + rel,
                                    kLargePulseWidth, ramp_samples_,
                                    baseline_level_mv, peak_level_mv);
    }
  }
}

// static
std::array<bool, 40> FmEncoder::BuildBitPattern(const FmData& data) {
  std::array<bool, 40> bits{};

  // Bits [0-3]: clock synchronising bits = 0011
  bits[0] = false;
  bits[1] = false;
  bits[2] = true;
  bits[3] = true;

  // Bit [4]: video field indicator (1 = first field)
  bits[4] = data.field_one;

  // Bits [5-11]: leading data recognition bits = 1110010
  bits[5] = true;
  bits[6] = true;
  bits[7] = true;
  bits[8] = false;
  bits[9] = false;
  bits[10] = true;
  bits[11] = false;

  // Bits [12-15]: data nibble X5, LSB first (bit 12 = LSB of X5)
  bits[12] = (data.x5 & 0x01u) != 0u;
  bits[13] = (data.x5 & 0x02u) != 0u;
  bits[14] = (data.x5 & 0x04u) != 0u;
  bits[15] = (data.x5 & 0x08u) != 0u;

  // Bits [16-19]: data nibble X4, LSB first
  bits[16] = (data.x4 & 0x01u) != 0u;
  bits[17] = (data.x4 & 0x02u) != 0u;
  bits[18] = (data.x4 & 0x04u) != 0u;
  bits[19] = (data.x4 & 0x08u) != 0u;

  // Bits [20-23]: data nibble X3, LSB first
  bits[20] = (data.x3 & 0x01u) != 0u;
  bits[21] = (data.x3 & 0x02u) != 0u;
  bits[22] = (data.x3 & 0x04u) != 0u;
  bits[23] = (data.x3 & 0x08u) != 0u;

  // Bits [24-27]: data nibble X2, LSB first
  bits[24] = (data.x2 & 0x01u) != 0u;
  bits[25] = (data.x2 & 0x02u) != 0u;
  bits[26] = (data.x2 & 0x04u) != 0u;
  bits[27] = (data.x2 & 0x08u) != 0u;

  // Bits [28-31]: data nibble X1, LSB first
  bits[28] = (data.x1 & 0x01u) != 0u;
  bits[29] = (data.x1 & 0x02u) != 0u;
  bits[30] = (data.x1 & 0x04u) != 0u;
  bits[31] = (data.x1 & 0x08u) != 0u;

  // Bit [32]: odd parity over data nibble bits [12-31] only (IEC 60857 §10.2).
  // The decoder extracts parity from decodedBytes & 0x80 and validates it
  // against dataValue = bits[12-31]; sync/recognition bits are excluded.
  int ones = 0;
  for (int i = 12; i < 32; ++i) {
    if (bits[static_cast<std::size_t>(i)]) ++ones;
  }
  // Set parity bit so that total count of '1's in bits [12-32] is odd.
  bits[32] = (ones % 2 == 0);

  // Bits [33-39]: trailing data recognition bits = 0001101
  bits[33] = false;
  bits[34] = false;
  bits[35] = false;
  bits[36] = true;
  bits[37] = true;
  bits[38] = false;
  bits[39] = true;

  return bits;
}

int FmEncoder::BitCellStart(int bit_index) const {
  return static_cast<int>(std::lround(bit_index * bit_cell_exact_));
}

int FmEncoder::BitCellCentre(int bit_index) const {
  return static_cast<int>(std::lround((bit_index + 0.5) * bit_cell_exact_));
}

std::vector<SampleFixed> FmEncoder::GenerateBitsFM(
    const std::array<bool, 40>& bits, double baseline_level_mv,
    double peak_level_mv) const {
  constexpr int kNumBits = 40;
  const int total = BitCellStart(kNumBits);

  // FM encoding rule (IEC 60857 §10.2, Figure 13):
  //   Signal starts HIGH (peak). The decoder initialises lastTransitionX at the
  //   first HIGH sample, so the code must begin at peak.
  //
  //   '0' bit: hold current level for the full cell (T0), then flip
  //   permanently.
  //            Decoder: long interval (T0 > 0.75 µs threshold) → bit '0'.
  //
  //   '1' bit: quick "pip" — hold for T1 = T0/2, flip briefly, return at T0.
  //            Two transitions per cell; net level unchanged.
  //            Decoder: short interval (T1 < 0.75 µs) → bit '1', then scans
  //            past the pip-end transition to re-anchor lastTransitionX.

  // Pass 1: fill constant-level regions.
  std::vector<double> samples(static_cast<std::size_t>(total),
                              baseline_level_mv);
  double level = peak_level_mv;  // Start HIGH

  for (int bit_idx = 0; bit_idx < kNumBits; ++bit_idx) {
    const bool bit = bits[static_cast<std::size_t>(bit_idx)];
    const int cell_start = BitCellStart(bit_idx);
    const int cell_end = BitCellStart(bit_idx + 1);
    const int pip_start = BitCellCentre(bit_idx);  // T1: half a cell in

    if (!bit) {
      for (int i = cell_start; i < cell_end; ++i) {
        samples[static_cast<std::size_t>(i)] = level;
      }
      level = (level == peak_level_mv) ? baseline_level_mv : peak_level_mv;
    } else {
      const double pip_level =
          (level == peak_level_mv) ? baseline_level_mv : peak_level_mv;
      for (int i = cell_start; i < pip_start; ++i) {
        samples[static_cast<std::size_t>(i)] = level;
      }
      for (int i = pip_start; i < cell_end; ++i) {
        samples[static_cast<std::size_t>(i)] = pip_level;
      }
      // level unchanged — two flips cancel
    }
  }

  // Pass 2: apply shaped ramp transitions at every transition point.
  level = peak_level_mv;
  for (int bit_idx = 0; bit_idx < kNumBits; ++bit_idx) {
    const bool bit = bits[static_cast<std::size_t>(bit_idx)];
    const int cell_end = BitCellStart(bit_idx + 1);

    if (!bit) {
      // One permanent-flip transition at end of cell.
      const bool rising = (level == baseline_level_mv);
      ApplyStepTransition(samples, cell_end, rising, baseline_level_mv,
                          peak_level_mv);
      level = (level == peak_level_mv) ? baseline_level_mv : peak_level_mv;
    } else {
      // Pip start at T1, pip end at T0 (return to original level).
      const bool pip_rising = (level == baseline_level_mv);
      ApplyStepTransition(samples, BitCellCentre(bit_idx), pip_rising,
                          baseline_level_mv, peak_level_mv);
      ApplyStepTransition(samples, cell_end, !pip_rising, baseline_level_mv,
                          peak_level_mv);
      // level unchanged
    }
  }

  std::vector<SampleFixed> result;
  result.reserve(static_cast<std::size_t>(total));
  for (const double lv : samples) {
    result.push_back(MillivoltsToSampleFixed(lv));
  }
  return result;
}

std::vector<SampleFixed> FmEncoder::Generate40BitWaveform(
    const FmData& data, double baseline_level_mv, double peak_level_mv) const {
  return GenerateBitsFM(BuildBitPattern(data), baseline_level_mv,
                        peak_level_mv);
}

std::vector<SampleFixed> FmEncoder::Generate40BitCode(
    const FmData& data, Standard standard, double baseline_level_mv,
    double peak_level_mv) const {
  const TimingConstants timing = GetTimingConstants(standard);
  const int line_samples = timing.samples_per_line_4fsc;

  // Initialise the full line buffer at baseline.
  std::vector<SampleFixed> line(static_cast<std::size_t>(line_samples),
                                MillivoltsToSampleFixed(baseline_level_mv));

  // Embed the 40-bit FM waveform starting at sample 0.
  const auto code_samples =
      GenerateBitsFM(BuildBitPattern(data), baseline_level_mv, peak_level_mv);
  const int copy_count =
      std::min(static_cast<int>(code_samples.size()), line_samples);
  for (int i = 0; i < copy_count; ++i) {
    line[static_cast<std::size_t>(i)] =
        code_samples[static_cast<std::size_t>(i)];
  }

  return line;
}

std::vector<SampleFixed> FmEncoder::GenerateWhiteFlag(
    Standard standard, double peak_level_mv, double baseline_level_mv) const {
  const TimingConstants timing = GetTimingConstants(standard);
  const int line_samples = timing.samples_per_line_4fsc;

  // White flag pulse length = 0.790 H per IEC 60857 Figure 12.
  const int flag_length_samples =
      static_cast<int>(std::round(kWhiteFlagLengthH * line_samples));

  // Fill full line with baseline; overlay shaped pulse from sample 0.
  std::vector<SampleFixed> line(static_cast<std::size_t>(line_samples),
                                MillivoltsToSampleFixed(baseline_level_mv));
  for (int i = 0; i < flag_length_samples; ++i) {
    line[static_cast<std::size_t>(i)] = MillivoltsToSampleFixed(
        ShapedPulseLevel(i, flag_length_samples, ramp_samples_,
                         baseline_level_mv, peak_level_mv));
  }
  return line;
}

}  // namespace videosynth
