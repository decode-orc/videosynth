/*
 * File:        signal_shaping.h
 * Module:      signal_shaping
 * Purpose:     Shared transition shaping helpers for CVBS timing sections.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

namespace videosynth {

// Thread-safety: All functions in this module are thread-safe (stateless pure
// functions that only operate on their parameters). They may be called
// concurrently from multiple threads.
int RiseTimeToRampSamples(double rise_time_seconds, double sample_rate_hz);

int TransitionTimeToRampSamples(double transition_time_seconds,
                                double sample_rate_hz,
                                double low_amplitude_fraction,
                                double high_amplitude_fraction);

int HalfAmplitudeTimeToRampSamples(double half_amplitude_time_seconds,
                                   double sample_rate_hz);

double ShapedPulseLevel(int relative_index, int pulse_width_samples,
                        int ramp_samples, double baseline_level,
                        double pulse_level);

// Level at time `t` (in samples) of a pulse from baseline_level to
// pulse_level whose falling and rising edges cross half amplitude at
// `fall_half_time` and `rise_half_time`. Each edge is the S-curve
// ShapedPulseLevel uses, spanning ramp_samples - 1 sample periods centred on
// its half-amplitude time, so the half-amplitude points and the pulse width
// between them may fall between samples.
double ShapedPulseLevelAt(double t, double fall_half_time,
                          double rise_half_time, int ramp_samples,
                          double baseline_level, double pulse_level);

double ShapedGateEnvelope(int relative_index, int gate_width_samples,
                          int ramp_samples);

}  // namespace videosynth
