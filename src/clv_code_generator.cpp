/*
 * File:        clv_code_generator.cpp
 * Module:      clv_code_generator
 * Purpose:     CLV LaserDisc biphase code generators per IEC 60856/60857.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/clv_code_generator.h"

#include <cstdint>

namespace videosynth {

// ---------------------------------------------------------------------------
// ProgrammeTimeCodeGenerator
// ---------------------------------------------------------------------------

ProgrammeTimeCodeGenerator::ProgrammeTimeCodeGenerator(int start_hours,
                                                       int start_minutes,
                                                       Standard standard)
    : start_hours_(start_hours),
      start_minutes_(start_minutes),
      standard_(standard),
      frame_count_(0) {}

uint32_t ProgrammeTimeCodeGenerator::CurrentCode() const {
  return EncodeTimeCode(current_hours(), current_minutes());
}

void ProgrammeTimeCodeGenerator::Advance() { ++frame_count_; }

void ProgrammeTimeCodeGenerator::Reset() { frame_count_ = 0; }

int ProgrammeTimeCodeGenerator::current_hours() const {
  const int total_minutes = start_hours_ * 60 + start_minutes_ +
                            frame_count_ / FramesPerMinute(standard_);
  const int hours = total_minutes / 60;
  return (hours > kMaxHours) ? kMaxHours : hours;
}

int ProgrammeTimeCodeGenerator::current_minutes() const {
  const int total_minutes = start_hours_ * 60 + start_minutes_ +
                            frame_count_ / FramesPerMinute(standard_);
  return total_minutes % 60;
}

// IEC 60856/60857: Programme time code FX₁DDX₂X₃.
//   F  = key nibble 0xF (bits 23-20)
//   X₁ = hours nibble (bits 19-16)
//   DD = fixed pattern 0xDD (bits 15-8)
//   X₂ = minutes tens digit (bits 7-4)
//   X₃ = minutes units digit (bits 3-0)
// static
uint32_t ProgrammeTimeCodeGenerator::EncodeTimeCode(int hours, int minutes) {
  const uint32_t x1 = static_cast<uint32_t>(hours);
  const uint32_t x2 = static_cast<uint32_t>(minutes / 10);
  const uint32_t x3 = static_cast<uint32_t>(minutes % 10);
  return 0xF00000u | (x1 << 16) | 0x00DD00u | (x2 << 4) | x3;
}

// static
bool ProgrammeTimeCodeGenerator::IsValidTimeCode(int hours, int minutes) {
  return hours >= 0 && hours <= kMaxHours && minutes >= 0 &&
         minutes <= kMaxMinutes;
}

// static
int ProgrammeTimeCodeGenerator::FramesPerMinute(Standard standard) {
  // System M (NTSC and PAL-M) runs at 30 frame/s nominal; 625-line at 25.
  return Is625LineStandard(standard) ? 1500 : 1800;
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator
// ---------------------------------------------------------------------------

ClvPictureNumberGenerator::ClvPictureNumberGenerator(Standard standard)
    : standard_(standard),
      total_frames_(0),
      current_seconds_(0),
      current_frame_(0) {}

uint32_t ClvPictureNumberGenerator::CurrentCode() const {
  return EncodeClvPictureNumber(current_seconds_, current_frame_);
}

void ClvPictureNumberGenerator::Advance() {
  ++total_frames_;

  if (!Is625LineStandard(standard_) && IsNtscCorrectionPoint(total_frames_)) {
    // IEC 60857 Amendment 2 §10.1.10: colour time error correction.
    // Seconds jumps one extra step forward; frame resets to zero.
    current_frame_ = 0;
    ++current_seconds_;
    if (current_seconds_ > kMaxSeconds) {
      current_seconds_ = 0;
    }
  } else {
    ++current_frame_;
    if (current_frame_ >= FramesPerSecond(standard_)) {
      current_frame_ = 0;
      ++current_seconds_;
      if (current_seconds_ > kMaxSeconds) {
        current_seconds_ = 0;
      }
    }
  }
}

void ClvPictureNumberGenerator::Reset() {
  total_frames_ = 0;
  current_seconds_ = 0;
  current_frame_ = 0;
}

// IEC 60856/60857: CLV picture number 8X₁EX₃X₄X₅.
//   8  = key nibble 0x8 (bits 23-20)
//   X₁ = 0xA + seconds/10 (bits 19-16), range A–F
//   E  = fixed nibble 0xE (bits 15-12)
//   X₃ = seconds%10 (bits 11-8), range 0–9
//   X₄ = frame/10 (bits 7-4), range 0–2
//   X₅ = frame%10 (bits 3-0), range 0–9
// static
uint32_t ClvPictureNumberGenerator::EncodeClvPictureNumber(int seconds,
                                                           int frame) {
  const uint32_t x1 = 0xAu + static_cast<uint32_t>(seconds / 10);
  const uint32_t x3 = static_cast<uint32_t>(seconds % 10);
  const uint32_t x4 = static_cast<uint32_t>(frame / 10);
  const uint32_t x5 = static_cast<uint32_t>(frame % 10);
  return 0x800000u | (x1 << 16) | 0x00E000u | (x3 << 8) | (x4 << 4) | x5;
}

// static
bool ClvPictureNumberGenerator::IsValidClvPictureNumber(int seconds,
                                                        int frame) {
  return seconds >= 0 && seconds <= kMaxSeconds && frame >= 0 &&
         frame <= kMaxFrame;
}

// IEC 60857 Amendment 2 §10.1.10: colour time error correction points.
// Returns true if N = 8991×L + 899×M for some integer L ≥ 0 and M ∈ [0, 9],
// with N > 0.
// static
bool ClvPictureNumberGenerator::IsNtscCorrectionPoint(int N) {
  if (N <= 0) {
    return false;
  }
  for (int L = 0; 8991 * L <= N; ++L) {
    const int remainder = N - 8991 * L;
    if (remainder % 899 == 0) {
      const int M = remainder / 899;
      if (M >= 0 && M <= 9) {
        return true;
      }
    }
  }
  return false;
}

// static
int ClvPictureNumberGenerator::FramesPerSecond(Standard standard) {
  return Is625LineStandard(standard) ? 25 : 30;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

ClvTimecode ClvTimecodeForFrame(std::size_t output_frame_index,
                                Standard standard) {
  const int fps = ClvPictureNumberGenerator::FramesPerSecond(standard);
  const std::size_t total_seconds =
      output_frame_index / static_cast<std::size_t>(fps);

  ClvTimecode tc;
  tc.frames =
      static_cast<int>(output_frame_index % static_cast<std::size_t>(fps));
  tc.seconds = static_cast<int>(total_seconds % 60U);
  tc.minutes = static_cast<int>((total_seconds / 60U) % 60U);
  tc.hours = static_cast<int>(total_seconds / 3600U);
  return tc;
}

}  // namespace videosynth
