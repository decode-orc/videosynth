/*
 * File:        chroma_encoder.cpp
 * Module:      chroma_encoder
 * Purpose:     Encodes fixed-format frame chroma into PAL or NTSC composite
 * subcarrier samples.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/chroma_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;
// SMPTE 170M-2004 Annex A eqs (4)/(5)/(10): NTSC encodes b-y onto the sin
// axis and r-y onto the cos axis, each scaled by 0.925 × reduction_factor.
// BT.601 normalises Cb as (B-Y)/(2*(1-Kb)) and Cr as (R-Y)/(2*(1-Kr)) into
// ±448 10-bit codes, so cb_norm = (B-Y)/0.886 and cr_norm = (R-Y)/0.701.
// Applying the NTSC reduction and 1000/140 mV-per-IRE conversion gives
// separate per-axis peak scales for cb_norm and cr_norm values in [−1, 1].
//   SMPTE 170M-2004 §A.3: b-y_factor = 0.492111, r-y_factor = 0.877283.
//   SMPTE 170M-2004 §A.5: overall luma+chroma scale factor = 0.925.
//   SMPTE 170M-2004 §15.4: 140 IRE ≡ 1 V → 1 IRE = 1000/140 mV.
constexpr double kNtscCbScaleMv =
    0.925 * 0.492111 * 0.886 * 100.0 * (1000.0 / 140.0);
constexpr double kNtscCrScaleMv =
    0.925 * 0.877283 * 0.701 * 100.0 * (1000.0 / 140.0);

// ITU-R BT.1700 Part B Table 1 items 8/9: PAL encodes E'_U onto the sin axis
// and E'_V onto the cos axis, where E'_U = 0.493*(B-Y) and E'_V = 0.877*(R-Y).
// PAL white level is 700 mV, so the peak chroma scale per cb_norm unit is:
//   U: 0.493 × (1-Kb) × 700 = 0.493 × 0.886 × 700 mV
//   V: 0.877 × (1-Kr) × 700 = 0.877 × 0.701 × 700 mV
constexpr double kPalUScaleMv = 0.493 * 0.886 * 700.0;
constexpr double kPalVScaleMv = 0.877 * 0.701 * 700.0;

// ITU-R BT.470-6 Table 2 (M/PAL): PAL-M uses the same PAL colour-difference
// equations (E'_U = 0.493*(B-Y), E'_V = 0.877*(R-Y)) but System M white level
// of 714.3 mV (100 IRE; 140 IRE = 1 V).
//   U: 0.493 × (1-Kb) × 714.3 = 0.493 × 0.886 × 714.3 mV
//   V: 0.877 × (1-Kr) × 714.3 = 0.877 × 0.701 × 714.3 mV
constexpr double kPalMUScaleMv = 0.493 * 0.886 * 714.3;
constexpr double kPalMVScaleMv = 0.877 * 0.701 * 714.3;

constexpr double kPalUvCutoffHz = 1.3e6;
constexpr double kNtscCbCrCutoffHz = 1.2e6;
constexpr int kPalFilterTaps = 33;
constexpr int kNtscFilterTaps = 17;
constexpr int kChromaAxisFractionBits = 20;
constexpr std::int64_t kChromaAxisScale = (1LL << kChromaAxisFractionBits);
constexpr int kFirCoeffFractionBits = 30;
constexpr std::int64_t kFirCoeffScale = (1LL << kFirCoeffFractionBits);

// Overflow envelope for the int32 × int32 → int64 FIR accumulator.
//
// A colour-difference sample is (code − 512) × 2^20 / 448 with the code clamped
// to [64, 960] (ExtractCb/CrAxisFixed), so its magnitude never exceeds
// kChromaAxisScale. A single Q30 tap of a unity-DC-gain low-pass kernel is well
// under 1.0; kMaxFirTapFixed caps it at exactly 1.0 and kMaxFirTaps caps the
// tap count, both enforced in QuantizeKernelToFixed. The static_assert below
// proves the worst-case accumulator stays inside int64.
constexpr std::int64_t kMaxFirTapFixed = kFirCoeffScale;
constexpr int kMaxFirTaps = 129;
static_assert(kMaxFirTaps <= (INT64_MAX / kMaxFirTapFixed) / kChromaAxisScale,
              "Fixed-point chroma FIR accumulator can overflow int64");
static_assert(kMaxFirTapFixed <= INT32_MAX,
              "Quantised chroma FIR taps must fit int32");
static_assert(kChromaAxisScale <= INT32_MAX,
              "Fixed-point colour-difference axis samples must fit int32");

int ClampCode(int code, int lo, int hi) {
  return std::max(lo, std::min(code, hi));
}

std::vector<std::int32_t> QuantizeKernelToFixed(
    const std::vector<double>& kernel) {
  std::vector<std::int32_t> fixed(kernel.size(), 0);
  for (std::size_t i = 0; i < kernel.size(); ++i) {
    const std::int64_t tap = static_cast<std::int64_t>(
        std::llround(kernel[i] * static_cast<double>(kFirCoeffScale)));
    if (tap > kMaxFirTapFixed || tap < -kMaxFirTapFixed) {
      throw std::invalid_argument(
          "Chroma low-pass tap exceeds the Q30 range the fixed-point filter "
          "accumulator is proven against");
    }
    fixed[i] = static_cast<std::int32_t>(tap);
  }

  // Proves the int64 accumulator in ApplyFirFilterFixed cannot overflow: the
  // worst case is every tap at its largest magnitude against a full-scale
  // colour-difference sample. kMaxFirTapFixed bounds a single tap and
  // kMaxFirTaps the tap count, so the product stays far inside int64.
  if (fixed.size() > static_cast<std::size_t>(kMaxFirTaps)) {
    throw std::invalid_argument("Chroma low-pass filter has too many taps");
  }
  return fixed;
}

void ExtractCbAxisFixed(const std::vector<YCbCr444Pixel>& source_samples,
                        std::vector<std::int32_t>* axis_fixed) {
  axis_fixed->resize(source_samples.size());
  for (std::size_t index = 0; index < source_samples.size(); ++index) {
    const int cb = ClampCode(source_samples[index].cb, 64, 960);
    (*axis_fixed)[index] = static_cast<std::int32_t>(
        static_cast<std::int64_t>(cb - 512) * kChromaAxisScale / 448);
  }
}

void ExtractCrAxisFixed(const std::vector<YCbCr444Pixel>& source_samples,
                        std::vector<std::int32_t>* axis_fixed) {
  axis_fixed->resize(source_samples.size());
  for (std::size_t index = 0; index < source_samples.size(); ++index) {
    const int cr = ClampCode(source_samples[index].cr, 64, 960);
    (*axis_fixed)[index] = static_cast<std::int32_t>(
        static_cast<std::int64_t>(cr - 512) * kChromaAxisScale / 448);
  }
}

// Convolution kernel of the chroma low-pass filter.
//
// Both operands are int32 and the accumulator int64, so each tap is a widening
// 32×32→64 signed multiply — the form SSE4.1 `pmuldq` and AVX2 `vpmuldq`
// implement directly, and the form the compiler will vectorise, unlike the
// int64×int64 product it replaces. The buffers are disjoint reusable members,
// declared __restrict so the compiler need not assume they alias.
//
// The widening multiply is absent from the baseline x86-64 (SSE2) target the
// project builds for, so the function is multi-versioned: the loader picks the
// SSE4.2 or AVX2 clone on a capable CPU and the portable default elsewhere.
// Every clone computes the same integer result, so output stays bit-identical
// across machines.
#if defined(__x86_64__) && defined(__has_attribute)
#if __has_attribute(target_clones)
#define VIDEOSYNTH_FIR_MULTIVERSION \
  __attribute__((target_clones("default", "sse4.2", "avx2")))
#endif
#endif
#ifndef VIDEOSYNTH_FIR_MULTIVERSION
#define VIDEOSYNTH_FIR_MULTIVERSION
#endif

VIDEOSYNTH_FIR_MULTIVERSION
void ConvolvePaddedAxis(const std::int32_t* __restrict padded,
                        const std::int32_t* __restrict taps, int kernel_size,
                        std::int32_t* __restrict filtered,
                        std::size_t sample_count) {
  for (std::size_t index = 0; index < sample_count; ++index) {
    std::int64_t acc = 0;
    for (int tap = 0; tap < kernel_size; ++tap) {
      acc += static_cast<std::int64_t>(
                 padded[index + static_cast<std::size_t>(tap)]) *
             static_cast<std::int64_t>(taps[tap]);
    }
    filtered[index] = static_cast<std::int32_t>(
        RoundShiftRightSigned(acc, kFirCoeffFractionBits));
  }
}

// Convolves one colour-difference axis with the shared Q30 kernel, replicating
// the end samples to pad the transient regions.
void ApplyFirFilterFixed(const std::vector<std::int32_t>& input,
                         const std::vector<std::int32_t>& kernel,
                         std::vector<std::int32_t>* output,
                         std::vector<std::int32_t>* padded_workspace) {
  if (input.empty()) {
    output->clear();
    padded_workspace->clear();
    return;
  }

  const int kernel_size = static_cast<int>(kernel.size());
  const int half_width = kernel_size / 2;
  padded_workspace->resize(input.size() +
                           static_cast<std::size_t>(2 * half_width));
  std::fill_n(padded_workspace->begin(), half_width, input.front());
  std::copy(input.begin(), input.end(), padded_workspace->begin() + half_width);
  std::fill(padded_workspace->begin() + half_width +
                static_cast<std::ptrdiff_t>(input.size()),
            padded_workspace->end(), input.back());

  output->resize(input.size());
  ConvolvePaddedAxis(padded_workspace->data(), kernel.data(), kernel_size,
                     output->data(), input.size());
}

// Modulates the two filtered colour-difference axes onto the quadrature
// subcarrier.
//
// The carrier advances exactly π/2 per sample at 4fsc, so the sin/cos pair
// cycles through four exact values and never drifts. Filtered samples are Q20
// fixed point; the division by kChromaAxisScale is exact (a power of two), so
// converting inline costs nothing in accuracy and removes the two intermediate
// double buffers the modulator used to read from.
void ModulateQuadratureFromPhaseStart(const std::vector<std::int32_t>& axis_sin,
                                      const std::vector<std::int32_t>& axis_cos,
                                      double sin_scale_mv, double cos_scale_mv,
                                      double carrier_phase_start_rad,
                                      std::vector<SampleFixed>* out_chroma_mv) {
  if (out_chroma_mv == nullptr) {
    throw std::invalid_argument("Output chroma line pointer must not be null");
  }

  out_chroma_mv->assign(axis_sin.size(), MillivoltsToSampleFixed(0.0));
  if (axis_sin.empty()) {
    return;
  }

  constexpr double kChromaAxisScaleDouble =
      static_cast<double>(kChromaAxisScale);
  double sin_phase = std::sin(carrier_phase_start_rad);
  double cos_phase = std::cos(carrier_phase_start_rad);
  for (std::size_t index = 0; index < axis_sin.size(); ++index) {
    const double axis_sin_value =
        static_cast<double>(axis_sin[index]) / kChromaAxisScaleDouble;
    const double axis_cos_value =
        static_cast<double>(axis_cos[index]) / kChromaAxisScaleDouble;
    (*out_chroma_mv)[index] =
        MillivoltsToSampleFixed((axis_sin_value * sin_scale_mv * sin_phase) +
                                (axis_cos_value * cos_scale_mv * cos_phase));

    const double next_sin = cos_phase;
    const double next_cos = -sin_phase;
    sin_phase = next_sin;
    cos_phase = next_cos;
  }
}

}  // namespace

std::vector<double> DesignChromaLowPassKernel(double cutoff_hz,
                                              double sample_rate_hz, int taps) {
  if (sample_rate_hz <= 0.0 || cutoff_hz <= 0.0 ||
      cutoff_hz >= (sample_rate_hz / 2.0) || taps < 3 || (taps % 2) == 0) {
    throw std::invalid_argument("Invalid low-pass filter design parameters");
  }

  const double normalized_cutoff = cutoff_hz / sample_rate_hz;
  const int half_width = taps / 2;
  std::vector<double> kernel(static_cast<std::size_t>(taps), 0.0);
  double gain_sum = 0.0;
  for (int tap = 0; tap < taps; ++tap) {
    const int offset = tap - half_width;
    const double sinc =
        (offset == 0)
            ? (2.0 * normalized_cutoff)
            : std::sin(2.0 * kPi * normalized_cutoff * offset) / (kPi * offset);
    const double window =
        0.54 - (0.46 * std::cos((2.0 * kPi * static_cast<double>(tap)) /
                                static_cast<double>(taps - 1)));
    kernel[static_cast<std::size_t>(tap)] = sinc * window;
    gain_sum += kernel[static_cast<std::size_t>(tap)];
  }

  for (double& tap : kernel) {
    tap /= gain_sum;
  }
  return kernel;
}

QuadratureChromaEncoder::QuadratureChromaEncoder(double sample_rate_hz,
                                                 double cutoff_hz, int taps,
                                                 double sin_scale_mv,
                                                 double cos_scale_mv)
    // Both colour-difference axes are filtered by an identically designed
    // low-pass kernel, so it is designed and quantised once and shared.
    : filter_taps_fixed_(QuantizeKernelToFixed(
          DesignChromaLowPassKernel(cutoff_hz, sample_rate_hz, taps))),
      sin_scale_mv_(sin_scale_mv),
      cos_scale_mv_(cos_scale_mv) {}

void QuadratureChromaEncoder::EncodeLineFromPhaseStart(
    const std::vector<YCbCr444Pixel>& source_samples,
    double carrier_phase_start_rad,
    std::vector<SampleFixed>* out_chroma_mv) const {
  if (out_chroma_mv == nullptr) {
    throw std::invalid_argument("Output chroma line pointer must not be null");
  }

  ExtractCbAxisFixed(source_samples, &sin_axis_fixed_);
  ExtractCrAxisFixed(source_samples, &cos_axis_fixed_);
  ApplyFirFilterFixed(sin_axis_fixed_, filter_taps_fixed_, &filtered_sin_fixed_,
                      &fir_pad_fixed_);
  ApplyFirFilterFixed(cos_axis_fixed_, filter_taps_fixed_, &filtered_cos_fixed_,
                      &fir_pad_fixed_);

  ModulateQuadratureFromPhaseStart(filtered_sin_fixed_, filtered_cos_fixed_,
                                   sin_scale_mv_, cos_scale_mv_,
                                   carrier_phase_start_rad, out_chroma_mv);
}

// ITU-R BT.1700 Part B Table 1 item 10d: PAL U on sin, V on cos.
PalChromaEncoder::PalChromaEncoder(double sample_rate_hz)
    : QuadratureChromaEncoder(sample_rate_hz, kPalUvCutoffHz, kPalFilterTaps,
                              kPalUScaleMv, kPalVScaleMv) {}

// SMPTE 170M-2004 §A.5 eq (10): NTSC b-y on sin axis, r-y on cos axis.
NtscChromaEncoder::NtscChromaEncoder(double sample_rate_hz)
    : QuadratureChromaEncoder(sample_rate_hz, kNtscCbCrCutoffHz,
                              kNtscFilterTaps, kNtscCbScaleMv, kNtscCrScaleMv) {
}

// ITU-R BT.470-6 Table 2 (M/PAL): same as 625-line PAL — U on sin, V on cos —
// but scaled to System M white level (714.3 mV) per kPalMUScaleMv.
PalMChromaEncoder::PalMChromaEncoder(double sample_rate_hz)
    : QuadratureChromaEncoder(sample_rate_hz, kPalUvCutoffHz, kPalFilterTaps,
                              kPalMUScaleMv, kPalMVScaleMv) {}

std::unique_ptr<IChromaEncoder> CreateChromaEncoder(Standard standard,
                                                    double sample_rate_hz) {
  if (standard == Standard::kPal) {
    return std::make_unique<PalChromaEncoder>(sample_rate_hz);
  }
  if (standard == Standard::kNtsc) {
    return std::make_unique<NtscChromaEncoder>(sample_rate_hz);
  }
  if (standard == Standard::kPalM) {
    return std::make_unique<PalMChromaEncoder>(sample_rate_hz);
  }
  return nullptr;
}

}  // namespace videosynth