/*
 * File:        secam_chroma_encoder.h
 * Module:      chroma_encoder
 * Purpose:     Defines the 625-line SECAM frequency-modulated chroma encoder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/progressive_frame_source.h"

namespace videosynth {

// Colour-difference component carried by one SECAM line.
//
// ITU-R BT.1700 Annex 1 Part C Table 4 item 10d: D'R and D'B are transmitted
// sequentially, alternately from line to line, on the f_OR and f_OB
// subcarriers respectively.
enum class SecamLineComponent { kDr, kDb };

// Returns the colour-difference component of a frame line.
//
// ITU-R BT.1700 Annex 1 Part C Figure 16: in the first frame of the 4-field
// sequence line 7 carries D'R and line 8 carries D'B. The alternation runs
// continuously through the 625-line (odd) frame, so a given line number swaps
// component every frame.
//
// Args:
//   line_number_1based: Frame line number, 1..625.
//   frame_parity:       0 for the first frame of the 2-frame cycle, 1 for the
//                       second.
SecamLineComponent SecamComponentForLine(int line_number_1based,
                                         int frame_parity);

// Returns the subcarrier phase at 0H for a frame line, in radians (0 or π).
//
// ITU-R BT.1700 Annex 1 Part C Table 4 Note 1: the initial subcarrier phase is
// inverted from frame to frame (0°, 180°, ...) and, within a frame, follows
// the 0°, 0°, 180° line pattern.
double SecamInitialPhaseRadForLine(int line_number_1based, int frame_parity);

// Returns true for the field-blanking lines that carry the SECAM field
// identification signals.
//
// ITU-R BT.1700 Annex 1 Part C Table 4 item 13: lines 7 to 15 (fields 1 and 3)
// and lines 320 to 328 (fields 2 and 4).
bool IsSecamFieldIdentificationLine(int line_number_1based);

// 625-line SECAM chroma encoder.
//
// Produces the complete chrominance subcarrier of one line, sampled from 0H,
// following ITU-R BT.1700 Annex 1 Part C Table 4:
//   1. Item 9: D'R = −1.902 (E'R − E'Y), D'B = 1.505 (E'B − E'Y).
//   2. Item 10c: colour-difference low-pass (≤ 3 dB at 1.3 MHz, ≥ 30 dB at
//      3.5 MHz).
//   3. Item 10f: low-frequency (video) precorrection
//      A(f) = (1 + j f/f1) / (1 + j f/3f1), f1 = 85 kHz, giving D'R*, D'B*.
//   4. Items 5/10e: frequency modulation of f_OR = 4.40625 MHz by
//      ΔfOR = 280 kHz × D'R* or of f_OB = 4.25 MHz by ΔfOB = 230 kHz × D'B*,
//      limited to the maximum deviations (+350/−506 kHz for D'R,
//      +506/−350 kHz for D'B).
//   5. Item 10d: high-frequency precorrection ("bell")
//      G = M0 (1 + j16F) / (1 + j1.26F), F = f/f0 − f0/f, f0 = 4.286 MHz,
//      with 2·M0 = 23 % of the 700 mV luminance amplitude.
//   6. BT.470-6 Table 2 item 2.17a: the subcarrier is blanked from the leading
//      edge of line blanking up to 5.6 µs after 0H.
//
// The back porch between the end of subcarrier blanking and the start of the
// active picture carries the undeviated subcarrier, which is the line
// identification reference of Part C item 13.
//
// Complexity: EncodeLine and EncodeIdentificationLine are O(line samples ×
// low-pass taps).
//
// Thread-safety: NOT thread-safe — the per-line workspaces are mutable
// members. Serialise access or construct one encoder per worker thread.
class SecamChromaEncoder {
 public:
  explicit SecamChromaEncoder(double sample_rate_hz);

  // Encodes one active-picture line.
  //
  // Args:
  //   component:           D'R or D'B line (see SecamComponentForLine).
  //   initial_phase_rad:   Subcarrier phase at 0H (see
  //                        SecamInitialPhaseRadForLine).
  //   active_pixels:       Source pixels of the active window, one per sample.
  //   active_start_sample: Offset of active_pixels[0] from 0H, in samples.
  //   line_samples:        Number of samples in the line (0H to next 0H).
  //   out_chroma_mv:       Receives line_samples chroma samples; caller-owned,
  //                        must be non-null.
  void EncodeLine(SecamLineComponent component, double initial_phase_rad,
                  const std::vector<YCbCr444Pixel>& active_pixels,
                  int active_start_sample, int line_samples,
                  std::vector<SampleFixed>* out_chroma_mv) const;

  // Encodes one field identification line.
  //
  // ITU-R BT.1700 Annex 1 Part C Table 4 item 13 and Figure 17: D'R* rises
  // linearly over 15 µs from 0 to +1.25, D'B* falls linearly over 18 µs from 0
  // to −1.52, then both stay constant to the end of the line. The ramp starts
  // at the back edge of line blanking. The trapezoid is already the
  // precorrected D* signal, so neither the low-pass nor the low-frequency
  // precorrection is applied to it.
  void EncodeIdentificationLine(SecamLineComponent component,
                                double initial_phase_rad, int line_samples,
                                std::vector<SampleFixed>* out_chroma_mv) const;

 private:
  // Modulates deviation_norm_ (D* per sample) onto the line's subcarrier,
  // applies the bell filter and the subcarrier blanking gate.
  void ModulateLine(SecamLineComponent component, double initial_phase_rad,
                    int line_samples,
                    std::vector<SampleFixed>* out_chroma_mv) const;

  double sample_rate_hz_;
  std::vector<double> low_pass_taps_;
  // Low-frequency precorrection, first-order IIR: y = b0 x + b1 x1 − a1 y1.
  double lf_b0_ = 0.0;
  double lf_b1_ = 0.0;
  double lf_a1_ = 0.0;
  // Bell filter, biquad: y = b0 x + b1 x1 + b2 x2 − a1 y1 − a2 y2.
  double bell_b0_ = 0.0;
  double bell_b1_ = 0.0;
  double bell_b2_ = 0.0;
  double bell_a1_ = 0.0;
  double bell_a2_ = 0.0;
  int subcarrier_start_samples_ = 0;
  int front_porch_samples_ = 0;
  int line_blanking_end_samples_ = 0;
  int gate_ramp_samples_ = 0;

  // Per-line scratch buffers, reused across calls (see thread-safety note).
  mutable std::vector<double> colour_difference_;
  mutable std::vector<double> filtered_;
  mutable std::vector<double> deviation_norm_;
  mutable std::vector<double> carrier_;
};

}  // namespace videosynth
