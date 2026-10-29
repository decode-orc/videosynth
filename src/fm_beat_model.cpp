/*
 * File:        fm_beat_model.cpp
 * Module:      channel
 * Purpose:     PAL LaserDisc FM-beat herringbone model: predicts the beat
 *              between the FM video carrier and the colour signal from one
 *              frame's Y and C.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/fm_beat_model.h"

#include <algorithm>
#include <cmath>

#include "videosynth/kaiser_window.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Box filter with the window of scipy.ndimage.uniform_filter1d (samples
// n - length / 2 .. n - length / 2 + length - 1) and its 'nearest' edges.
std::vector<double> BoxFilter(const std::vector<double>& input, int length) {
  const auto count = static_cast<std::ptrdiff_t>(input.size());
  const std::ptrdiff_t before = length / 2;
  std::vector<double> output(input.size());
  for (std::ptrdiff_t n = 0; n < count; ++n) {
    double sum = 0.0;
    for (std::ptrdiff_t k = 0; k < length; ++k) {
      const std::ptrdiff_t at =
          std::clamp<std::ptrdiff_t>(n - before + k, 0, count - 1);
      sum += input[static_cast<std::size_t>(at)];
    }
    output[static_cast<std::size_t>(n)] = sum / static_cast<double>(length);
  }
  return output;
}

// Impulse response tap n of a zero-phase low-pass that is unity below
// ramp_start and falls linearly to zero at ramp_end (cycles per sample).
double RampLowPassTap(int n, double ramp_start, double ramp_end) {
  if (n == 0) {
    return ramp_start + ramp_end;
  }
  const double t = static_cast<double>(n);
  return (std::cos(2.0 * kPi * ramp_start * t) -
          std::cos(2.0 * kPi * ramp_end * t)) /
         (2.0 * kPi * kPi * t * t * (ramp_end - ramp_start));
}

// Kaiser-windowed zero-phase FIR of the reference's trapezoidal beat band.
std::vector<double> DesignBandPassKernel(double sample_rate_hz) {
  constexpr int kHalf = kFmBeatBandPassHalfTaps;
  const double half_ramp = 0.5 * kFmBeatBandRampHz / sample_rate_hz;
  const double low = kFmBeatBandLowHz / sample_rate_hz;
  const double high = kFmBeatBandHighHz / sample_rate_hz;
  std::vector<double> kernel(2U * static_cast<std::size_t>(kHalf) + 1U);
  for (int n = -kHalf; n <= kHalf; ++n) {
    const double ideal = RampLowPassTap(n, high - half_ramp, high + half_ramp) -
                         RampLowPassTap(n, low - half_ramp, low + half_ramp);
    const double ratio = static_cast<double>(n) / static_cast<double>(kHalf);
    const int tap_index = n + kHalf;
    kernel[static_cast<std::size_t>(tap_index)] =
        ideal * KaiserWindow(ratio, kFmBeatBandPassKaiserBeta);
  }
  return kernel;
}

}  // namespace

std::complex<double> FirstOrderIir::Response(double frequency_hz,
                                             double sample_rate_hz) const {
  const std::complex<double> z_inverse =
      std::polar(1.0, -2.0 * kPi * frequency_hz / sample_rate_hz);
  return (b0 + (b1 * z_inverse)) / (1.0 + (a1 * z_inverse));
}

FirstOrderIir DesignEmphasisFilter(double zero_time_s, double pole_time_s,
                                   double sample_rate_hz) {
  const double two_fs = 2.0 * sample_rate_hz;
  const double zero = two_fs * std::tan((1.0 / zero_time_s) / two_fs);
  const double pole = two_fs * std::tan((1.0 / pole_time_s) / two_fs);
  const double gain = pole / zero;
  const double a0 = two_fs + pole;
  return FirstOrderIir{.b0 = gain * (two_fs + zero) / a0,
                       .b1 = gain * (zero - two_fs) / a0,
                       .a1 = (pole - two_fs) / a0};
}

FmBeatModel::FmBeatModel(double sample_rate_hz, double carrier_black_hz,
                         double black_mv, double white_mv)
    : sample_rate_hz_(sample_rate_hz),
      subcarrier_hz_(sample_rate_hz / 4.0),
      carrier_black_hz_(carrier_black_hz),
      black_mv_(black_mv),
      ire_per_mv_(100.0 / (white_mv - black_mv)),
      pre_emphasis_(DesignEmphasisFilter(kFmBeatEmphasisSlowS,
                                         kFmBeatEmphasisFastS, sample_rate_hz)),
      de_emphasis_(DesignEmphasisFilter(kFmBeatEmphasisFastS,
                                        kFmBeatEmphasisSlowS, sample_rate_hz)),
      pre_emphasis_at_subcarrier_(
          pre_emphasis_.Response(subcarrier_hz_, sample_rate_hz)),
      hilbert_kernel_(DesignChromaHilbertKernel()),
      band_pass_kernel_(DesignBandPassKernel(sample_rate_hz)) {}

void FmBeatModel::ComputeBasis(const SampleFixed* y, const SampleFixed* c,
                               std::size_t count,
                               const std::vector<PictureLineWindow>& windows,
                               std::vector<double>* basis) const {
  basis->assign(count, 0.0);
  if (count == 0U || windows.empty()) {
    return;
  }

  // Analytic chroma of the picture windows; zero everywhere else.
  std::vector<double> chroma(count, 0.0);
  std::vector<double> quadrature(count, 0.0);
  for (const PictureLineWindow& window : windows) {
    for (std::size_t n = 0; n < window.count; ++n) {
      chroma[window.start + n] = SampleFixedToMillivolts(c[window.start + n]);
    }
    HilbertTransformWindow(hilbert_kernel_, &chroma[window.start], window.count,
                           &quadrature[window.start]);
  }

  // Instantaneous carrier frequency of the pre-emphasised composite, with
  // (1 - gamma) of its pre-emphasised chroma removed.
  const auto composite_ire = [&](std::size_t n) {
    return (SampleFixedToMillivolts(y[n]) + SampleFixedToMillivolts(c[n]) -
            black_mv_) *
           ire_per_mv_;
  };
  const double initial_ire = composite_ire(0U);
  const double pre_real = pre_emphasis_at_subcarrier_.real();
  const double pre_imag = pre_emphasis_at_subcarrier_.imag();
  std::vector<double> carrier(count);
  double previous_input = 0.0;
  double previous_output = 0.0;
  for (std::size_t n = 0; n < count; ++n) {
    const double input = composite_ire(n) - initial_ire;
    const double output = (pre_emphasis_.b0 * input) +
                          (pre_emphasis_.b1 * previous_input) -
                          (pre_emphasis_.a1 * previous_output);
    previous_input = input;
    previous_output = output;
    const double chroma_part =
        ((chroma[n] * pre_real) - (quadrature[n] * pre_imag)) * ire_per_mv_;
    carrier[n] =
        carrier_black_hz_ +
        (kFmBeatHzPerIre * (output + initial_ire -
                            ((1.0 - kFmBeatChromaPmWeight) * chroma_part)));
  }

  const std::vector<double> smoothed = BoxFilter(
      BoxFilter(carrier, kFmBeatCarrierBoxShort), kFmBeatCarrierBoxLong);

  // Carrier phasor times the conjugate analytic chroma, real part, then
  // de-emphasis from a zero state.
  std::vector<double> beat(count, 0.0);
  const double radians_per_hz = 2.0 * kPi / sample_rate_hz_;
  double phase = 0.0;
  previous_input = 0.0;
  previous_output = 0.0;
  for (std::size_t n = 0; n < count; ++n) {
    phase += carrier[n] * radians_per_hz;
    double mixed = 0.0;
    if (chroma[n] != 0.0 || quadrature[n] != 0.0) {
      const double amplitude =
          (smoothed[n] - subcarrier_hz_) / kFmBeatAmplitudeScaleHz *
          std::exp(kFmBeatAmplitudeLawAlpha *
                   (smoothed[n] - kFmBeatAmplitudeLawPivotHz) / 1.0e6);
      mixed = amplitude * ((std::cos(phase) * chroma[n]) +
                           (std::sin(phase) * quadrature[n]));
    }
    const double output = (de_emphasis_.b0 * mixed) +
                          (de_emphasis_.b1 * previous_input) -
                          (de_emphasis_.a1 * previous_output);
    previous_input = mixed;
    previous_output = output;
    beat[n] = output;
  }

  // Band-pass, evaluated inside the windows only.
  constexpr int kHalf = kFmBeatBandPassHalfTaps;
  const auto length = static_cast<std::ptrdiff_t>(count);
  for (const PictureLineWindow& window : windows) {
    for (std::size_t i = 0; i < window.count; ++i) {
      const auto n = static_cast<std::ptrdiff_t>(window.start + i);
      const std::ptrdiff_t first =
          std::max<std::ptrdiff_t>(-kHalf, n - length + 1);
      const std::ptrdiff_t last = std::min<std::ptrdiff_t>(kHalf, n);
      double sum = 0.0;
      for (std::ptrdiff_t k = first; k <= last; ++k) {
        sum += band_pass_kernel_[static_cast<std::size_t>(k + kHalf)] *
               beat[static_cast<std::size_t>(n - k)];
      }
      (*basis)[static_cast<std::size_t>(n)] = sum;
    }
  }
}

double FmBeatModel::FlatColourGain(double carrier_hz) const {
  const double beat_hz = carrier_hz - subcarrier_hz_;
  const double amplitude =
      (carrier_hz - subcarrier_hz_) / kFmBeatAmplitudeScaleHz *
      std::exp(kFmBeatAmplitudeLawAlpha *
               (carrier_hz - kFmBeatAmplitudeLawPivotHz) / 1.0e6);
  constexpr int kHalf = kFmBeatBandPassHalfTaps;
  double band_pass = band_pass_kernel_[static_cast<std::size_t>(kHalf)];
  for (int k = 1; k <= kHalf; ++k) {
    const int tap_index = k + kHalf;
    band_pass += 2.0 * band_pass_kernel_[static_cast<std::size_t>(tap_index)] *
                 std::cos(2.0 * kPi * beat_hz * k / sample_rate_hz_);
  }
  return std::abs(amplitude) *
         std::abs(de_emphasis_.Response(beat_hz, sample_rate_hz_)) *
         std::abs(band_pass);
}

double FmBeatModel::LevelScale(double level_db) const {
  constexpr double kReferenceIre = 50.0;
  const double reference_carrier_hz =
      carrier_black_hz_ + (kFmBeatHzPerIre * kReferenceIre);
  return std::pow(10.0, level_db / 20.0) / FlatColourGain(reference_carrier_hz);
}

}  // namespace videosynth
