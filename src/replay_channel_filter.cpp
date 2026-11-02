/*
 * File:        replay_channel_filter.cpp
 * Module:      channel
 * Purpose:     LaserVision replay channel FIR: the group-delay
 *              pre-distortion and the replay amplitude response of one frame
 *              of fixed-point mV samples.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/replay_channel_filter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "videosynth/noise_spectrum_filter.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

double InterpolateGroupDelayNs(const std::vector<GroupDelayPoint>& curve,
                               double frequency_mhz) {
  if (curve.empty()) {
    return 0.0;
  }
  if (frequency_mhz <= curve.front().frequency_mhz) {
    return curve.front().delay_ns;
  }
  for (std::size_t i = 1; i < curve.size(); ++i) {
    const GroupDelayPoint& lower = curve[i - 1];
    const GroupDelayPoint& upper = curve[i];
    if (frequency_mhz <= upper.frequency_mhz) {
      const double fraction = (frequency_mhz - lower.frequency_mhz) /
                              (upper.frequency_mhz - lower.frequency_mhz);
      return lower.delay_ns + (fraction * (upper.delay_ns - lower.delay_ns));
    }
  }
  return curve.back().delay_ns;
}

double ReplayResponseDb(const LaserdiscChannelParameters& channel,
                        double picture, double frequency_mhz) {
  if (!channel.response_db.empty()) {
    return InterpolateNoiseSpectrumDb(channel.response_db, frequency_mhz);
  }
  const std::vector<ResponseByPictureEntry>& entries =
      channel.response_by_picture;
  if (entries.empty()) {
    return 0.0;
  }
  if (picture <= entries.front().picture) {
    return InterpolateNoiseSpectrumDb(entries.front().response_db,
                                      frequency_mhz);
  }
  for (std::size_t i = 1; i < entries.size(); ++i) {
    const ResponseByPictureEntry& lower = entries[i - 1];
    const ResponseByPictureEntry& upper = entries[i];
    if (picture <= upper.picture) {
      const double fraction =
          (picture - lower.picture) /
          static_cast<double>(upper.picture - lower.picture);
      const double lower_db =
          InterpolateNoiseSpectrumDb(lower.response_db, frequency_mhz);
      const double upper_db =
          InterpolateNoiseSpectrumDb(upper.response_db, frequency_mhz);
      return lower_db + (fraction * (upper_db - lower_db));
    }
  }
  return InterpolateNoiseSpectrumDb(entries.back().response_db, frequency_mhz);
}

ReplayChannelFilter::ReplayChannelFilter(
    const std::function<double(double)>& amplitude_db,
    const std::function<double(double)>& delay_ns, double sample_rate_hz) {
  if (!(sample_rate_hz > 0.0)) {
    throw std::invalid_argument(
        "ReplayChannelFilter: sample rate must be positive");
  }

  constexpr int kTaps = kReplayChannelTaps;
  constexpr int kCentre = (kTaps - 1) / 2;
  const double bin_hz = sample_rate_hz / static_cast<double>(kTaps);

  // Amplitude and phase on the lower half of the DFT grid; the upper half is
  // the complex conjugate, so the kernel is real. The phase is the running
  // integral of the group delay, by the trapezoidal rule on a grid 16 times
  // finer than the bins.
  constexpr int kPhaseSubsteps = 16;
  std::vector<double> amplitude(static_cast<std::size_t>(kCentre) + 1U);
  std::vector<double> phase(static_cast<std::size_t>(kCentre) + 1U, 0.0);
  double accumulated = 0.0;
  for (int k = 0; k <= kCentre; ++k) {
    const double frequency_hz = static_cast<double>(k) * bin_hz;
    if (k > 0) {
      const double step_hz = bin_hz / kPhaseSubsteps;
      for (int sub = 0; sub < kPhaseSubsteps; ++sub) {
        const double f0 = frequency_hz - bin_hz + (sub * step_hz);
        const double tau0 = delay_ns(f0 / 1.0e6) * 1.0e-9;
        const double tau1 = delay_ns((f0 + step_hz) / 1.0e6) * 1.0e-9;
        accumulated += 0.5 * (tau0 + tau1) * step_hz;
      }
    }
    amplitude[static_cast<std::size_t>(k)] =
        std::pow(10.0, amplitude_db(frequency_hz / 1.0e6) / 20.0);
    phase[static_cast<std::size_t>(k)] = -2.0 * kPi * accumulated;
  }

  kernel_.assign(static_cast<std::size_t>(kTaps), 0.0);
  double dc_gain = 0.0;
  for (int n = 0; n < kTaps; ++n) {
    const int offset = n - kCentre;
    double sum = amplitude[0];
    for (int k = 1; k <= kCentre; ++k) {
      const auto bin = static_cast<std::size_t>(k);
      sum += 2.0 * amplitude[bin] *
             std::cos((2.0 * kPi * static_cast<double>(k) *
                       static_cast<double>(offset) / kTaps) +
                      phase[bin]);
    }
    // Hann window with non-zero end taps, unity at the centre.
    const double window =
        0.5 - (0.5 * std::cos(2.0 * kPi * static_cast<double>(n + 1) /
                              static_cast<double>(kTaps + 1)));
    const double tap = (sum / static_cast<double>(kTaps)) * window;
    kernel_[static_cast<std::size_t>(n)] = tap;
    dc_gain += tap;
  }

  const double scale = amplitude[0] / dc_gain;
  for (double& tap : kernel_) {
    tap *= scale;
  }
}

void ReplayChannelFilter::Apply(SampleFixed* frame, std::size_t count) const {
  if (count == 0U) {
    return;
  }
  constexpr std::size_t kCentre = (kReplayChannelTaps - 1) / 2;
  const std::size_t taps = kernel_.size();

  // ext[j] = frame[(j - kCentre) mod count], so output[n] =
  // sum_r reversed[r] * ext[n + r] with reversed[r] = h[kCentre - r].
  std::vector<double> ext(count + taps - 1U);
  for (std::size_t j = 0; j < ext.size(); ++j) {
    const std::size_t source = (j + count - (kCentre % count)) % count;
    ext[j] = static_cast<double>(frame[source]);
  }
  const std::vector<double> reversed(kernel_.rbegin(), kernel_.rend());
  for (std::size_t n = 0; n < count; ++n) {
    const double* window = &ext[n];
    double acc = 0.0;
    for (std::size_t r = 0; r < taps; ++r) {
      acc += reversed[r] * window[r];
    }
    frame[n] = static_cast<SampleFixed>(std::llround(acc));
  }
}

}  // namespace videosynth
