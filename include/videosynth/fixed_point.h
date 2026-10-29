/*
 * File:        fixed_point.h
 * Module:      fixed_point
 * Purpose:     Provides fixed-point helpers for millivolt-domain signal
 * processing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace videosynth {

// Thread-safety: All functions and types in this module are thread-safe.
// They are stateless and only operate on their parameters or constants.
// May be called concurrently from multiple threads.
using SampleFixed = std::int64_t;

constexpr int kSampleFractionBits = 20;
constexpr std::int64_t kSampleScale = (1LL << kSampleFractionBits);

inline SampleFixed MillivoltsToSampleFixed(double millivolts) {
  return static_cast<SampleFixed>(
      std::llround(millivolts * static_cast<double>(kSampleScale)));
}

inline double SampleFixedToMillivolts(SampleFixed sample) {
  return static_cast<double>(sample) / static_cast<double>(kSampleScale);
}

inline std::int64_t RoundShiftRightSigned(std::int64_t value, int shift_bits) {
  if (shift_bits <= 0) {
    return value;
  }

  const std::int64_t rounding = static_cast<std::int64_t>(1)
                                << (shift_bits - 1);
  // Rounds half away from zero, matching std::llround. Negative values are
  // rounded on their magnitude: an arithmetic right shift floors, so shifting
  // (value - rounding) directly would bias every negative result one step low.
  if (value >= 0) {
    return (value + rounding) >> shift_bits;
  }
  return -((-value + rounding) >> shift_bits);
}

}  // namespace videosynth