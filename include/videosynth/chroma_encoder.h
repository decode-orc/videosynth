/*
 * File:        chroma_encoder.h
 * Module:      chroma_encoder
 * Purpose:     Defines standard-specific chroma encoders for fixed-format frame
 * data.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/progressive_frame_source.h"

namespace videosynth {

// Thread-safety: Implementations of IChromaEncoder are NOT thread-safe.
// Callers must ensure sequential access. Concurrent calls to
// EncodeLineFromPhaseStart from multiple threads will result in undefined
// behavior due to mutable workspace state.
class IChromaEncoder {
 public:
  virtual ~IChromaEncoder() = default;
  // Ownership: out_chroma_mv is an output parameter. The caller owns the
  // pointed-to vector and must ensure the pointer is valid (non-null).
  // The implementation clears and populates this vector but does not take
  // ownership.
  virtual void EncodeLineFromPhaseStart(
      const std::vector<YCbCr444Pixel>& source_samples,
      double carrier_phase_start_rad,
      std::vector<SampleFixed>* out_chroma_mv) const = 0;
};

// Shared implementation of the quadrature-modulated chroma encoders.
//
// PAL, NTSC and PAL-M differ only in the low-pass cutoff, the tap count and
// the per-axis millivolt scales; the filtering and modulation arithmetic is
// identical. The FIR taps are designed and quantised once in the constructor
// (both colour-difference axes share one kernel because they are designed
// identically), and every per-line buffer is a reusable member so the encode
// path performs no heap allocation in steady state.
//
// Both the Q30 taps and the Q20 colour-difference axis samples are held as
// int32 so the filter accumulates through a widening 32×32→64 multiply, which
// the compiler can vectorise; the constructor proves the accumulator cannot
// overflow for any legal input. Filtered samples feed the modulator directly
// from fixed point — there is no intermediate double buffer.
//
// Complexity: EncodeLineFromPhaseStart is O(samples × taps).
//
// Thread-safety: NOT thread-safe — the per-line workspaces are mutable
// members. Serialise access or construct one encoder per worker thread.
class QuadratureChromaEncoder : public IChromaEncoder {
 public:
  // Encodes a line of source pixels into chroma samples with colour
  // subcarrier.
  //
  // Args:
  //   source_samples: Input pixels in YCbCr 4:4:4 format.
  //   carrier_phase_start_rad: Phase of the colour subcarrier at the start
  //     of the line in radians.
  //   out_chroma_mv: Output vector for chroma samples (C channel).
  void EncodeLineFromPhaseStart(
      const std::vector<YCbCr444Pixel>& source_samples,
      double carrier_phase_start_rad,
      std::vector<SampleFixed>* out_chroma_mv) const override;

 protected:
  // Args:
  //   sample_rate_hz: The output sample rate, used to design the filter.
  //   cutoff_hz:      Colour-difference low-pass cutoff.
  //   taps:           Odd FIR tap count.
  //   sin_scale_mv:   Peak millivolts per unit of the sin-axis component.
  //   cos_scale_mv:   Peak millivolts per unit of the cos-axis component.
  QuadratureChromaEncoder(double sample_rate_hz, double cutoff_hz, int taps,
                          double sin_scale_mv, double cos_scale_mv);

 private:
  // Q30 filter taps, quantised once at construction and shared by both axes.
  std::vector<std::int32_t> filter_taps_fixed_;
  double sin_scale_mv_;
  double cos_scale_mv_;

  // Per-line scratch buffers, reused across calls (see thread-safety note).
  mutable std::vector<std::int32_t> sin_axis_fixed_;
  mutable std::vector<std::int32_t> cos_axis_fixed_;
  mutable std::vector<std::int32_t> filtered_sin_fixed_;
  mutable std::vector<std::int32_t> filtered_cos_fixed_;
  mutable std::vector<std::int32_t> fir_pad_fixed_;
};

// 625-line PAL chroma encoder.
//
// ITU-R BT.1700 Annex 1 Part B Table 1 item 10d: U is modulated onto the sin
// axis and V onto the cos axis, scaled to the 700 mV PAL white level.
//
// Thread-safety: NOT thread-safe; see QuadratureChromaEncoder.
class PalChromaEncoder final : public QuadratureChromaEncoder {
 public:
  // Constructs a PAL chroma encoder with the given sample rate.
  explicit PalChromaEncoder(double sample_rate_hz);
};

// 525-line NTSC chroma encoder.
//
// SMPTE 170M-2004 §A.5 eq (10): b-y is modulated onto the sin axis and r-y
// onto the cos axis, scaled per §A.3 with the 0.925 reduction factor.
//
// Thread-safety: NOT thread-safe; see QuadratureChromaEncoder.
class NtscChromaEncoder final : public QuadratureChromaEncoder {
 public:
  // Constructs an NTSC chroma encoder with the given sample rate.
  explicit NtscChromaEncoder(double sample_rate_hz);
};

// PAL-M chroma encoder: PAL UV quadrature encoding with System M signal levels.
//
// ITU-R BT.470-6 Table 2 (M/PAL): PAL-M uses the same PAL colour-difference
// equations (U on sin, V on cos, phase-alternating V) as 625-line PAL, but with
// System M white level (714.3 mV) instead of 625-line PAL white level (700 mV).
//
// Thread-safety: NOT thread-safe; see QuadratureChromaEncoder.
class PalMChromaEncoder final : public QuadratureChromaEncoder {
 public:
  explicit PalMChromaEncoder(double sample_rate_hz);
};

// Designs a unity-DC-gain Hamming-windowed sinc low-pass kernel.
//
// Shared by the quadrature encoders and the SECAM encoder, which filter their
// colour-difference signals the same way. Throws std::invalid_argument when
// the cutoff is not inside (0, Nyquist) or taps is not an odd number >= 3.
//
// Thread-safety: Stateless; safe to call concurrently.
std::vector<double> DesignChromaLowPassKernel(double cutoff_hz,
                                              double sample_rate_hz, int taps);

// Thread-safety: CreateChromaEncoder returns a new encoder instance. The
// caller owns the returned unique_ptr and is responsible for thread-safety.
std::unique_ptr<IChromaEncoder> CreateChromaEncoder(Standard standard,
                                                    double sample_rate_hz);

}  // namespace videosynth
