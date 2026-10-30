/*
 * File:        fm_code_generator.h
 * Module:      fm_code_generator
 * Purpose:     40-bit FM code type generators for NTSC LaserDisc VBI injection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

#include "videosynth/biphase_types.h"
#include "videosynth/fm_encoder.h"
#include "videosynth/model.h"

namespace videosynth {

// Generates FmData for NTSC CAV 40-bit FM picture number codes.
//
// Section-aware frozen value behaviour (IEC 60857 Appendix F):
//   kLeadIn:        value emitted = 0 (frozen at zero)
//   kProgrammeArea: value auto-increments from start_value on each Advance()
//   kLeadOut:       value emitted = frozen_lead_out_value() (last programme
//   value)
//
// Per IEC 60857 §10.2.3, the picture number is updated on the second field
// of each new picture. The generator holds the number of the next picture;
// BiphaseInjectionManager handles the update timing, repeating the previous
// number on a picture's first field (see DataForValue). The caller supplies
// field_one to reflect the current field.
//
// NTSC maximum: 79,999 (IEC 60857 Amendment 2 §10.2.3).
// Saturates at the maximum rather than wrapping.
//
// Valid for NTSC CAV discs only (lines 10 and 273).
//
// Thread-safety: NOT thread-safe (mutable state).
class FmPictureNumberGenerator {
 public:
  // IEC 60857 Amendment 2 §10.2.3: maximum NTSC picture number.
  static constexpr int kNtscMaxValue = 79999;

  // Constructs the generator.
  // start_value: initial picture number for the programme area (default 1).
  //   Must be in [0, kNtscMaxValue].
  explicit FmPictureNumberGenerator(int start_value = 1);

  // Returns FmData for the current frame.
  //   field_one:    true = first field (bit 5 = 1 in the FM word).
  //   section_type: kLeadIn  → value 0
  //                 kProgrammeArea → current_programme_value()
  //                 kLeadOut → frozen_lead_out_value()
  FmData CurrentData(bool field_one, SectionType section_type) const;

  // Returns FmData carrying picture number `value` (0–79999).
  //   field_one: true = first field (bit 5 = 1 in the FM word).
  static FmData DataForValue(int value, bool field_one);

  // Advances the generator by one frame.
  // In kProgrammeArea: increments current_programme_value and updates
  //   frozen_lead_out_value to the current value before incrementing.
  // In kLeadIn or kLeadOut: no-op.
  void Advance(SectionType section_type);

  // Resets to initial state (start_value, frozen_lead_out_value = 0).
  void Reset();

  // Returns the current auto-incrementing value for the programme area.
  int current_programme_value() const { return current_programme_value_; }

  // Returns the last programme area value frozen for lead-out use.
  int frozen_lead_out_value() const { return frozen_lead_out_value_; }

  // Encodes integer n (0–79999) into five decimal-digit nibbles x1–x5.
  // x1 is the most-significant digit; x5 is the least-significant digit.
  static void EncodeValue(int n, uint8_t& x1, uint8_t& x2, uint8_t& x3,
                          uint8_t& x4, uint8_t& x5);

  // Returns true if n is in the valid NTSC range [0, kNtscMaxValue].
  static bool IsValidValue(int n);

 private:
  int start_value_;
  int current_programme_value_;
  int frozen_lead_out_value_;
};

// Generates FmData for NTSC CLV 40-bit FM programme time codes.
//
// Format: X₁X₂ = minutes BCD, X₃X₄ = seconds BCD, X₅ = mode indicator.
//
// Mode indicator X₅ (IEC 60857 §10.2 / Appendix G):
//   0xA (kModeLeadIn)     = lead-in period
//   0xB (kModeTransition) = end of lead-in through lead-in + 100 frames
//   0xD (kModePicture)    = active programme (picture area)
//   0xC (kModeLeadOut)    = lead-out period
//
// Section-aware frozen value behaviour (IEC 60857 Appendix F):
//   kLeadIn:        time = 0:00, mode = kModeLeadIn
//   kProgrammeArea: time auto-increments each Advance(), mode = kModePicture
//   kLeadOut:       time frozen at last programme_area value, mode =
//   kModeLeadOut
//
// Frames per second: 30 (NTSC nominal). Minutes wrap at 59.
//
// Valid for NTSC CLV discs only (lines 10 and 273).
//
// Thread-safety: NOT thread-safe (mutable state).
class FmProgrammeTimeGenerator {
 public:
  static constexpr uint8_t kModeLeadIn = 0xAu;
  static constexpr uint8_t kModeTransition = 0xBu;
  static constexpr uint8_t kModePicture = 0xDu;
  static constexpr uint8_t kModeLeadOut = 0xCu;

  // NTSC nominal frame rate for second and minute boundary calculation.
  static constexpr int kFramesPerSecond = 30;
  static constexpr int kFramesPerMinute = 1800;

  // Constructs the generator; time starts at 0:00 in the programme area.
  FmProgrammeTimeGenerator();

  // Returns FmData for the current frame.
  //   field_one:    true = first field (bit 5 = 1 in the FM word).
  //   section_type: kLeadIn  → time 0:00, mode kModeLeadIn
  //                 kProgrammeArea → current time, mode kModePicture
  //                 kLeadOut → frozen time, mode kModeLeadOut
  FmData CurrentData(bool field_one, SectionType section_type) const;

  // Advances the generator by one frame.
  // In kProgrammeArea: increments the frame counter and updates frozen values.
  // In kLeadIn or kLeadOut: no-op.
  void Advance(SectionType section_type);

  // Resets to initial state (programme_frame_count = 0, frozen values = 0:00).
  void Reset();

  // Returns the current minutes (0–59) in the programme area.
  int current_minutes() const;

  // Returns the current seconds (0–59) in the programme area.
  int current_seconds() const;

  // Returns the total frames elapsed in the programme area since Reset().
  int total_programme_frames() const { return programme_frame_count_; }

  // Returns the frozen minutes (last programme area value, used in lead-out).
  int frozen_minutes() const { return frozen_minutes_; }

  // Returns the frozen seconds (last programme area value, used in lead-out).
  int frozen_seconds() const { return frozen_seconds_; }

  // Builds FmData from time components and field indicator.
  // minutes and seconds are each encoded as two BCD nibbles.
  static FmData BuildTimeData(bool field_one, int minutes, int seconds,
                              uint8_t mode);

 private:
  int programme_frame_count_;
  int frozen_minutes_;
  int frozen_seconds_;
};

}  // namespace videosynth
