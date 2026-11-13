/*
 * File:        signal_shaping.cpp
 * Module:      signal_shaping
 * Purpose:     Shared transition shaping helpers for CVBS timing sections.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/signal_shaping.h"

#include <algorithm>
#include <cmath>

namespace videosynth {
namespace {

// Binary search iteration count for InverseSCurve01. 48 iterations provides
// sufficient precision for double-precision inverse of the S-curve.
constexpr int kInverseSCurveIterations = 48;

// Minimum number of samples for a non-degenerate S-curve transition.
// Fewer than 4 samples would produce a degenerate (flat or single-step) curve.
constexpr int kMinRampSamples = 4;

// Factor for converting half-amplitude time to full transition time.
// Half-amplitude spans start->50%, so full transition is 2x the half-amplitude
// span.
constexpr double kHalfToFullTransitionFactor = 2.0;

double ClampUnitInterval(double value) {
  return std::max(0.0, std::min(1.0, value));
}

double SCurve01(double x) {
  const double clamped = ClampUnitInterval(x);
  // Quintic smootherstep gives an S-curve with zero slope and curvature at
  // the endpoints, reducing sharp corners at transition start/end.
  return clamped * clamped * clamped *
         ((clamped * ((6.0 * clamped) - 15.0)) + 10.0);
}

double InverseSCurve01(double y) {
  const double target = ClampUnitInterval(y);
  if (target <= 0.0) {
    return 0.0;
  }
  if (target >= 1.0) {
    return 1.0;
  }

  double lo = 0.0;
  double hi = 1.0;

  for (int i = 0; i < kInverseSCurveIterations; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (SCurve01(mid) < target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  return 0.5 * (lo + hi);
}

}  // namespace

// Standard rise/fall time measurement fractions: 10% to 90% amplitude.
// ITU-R BT.1700 and SMPTE 170M specify sync edge timing at these levels.
constexpr double kStandardRiseLowFraction = 0.1;
constexpr double kStandardRiseHighFraction = 0.9;

int RiseTimeToRampSamples(double rise_time_seconds, double sample_rate_hz) {
  // Most sync-edge specs quote rise/fall times between 10%-90% amplitude.
  return TransitionTimeToRampSamples(rise_time_seconds, sample_rate_hz,
                                     kStandardRiseLowFraction,
                                     kStandardRiseHighFraction);
}

int TransitionTimeToRampSamples(double transition_time_seconds,
                                double sample_rate_hz,
                                double low_amplitude_fraction,
                                double high_amplitude_fraction) {
  if (transition_time_seconds <= 0.0 || sample_rate_hz <= 0.0) {
    return kMinRampSamples;
  }

  const double low = ClampUnitInterval(low_amplitude_fraction);
  const double high = ClampUnitInterval(high_amplitude_fraction);
  if (high <= low) {
    return kMinRampSamples;
  }

  // Convert a measured interval (for example 10%-90% or 50%-50%) into the
  // full 0%-100% S-curve duration used by sample-domain synthesis.
  const double low_progress = InverseSCurve01(low);
  const double high_progress = InverseSCurve01(high);
  const double measured_fraction =
      std::max(1e-12, high_progress - low_progress);
  const double measured_samples = transition_time_seconds * sample_rate_hz;
  const double full_transition_samples = measured_samples / measured_fraction;

  // Keep at least kMinRampSamples samples so discrete S-curves are not
  // degenerate.
  return std::max(kMinRampSamples,
                  static_cast<int>(std::ceil(full_transition_samples)));
}

int HalfAmplitudeTimeToRampSamples(double half_amplitude_time_seconds,
                                   double sample_rate_hz) {
  if (half_amplitude_time_seconds <= 0.0 || sample_rate_hz <= 0.0) {
    return kMinRampSamples;
  }

  // Half-amplitude timing spans start->50%; the complete edge is twice this.
  return TransitionTimeToRampSamples(
      kHalfToFullTransitionFactor * half_amplitude_time_seconds, sample_rate_hz,
      0.0, 1.0);
}

double ShapedPulseLevel(int relative_index, int pulse_width_samples,
                        int ramp_samples, double baseline_level,
                        double pulse_level) {
  if (relative_index < 0 || relative_index >= pulse_width_samples ||
      pulse_width_samples <= 0) {
    return baseline_level;
  }

  const int ramp =
      std::max(1, std::min(ramp_samples, std::max(1, pulse_width_samples / 2)));
  const int trailing_start = pulse_width_samples - ramp;
  double depth = 1.0;

  if (relative_index < ramp) {
    const double x = static_cast<double>(relative_index) /
                     static_cast<double>(std::max(1, ramp - 1));
    depth = SCurve01(x);
  } else if (relative_index >= trailing_start) {
    const int trailing_index = relative_index - trailing_start;
    const double x = static_cast<double>((ramp - 1) - trailing_index) /
                     static_cast<double>(std::max(1, ramp - 1));
    depth = SCurve01(x);
  }

  return baseline_level + ((pulse_level - baseline_level) * depth);
}

double ShapedPulseLevelAt(double t, double fall_half_time,
                          double rise_half_time, int ramp_samples,
                          double baseline_level, double pulse_level) {
  const auto span = static_cast<double>(std::max(1, ramp_samples - 1));
  const double leading = SCurve01(((t - fall_half_time) / span) + 0.5);
  const double trailing = SCurve01(((rise_half_time - t) / span) + 0.5);
  const double depth = std::min(leading, trailing);
  return baseline_level + ((pulse_level - baseline_level) * depth);
}

double ShapedGateEnvelope(int relative_index, int gate_width_samples,
                          int ramp_samples) {
  if (relative_index < 0 || relative_index >= gate_width_samples ||
      gate_width_samples <= 0) {
    return 0.0;
  }

  const int ramp =
      std::max(1, std::min(ramp_samples, std::max(1, gate_width_samples / 2)));
  const int trailing_start = gate_width_samples - ramp;

  if (relative_index < ramp) {
    const double x = static_cast<double>(relative_index) /
                     static_cast<double>(std::max(1, ramp - 1));
    return SCurve01(x);
  }
  if (relative_index >= trailing_start) {
    const int trailing_index = relative_index - trailing_start;
    const double x = static_cast<double>((ramp - 1) - trailing_index) /
                     static_cast<double>(std::max(1, ramp - 1));
    return SCurve01(x);
  }

  return 1.0;
}

}  // namespace videosynth
