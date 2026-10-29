/*
 * File:        picture_chroma.h
 * Module:      picture_chroma
 * Purpose:     Picture-chroma helpers shared by the chroma error stage and
 *              the FM-beat model: the picture line windows of a 4fsc frame
 *              and the chroma Hilbert transformer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/model.h"

namespace videosynth {

// Half-length of the Hilbert transformer; the kernel has 2 * this + 1 taps,
// of which only the odd offsets are non-zero. With kChromaHilbertKaiserBeta
// the response stays within 1e-4 of the ideal -j from 2 MHz to
// (Nyquist - 2 MHz): 2-6.87 MHz at 625-line 4fsc (17.73 MHz) and
// 2-5.16 MHz at 525-line 4fsc (14.32 MHz), covering every chroma band.
constexpr int kChromaHilbertHalfTaps = 23;
constexpr double kChromaHilbertKaiserBeta = 8.0;

// Kaiser-windowed ideal Hilbert transformer, indexed by offset + half-length:
// y[n] = sum_k kernel[k + half] * x[n - k] maps cos(wn) to sin(wn).
//
// Thread-safety: stateless; safe to call concurrently.
std::vector<double> DesignChromaHilbertKernel();

// Hilbert transform of input[0, count) alone, treating samples outside the
// window as zero, so nothing beyond the window leaks into it.
//
// Args:
//   kernel: A kernel from DesignChromaHilbertKernel().
//   input:  count samples.
//   count:  Window length.
//   output: Receives count samples; must not overlap input.
//
// Thread-safety: stateless; safe to call concurrently on distinct buffers.
void HilbertTransformWindow(const std::vector<double>& kernel,
                            const double* input, std::size_t count,
                            double* output);

// The sample window of one picture line, as offsets into a 4fsc frame.
struct PictureLineWindow {
  std::size_t start = 0;
  std::size_t count = 0;
  int line_1based = 0;
};

// Samples kept either side of the burst gate in a burst window, so a burst
// moved by the channel's largest timing offset (8 samples) stays inside it.
constexpr int kBurstWindowGuardSamples = 8;

// Picture lines of one 4fsc frame in frame order: the active window of every
// horizontal-sync active picture line of the active raster, less any line a
// project VITS is placed on.
//
// Thread-safety: stateless; safe to call concurrently.
std::vector<PictureLineWindow> BuildPictureLineWindows(const Project& project,
                                                       Standard standard);

// Burst windows of one 4fsc frame, one per frame line in frame order: the
// burst gate widened by kBurstWindowGuardSamples on each side. A line that
// carries no burst on a given frame has zero chroma there.
//
// Thread-safety: stateless; safe to call concurrently.
std::vector<PictureLineWindow> BuildBurstLineWindows(Standard standard);

}  // namespace videosynth
