/*
 * File:        video_band_limit.h
 * Module:      generation
 * Purpose:     Always-on band limit of active picture luma and OSD overlays
 *              to the standard's nominal video bandwidth, so no picture
 *              transition is faster than the system can carry.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/luma_bandwidth_filter.h"
#include "videosynth/model.h"

namespace videosynth {

// Pass and stop band edges of a standard's video band limit, in Hz.
struct VideoBandwidth {
  double passband_edge_hz = 0.0;
  double stopband_edge_hz = 0.0;
};

// The passband is the widest nominal video bandwidth of ITU-R BT.470-6
// Table 1 item 6 among the broadcast systems that carry the standard: 6 MHz
// for 625-line PAL and SECAM (systems D, K, L), 4.2 MHz for NTSC and PAL-M
// (system M). The stopband edge sits below half the 4fsc sample rate, so the
// band-limited picture holds nothing a fractional delay of the line
// (LatticeLineShifter) cannot reproduce: 8.0 MHz on 625 lines (fs / 2 =
// 8.87 MHz) and 6.5 MHz on 525 lines (fs / 2 = 7.16 MHz).
//
// Thread-safety: pure function.
VideoBandwidth NominalVideoBandwidth(Standard standard);

// A linear-phase Kaiser-windowed-sinc low-pass (LumaBandwidthFilter's design)
// between NominalVideoBandwidth's edges: within 0.003 dB of unity in the
// passband and designed for kLumaBandwidthStopbandDb (70 dB) of stopband
// attenuation from the stopband edge to fs / 2; Kaiser's length estimate
// leaves the worst stopband ripple within 0.5 dB of that.
//
// A hard step through it rings by about 9 % of the step either side of the
// edge (the Gibbs overshoot of any filter this flat). That is what a real
// band-limited source carries at a hard edge.
class VideoBandLimiter {
 public:
  // Caller-owned buffers for Apply, resized as needed.
  struct Workspace {
    std::vector<double> padded;
    std::vector<double> accumulator;
  };

  VideoBandLimiter(Standard standard, double sample_rate_hz);

  // Samples the output reaches beyond either end of the input.
  int reach() const { return reach_; }
  const std::vector<double>& kernel() const { return filter_.kernel(); }

  // Band-limits in[0 .. count - 1] as a run of samples with background on
  // both sides of it, and writes count + 2 * reach() samples to out: out[k]
  // is the output at input position k - reach(). Outputs are rounded to the
  // nearest fixed-point step. A run at background throughout comes out at
  // background.
  //
  // Thread-safety: const; safe to call concurrently with distinct
  // workspaces and output buffers.
  void Apply(const SampleFixed* in, std::size_t count, SampleFixed background,
             Workspace* workspace, SampleFixed* out) const;

 private:
  LumaBandwidthFilter filter_;
  int reach_ = 0;
};

}  // namespace videosynth
