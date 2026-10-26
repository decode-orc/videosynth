/*
 * File:        line_placement_engine.h
 * Module:      line_placement_engine
 * Purpose:     Field-aware VBI line placement for LaserDisc biphase and 40-bit
 *              FM code injection per IEC 60856 (PAL) and IEC 60857 (NTSC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <set>
#include <string>
#include <vector>

#include "videosynth/biphase_types.h"
#include "videosynth/model.h"

namespace videosynth {

// Describes the biphase or FM code injection assignment for a single VBI line.
//
// is_fm and is_white_flag are mutually exclusive with a 24-bit biphase
// assignment (assigned=true, is_fm=false, is_white_flag=false).
// When is_fm=true the injection layer must use the 40-bit FM encoder.
// When is_white_flag=true the injection layer must fill the line at 100 IRE.
// uses_172h_offset=true means the signal must begin at 0.172 × line_period
// rather than at sample 0 of the active region. This applies to
// programme_status (PAL and NTSC) and clv_code (NTSC only).
struct LineCodeAssignment {
  bool assigned = false;
  std::string code_type;
  bool is_fm = false;
  bool is_white_flag = false;
  bool uses_172h_offset = false;
};

// Determines which biphase or FM code type should be injected on each VBI line,
// given a disc configuration and the set of code types present in the section.
//
// Usage pattern:
//   LinePlacementEngine engine(standard, disc_type, section_type,
//   codes_present); for each line in [1..lines_per_frame]:
//     bool f1 = LinePlacementEngine::IsFieldOne(standard, line);
//     auto a = engine.GetAssignment(line, f1);
//     if (a.assigned) { ... inject a.code_type ... }
//
// Line ranges queried outside the biphase-reserved VBI range always return
// an unassigned result (assigned=false).
//
// Placement rules per IEC 60856/60857 clause 10:
//   1. lead_in / lead_out codes have absolute priority in their sections.
//   2. Every output frame is one picture whose first field is field 1. That
//      field carries picture_number (CAV) or programme_time_code and
//      clv_picture_number (CLV); the other field carries picture_stop,
//      chapter_number, clv_code and programme_status (CAV programme_status
//      is written in both fields). A section with none of the first-field
//      codes writes the other-field codes in both fields.
//   3. picture_stop > programme_status on line 16/279/329 (CAV).
//   4. picture_stop > chapter_number on line 17/280/330 (CAV).
//   5. fm_picture_number / fm_programme_time occupy FM lines 10/273 (NTSC).
//   6. fm_white_flag occupies line 11 in lead-in and the programme area, and
//      lines 11 and 274 in lead-out (NTSC).
//
// Thread-safety: LinePlacementEngine is immutable after construction and may be
// called concurrently from multiple threads once constructed.
class LinePlacementEngine {
 public:
  // Constructs the engine.
  //   standard:       PAL or NTSC.
  //   disc_type:      CAV or CLV.
  //   section_type:   lead_in, programme_area, or lead_out.
  //   codes_present:  All code type strings declared for this section.
  //                   Drives priority resolution — only listed codes are
  //                   considered for placement.
  LinePlacementEngine(Standard standard, DiscType disc_type,
                      SectionType section_type,
                      const std::vector<std::string>& codes_present);

  // Returns the code assignment for the given VBI line.
  //   line_number:  1-based line number within the frame.
  //   field_one:    true when line_number is in field 1.  Typically derived
  //                 from IsFieldOne(); accepted as a parameter so callers can
  //                 pass a precomputed value or override for testing.
  //                 Decides which field carries the picture's first-field
  //                 codes in the programme area.
  //
  // Returns an assignment with assigned=false if no biphase or FM code should
  // be injected on this line (line is outside reserved range, or the section
  // configuration does not call for any code here).
  LineCodeAssignment GetAssignment(int line_number, bool field_one) const;

  // Returns true if line_number falls within the IEC-defined biphase VBI
  // reserved range for the given standard:
  //   PAL:  6–18  (field 1)  or  319–331 (field 2)
  //   NTSC: 10–18 (field 1)  or  273–281 (field 2)
  static bool IsInBiphaseReservedRange(Standard standard, int line_number);

  // Returns true if line_number is in field 1.
  //   PAL:  field 1 = lines 1–312
  //   NTSC: field 1 = lines 1–262
  static bool IsFieldOne(Standard standard, int line_number);

 private:
  Standard standard_;
  DiscType disc_type_;
  SectionType section_type_;
  std::set<std::string> codes_present_;

  // Returns true if code_type is in codes_present_.
  bool Has(const std::string& code_type) const;

  // Factory helpers — construct a LineCodeAssignment quickly.
  static LineCodeAssignment Biphase(const std::string& code_type,
                                    bool uses_172h_offset = false);
  static LineCodeAssignment Fm(const std::string& code_type);
  static LineCodeAssignment WhiteFlag();
  static LineCodeAssignment None();

  // Per-standard dispatch.
  LineCodeAssignment GetPalAssignment(int line, bool field_one) const;
  LineCodeAssignment GetNtscAssignment(int line, bool field_one) const;

  // Section-type and disc-type handlers.
  LineCodeAssignment GetPalLeadInOut(int line) const;
  LineCodeAssignment GetNtscLeadInOut(int line, bool field_one) const;
  LineCodeAssignment GetPalCavProgramme(int line, bool field_one) const;
  LineCodeAssignment GetPalClvProgramme(int line, bool field_one) const;
  LineCodeAssignment GetNtscCavProgramme(int line, bool field_one) const;
  LineCodeAssignment GetNtscClvProgramme(int line, bool field_one) const;

  // True when field_one is the picture's first field and the section carries
  // a code placed only there.
  bool IsPictureField(bool field_one) const;

  // Programme-area biphase for the field-1 line `canonical` (16-18).
  LineCodeAssignment GetCavProgrammeBiphase(int canonical,
                                            bool field_one) const;
  LineCodeAssignment GetClvProgrammeBiphase(int canonical,
                                            bool field_one) const;
};

}  // namespace videosynth
