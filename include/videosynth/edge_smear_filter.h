/*
 * File:        edge_smear_filter.h
 * Module:      channel
 * Purpose:     Channel readout edge smear of one frame of fixed-point mV
 *              samples (one-pole IIR with circular warm-up).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>

#include "videosynth/fixed_point.h"

namespace videosynth {

// Warm-up length in time constants. The IIR state is primed by running it over
// the last ceil(kEdgeSmearWarmupTaus * tau) samples of the same frame, so the
// residual of the unprimed start is below exp(-20) (~2e-9) of the signal.
constexpr double kEdgeSmearWarmupTaus = 20.0;

// Applies H(z) = (1 - c) + c (1 - a) / (1 - a z^-1), a = exp(-1 / tau), to
// one frame in place: a fraction c of the signal passes through a one-pole
// low-pass with time constant tau samples. The step response is
// 1 - c a^(n + 1) and the DC gain is 1. The frame is treated as circular: the
// filter state is warmed up over the frame's own tail, so the result depends
// on that frame alone.
//
// Results are rounded to the nearest fixed-point step and are not clamped.
//
// Args:
//   fraction:    c, in [0, 0.5].
//   tau_samples: Time constant, in samples; positive.
//   frame:       count samples, rewritten in place.
//   count:       Frame length.
//
// Thread-safety: stateless; safe to call concurrently on distinct buffers.
void ApplyEdgeSmear(double fraction, double tau_samples, SampleFixed* frame,
                    std::size_t count);

}  // namespace videosynth
