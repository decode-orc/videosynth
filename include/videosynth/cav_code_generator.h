/*
 * File:        cav_code_generator.h
 * Module:      cav_code_generator
 * Purpose:     CAV LaserDisc biphase code generators per IEC 60856/60857.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

#include "videosynth/code_generator.h"
#include "videosynth/model.h"

namespace videosynth {

// Fixed 24-bit code 88FFFF: lead-in section marker.
// IEC 60856 §10.1.1 (PAL), IEC 60857 §10.1.1 (NTSC).
// Valid section: lead_in only. Valid for both CAV and CLV discs.
class LeadInCodeGenerator final : public CodeGenerator {
 public:
  // IEC 60856/60857: Lead-in code value.
  static constexpr uint32_t kCode = 0x88FFFFu;

  uint32_t CurrentCode() const override { return kCode; }
};

// Fixed 24-bit code 80EEEE: lead-out section marker.
// IEC 60856 §10.1.2 (PAL), IEC 60857 §10.1.2 (NTSC).
// Valid section: lead_out only. Valid for both CAV and CLV discs.
class LeadOutCodeGenerator final : public CodeGenerator {
 public:
  // IEC 60856/60857: Lead-out code value.
  static constexpr uint32_t kCode = 0x80EEEEu;

  uint32_t CurrentCode() const override { return kCode; }
};

// Fixed 24-bit code 82CFFF: picture stop marker.
// IEC 60856 §10.1.4. CAV only, programme_area only.
class PictureStopCodeGenerator final : public CodeGenerator {
 public:
  // IEC 60856 §10.1.4: Picture stop code value.
  static constexpr uint32_t kCode = 0x82CFFFu;

  uint32_t CurrentCode() const override { return kCode; }
};

// Auto-incrementing picture number generator for CAV discs.
//
// Format: FX₁X₂X₃X₄X₅ where X₁–X₅ are BCD decimal digits (0–9) encoded
// as hex nibbles. The key nibble F = 1111b satisfies the IEC requirement that
// bit 23 = 1.
//
// IEC 60856 §10.1.3 (PAL): starts at 1, max 99,999.
// IEC 60857 §10.1.3 (NTSC): starts at 1, max 79,999.
//
// The generator increments by one per Advance() call and saturates at the
// IEC-defined maximum rather than wrapping.
//
// Thread-safety: NOT thread-safe (mutable state).
class CavPictureNumberGenerator final : public CodeGenerator {
 public:
  // IEC 60856 §10.1.3 (PAL): maximum picture number.
  static constexpr int kPalMaxValue = 99999;
  // IEC 60857 §10.1.3 (NTSC): maximum picture number.
  static constexpr int kNtscMaxValue = 79999;

  // Constructs the generator.
  // start_value: Initial picture number. Must satisfy
  //   IsValidPictureNumber(start_value, standard).
  // standard: Determines the maximum allowed value (PAL: 99,999; NTSC: 79,999).
  CavPictureNumberGenerator(int start_value, Standard standard);

  uint32_t CurrentCode() const override;
  void Advance() override;
  void Reset() override;

  // Returns the current picture number integer value.
  int current_value() const { return current_value_; }

  // Encodes an integer picture number into the FX₁X₂X₃X₄X₅ 24-bit code.
  // The five decimal digits of n become hex nibbles X₁–X₅.
  // n must be in [0, 99999]; results are undefined for larger values.
  static uint32_t EncodePictureNumber(int n);

  // Returns true if n is within the IEC-defined range for the given standard:
  //   PAL:  [0, 99,999]
  //   NTSC: [0, 79,999]
  static bool IsValidPictureNumber(int n, Standard standard);

  // Returns the IEC-defined maximum picture number for the given standard.
  static int MaxPictureNumber(Standard standard);

 private:
  int start_value_;
  int current_value_;
  Standard standard_;
};

// Chapter number generator with IEC-compliant stop-bit logic.
//
// Format: 8X₁X₂DDD, where:
//   Chapter = (X₁ & 7) × 10 + X₂       (two BCD digits, IEC 60856/60857
//   §10.1.5) Stop-bit = (X₁ & 8) >> 3             (MSB of X₁ nibble) DDD =
//   constant pattern 0xDDD
//
// Stop-bit rules (IEC 60856/60857 §10.1.5):
//   1. Stop-bit = 0 for the first 400 tracks of the chapter.
//   2. Stop-bit = 1 for all subsequent tracks.
//   3. The first chapter directly after the lead-in MUST have stop-bit = 1
//      (no zero stop-bit period).
//   4. On a disc with any chapter shorter than 800 tracks, every chapter has
//      stop-bit = 1 on all tracks.
//   always_stop_bit_one selects stop-bit = 1 on all tracks (cases 3 and 4);
//   ResolveChapterStopBitOne decides it from the schedule.
//
// Valid for both CAV and CLV discs, programme_area only. Maximum chapter: 79.
//
// Thread-safety: NOT thread-safe (mutable state).
class ChapterNumberGenerator final : public CodeGenerator {
 public:
  // IEC 60856/60857: maximum chapter number.
  static constexpr int kMaxChapterNumber = 79;
  // IEC 60856/60857 §10.1.5: minimum chapter length in tracks.
  static constexpr int kMinimumChapterTracks = 30;
  // IEC 60856/60857 §10.1.5: tracks with stop-bit = 0.
  static constexpr int kStopBitTransitionTrack = 400;
  // IEC 60856/60857 §10.1.5: on a disc with a chapter shorter than this many
  // tracks, every chapter has stop-bit 1 throughout.
  static constexpr int kShortChapterTracks = 800;
  // IEC 60856/60857 §10.1.5: value of the constant DDD pattern in the code.
  static constexpr uint32_t kDddPattern = 0x000DDDu;

  // Constructs the generator.
  // chapter_num: Chapter number, 0–79.
  // always_stop_bit_one: When true, stop-bit is 1 for every track. Required
  //   for (a) the first chapter after lead-in, and (b) every chapter of a disc
  //   with a chapter shorter than 800 tracks (IEC 60856/60857 §10.1.5).
  explicit ChapterNumberGenerator(int chapter_num,
                                  bool always_stop_bit_one = false);

  uint32_t CurrentCode() const override;
  void Advance() override;
  void Reset() override;

  // Returns true if chapter_num is in the valid IEC range [0, 79].
  static bool IsValidChapterNumber(int chapter_num);

  // Encodes chapter_num (as two BCD digits) and stop_bit into the 24-bit code
  // 8X₁X₂DDD.
  // chapter_num must be [0, 79].
  static uint32_t EncodeChapterCode(int chapter_num, bool stop_bit);

  // Decodes the chapter number from a 24-bit code value.
  // Returns Chapter = (X₁ & 7) × 10 + X₂.
  static int DecodeChapterNumber(uint32_t code_value);

  // Decodes the stop-bit from a 24-bit code value.
  // Returns true if Stop-bit = (X₁ & 8) >> 3 equals 1.
  static bool DecodeStopBit(uint32_t code_value);

 private:
  int chapter_num_;
  bool always_stop_bit_one_;
  int track_count_;

  // Returns the stop-bit value for the current track position.
  bool CurrentStopBit() const;
};

// Programme status code generator.
// Format: 8DC X₃X₄X₅ (CX on) or 8BA X₃X₄X₅ (CX off).
// IEC 60856 Appendix C / IEC 60857 Appendix C (Amendment 2 semantics).
// Accepts a caller-provided 24-bit code; validates key nibble (bit 23 = 1).
// programme_area only; valid for both CAV and CLV discs.
//
// Thread-safety: Thread-safe (immutable after construction).
class ProgrammeStatusCodeGenerator final : public CodeGenerator {
 public:
  // Constructs the generator with a user-provided 24-bit code value.
  // code_value must satisfy IsValidProgrammeStatusCode(code_value).
  explicit ProgrammeStatusCodeGenerator(uint32_t code_value);

  uint32_t CurrentCode() const override { return code_value_; }

  // Returns true if code_value has a valid key nibble (bit 23 = 1).
  static bool IsValidProgrammeStatusCode(uint32_t code_value);

 private:
  uint32_t code_value_;
};

// Users code generator.
// Format: 8X₁DX₃X₄X₅ where X₁ is constrained to 0–7 (IEC §10.1.6).
// IEC 60856 §10.1.6 (PAL), IEC 60857 §10.1.6 (NTSC).
// Valid sections: lead_in and lead_out only. Valid for both CAV and CLV.
// Accepts a caller-provided 24-bit code; X₁ range is validated by
// IsValidUsersCode().
//
// Thread-safety: Thread-safe (immutable after construction).
class UsersCodeGenerator final : public CodeGenerator {
 public:
  // Constructs the generator with a user-provided 24-bit code value.
  // code_value must satisfy IsValidUsersCode(code_value).
  explicit UsersCodeGenerator(uint32_t code_value);

  uint32_t CurrentCode() const override { return code_value_; }

  // Returns true if the code has a valid X₁ nibble (bits 19–16 in [0, 7]) AND
  // a valid D nibble (bits 15–12 == 0xD).
  // IEC 60856/60857 §10.1.9: format 8 X₁ D X₃ X₄ X₅ where D is fixed 0xD.
  static bool IsValidUsersCode(uint32_t code_value);

  // Extracts the X₁ nibble (bits 19–16) from a 24-bit users_code value.
  static uint8_t ExtractX1(uint32_t code_value);

 private:
  uint32_t code_value_;
};

}  // namespace videosynth
