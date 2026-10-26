/*
 * File:        fm_code_generator.cpp
 * Module:      fm_code_generator
 * Purpose:     40-bit FM code type generators for NTSC LaserDisc VBI injection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/fm_code_generator.h"

#include <cstdint>

namespace videosynth {

// ---------------------------------------------------------------------------
// FmPictureNumberGenerator
// ---------------------------------------------------------------------------

FmPictureNumberGenerator::FmPictureNumberGenerator(int start_value)
    : start_value_(start_value),
      current_programme_value_(start_value),
      frozen_lead_out_value_(0) {}

// static
void FmPictureNumberGenerator::EncodeValue(int n, uint8_t& x1, uint8_t& x2,
                                           uint8_t& x3, uint8_t& x4,
                                           uint8_t& x5) {
  x5 = static_cast<uint8_t>(n % 10);
  n /= 10;
  x4 = static_cast<uint8_t>(n % 10);
  n /= 10;
  x3 = static_cast<uint8_t>(n % 10);
  n /= 10;
  x2 = static_cast<uint8_t>(n % 10);
  x1 = static_cast<uint8_t>(n / 10);
}

// static
bool FmPictureNumberGenerator::IsValidValue(int n) {
  return n >= 0 && n <= kNtscMaxValue;
}

FmData FmPictureNumberGenerator::CurrentData(bool field_one,
                                             SectionType section_type) const {
  int value = 0;
  switch (section_type) {
    case SectionType::kProgrammeArea:
      value = current_programme_value_;
      break;
    case SectionType::kLeadOut:
      value = frozen_lead_out_value_;
      break;
    case SectionType::kLeadIn:
    default:
      value = 0;
      break;
  }

  FmData data;
  data.field_one = field_one;
  EncodeValue(value, data.x1, data.x2, data.x3, data.x4, data.x5);
  return data;
}

void FmPictureNumberGenerator::Advance(SectionType section_type) {
  if (section_type == SectionType::kProgrammeArea) {
    frozen_lead_out_value_ = current_programme_value_;
    if (current_programme_value_ < kNtscMaxValue) {
      ++current_programme_value_;
    }
  }
}

void FmPictureNumberGenerator::Reset() {
  current_programme_value_ = start_value_;
  frozen_lead_out_value_ = 0;
}

// ---------------------------------------------------------------------------
// FmProgrammeTimeGenerator
// ---------------------------------------------------------------------------

FmProgrammeTimeGenerator::FmProgrammeTimeGenerator()
    : programme_frame_count_(0), frozen_minutes_(0), frozen_seconds_(0) {}

int FmProgrammeTimeGenerator::current_minutes() const {
  const int total_seconds = programme_frame_count_ / kFramesPerSecond;
  return (total_seconds / 60) % 60;
}

int FmProgrammeTimeGenerator::current_seconds() const {
  const int total_seconds = programme_frame_count_ / kFramesPerSecond;
  return total_seconds % 60;
}

// static
FmData FmProgrammeTimeGenerator::BuildTimeData(bool field_one, int minutes,
                                               int seconds, uint8_t mode) {
  FmData data;
  data.field_one = field_one;
  data.x1 = static_cast<uint8_t>(minutes / 10);
  data.x2 = static_cast<uint8_t>(minutes % 10);
  data.x3 = static_cast<uint8_t>(seconds / 10);
  data.x4 = static_cast<uint8_t>(seconds % 10);
  data.x5 = mode & 0x0Fu;
  return data;
}

FmData FmProgrammeTimeGenerator::CurrentData(bool field_one,
                                             SectionType section_type) const {
  switch (section_type) {
    case SectionType::kProgrammeArea:
      return BuildTimeData(field_one, current_minutes(), current_seconds(),
                           kModePicture);

    case SectionType::kLeadOut:
      return BuildTimeData(field_one, frozen_minutes_, frozen_seconds_,
                           kModeLeadOut);

    case SectionType::kLeadIn:
    default:
      return BuildTimeData(field_one, 0, 0, kModeLeadIn);
  }
}

void FmProgrammeTimeGenerator::Advance(SectionType section_type) {
  if (section_type == SectionType::kProgrammeArea) {
    ++programme_frame_count_;
    // Frozen values track the current state after each advance so that
    // lead-out freezes at the time corresponding to the last active frame.
    frozen_minutes_ = current_minutes();
    frozen_seconds_ = current_seconds();
  }
}

void FmProgrammeTimeGenerator::Reset() {
  programme_frame_count_ = 0;
  frozen_minutes_ = 0;
  frozen_seconds_ = 0;
}

}  // namespace videosynth
