/*
 * File:        line_placement_engine.cpp
 * Module:      line_placement_engine
 * Purpose:     Field-aware VBI line placement for LaserDisc biphase and 40-bit
 *              FM code injection per IEC 60856 (PAL) and IEC 60857 (NTSC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/line_placement_engine.h"

#include <algorithm>

namespace videosynth {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LinePlacementEngine::LinePlacementEngine(
    Standard standard, DiscType disc_type, SectionType section_type,
    const std::vector<std::string>& codes_present)
    : standard_(standard),
      disc_type_(disc_type),
      section_type_(section_type),
      codes_present_(codes_present.begin(), codes_present.end()) {}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool LinePlacementEngine::IsInBiphaseReservedRange(Standard standard,
                                                   int line_number) {
  if (standard == Standard::kPal) {
    return (line_number >= 6 && line_number <= 18) ||
           (line_number >= 319 && line_number <= 331);
  }
  // NTSC / PAL-M: IEC 60857 §10 — lines 10–18 (field 1), 273–281 (field 2).
  return (line_number >= 10 && line_number <= 18) ||
         (line_number >= 273 && line_number <= 281);
}

bool LinePlacementEngine::IsFieldOne(Standard standard, int line_number) {
  // PAL: 625-line frame; field 1 = lines 1–312 (ITU-R BT.1700).
  // NTSC / PAL-M: 525-line frame; field 1 = lines 1–262 (SMPTE 170M-2004).
  if (standard == Standard::kPal) {
    return line_number <= 312;
  }
  return line_number <= 262;
}

// ---------------------------------------------------------------------------
// Private factory helpers
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::None() { return LineCodeAssignment{}; }

LineCodeAssignment LinePlacementEngine::Biphase(const std::string& code_type,
                                                bool uses_172h_offset) {
  return LineCodeAssignment{
      .assigned = true,
      .code_type = code_type,
      .is_fm = false,
      .is_white_flag = false,
      .uses_172h_offset = uses_172h_offset,
  };
}

LineCodeAssignment LinePlacementEngine::Fm(const std::string& code_type) {
  return LineCodeAssignment{
      .assigned = true,
      .code_type = code_type,
      .is_fm = true,
      .is_white_flag = false,
      .uses_172h_offset = false,
  };
}

LineCodeAssignment LinePlacementEngine::WhiteFlag() {
  return LineCodeAssignment{
      .assigned = true,
      .code_type = "fm_white_flag",
      .is_fm = false,
      .is_white_flag = true,
      .uses_172h_offset = false,
  };
}

bool LinePlacementEngine::Has(const std::string& code_type) const {
  return codes_present_.count(code_type) > 0;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetAssignment(int line_number,
                                                      bool field_one) const {
  return GetAssignment(line_number,
                       PictureFieldRole{.starts_picture = field_one,
                                        .follows_picture_start = !field_one});
}

LineCodeAssignment LinePlacementEngine::GetAssignment(
    int line_number, const PictureFieldRole& role) const {
  if (!IsInBiphaseReservedRange(standard_, line_number)) {
    return None();
  }

  if (standard_ == Standard::kPal) {
    return GetPalAssignment(line_number, role);
  }
  return GetNtscAssignment(line_number, role);
}

// ---------------------------------------------------------------------------
// PAL dispatch
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetPalAssignment(
    int line, const PictureFieldRole& role) const {
  if (section_type_ == SectionType::kLeadIn ||
      section_type_ == SectionType::kLeadOut) {
    return GetPalLeadInOut(line);
  }
  if (section_type_ == SectionType::kProgrammeArea) {
    // PAL field-2 biphase lines 329-331 map to field-1 lines 16-18.
    const int canonical = (line >= 329) ? (line - 313) : line;
    if (disc_type_ == DiscType::kCAV) {
      return GetCavProgrammeBiphase(canonical, role);
    }
    if (disc_type_ == DiscType::kCLV) {
      return GetClvProgrammeBiphase(canonical, role);
    }
  }
  return None();
}

// ---------------------------------------------------------------------------
// PAL lead-in / lead-out
// PAL field 1: 16 → users_code, 17–18 → section marker
// PAL field 2: 329 → users_code, 330–331 → section marker
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetPalLeadInOut(int line) const {
  const std::string marker =
      (section_type_ == SectionType::kLeadIn) ? "lead_in" : "lead_out";

  switch (line) {
    case 16:
      if (Has("users_code")) return Biphase("users_code");
      return None();
    case 17:
    case 18:
      return Biphase(marker);
    case 329:
      if (Has("users_code")) return Biphase("users_code");
      return None();
    case 330:
    case 331:
      return Biphase(marker);
    default:
      return None();
  }
}

// ---------------------------------------------------------------------------
// Picture fields
//
// IEC 60856/60857 place some codes in "the first field of the picture" and
// the rest in the fields without them. A video-cadence frame is one picture
// opened by field 1; a 2:3 pulldown picture opens on whichever field starts
// it. When a section carries none of the first-field codes, no field is a
// picture field and the remaining-field codes are written in every field.
// ---------------------------------------------------------------------------

bool LinePlacementEngine::HasPictureFieldCodes() const {
  if (disc_type_ == DiscType::kCAV) {
    return Has("picture_number");
  }
  return Has("programme_time_code") || Has("clv_picture_number");
}

bool LinePlacementEngine::IsPictureField(const PictureFieldRole& role) const {
  return role.starts_picture && HasPictureFieldCodes();
}

// ---------------------------------------------------------------------------
// CAV programme_area biphase (IEC 60856 §10.1.3-10.1.8, IEC 60857 same):
//   Picture field, lines 17/18:  picture_number
//   Stop field,    lines 16/17:  picture_stop
//   Other lines,   line 16:      programme_status
//                  lines 17/18:  chapter_number
// The stop field is the one after a picture's first field (§10.1.4), or
// every field when the section carries no picture_number. A picture field
// that is also a stop field keeps its picture_number on line 17.
// programme_status uses the 0.172H offset. `canonical` is the field-1 line.
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetCavProgrammeBiphase(
    int canonical, const PictureFieldRole& role) const {
  const bool picture_field = IsPictureField(role);
  const bool stop_field = Has("picture_stop") && (role.follows_picture_start ||
                                                  !HasPictureFieldCodes());

  switch (canonical) {
    case 16:
      if (stop_field) return Biphase("picture_stop");
      if (Has("programme_status")) return Biphase("programme_status", true);
      return None();

    case 17:
      if (picture_field) return Biphase("picture_number");
      // IEC 60856 §10.1.5: picture stop has priority over chapter on line 17.
      if (stop_field) return Biphase("picture_stop");
      if (Has("chapter_number")) return Biphase("chapter_number");
      return None();

    case 18:
      if (picture_field) return Biphase("picture_number");
      if (Has("chapter_number")) return Biphase("chapter_number");
      return None();

    default:
      return None();
  }
}

// ---------------------------------------------------------------------------
// CLV programme_area biphase (IEC 60856 §10.1.6-10.1.11, IEC 60857 same):
//   Picture field, line 16:      clv_picture_number
//                  lines 17/18:  programme_time_code
//   Other field,   line 16:      programme_status (0.172H offset)
//                  line 17:      clv_code (0.172H offset on NTSC only)
//                  line 18:      chapter_number
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetClvProgrammeBiphase(
    int canonical, const PictureFieldRole& role) const {
  const bool picture_field = IsPictureField(role);

  switch (canonical) {
    case 16:
      if (picture_field) {
        if (Has("clv_picture_number")) return Biphase("clv_picture_number");
        return None();
      }
      if (Has("programme_status")) return Biphase("programme_status", true);
      return None();

    case 17:
      if (picture_field) {
        if (Has("programme_time_code")) return Biphase("programme_time_code");
        return None();
      }
      // IEC 60857 Figure 11: NTSC clv_code uses 0.172H horizontal offset.
      if (Has("clv_code")) {
        return Biphase("clv_code", standard_ != Standard::kPal);
      }
      return None();

    case 18:
      if (picture_field) {
        if (Has("programme_time_code")) return Biphase("programme_time_code");
        return None();
      }
      if (Has("chapter_number")) return Biphase("chapter_number");
      return None();

    default:
      return None();
  }
}

// ---------------------------------------------------------------------------
// NTSC dispatch
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetNtscAssignment(
    int line, const PictureFieldRole& role) const {
  if (section_type_ == SectionType::kLeadIn ||
      section_type_ == SectionType::kLeadOut) {
    return GetNtscLeadInOut(line);
  }
  if (section_type_ == SectionType::kProgrammeArea) {
    if (disc_type_ == DiscType::kCAV) {
      return GetNtscCavProgramme(line, role);
    }
    if (disc_type_ == DiscType::kCLV) {
      return GetNtscClvProgramme(line, role);
    }
  }
  return None();
}

// ---------------------------------------------------------------------------
// NTSC lead-in / lead-out
//
// FM white flag placement (IEC 60857 §10.2.1, §10.2.2):
//   lead_in:  line 11 (field 1) only; line 274 (field 2) NOT used.
//   lead_out: both lines 11 and 274.
//
// 24-bit biphase:
//   Field 1: 16 → users_code, 17–18 → section marker
//   Field 2: 279 → users_code, 280–281 → section marker
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetNtscLeadInOut(int line) const {
  const std::string marker =
      (section_type_ == SectionType::kLeadIn) ? "lead_in" : "lead_out";

  // White flag lines (FM, NTSC only).
  if (line == 11) {
    if (Has("fm_white_flag")) return WhiteFlag();
    return None();
  }
  if (line == 274) {
    // White flag on line 274 only during lead-out (§10.2.2).
    if (section_type_ == SectionType::kLeadOut && Has("fm_white_flag")) {
      return WhiteFlag();
    }
    return None();
  }

  // Lines 10 and 273 carry no FM data in lead-in/lead-out sections.
  if (line == 10 || line == 273) {
    return None();
  }

  // 24-bit biphase lines.
  if (line == 16) {
    if (Has("users_code")) return Biphase("users_code");
    return None();
  }
  if (line == 17 || line == 18) {
    return Biphase(marker);
  }
  if (line == 279) {
    if (Has("users_code")) return Biphase("users_code");
    return None();
  }
  if (line == 280 || line == 281) {
    return Biphase(marker);
  }

  return None();
}

// ---------------------------------------------------------------------------
// NTSC CAV programme_area
//
// FM lines (IEC 60857 §10.2):
//   Line 10/273:  fm_picture_number (40-bit FM)
//   Line 11/274:  fm_white_flag (100 IRE) on the picture's first field —
//                 line 11 when field 1 starts the picture, line 274 when
//                 field 2 does (§10.2.4)
//
// 24-bit biphase on lines 16-18 / 279-281: see GetCavProgrammeBiphase.
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetNtscCavProgramme(
    int line, const PictureFieldRole& role) const {
  // FM signal lines.
  if (line == 10 || line == 273) {
    if (Has("fm_picture_number")) return Fm("fm_picture_number");
    return None();
  }
  if (line == 11 || line == 274) {
    if (role.starts_picture && Has("fm_white_flag")) return WhiteFlag();
    return None();
  }

  // NTSC field-2 biphase lines 279-281 map to field-1 lines 16-18.
  return GetCavProgrammeBiphase((line >= 279) ? (line - 263) : line, role);
}

// ---------------------------------------------------------------------------
// NTSC CLV programme_area
//
// FM lines (IEC 60857 §10.2):
//   Line 10/273:  fm_programme_time (40-bit FM)
//   Line 11/274:  fm_white_flag (100 IRE) on the picture's first field —
//                 line 11 when field 1 starts the picture, line 274 when
//                 field 2 does (§10.2.4)
//
// 24-bit biphase on lines 16-18 / 279-281: see GetClvProgrammeBiphase.
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetNtscClvProgramme(
    int line, const PictureFieldRole& role) const {
  // FM signal lines.
  if (line == 10 || line == 273) {
    if (Has("fm_programme_time")) return Fm("fm_programme_time");
    return None();
  }
  if (line == 11 || line == 274) {
    if (role.starts_picture && Has("fm_white_flag")) return WhiteFlag();
    return None();
  }

  // NTSC field-2 biphase lines 279-281 map to field-1 lines 16-18.
  return GetClvProgrammeBiphase((line >= 279) ? (line - 263) : line, role);
}

}  // namespace videosynth
