/*
 * File:        clv_code_generator.h
 * Module:      clv_code_generator
 * Purpose:     CLV LaserDisc biphase code generators per IEC 60856/60857.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "videosynth/code_generator.h"
#include "videosynth/model.h"

namespace videosynth {

// Fixed 24-bit code 87FFFF: CLV identifier code.
// IEC 60856 §10.1.8 (PAL), IEC 60857 §10.1.8 (NTSC).
// CLV discs only, programme_area only.
//
// Thread-safety: Thread-safe (immutable).
class ClvCodeGenerator final : public CodeGenerator {
 public:
  // IEC 60856/60857: CLV identifier code value.
  static constexpr uint32_t kCode = 0x87FFFFu;

  uint32_t CurrentCode() const override { return kCode; }
};

// Auto-incrementing programme time code generator for CLV discs.
//
// Format: FX₁DDX₂X₃ where:
//   F  = key nibble (0xF, bits 23-20)
//   X₁ = hours (0–F, bits 19-16)
//   DD = fixed pattern (0xDD, bits 15-8)
//   X₂ = minutes tens digit (0–5, bits 7-4)   ← BCD
//   X₃ = minutes units digit (0–9, bits 3-0)   ← BCD
//
// IEC 60856 §10.1.7 (PAL), IEC 60857 §10.1.7 (NTSC).
// CLV discs only, programme_area only. Starts at start_hours:start_minutes.
//
// The counter advances one frame at a time; minutes increment at
// FramesPerMinute() boundaries (PAL: every 1500 frames, NTSC: every 1800
// frames). The hours field saturates at kMaxHours (15).
//
// Thread-safety: NOT thread-safe (mutable state).
class ProgrammeTimeCodeGenerator final : public CodeGenerator {
 public:
  // IEC 60856/60857: maximum hours value (4-bit nibble, 0–F).
  static constexpr int kMaxHours = 15;
  // Maximum minutes value (BCD decades).
  static constexpr int kMaxMinutes = 59;

  // Constructs the generator.
  // start_hours:   Initial hours value, must be in [0, kMaxHours].
  // start_minutes: Initial minutes value, must be in [0, kMaxMinutes].
  // standard:      Determines frames per minute (PAL: 1500, NTSC/PAL-M: 1800).
  ProgrammeTimeCodeGenerator(int start_hours, int start_minutes,
                             Standard standard);

  uint32_t CurrentCode() const override;
  void Advance() override;
  void Reset() override;

  // Returns the current hours value (0–15).
  int current_hours() const;
  // Returns the current minutes value (0–59).
  int current_minutes() const;
  // Returns the total frames elapsed since construction / last Reset().
  int total_frames() const { return frame_count_; }

  // Encodes hours and minutes into the FX₁DDX₂X₃ 24-bit code.
  // hours must be in [0, 15]; minutes must be in [0, 59].
  static uint32_t EncodeTimeCode(int hours, int minutes);

  // Returns true if hours ∈ [0, kMaxHours] and minutes ∈ [0, kMaxMinutes].
  static bool IsValidTimeCode(int hours, int minutes);

  // Returns the number of frames per minute for the given standard.
  // 625-line: 1500 (25 fps × 60 s); NTSC and PAL-M: 1800 (30 fps nominal × 60
  // s).
  static int FramesPerMinute(Standard standard);

 private:
  int start_hours_;
  int start_minutes_;
  Standard standard_;
  int frame_count_;
};

// Auto-incrementing CLV picture number generator.
//
// Format: 8X₁EX₃X₄X₅ where:
//   8  = key nibble (0x8, bits 23-20)
//   X₁ = seconds tens encoded as offset from 0xA (bits 19-16), range A–F
//   E  = fixed nibble (0xE, bits 15-12)
//   X₃ = seconds units (bits 11-8), range 0–9
//   X₄ = frame-within-second tens (bits 7-4), range 0–2
//   X₅ = frame-within-second units (bits 3-0), range 0–9
//
// Time decoding:
//   Seconds = (X₁ − 0xA) × 10 + X₃    (0–59)
//   Frame   = X₄ × 10 + X₅             (0–29)
//
// IEC 60856 §10.1.9 (PAL), IEC 60857 §10.1.9 / Amendment 2 §10.1.10 (NTSC).
// CLV discs only, programme_area only. Starts at seconds=0, frame=0.
//
// NTSC Colour Time Error Correction (IEC 60857 Amendment 2 §10.1.10):
//   On NTSC discs, at each accumulated frame count N that satisfies
//     N = 8991 × L + 899 × M   (L integer ≥ 0, M integer 0–9, N > 0),
//   the seconds count advances by one extra step and the frame count resets
//   to zero. This correction does NOT apply to PAL discs.
//
// Thread-safety: NOT thread-safe (mutable state).
class ClvPictureNumberGenerator final : public CodeGenerator {
 public:
  // Maximum seconds value (6 values of X₁ × 10 units of X₃ − 1).
  static constexpr int kMaxSeconds = 59;
  // Maximum frame-within-second value.
  static constexpr int kMaxFrame = 29;

  // Constructs the generator starting at seconds=0, frame=0.
  // standard: Determines frames per second and whether NTSC correction applies.
  explicit ClvPictureNumberGenerator(Standard standard);

  uint32_t CurrentCode() const override;
  void Advance() override;
  void Reset() override;

  // Returns the current seconds value (0–59).
  int current_seconds() const { return current_seconds_; }
  // Returns the current frame-within-second (0–fps-1).
  int current_frame() const { return current_frame_; }
  // Returns the total frames elapsed since construction / last Reset().
  int total_frames() const { return total_frames_; }

  // Encodes seconds and frame into the 8X₁EX₃X₄X₅ 24-bit code.
  // seconds must be in [0, kMaxSeconds]; frame must be in [0, kMaxFrame].
  static uint32_t EncodeClvPictureNumber(int seconds, int frame);

  // Returns true if seconds ∈ [0, kMaxSeconds] and frame ∈ [0, kMaxFrame].
  static bool IsValidClvPictureNumber(int seconds, int frame);

  // Returns true if N is an NTSC colour time correction point per IEC 60857
  // Amendment 2 §10.1.10: N = 8991×L + 899×M (L ≥ 0, M in 0–9), N > 0.
  static bool IsNtscCorrectionPoint(int N);

  // Returns the nominal frames per second for the given standard.
  // PAL: 25, NTSC: 30.
  static int FramesPerSecond(Standard standard);

 private:
  Standard standard_;
  int total_frames_;
  int current_seconds_;
  int current_frame_;
};

// A decoded CLV programme timecode (HH:MM:SS:FF).
struct ClvTimecode {
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  int frames = 0;
};

// Computes the continuous CLV programme timecode for a 0-based sequential
// output frame position, at the standard's nominal CLV frame rate (PAL/PAL-M:
// 25 fps, NTSC: 30 fps). This drives the {timecode} OSD token, which runs from
// the start of the generated output regardless of which VBI codes a section
// injects; the frames field wraps at FramesPerSecond() and the higher fields
// carry as usual.
ClvTimecode ClvTimecodeForFrame(std::size_t output_frame_index,
                                Standard standard);

}  // namespace videosynth
