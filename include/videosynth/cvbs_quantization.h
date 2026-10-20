/*
 * File:        cvbs_quantization.h
 * Module:      cvbs_quantization
 * Purpose:     Maps mV-domain composite and chroma samples to the 10-bit CVBS
 *              code space shared by the output stage and the GUI preview.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cmath>
#include <cstdint>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"

namespace videosynth {

// Thread-safety: all functions and types in this module are thread-safe.
// They are stateless and only operate on their parameters or return new
// values. May be called concurrently from multiple threads.

struct QuantizationProfile {
  double millivolts_per_code = 1.0;
  int blanking_code = 0;
  int minimum_legal_code = 0;
  int maximum_legal_code = 1023;
  std::int64_t reciprocal_q30 = 0;
};

// Populates the 10-bit quantisation profile for the standard. Returns false
// (leaving profile untouched) for Standard::kUnknown.
inline bool BuildQuantizationProfile(Standard standard,
                                     QuantizationProfile* profile) {
  if (profile == nullptr) {
    return false;
  }

  if (Is625LineStandard(standard)) {
    *profile = QuantizationProfile{
        .millivolts_per_code = 1.1905,
        .blanking_code = 256,
        .minimum_legal_code = 4,
        .maximum_legal_code = 1019,
        .reciprocal_q30 = 0,
    };
  } else if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // PAL-M uses System M signal levels identical to M/NTSC, so the same
    // quantization profile applies. ITU-R BT.470-6 Table 1 item 4.
    *profile = QuantizationProfile{
        .millivolts_per_code = 1.2755,
        .blanking_code = 240,
        .minimum_legal_code = 16,
        .maximum_legal_code = 1019,
        .reciprocal_q30 = 0,
    };
  } else {
    return false;
  }

  profile->reciprocal_q30 = static_cast<std::int64_t>(std::llround(
      (1.0 / profile->millivolts_per_code) * static_cast<double>(1LL << 30)));
  if (profile->reciprocal_q30 <= 0) {
    return false;
  }

  return true;
}

inline int MapCompositeMillivoltsToCode(double composite_mv,
                                        const QuantizationProfile& profile) {
  return static_cast<int>(
             std::lround(composite_mv / profile.millivolts_per_code)) +
         profile.blanking_code;
}

inline int MapCompositeFixedToCode(SampleFixed composite_mv_fixed,
                                   const QuantizationProfile& profile) {
  constexpr int kReciprocalFractionBits = 30;
  const std::int64_t product = composite_mv_fixed * profile.reciprocal_q30;
  const std::int64_t mapped_delta = RoundShiftRightSigned(
      product, kReciprocalFractionBits + kSampleFractionBits);
  return static_cast<int>(mapped_delta) + profile.blanking_code;
}

inline int ClampToLegalCodeRange(
    int mapped_code, [[maybe_unused]] const QuantizationProfile& profile) {
  // Preserve sub-black (4-15) and over-white-adjacent (1020-1023 are reserved).
  // Only clamp reserved/protected values: 0-3 (reserved low) and 1020-1023
  // (reserved high). Allow excursions in ranges 4-1019 to pass through (legal +
  // sub-black).
  constexpr int kReservedLow = 4;
  constexpr int kReservedHigh = 1020;

  if (mapped_code < kReservedLow) {
    return kReservedLow;  // Clamp reserved low values (0-3) to first
                          // non-reserved sub-black (4)
  }
  if (mapped_code > kReservedHigh - 1) {
    return kReservedHigh - 1;  // Clamp reserved high values (1020-1023) to
                               // maximum non-reserved (1019)
  }
  return mapped_code;
}

// Maps a chroma sample (oscillates around 0 mV) to a 10-bit code centred at
// 512, as required by the CVBS spec for dual-file YC output.
// CVBS file format spec §3.2: chroma zero is at 512 in the 0-1023 domain.
inline int MapChromaFixedToCode(SampleFixed chroma_mv_fixed,
                                const QuantizationProfile& profile) {
  constexpr int kChromaCentreCode = 512;
  constexpr int kReciprocalFractionBits = 30;
  const std::int64_t product = chroma_mv_fixed * profile.reciprocal_q30;
  const std::int64_t mapped_delta = RoundShiftRightSigned(
      product, kReciprocalFractionBits + kSampleFractionBits);
  return static_cast<int>(mapped_delta) + kChromaCentreCode;
}

}  // namespace videosynth
