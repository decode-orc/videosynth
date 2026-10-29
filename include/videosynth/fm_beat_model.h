/*
 * File:        fm_beat_model.h
 * Module:      channel
 * Purpose:     PAL LaserDisc FM-beat herringbone model: predicts the beat
 *              between the FM video carrier and the colour signal from one
 *              frame's Y and C.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/picture_chroma.h"

namespace videosynth {

// Constants of the reference beat model, `_beat_basis` in
// domesday-aiv-restore newdecode/deherring.py (model-2026-10-27b,
// GPL-3.0-or-later), fitted on the EBU bars of PAL LaserDisc pressings.

// FM carrier deviation per IRE of the pre-emphasised composite (HZ_IRE).
constexpr double kFmBeatHzPerIre = 8000.0;
// Weight of the chroma's own phase modulation in the carrier phase (GAMMA).
constexpr double kFmBeatChromaPmWeight = 0.4;
// Beat amplitude law exp(alpha * (f_c - pivot) / 1 MHz): the beat falls with
// brightness (ALPHA and its 7.5 MHz pivot).
constexpr double kFmBeatAmplitudeLawAlpha = -1.0;
constexpr double kFmBeatAmplitudeLawPivotHz = 7.5e6;
// Linear term of the amplitude law, am = (f_c - f_sc) / scale * exp(...).
constexpr double kFmBeatAmplitudeScaleHz = 3.0e9;
// ld-decode PAL video emphasis time constants: pre-emphasis has its zero at
// 1 / 400 ns and its pole at 1 / 100 ns; de-emphasis is the inverse.
constexpr double kFmBeatEmphasisSlowS = 400.0e-9;
constexpr double kFmBeatEmphasisFastS = 100.0e-9;
// Beat band (BAND): unity from low + ramp / 2 to high - ramp / 2, falling
// linearly to zero over ramp centred on each edge.
constexpr double kFmBeatBandLowHz = 2.2e6;
constexpr double kFmBeatBandHighHz = 3.9e6;
constexpr double kFmBeatBandRampHz = 0.3e6;
// Box lengths that strip the chroma phase modulation from the carrier
// frequency before the amplitude law (4 samples nulls f_sc at 4fsc).
constexpr int kFmBeatCarrierBoxShort = 4;
constexpr int kFmBeatCarrierBoxLong = 17;

// Half-length of the beat band-pass FIR (2 * this + 1 taps). The reference
// applies its band as an FFT mask; at 4fsc PAL the FIR's Kaiser window widens
// each 0.3 MHz ramp by under 0.4 MHz and stays within 1e-3 of unity from
// 2.5 to 3.6 MHz, which holds every flat colour's beat.
constexpr int kFmBeatBandPassHalfTaps = 127;
constexpr double kFmBeatBandPassKaiserBeta = 8.0;

// First-order IIR y[n] = b0 x[n] + b1 x[n-1] - a1 y[n-1].
struct FirstOrderIir {
  double b0 = 1.0;
  double b1 = 0.0;
  double a1 = 0.0;

  std::complex<double> Response(double frequency_hz,
                                double sample_rate_hz) const;
};

// Bilinear transform of H(s) = (w2 / w1) (s + w1) / (s + w2), w = 1 / t,
// pre-warped: unity DC gain and a high-frequency gain of t1 / t2.
FirstOrderIir DesignEmphasisFilter(double zero_time_s, double pole_time_s,
                                   double sample_rate_hz);

// Predicts the FM-beat herringbone of a PAL LaserDisc from its own signal,
// following the reference `_beat_basis`:
//   1. composite (Y + C) to IRE;
//   2. 400/100 ns pre-emphasis, starting from the first sample's level;
//   3. analytic chroma ca = C + j H{C}, from the picture line windows only;
//   4. f_c = f0 + 8 kHz/IRE * (ip - (1 - gamma) Re(ca H_pre(f_sc)));
//   5. Phi = cumulative sum of 2 pi f_c / fs from the first sample;
//   6. am = (lf - f_sc) / 3e9 * exp(alpha (lf - 7.5 MHz) / 1 MHz), lf the
//      box-filtered f_c; r = Re(e^{j Phi} am conj(ca));
//   7. de-emphasis, then the 2.2-3.9 MHz band-pass.
// Only the real part of the reference's complex basis is formed: the beat on
// the disc is Re(c * basis) for a real c, and both filters are real.
//
// The beat is the analytic chroma times a carrier phasor, so it is exactly
// zero wherever the picture carries no chroma. It depends only on the frame's
// content, so every capture of one pressing carries the same beat.
//
// Thread-safety: immutable after construction; ComputeBasis is const and may
// be called concurrently on distinct buffers.
class FmBeatModel {
 public:
  // Args:
  //   sample_rate_hz:   4fsc rate; the subcarrier is a quarter of it.
  //   carrier_black_hz: FM video carrier at black (f0).
  //   black_mv:         Composite black level (0 IRE).
  //   white_mv:         Composite white level (100 IRE).
  FmBeatModel(double sample_rate_hz, double carrier_black_hz, double black_mv,
              double white_mv);

  // Writes the beat basis, in mV, for samples [0, count) of y and c. The
  // basis is formed inside the windows only and is zero elsewhere.
  //
  // Args:
  //   y, c:    count samples of luma and chroma.
  //   count:   Signal length.
  //   windows: Picture line windows within [0, count), in order.
  //   basis:   Receives count samples (resized).
  void ComputeBasis(const SampleFixed* y, const SampleFixed* c,
                    std::size_t count,
                    const std::vector<PictureLineWindow>& windows,
                    std::vector<double>* basis) const;

  // Basis amplitude per unit chroma amplitude for a flat colour whose
  // carrier (without chroma phase modulation) is carrier_hz.
  double FlatColourGain(double carrier_hz) const;

  // Factor applied to the basis so that the beat on a 50 IRE flat colour is
  // level_db relative to its chroma amplitude.
  double LevelScale(double level_db) const;

  const std::vector<double>& band_pass_kernel() const {
    return band_pass_kernel_;
  }

 private:
  double sample_rate_hz_;
  double subcarrier_hz_;
  double carrier_black_hz_;
  double black_mv_;
  double ire_per_mv_;
  FirstOrderIir pre_emphasis_;
  FirstOrderIir de_emphasis_;
  std::complex<double> pre_emphasis_at_subcarrier_;
  std::vector<double> hilbert_kernel_;
  std::vector<double> band_pass_kernel_;
};

}  // namespace videosynth
