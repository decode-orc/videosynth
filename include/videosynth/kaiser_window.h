/*
 * File:        kaiser_window.h
 * Module:      kaiser_window
 * Purpose:     Kaiser window used by the windowed-FIR impairment filters.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cmath>

namespace videosynth {

// Thread-safety: all functions in this module are thread-safe pure functions.

// Zeroth-order modified Bessel function of the first kind, by its power
// series; converges quickly for the argument range a Kaiser window uses.
inline double BesselI0(double x) {
  const double quarter_x_squared = 0.25 * x * x;
  double term = 1.0;
  double sum = 1.0;
  for (int k = 1; k < 64; ++k) {
    term *= quarter_x_squared / (static_cast<double>(k) * k);
    sum += term;
    if (term < sum * 1e-17) {
      break;
    }
  }
  return sum;
}

// Kaiser window value at ratio = (distance from centre) / (half-length),
// unity at the centre and zero beyond |ratio| = 1.
inline double KaiserWindow(double ratio, double beta) {
  return BesselI0(beta * std::sqrt(std::max(0.0, 1.0 - (ratio * ratio)))) /
         BesselI0(beta);
}

}  // namespace videosynth
