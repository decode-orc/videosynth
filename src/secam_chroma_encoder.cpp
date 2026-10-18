/*
 * File:        secam_chroma_encoder.cpp
 * Module:      chroma_encoder
 * Purpose:     Encodes frame chroma into the 625-line SECAM frequency-modulated
 * colour subcarrier.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "videosynth/secam_chroma_encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "videosynth/chroma_encoder.h"
#include "videosynth/signal_shaping.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 5: undeviated subcarriers.
constexpr double kSecamForHz = 4406250.0;
constexpr double kSecamFobHz = 4250000.0;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10e: nominal deviation for D* = 1
// and the maximum deviations.
constexpr double kSecamDeltaForHz = 280.0e3;
constexpr double kSecamDeltaFobHz = 230.0e3;
constexpr double kSecamDrMaxPositiveHz = 350.0e3;
constexpr double kSecamDrMaxNegativeHz = -506.0e3;
constexpr double kSecamDbMaxPositiveHz = 506.0e3;
constexpr double kSecamDbMaxNegativeHz = -350.0e3;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 9: D'R = −1.902 (E'R − E'Y) and
// D'B = 1.505 (E'B − E'Y). BT.601 normalises Cb as (B−Y)/(2(1−Kb)) and Cr as
// (R−Y)/(2(1−Kr)) into ±448 10-bit codes, so (B−Y) = 0.886 × cb_norm and
// (R−Y) = 0.701 × cr_norm for cb_norm, cr_norm in [−1, 1].
constexpr double kSecamDrPerCrNorm = -1.902 * 0.701;
constexpr double kSecamDbPerCbNorm = 1.505 * 0.886;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10c: colour-difference attenuation
// ≤ 3 dB at 1.3 MHz and ≥ 30 dB at 3.5 MHz. A 33-tap Hamming-windowed sinc
// with a 1.7 MHz cutoff gives about −1.5 dB at 1.3 MHz and better than −55 dB
// from 3.5 MHz at the 4fsc PAL sampling rate.
constexpr double kSecamColourDifferenceCutoffHz = 1.7e6;
constexpr int kSecamColourDifferenceTaps = 33;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10f: low-frequency precorrection
// corner f1 = 85 kHz; the high-frequency asymptote is 3 × f1.
constexpr double kSecamLfPrecorrectionF1Hz = 85.0e3;
constexpr double kSecamLfPrecorrectionRatio = 3.0;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10d: bell filter centre and
// numerator/denominator coefficients of (1 + j16F) / (1 + j1.26F).
constexpr double kSecamBellCentreHz = 4286.0e3;
constexpr double kSecamBellNumeratorK = 16.0;
constexpr double kSecamBellDenominatorK = 1.26;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10d / Table 5 item 6: the
// subcarrier peak-to-peak amplitude 2·M0 is 23 % of the 700 mV luminance
// amplitude.
constexpr double kSecamWhiteLevelMv = 700.0;
constexpr double kSecamM0Mv = 0.23 * kSecamWhiteLevelMv / 2.0;

// ITU-R BT.470-6 Table 2 item 2.17a / BT.1700 Part C Table 5 item i: chroma
// subcarrier blanked from the leading edge of line blanking up to 5.6 µs after
// 0H. Table 5 item c: the front porch (start of line blanking) is 1.5 µs
// before the next 0H; item b: line blanking ends 10.5 µs after 0H; item e:
// line-blanking edges rise in 300 ns, which is also used for the subcarrier
// gate.
constexpr double kSecamSubcarrierStartSeconds = 5.6e-6;
constexpr double kSecamFrontPorchSeconds = 1.5e-6;
constexpr double kSecamLineBlankingEndSeconds = 10.5e-6;
constexpr double kSecamGateRiseSeconds = 300.0e-9;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 13 / Figure 17: field
// identification trapezoids.
constexpr double kSecamIdDrLevel = 1.25;
constexpr double kSecamIdDrRiseSeconds = 15.0e-6;
constexpr double kSecamIdDbLevel = -1.52;
constexpr double kSecamIdDbRiseSeconds = 18.0e-6;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 13: field identification lines.
constexpr int kSecamIdField1FirstLine = 7;
constexpr int kSecamIdField1LastLine = 15;
constexpr int kSecamIdField2FirstLine = 320;
constexpr int kSecamIdField2LastLine = 328;

double NormalisedColourDifference(int code) {
  const int clamped = std::max(64, std::min(code, 960));
  return static_cast<double>(clamped - 512) / 448.0;
}

}  // namespace

SecamLineComponent SecamComponentForLine(int line_number_1based,
                                         int frame_parity) {
  const bool odd_line = (line_number_1based % 2) != 0;
  const bool first_frame = (frame_parity % 2) == 0;
  return (odd_line == first_frame) ? SecamLineComponent::kDr
                                   : SecamLineComponent::kDb;
}

double SecamInitialPhaseRadForLine(int line_number_1based, int frame_parity) {
  const bool line_inverted = ((line_number_1based - 1) % 3) == 2;
  const bool frame_inverted = (frame_parity % 2) != 0;
  return (line_inverted != frame_inverted) ? kPi : 0.0;
}

bool IsSecamFieldIdentificationLine(int line_number_1based) {
  return (line_number_1based >= kSecamIdField1FirstLine &&
          line_number_1based <= kSecamIdField1LastLine) ||
         (line_number_1based >= kSecamIdField2FirstLine &&
          line_number_1based <= kSecamIdField2LastLine);
}

SecamChromaEncoder::SecamChromaEncoder(double sample_rate_hz)
    : sample_rate_hz_(sample_rate_hz),
      low_pass_taps_(DesignChromaLowPassKernel(kSecamColourDifferenceCutoffHz,
                                               sample_rate_hz,
                                               kSecamColourDifferenceTaps)) {
  if (sample_rate_hz <= 2.0 * (kSecamForHz + kSecamDrMaxPositiveHz)) {
    throw std::invalid_argument(
        "SECAM chroma encoding needs a sample rate above twice the highest "
        "subcarrier frequency");
  }

  // Low-frequency precorrection is discretised with the bilinear transform,
  // pre-warped at f1 so the discrete response matches the analogue one
  // exactly there; the 85 kHz corner is far below Nyquist, so warping is
  // negligible elsewhere.
  //
  // Low-frequency precorrection H(s) = (1 + s/w1) / (1 + s/(3·w1)).
  {
    const double w1 = kTwoPi * kSecamLfPrecorrectionF1Hz;
    const double k = w1 / std::tan(w1 / (2.0 * sample_rate_hz));
    const double num_s = 1.0 / w1;
    const double den_s = 1.0 / (kSecamLfPrecorrectionRatio * w1);
    const double a0 = (den_s * k) + 1.0;
    lf_b0_ = ((num_s * k) + 1.0) / a0;
    lf_b1_ = (1.0 - (num_s * k)) / a0;
    lf_a1_ = (1.0 - (den_s * k)) / a0;
  }

  // Bell filter H = (1 + j·16·F) / (1 + j·1.26·F) with jF = (s² + w0²)/(s·w0),
  // i.e. H(s) = (16 s² + w0 s + 16 w0²) / (1.26 s² + w0 s + 1.26 w0²).
  //
  // The numerator zeros are sharp (Q = 16) and sit near a quarter of the
  // sampling rate, where the bilinear transform would stretch the bell's
  // bandwidth by about 1.5×. The matched-z transform (z = e^{sT}) keeps the
  // frequency and damping of every pole and zero instead; the gain is then
  // normalised to unity at f0. Over 3.9–4.9 MHz the magnitude stays within
  // 0.5 % of the analogue curve; the phase differs by a near-constant ~9°,
  // equivalent to a fraction-of-a-sample delay.
  {
    const double w0 = kTwoPi * kSecamBellCentreHz;
    const double sample_period = 1.0 / sample_rate_hz;
    // Returns {1, c1, c2} of (1 − r z⁻¹)(1 − r* z⁻¹) for the complex root pair
    // of c·s² + w0·s + c·w0², mapped through z = e^{sT}.
    const auto matched_pair = [&](double c) {
      const std::complex<double> root =
          std::complex<double>(-w0, w0 * std::sqrt((4.0 * c * c) - 1.0)) /
          (2.0 * c);
      const std::complex<double> z = std::exp(root * sample_period);
      return std::array<double, 3>{1.0, -2.0 * z.real(), std::norm(z)};
    };
    const std::array<double, 3> num = matched_pair(kSecamBellNumeratorK);
    const std::array<double, 3> den = matched_pair(kSecamBellDenominatorK);
    const std::complex<double> z_inv =
        std::exp(std::complex<double>(0.0, -w0 * sample_period));
    const std::complex<double> response_at_f0 =
        (num[0] + (num[1] * z_inv) + (num[2] * z_inv * z_inv)) /
        (den[0] + (den[1] * z_inv) + (den[2] * z_inv * z_inv));
    const double gain = 1.0 / std::abs(response_at_f0);
    bell_b0_ = gain * num[0];
    bell_b1_ = gain * num[1];
    bell_b2_ = gain * num[2];
    bell_a1_ = den[1];
    bell_a2_ = den[2];
  }

  subcarrier_start_samples_ = static_cast<int>(
      std::lround(kSecamSubcarrierStartSeconds * sample_rate_hz));
  front_porch_samples_ =
      static_cast<int>(std::lround(kSecamFrontPorchSeconds * sample_rate_hz));
  line_blanking_end_samples_ = static_cast<int>(
      std::lround(kSecamLineBlankingEndSeconds * sample_rate_hz));
  gate_ramp_samples_ =
      RiseTimeToRampSamples(kSecamGateRiseSeconds, sample_rate_hz);
}

void SecamChromaEncoder::EncodeLine(
    SecamLineComponent component, double initial_phase_rad,
    const std::vector<YCbCr444Pixel>& active_pixels, int active_start_sample,
    int line_samples, std::vector<SampleFixed>* out_chroma_mv) const {
  if (out_chroma_mv == nullptr) {
    throw std::invalid_argument("Output chroma line pointer must not be null");
  }
  if (line_samples <= 0) {
    out_chroma_mv->clear();
    return;
  }

  const std::size_t count = static_cast<std::size_t>(line_samples);
  const bool dr_line = component == SecamLineComponent::kDr;

  // Colour difference over the whole line: zero (undeviated subcarrier)
  // outside the active window, so the back porch carries the line
  // identification reference and the low-pass and precorrection filters see
  // the picture edges as real transitions.
  colour_difference_.assign(count, 0.0);
  for (std::size_t index = 0; index < active_pixels.size(); ++index) {
    const std::ptrdiff_t position =
        static_cast<std::ptrdiff_t>(active_start_sample) +
        static_cast<std::ptrdiff_t>(index);
    if (position < 0 || position >= static_cast<std::ptrdiff_t>(count)) {
      continue;
    }
    const YCbCr444Pixel& pixel = active_pixels[index];
    colour_difference_[static_cast<std::size_t>(position)] =
        dr_line ? (kSecamDrPerCrNorm * NormalisedColourDifference(pixel.cr))
                : (kSecamDbPerCbNorm * NormalisedColourDifference(pixel.cb));
  }

  // Colour-difference low-pass (item 10c). Outside the line the signal is the
  // zero reference, so the kernel is zero-padded.
  const int half_width = static_cast<int>(low_pass_taps_.size()) / 2;
  filtered_.assign(count, 0.0);
  for (int index = 0; index < line_samples; ++index) {
    double acc = 0.0;
    for (int tap = 0; tap < static_cast<int>(low_pass_taps_.size()); ++tap) {
      const int source = index + tap - half_width;
      if (source >= 0 && source < line_samples) {
        acc += low_pass_taps_[static_cast<std::size_t>(tap)] *
               colour_difference_[static_cast<std::size_t>(source)];
      }
    }
    filtered_[static_cast<std::size_t>(index)] = acc;
  }

  // Low-frequency precorrection (item 10f). The filter starts at rest on the
  // zero reference at 0H.
  deviation_norm_.assign(count, 0.0);
  double x1 = 0.0;
  double y1 = 0.0;
  for (std::size_t index = 0; index < count; ++index) {
    const double x0 = filtered_[index];
    const double y0 = (lf_b0_ * x0) + (lf_b1_ * x1) - (lf_a1_ * y1);
    deviation_norm_[index] = y0;
    x1 = x0;
    y1 = y0;
  }

  ModulateLine(component, initial_phase_rad, line_samples, out_chroma_mv);
}

void SecamChromaEncoder::EncodeIdentificationLine(
    SecamLineComponent component, double initial_phase_rad, int line_samples,
    std::vector<SampleFixed>* out_chroma_mv) const {
  if (out_chroma_mv == nullptr) {
    throw std::invalid_argument("Output chroma line pointer must not be null");
  }
  if (line_samples <= 0) {
    out_chroma_mv->clear();
    return;
  }

  const bool dr_line = component == SecamLineComponent::kDr;
  const double level = dr_line ? kSecamIdDrLevel : kSecamIdDbLevel;
  const double rise_samples =
      (dr_line ? kSecamIdDrRiseSeconds : kSecamIdDbRiseSeconds) *
      sample_rate_hz_;

  deviation_norm_.assign(static_cast<std::size_t>(line_samples), 0.0);
  for (int index = line_blanking_end_samples_; index < line_samples; ++index) {
    const double progress =
        static_cast<double>(index - line_blanking_end_samples_) / rise_samples;
    deviation_norm_[static_cast<std::size_t>(index)] =
        level * std::min(1.0, progress);
  }

  ModulateLine(component, initial_phase_rad, line_samples, out_chroma_mv);
}

void SecamChromaEncoder::ModulateLine(
    SecamLineComponent component, double initial_phase_rad, int line_samples,
    std::vector<SampleFixed>* out_chroma_mv) const {
  const std::size_t count = static_cast<std::size_t>(line_samples);
  const bool dr_line = component == SecamLineComponent::kDr;
  const double rest_hz = dr_line ? kSecamForHz : kSecamFobHz;
  const double deviation_hz = dr_line ? kSecamDeltaForHz : kSecamDeltaFobHz;
  const double max_positive_hz =
      dr_line ? kSecamDrMaxPositiveHz : kSecamDbMaxPositiveHz;
  const double max_negative_hz =
      dr_line ? kSecamDrMaxNegativeHz : kSecamDbMaxNegativeHz;

  // Frequency modulation from 0H (items 5, 10d, 10e). Since f_OR = 282 fH and
  // f_OB = 272 fH, the undeviated subcarrier has the same phase at every 0H,
  // so initial_phase_rad alone carries the Note 1 phase pattern. The
  // oscillator and the bell filter run from 0H so the filter has settled by
  // the time the subcarrier gate opens 5.6 µs later.
  carrier_.assign(count, 0.0);
  double phase = initial_phase_rad;
  double x1 = 0.0;
  double x2 = 0.0;
  double y1 = 0.0;
  double y2 = 0.0;
  for (std::size_t index = 0; index < count; ++index) {
    const double x0 = std::cos(phase);
    const double y0 = (bell_b0_ * x0) + (bell_b1_ * x1) + (bell_b2_ * x2) -
                      (bell_a1_ * y1) - (bell_a2_ * y2);
    carrier_[index] = y0;
    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;

    const double offset_hz = std::clamp(deviation_hz * deviation_norm_[index],
                                        max_negative_hz, max_positive_hz);
    phase += kTwoPi * (rest_hz + offset_hz) / sample_rate_hz_;
    if (phase >= kTwoPi) {
      phase -= kTwoPi;
    }
  }

  // Subcarrier blanking (BT.470-6 Table 2 item 2.17a): gate open from 5.6 µs
  // after 0H to the start of the front porch.
  out_chroma_mv->assign(count, MillivoltsToSampleFixed(0.0));
  const int gate_start = std::min(subcarrier_start_samples_, line_samples);
  const int gate_end =
      std::max(gate_start, line_samples - front_porch_samples_);
  const int gate_width = gate_end - gate_start;
  for (int index = gate_start; index < gate_end; ++index) {
    const double envelope =
        ShapedGateEnvelope(index - gate_start, gate_width, gate_ramp_samples_);
    (*out_chroma_mv)[static_cast<std::size_t>(index)] = MillivoltsToSampleFixed(
        kSecamM0Mv * envelope * carrier_[static_cast<std::size_t>(index)]);
  }
}

}  // namespace videosynth
