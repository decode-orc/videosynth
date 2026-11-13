/*
 * File:        lattice_line_shifter.h
 * Module:      lattice_line_shifter
 * Purpose:     Moves each storage line of a synthesised 4fsc frame from its
 *              0H-relative render onto the subcarrier-locked sampling lattice
 *              by delaying it by the line's lattice delay.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/timing_offset_filter.h"

namespace videosynth {

// The generator renders every line relative to its own 0H: storage sample j
// of line k holds the waveform j samples after the line's first sampling
// instant on a line-locked grid. On PAL and SECAM that grid is not the 4fsc
// lattice. Line k's 0H lies d_k = LatticeLineDelay(standard, k + 1) samples
// later on the lattice than on the render (EBU Tech. 3280-E §1.2), so the
// shifter delays each line by d_k:
//
//   out(offset_k + n) += render_k(n - d_k) - background
//
// render_k is line k's own samples, with `lead_in` just before its first
// sample and `background` everywhere else. The output starts at `background`
// everywhere. Each line is delayed from its own render alone, so no line
// leaks into its neighbour at the wrong delay. A line's delayed waveform may
// reach past either end of its storage line, for example the pre-0H half of
// its sync edge. That part is added into the neighbouring storage line. Line 1
// wraps to the frame's last line, because every frame begins and ends alike.
//
// The delay is TimingOffsetFilter's Kaiser-windowed sinc (32 taps, beta 8),
// flat within 0.001 dB and 0.0002 sample up to 6 MHz. On NTSC and PAL-M every
// d_k is 0, and the shifter only adds each lead_in into the previous line.
//
// Thread-safety: immutable after construction; Apply may be called
// concurrently with distinct frames and workspaces.
class LatticeLineShifter {
 public:
  // `line_sample_counts` is the storage layout of BuildLineSampleCounts.
  LatticeLineShifter(Standard standard,
                     const std::vector<int>& line_sample_counts);

  // Per-line working storage, reused across calls by one caller.
  struct Workspace {
    std::vector<SampleFixed> input;
    std::vector<double> line;
    std::vector<double> output;
  };

  // True when every line's delay is zero.
  bool all_delays_zero() const { return all_delays_zero_; }

  // Delay of storage line `line_index` (0-based), in samples.
  double LineDelaySamples(std::size_t line_index) const;

  // Replaces `frame`, one frame in the render domain, with its lattice-domain
  // equivalent. `lead_in` holds the samples just before each line's first
  // sample in its render, the same for every line. It holds at most
  // kMaxLeadInSamples samples. Values are rounded to the nearest fixed-point
  // step and are not clamped. With an empty lead_in and zero delays this does
  // nothing. Complexity O(frame samples x 32).
  void Apply(SampleFixed* frame, SampleFixed background,
             const std::vector<SampleFixed>& lead_in,
             Workspace* workspace) const;

  // `lattice` holds Apply's result for the frame `old_render`. Updates it to
  // Apply's result for `new_render`, re-delaying only the lines whose render
  // differs, exactly: the result is byte-identical to Apply(new_render).
  // Complexity O(frame samples) to compare, plus O(changed line samples x 64).
  void Update(const SampleFixed* old_render, const SampleFixed* new_render,
              SampleFixed* lattice, SampleFixed background,
              const std::vector<SampleFixed>& lead_in,
              Workspace* workspace) const;

  static constexpr int kMaxLeadInSamples = 8;

 private:
  // The last kMaxLeadInSamples samples of a lead-in.
  struct LeadIn {
    const SampleFixed* first = nullptr;
    int samples = 0;
  };
  static LeadIn TrimLeadIn(const std::vector<SampleFixed>& lead_in);

  // Adds `sign` times line `index`'s delayed render, rounded sample by sample
  // and less the background, into `frame`, including what reaches into the
  // neighbouring lines. `render` is a whole render-domain frame.
  void AddLineContribution(std::size_t index, const SampleFixed* render,
                           SampleFixed background, const LeadIn& lead, int sign,
                           SampleFixed* frame, Workspace* workspace) const;

  std::vector<int> line_sample_counts_;
  std::vector<int> line_sample_offsets_;
  std::vector<TimingOffsetFilter> line_filters_;
  int frame_samples_ = 0;
  bool all_delays_zero_ = true;
};

}  // namespace videosynth
