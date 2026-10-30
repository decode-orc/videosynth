/*
 * File:        osd_token_resolver.h
 * Module:      osd
 * Purpose:     Resolves per-frame template tokens in OSD overlay text strings.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>

#include "videosynth/biphase_injection_manager.h"

namespace videosynth {

// Resolves per-frame template tokens embedded in OSD overlay text strings.
//
// Supported tokens (delimited by braces):
//   {picture_number} — zero-padded 5-digit CAV picture number (IEC 60856/60857
//                      max 99999); "00000" when no picture-number code is
//                      active.
//   {biphase_hex}    — space-separated 6-digit hex biphase words; "000000"
//                      when the active-generator list is empty.
//   {phase_id}       — colour-frame sequence index as a single decimal digit.
//   {section_name}   — the section's name field verbatim.
//   {timecode}       — CLV programme timecode HH:MM:SS:FF from the output frame
//                      position; "00:00:00:00" on non-CLV discs.
//   {frame_number}   — 1-based sequential frame position in the whole output,
//                      zero-padded to 5 digits.
//   {film_frame}     — 0-based source frame shown in the field being drawn.
//   {cadence}        — 2:3 pulldown cycle letter and field within it for the
//                      field being drawn (e.g. "B2"); "-" for a video cadence.
//
// Unavailable values render as all-zero fields of the same width as a real
// value so overlay layout never shifts between preview and disc variants.
//
// Static text (no token braces) is returned unchanged.  Unknown token names
// (e.g. {foo}) are caught at project-validation time by HasOnlyKnownTokens()
// and passed through unchanged at render time.
//
// Thread-safety: Resolve() is const and does not mutate shared state; multiple
// threads may call it concurrently on the same instance with independent
// arguments.
class OsdTokenResolver {
 public:
  OsdTokenResolver() = default;

  // Resolves all recognised tokens in text and returns the result.
  //
  // Args:
  //   text:         Raw overlay text, possibly containing {token} placeholders.
  //   ctx:          Per-frame context captured by BiphaseInjectionManager.
  //   section_name: The section name substituted for {section_name}.
  std::string Resolve(const std::string& text, const PerFrameContext& ctx,
                      const std::string& section_name) const;

  // Returns true if every {token} in text is one of the recognised names.
  // When false and unknown_token is non-null, *unknown_token is set to the
  // first unrecognised token name (without braces).
  static bool HasOnlyKnownTokens(const std::string& text,
                                 std::string* unknown_token = nullptr);
};

}  // namespace videosynth
