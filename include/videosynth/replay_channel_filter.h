/*
 * File:        replay_channel_filter.h
 * Module:      channel
 * Purpose:     LaserVision replay channel FIR: the group-delay
 *              pre-distortion and the replay amplitude response of one frame
 *              of fixed-point mV samples.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"

namespace videosynth {

// Kernel length of the replay channel filter. Odd, so the kernel has an
// integer centre. The frequency-sampling grid is fs / 255: about 70 kHz at
// 625-line 4fsc (17.73 MHz) and 56 kHz at 525-line 4fsc (14.32 MHz).
constexpr int kReplayChannelTaps = 255;

// Interpolates a group-delay curve at frequency_mhz, in ns: breakpoints are
// joined by straight lines and the end values are held beyond them. An empty
// curve is zero.
//
// Thread-safety: stateless; safe to call concurrently.
double InterpolateGroupDelayNs(const std::vector<GroupDelayPoint>& curve,
                               double frequency_mhz);

// The replay amplitude response, in dB, of the picture numbered `picture`:
// response_db when set, otherwise response_by_picture interpolated linearly
// in picture number (and in dB, frequency by frequency) between the entries
// that bracket it, holding the end entries beyond them. 0 dB when neither is
// set.
//
// Thread-safety: stateless; safe to call concurrently.
double ReplayResponseDb(const LaserdiscChannelParameters& channel,
                        double picture, double frequency_mhz);

// A FIR with the complex response
//   H(f) = 10^(A(f)/20) * exp(-j 2 pi integral_0^f tau(v) dv),
// so its amplitude follows A(f) in dB and its group delay follows tau(f)
// (negative tau arrives early). The kernel is centred, so with tau = 0 it is
// linear-phase with no delay.
//
// Design (frequency sampling): H is sampled on the kReplayChannelTaps-point
// DFT grid from 0 Hz to Nyquist, inverse-transformed directly about the
// centre tap, Hann windowed, and scaled so the DC gain is exactly
// 10^(A(0)/20): flat levels (blanking, sync tips) pass at the response's own
// DC gain.
//
// Thread-safety: immutable after construction; Apply is safe to call
// concurrently on distinct buffers.
class ReplayChannelFilter {
 public:
  // amplitude_db and delay_ns take a frequency in MHz. Throws
  // std::invalid_argument when sample_rate_hz is not positive.
  ReplayChannelFilter(const std::function<double(double)>& amplitude_db,
                      const std::function<double(double)>& delay_ns,
                      double sample_rate_hz);

  // Taps h[m] for m = -(kReplayChannelTaps - 1) / 2 .. +(kReplayChannelTaps
  // - 1) / 2, applied as output[n] = sum h[m] * input[n - m].
  const std::vector<double>& kernel() const { return kernel_; }

  // Filters frame[0, count) in place, treating the frame as circular so the
  // result depends on that frame alone. The frame boundary lies in vertical
  // blanking, so the wrapped samples are sync and equalising pulses only.
  // Results are rounded to the nearest fixed-point step and are not clamped.
  void Apply(SampleFixed* frame, std::size_t count) const;

 private:
  std::vector<double> kernel_;
};

}  // namespace videosynth
