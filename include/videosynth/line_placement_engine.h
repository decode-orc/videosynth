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

// A field's place in the picture sequence, which decides the programme-area
// codes it carries (IEC 60856/60857 clause 10). A video-cadence frame is one
// picture: field 1 starts it and field 2 follows it. Under 2:3 pulldown a
// picture lasts two or three fields, so a field may do either, both (a
// one-field picture after another picture) or neither (the third field of a
// three-field picture).
struct PictureFieldRole {
  // The field is the first field of a picture.
  bool starts_picture = false;
  // The field immediately follows the first field of a picture.
  bool follows_picture_start = false;
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
//   2. A picture's first field carries picture_number (CAV) or
//      programme_time_code and clv_picture_number (CLV). Every other field
//      carries chapter_number, clv_code and programme_status (CAV
//      programme_status is written in every field). A section with none of
//      the first-field codes writes the other-field codes in every field.
//   3. picture_stop goes in the field after a picture's first field (in
//      every field when the section has no picture_number). It has priority
//      over programme_status on line 16/279/329 and over chapter_number on
//      line 17/280/330 (CAV). When that field starts a picture of its own,
//      its picture_number keeps line 17/280 and the stop code is on line
//      16/279 only.
//   4. fm_picture_number / fm_programme_time occupy FM lines 10/273 (NTSC).
//   5. fm_white_flag occupies line 11 in lead-in, lines 11 and 274 in
//      lead-out, and in the programme area line 11 or 274 of each picture's
//      first field (NTSC).
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
  //                 The field's role is that of a video-cadence frame:
  //                 field 1 starts the picture and field 2 follows it.
  //
  // Returns an assignment with assigned=false if no biphase or FM code should
  // be injected on this line (line is outside reserved range, or the section
  // configuration does not call for any code here).
  LineCodeAssignment GetAssignment(int line_number, bool field_one) const;

  // As above, for a line whose field has the given role in the picture
  // sequence (used for 2:3 pulldown, where pictures span two or three
  // fields).
  LineCodeAssignment GetAssignment(int line_number,
                                   const PictureFieldRole& role) const;

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
  LineCodeAssignment GetPalAssignment(int line,
                                      const PictureFieldRole& role) const;
  LineCodeAssignment GetNtscAssignment(int line,
                                       const PictureFieldRole& role) const;

  // Section-type and disc-type handlers.
  LineCodeAssignment GetPalLeadInOut(int line) const;
  LineCodeAssignment GetNtscLeadInOut(int line) const;
  LineCodeAssignment GetNtscCavProgramme(int line,
                                         const PictureFieldRole& role) const;
  LineCodeAssignment GetNtscClvProgramme(int line,
                                         const PictureFieldRole& role) const;

  // True when the section carries a code placed only in a picture's first
  // field.
  bool HasPictureFieldCodes() const;

  // True when the field is a picture's first field and the section carries
  // a code placed only there.
  bool IsPictureField(const PictureFieldRole& role) const;

  // Programme-area biphase for the field-1 line `canonical` (16-18).
  LineCodeAssignment GetCavProgrammeBiphase(int canonical,
                                            const PictureFieldRole& role) const;
  LineCodeAssignment GetClvProgrammeBiphase(int canonical,
                                            const PictureFieldRole& role) const;
};

}  // namespace videosynth
