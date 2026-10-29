/*
 * File:        edge_smear_filter.cpp
 * Module:      channel
 * Purpose:     Channel readout edge smear of one frame of fixed-point mV
 *              samples (one-pole IIR with circular warm-up).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/edge_smear_filter.h"

#include <algorithm>
#include <cmath>

namespace videosynth {

void ApplyEdgeSmear(double fraction, double tau_samples, SampleFixed* frame,
                    std::size_t count) {
  if (count == 0U || fraction == 0.0) {
    return;
  }

  const double pole = std::exp(-1.0 / tau_samples);
  const double input_gain = 1.0 - pole;
  const double direct_gain = 1.0 - fraction;

  // Prime the low-pass state on the frame's own tail, before any sample is
  // rewritten.
  const auto warmup = std::min(
      count,
      static_cast<std::size_t>(std::ceil(kEdgeSmearWarmupTaus * tau_samples)));
  double state = 0.0;
  for (std::size_t n = count - warmup; n < count; ++n) {
    state = (pole * state) + (input_gain * static_cast<double>(frame[n]));
  }

  for (std::size_t n = 0; n < count; ++n) {
    const auto input = static_cast<double>(frame[n]);
    state = (pole * state) + (input_gain * input);
    frame[n] = static_cast<SampleFixed>(
        std::llround((direct_gain * input) + (fraction * state)));
  }
}

}  // namespace videosynth
