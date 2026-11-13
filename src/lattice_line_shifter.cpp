/*
 * File:        lattice_line_shifter.cpp
 * Module:      lattice_line_shifter
 * Purpose:     Moves each storage line of a synthesised 4fsc frame from its
 *              0H-relative render onto the subcarrier-locked sampling lattice
 *              by delaying it by the line's lattice delay.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/lattice_line_shifter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "videosynth/frame_line_layout.h"

namespace videosynth {

namespace {

constexpr int kHalf = kTimingOffsetHalfTaps;
constexpr int kTaps = 2 * kHalf;

// Output samples computed either side of a storage line: they cover the lead
// in, the integer part of the delay and the kernel's reach.
constexpr int kSpillSamples = LatticeLineShifter::kMaxLeadInSamples + kHalf + 2;
// Render samples held either side of a storage line: the spill plus the
// kernel's reach from the outermost spill sample.
constexpr int kRenderMargin = kSpillSamples + kHalf + 2;

std::size_t WrapIndex(std::int64_t position, std::int64_t count) {
  std::int64_t wrapped = position % count;
  if (wrapped < 0) {
    wrapped += count;
  }
  return static_cast<std::size_t>(wrapped);
}

}  // namespace

LatticeLineShifter::LatticeLineShifter(
    Standard standard, const std::vector<int>& line_sample_counts)
    : line_sample_counts_(line_sample_counts),
      line_sample_offsets_(BuildLineSampleOffsets(line_sample_counts)) {
  line_filters_.reserve(line_sample_counts.size());
  for (std::size_t index = 0; index < line_sample_counts.size(); ++index) {
    const double delay =
        LatticeLineDelay(standard, static_cast<int>(index) + 1).Samples();
    line_filters_.emplace_back(delay);
    if (delay != 0.0) {
      all_delays_zero_ = false;
    }
    frame_samples_ += line_sample_counts[index];
  }
}

double LatticeLineShifter::LineDelaySamples(std::size_t line_index) const {
  const TimingOffsetFilter& filter = line_filters_[line_index];
  return static_cast<double>(filter.integer_shift()) + filter.fraction();
}

void LatticeLineShifter::Apply(SampleFixed* frame, SampleFixed background,
                               const std::vector<SampleFixed>& lead_in,
                               Workspace* workspace) const {
  if (frame_samples_ <= 0 || (all_delays_zero_ && lead_in.empty())) {
    return;
  }
  const LeadIn lead = TrimLeadIn(lead_in);
  const auto frame_count = static_cast<std::int64_t>(frame_samples_);

  if (all_delays_zero_) {
    // Every line stays where it is; only each lead-in lands in the tail of
    // the line before (the frame's last line, for line 1).
    for (const int line_offset : line_sample_offsets_) {
      for (int i = 0; i < lead.samples; ++i) {
        frame[WrapIndex(
            static_cast<std::int64_t>(line_offset) - lead.samples + i,
            frame_count)] += lead.first[i] - background;
      }
    }
    return;
  }

  std::vector<SampleFixed>& input = workspace->input;
  input.assign(frame, frame + frame_samples_);
  std::fill_n(frame, frame_samples_, background);
  for (std::size_t index = 0; index < line_sample_counts_.size(); ++index) {
    AddLineContribution(index, input.data(), background, lead, 1, frame,
                        workspace);
  }
}

void LatticeLineShifter::Update(const SampleFixed* old_render,
                                const SampleFixed* new_render,
                                SampleFixed* lattice, SampleFixed background,
                                const std::vector<SampleFixed>& lead_in,
                                Workspace* workspace) const {
  const LeadIn lead = TrimLeadIn(lead_in);
  for (std::size_t index = 0; index < line_sample_counts_.size(); ++index) {
    const int line_offset = line_sample_offsets_[index];
    const int line_samples = line_sample_counts_[index];
    if (std::equal(old_render + line_offset,
                   old_render + line_offset + line_samples,
                   new_render + line_offset)) {
      continue;
    }
    if (all_delays_zero_) {
      // A line's own samples carry its render unchanged.
      for (int j = 0; j < line_samples; ++j) {
        lattice[line_offset + j] +=
            new_render[line_offset + j] - old_render[line_offset + j];
      }
      continue;
    }
    // Each line's contribution is rounded on its own before it is added, so
    // taking the old one away and adding the new one gives exactly what
    // Apply would.
    AddLineContribution(index, old_render, background, lead, -1, lattice,
                        workspace);
    AddLineContribution(index, new_render, background, lead, 1, lattice,
                        workspace);
  }
}

LatticeLineShifter::LeadIn LatticeLineShifter::TrimLeadIn(
    const std::vector<SampleFixed>& lead_in) {
  const auto samples = static_cast<int>(
      std::min<std::size_t>(lead_in.size(), kMaxLeadInSamples));
  return LeadIn{
      .first =
          lead_in.data() + (lead_in.size() - static_cast<std::size_t>(samples)),
      .samples = samples,
  };
}

void LatticeLineShifter::AddLineContribution(std::size_t index,
                                             const SampleFixed* render,
                                             SampleFixed background,
                                             const LeadIn& lead, int sign,
                                             SampleFixed* frame,
                                             Workspace* workspace) const {
  const int line_samples = line_sample_counts_[index];
  const int line_offset = line_sample_offsets_[index];
  const TimingOffsetFilter& filter = line_filters_[index];
  const auto frame_count = static_cast<std::int64_t>(frame_samples_);

  // The line's render less the background: zero, the lead-in just before
  // sample 0, then the line's own samples.
  std::vector<double>& line = workspace->line;
  line.assign(static_cast<std::size_t>(line_samples) +
                  (2U * static_cast<std::size_t>(kRenderMargin)),
              0.0);
  double* const render_origin = line.data() + kRenderMargin;
  for (int i = 0; i < lead.samples; ++i) {
    render_origin[i - lead.samples] =
        static_cast<double>(lead.first[i] - background);
  }
  const SampleFixed* const source = render + line_offset;
  for (int j = 0; j < line_samples; ++j) {
    render_origin[j] = static_cast<double>(source[j] - background);
  }

  // output(n) = sum h[m] render(n - shift - m), m = -kHalf + 1 .. kHalf, for
  // n = -kSpillSamples .. line_samples + kSpillSamples - 1. Taking the taps
  // in the outer loop leaves a contiguous inner loop over n that the
  // compiler vectorises, while each output still sums its taps in order.
  // kernel()[i] is h[i - kHalf + 1]; a whole-sample delay has no kernel.
  const auto shift = static_cast<int>(filter.integer_shift());
  const int first = -kSpillSamples;
  const int count = line_samples + (2 * kSpillSamples);
  std::vector<double>& output = workspace->output;
  output.assign(static_cast<std::size_t>(count), 0.0);
  double* const out = output.data();
  const std::vector<double>& kernel = filter.kernel();
  if (kernel.empty()) {
    const double* const in = render_origin + (first - shift);
    for (int n = 0; n < count; ++n) {
      out[n] = in[n];
    }
  } else {
    for (int i = 0; i < kTaps; ++i) {
      const double tap = kernel[static_cast<std::size_t>(i)];
      const double* const in =
          render_origin + (first - shift - (i - kHalf + 1));
      for (int n = 0; n < count; ++n) {
        out[n] += tap * in[n];
      }
    }
  }

  // Add the delayed line into its storage line, and what reaches past either
  // end into the neighbouring lines.
  for (int n = 0; n < count; ++n) {
    const int position = first + n;
    const SampleFixed difference =
        sign * static_cast<SampleFixed>(std::llround(out[n]));
    if (position >= 0 && position < line_samples) {
      frame[line_offset + position] += difference;
    } else {
      frame[WrapIndex(static_cast<std::int64_t>(line_offset) + position,
                      frame_count)] += difference;
    }
  }
}

}  // namespace videosynth
