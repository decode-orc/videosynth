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
  if (!IsInBiphaseReservedRange(standard_, line_number)) {
    return None();
  }

  if (standard_ == Standard::kPal) {
    return GetPalAssignment(line_number, field_one);
  }
  return GetNtscAssignment(line_number, field_one);
}

// ---------------------------------------------------------------------------
// PAL dispatch
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetPalAssignment(int line,
                                                         bool field_one) const {
  if (section_type_ == SectionType::kLeadIn ||
      section_type_ == SectionType::kLeadOut) {
    return GetPalLeadInOut(line);
  }
  if (section_type_ == SectionType::kProgrammeArea) {
    if (disc_type_ == DiscType::kCAV) {
      return GetPalCavProgramme(line, field_one);
    }
    if (disc_type_ == DiscType::kCLV) {
      return GetPalClvProgramme(line, field_one);
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
// Every output frame is one picture whose first field is field 1, so the
// codes IEC 60856/60857 place in "the first field of the picture" go in
// field 1 and the codes placed in the remaining fields go in field 2. When a
// section carries none of the first-field codes, no field is a picture field
// and the remaining-field codes are written in both fields.
// ---------------------------------------------------------------------------

bool LinePlacementEngine::IsPictureField(bool field_one) const {
  if (!field_one) {
    return false;
  }
  if (disc_type_ == DiscType::kCAV) {
    return Has("picture_number");
  }
  return Has("programme_time_code") || Has("clv_picture_number");
}

// ---------------------------------------------------------------------------
// CAV programme_area biphase (IEC 60856 §10.1.4-10.1.7, IEC 60857 same):
//   Picture field, lines 16/17/18:  programme_status / picture_number x2
//   Other field,   line 16:         picture_stop > programme_status
//                  line 17:         picture_stop > chapter_number
//                  line 18:         chapter_number
// programme_status uses the 0.172H offset. `canonical` is the field-1 line.
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetCavProgrammeBiphase(
    int canonical, bool field_one) const {
  const bool picture_field = IsPictureField(field_one);

  switch (canonical) {
    case 16:
      if (!picture_field && Has("picture_stop")) {
        return Biphase("picture_stop");
      }
      if (Has("programme_status")) return Biphase("programme_status", true);
      return None();

    case 17:
      if (picture_field) return Biphase("picture_number");
      // IEC 60856 §10.1.6: picture stop has priority over chapter on line 17.
      if (Has("picture_stop")) return Biphase("picture_stop");
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
    int canonical, bool field_one) const {
  const bool picture_field = IsPictureField(field_one);

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

LineCodeAssignment LinePlacementEngine::GetPalCavProgramme(
    int line, bool field_one) const {
  // PAL field-2 biphase lines 329-331 map to field-1 lines 16-18.
  return GetCavProgrammeBiphase((line >= 329) ? (line - 313) : line, field_one);
}

LineCodeAssignment LinePlacementEngine::GetPalClvProgramme(
    int line, bool field_one) const {
  return GetClvProgrammeBiphase((line >= 329) ? (line - 313) : line, field_one);
}

// ---------------------------------------------------------------------------
// NTSC dispatch
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetNtscAssignment(
    int line, bool field_one) const {
  if (section_type_ == SectionType::kLeadIn ||
      section_type_ == SectionType::kLeadOut) {
    return GetNtscLeadInOut(line, field_one);
  }
  if (section_type_ == SectionType::kProgrammeArea) {
    if (disc_type_ == DiscType::kCAV) {
      return GetNtscCavProgramme(line, field_one);
    }
    if (disc_type_ == DiscType::kCLV) {
      return GetNtscClvProgramme(line, field_one);
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

LineCodeAssignment LinePlacementEngine::GetNtscLeadInOut(int line,
                                                         bool field_one) const {
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

  (void)field_one;
  return None();
}

// ---------------------------------------------------------------------------
// NTSC CAV programme_area
//
// FM lines (IEC 60857 §10.2):
//   Line 10/273:  fm_picture_number (40-bit FM)
//   Line 11:      fm_white_flag (100 IRE, first field only — §10.2.4)
//   Line 274:     (not used — white flag is first-field only in programme area)
//
// 24-bit biphase on lines 16-18 / 279-281: see GetCavProgrammeBiphase.
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetNtscCavProgramme(
    int line, bool field_one) const {
  // FM signal lines.
  if (line == 10 || line == 273) {
    if (Has("fm_picture_number")) return Fm("fm_picture_number");
    return None();
  }
  // White flag is first-field only in programme area (IEC 60857 §10.2.4).
  if (line == 11) {
    if (Has("fm_white_flag")) return WhiteFlag();
    return None();
  }
  if (line == 274) {
    return None();
  }

  // NTSC field-2 biphase lines 279-281 map to field-1 lines 16-18.
  return GetCavProgrammeBiphase((line >= 279) ? (line - 263) : line, field_one);
}

// ---------------------------------------------------------------------------
// NTSC CLV programme_area
//
// FM lines (IEC 60857 §10.2):
//   Line 10/273:  fm_programme_time (40-bit FM)
//   Line 11:      fm_white_flag (100 IRE, first field only — §10.2.4)
//   Line 274:     (not used — white flag is first-field only in programme area)
//
// 24-bit biphase on lines 16-18 / 279-281: see GetClvProgrammeBiphase.
// ---------------------------------------------------------------------------

LineCodeAssignment LinePlacementEngine::GetNtscClvProgramme(
    int line, bool field_one) const {
  // FM signal lines.
  if (line == 10 || line == 273) {
    if (Has("fm_programme_time")) return Fm("fm_programme_time");
    return None();
  }
  // White flag is first-field only in programme area (IEC 60857 §10.2.4).
  if (line == 11) {
    if (Has("fm_white_flag")) return WhiteFlag();
    return None();
  }
  if (line == 274) {
    return None();
  }

  // NTSC field-2 biphase lines 279-281 map to field-1 lines 16-18.
  return GetClvProgrammeBiphase((line >= 279) ? (line - 263) : line, field_one);
}

}  // namespace videosynth
