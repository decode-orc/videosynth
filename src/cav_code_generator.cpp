/*
 * File:        cav_code_generator.cpp
 * Module:      cav_code_generator
 * Purpose:     CAV LaserDisc biphase code generators per IEC 60856/60857.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/cav_code_generator.h"

#include <cstdint>

namespace videosynth {

// ---------------------------------------------------------------------------
// CavPictureNumberGenerator
// ---------------------------------------------------------------------------

CavPictureNumberGenerator::CavPictureNumberGenerator(int start_value,
                                                     Standard standard)
    : start_value_(start_value),
      current_value_(start_value),
      standard_(standard) {}

uint32_t CavPictureNumberGenerator::CurrentCode() const {
  return EncodePictureNumber(current_value_);
}

void CavPictureNumberGenerator::Advance() {
  if (current_value_ < MaxPictureNumber(standard_)) {
    ++current_value_;
  }
}

void CavPictureNumberGenerator::Reset() { current_value_ = start_value_; }

// IEC 60856/60857: Picture number FX₁X₂X₃X₄X₅.
// Each decimal digit of n becomes one 4-bit hex nibble. The key nibble F
// (0xF = 1111b) always occupies bits 23–20, satisfying bit 23 = 1.
// static
uint32_t CavPictureNumberGenerator::EncodePictureNumber(int n) {
  const int x5 = n % 10;
  n /= 10;
  const int x4 = n % 10;
  n /= 10;
  const int x3 = n % 10;
  n /= 10;
  const int x2 = n % 10;
  const int x1 = n / 10;
  return 0xF00000u | (static_cast<uint32_t>(x1) << 16) |
         (static_cast<uint32_t>(x2) << 12) | (static_cast<uint32_t>(x3) << 8) |
         (static_cast<uint32_t>(x4) << 4) | static_cast<uint32_t>(x5);
}

// static
bool CavPictureNumberGenerator::IsValidPictureNumber(int n, Standard standard) {
  return n >= 0 && n <= MaxPictureNumber(standard);
}

// static
int CavPictureNumberGenerator::MaxPictureNumber(Standard standard) {
  // IEC 60857 limits apply to both System M standards (NTSC and PAL-M).
  return Is625LineStandard(standard) ? kPalMaxValue : kNtscMaxValue;
}

// ---------------------------------------------------------------------------
// ChapterNumberGenerator
// ---------------------------------------------------------------------------

ChapterNumberGenerator::ChapterNumberGenerator(int chapter_num,
                                               bool always_stop_bit_one)
    : chapter_num_(chapter_num),
      always_stop_bit_one_(always_stop_bit_one),
      track_count_(0) {}

uint32_t ChapterNumberGenerator::CurrentCode() const {
  return EncodeChapterCode(chapter_num_, CurrentStopBit());
}

void ChapterNumberGenerator::Advance() { ++track_count_; }

void ChapterNumberGenerator::Reset() { track_count_ = 0; }

// IEC 60856/60857 §10.1.5:
//   Stop-bit = 0 for the first 400 tracks, then 1.
//   Exception: always 1 when always_stop_bit_one_ is set.
bool ChapterNumberGenerator::CurrentStopBit() const {
  if (always_stop_bit_one_) {
    return true;
  }
  return track_count_ >= kStopBitTransitionTrack;
}

// static
bool ChapterNumberGenerator::IsValidChapterNumber(int chapter_num) {
  return chapter_num >= 0 && chapter_num <= kMaxChapterNumber;
}

// IEC 60856/60857 §10.1.5 encoding:
//   X₁ = (stop_bit << 3) | (chapter_num / 16)
//   X₂ = chapter_num % 16
//   Code = 0x800000 | (X₁ << 16) | (X₂ << 12) | 0xDDD
// static
uint32_t ChapterNumberGenerator::EncodeChapterCode(int chapter_num,
                                                   bool stop_bit) {
  const uint8_t x1_nibble = (stop_bit ? 0x08u : 0x00u) |
                            (static_cast<uint8_t>(chapter_num / 16) & 0x07u);
  const uint8_t x2_nibble = static_cast<uint8_t>(chapter_num % 16);
  return 0x800000u | (static_cast<uint32_t>(x1_nibble) << 16) |
         (static_cast<uint32_t>(x2_nibble) << 12) | kDddPattern;
}

// IEC 60856/60857 §10.1.5 decoding:
//   X₁ = (code >> 16) & 0x0F
//   X₂ = (code >> 12) & 0x0F
//   Chapter = (X₁ & 7) × 16 + X₂
// static
int ChapterNumberGenerator::DecodeChapterNumber(uint32_t code_value) {
  const uint8_t x1_nibble = static_cast<uint8_t>((code_value >> 16) & 0x0Fu);
  const uint8_t x2_nibble = static_cast<uint8_t>((code_value >> 12) & 0x0Fu);
  return static_cast<int>((x1_nibble & 0x07u) * 16u + x2_nibble);
}

// IEC 60856/60857 §10.1.5 decoding:
//   Stop-bit = (X₁ & 8) >> 3
// static
bool ChapterNumberGenerator::DecodeStopBit(uint32_t code_value) {
  const uint8_t x1_nibble = static_cast<uint8_t>((code_value >> 16) & 0x0Fu);
  return (x1_nibble & 0x08u) != 0u;
}

// ---------------------------------------------------------------------------
// ProgrammeStatusCodeGenerator
// ---------------------------------------------------------------------------

ProgrammeStatusCodeGenerator::ProgrammeStatusCodeGenerator(uint32_t code_value)
    : code_value_(code_value) {}

// static
bool ProgrammeStatusCodeGenerator::IsValidProgrammeStatusCode(
    uint32_t code_value) {
  return (code_value & 0x800000u) != 0u;
}

// ---------------------------------------------------------------------------
// UsersCodeGenerator
// ---------------------------------------------------------------------------

UsersCodeGenerator::UsersCodeGenerator(uint32_t code_value)
    : code_value_(code_value) {}

// IEC 60856/60857 §10.1.9: X₁ (bits 19–16) must be in [0, 7];
// D (bits 15–12) must be the fixed hex digit 0xD.
// static
bool UsersCodeGenerator::IsValidUsersCode(uint32_t code_value) {
  const uint8_t d_nibble = static_cast<uint8_t>((code_value >> 12) & 0x0Fu);
  return ExtractX1(code_value) <= 7u && d_nibble == 0xDu;
}

// static
uint8_t UsersCodeGenerator::ExtractX1(uint32_t code_value) {
  return static_cast<uint8_t>((code_value >> 16) & 0x0Fu);
}

}  // namespace videosynth
