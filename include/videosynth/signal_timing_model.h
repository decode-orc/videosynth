/*
 * File:        signal_timing_model.h
 * Module:      signal_timing_model
 * Purpose:     Computes per-line sync and active-picture timing primitives.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

// Thread-safety: All functions and types in this module are thread-safe.
// They are stateless and only operate on their parameters or return new
// values. May be called concurrently from multiple threads.
enum class SyncPulseKind {
  kHorizontal,
  kEqualizing,
  kVerticalSync,
};

enum class LineContentKind {
  kVbiBlanking,
  kActivePicture,
};

struct LineTimingPrimitive {
  int line_number_1based = 0;
  int field_index_1based = 0;
  SyncPulseKind sync_pulse_kind = SyncPulseKind::kHorizontal;
  LineContentKind content_kind = LineContentKind::kActivePicture;
  bool has_two_half_line_pulses = false;
  bool burst_enabled = true;
  double burst_phase_rad = 0.0;
};

inline bool IsLineInRange(int line, int start, int end) {
  return line >= start && line <= end;
}

inline int Field1LineCount(Standard standard) {
  if (Is625LineStandard(standard)) {
    // ITU-R BT.1700 Annex 1 Part B Table 1 item 1: defines 625 lines/frame.
    // With 2:1 interlace this model treats lines 1-312 as field 1.
    return 312;
  }
  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // SMPTE 170M-2004 Section 11.3: defines 525 lines/frame.
    // PAL-M uses the same System M line structure as M/NTSC.
    // With 2:1 interlace this model treats lines 1-262 as field 1.
    return 262;
  }
  throw std::invalid_argument(
      "Field line count requested for unknown standard");
}

inline int GetFieldIndex(Standard standard, int line_1based) {
  return (line_1based <= Field1LineCount(standard)) ? 1 : 2;
}

inline SyncPulseKind GetSyncPulseKind(Standard standard, int line_1based) {
  if (Is625LineStandard(standard)) {
    // ITU-R BT.1700 Annex 1 Part B Figures 3-5 and Table 2 items l/m/n: the
    // pre-equalizing, field-sync, and post-equalizing sequences are 2.5H each,
    // so field 1's post-equalizing sequence ends before line 6 and line 6 is a
    // normal burst-carrying line (required for the Figure 8 burst-blanking
    // identification). Line-granular framing yields the regions below; mixed
    // half-line combinations are applied by the generation pulse schedule.
    // Line 623 starts with a normal line-sync pulse: its picture-carrying
    // first half ends before the mid-line start of the pre-equalizing
    // sequence, which the pulse schedule adds.
    if (IsLineInRange(line_1based, 4, 5) ||
        IsLineInRange(line_1based, 311, 313) ||
        IsLineInRange(line_1based, 316, 318) ||
        IsLineInRange(line_1based, 624, 625)) {
      return SyncPulseKind::kEqualizing;
    }
    if (IsLineInRange(line_1based, 1, 3) ||
        IsLineInRange(line_1based, 314, 315)) {
      return SyncPulseKind::kVerticalSync;
    }
    return SyncPulseKind::kHorizontal;
  }

  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // SMPTE 170M-2004 Section 13.3/Table 3: defines a 9-line vertical sync
    // block per field. PAL-M uses System M sync structure identical to M/NTSC.
    if (IsLineInRange(line_1based, 1, 3) || IsLineInRange(line_1based, 7, 9) ||
        IsLineInRange(line_1based, 264, 266) ||
        IsLineInRange(line_1based, 270, 272)) {
      return SyncPulseKind::kEqualizing;
    }
    if (IsLineInRange(line_1based, 4, 6) ||
        IsLineInRange(line_1based, 267, 269)) {
      return SyncPulseKind::kVerticalSync;
    }
    return SyncPulseKind::kHorizontal;
  }

  throw std::invalid_argument("Sync pulse kind requested for unknown standard");
}

inline LineContentKind GetLineContentKind(Standard standard, int line_1based) {
  if (Is625LineStandard(standard)) {
    // ITU-R BT.1700 Annex 1 Part B Table 1 item 1a and Table 2 item j: 576
    // active lines with a 25H + a field-blanking interval place active
    // picture on lines 23-310 and 336-623 (line 623 on its first half only;
    // see GetActiveRasterGeometry).
    if (IsLineInRange(line_1based, 16, 22) ||
        IsLineInRange(line_1based, 319, 335)) {
      return LineContentKind::kVbiBlanking;
    }
    return LineContentKind::kActivePicture;
  }

  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // SMPTE 170M-2004 Section 13.3/Table 3: defines a 20-line + 1.5 us
    // vertical blanking interval. PAL-M uses System M blanking structure.
    if (IsLineInRange(line_1based, 10, 21) ||
        IsLineInRange(line_1based, 263, 283)) {
      return LineContentKind::kVbiBlanking;
    }
    return LineContentKind::kActivePicture;
  }

  throw std::invalid_argument(
      "Line content kind requested for unknown standard");
}

inline bool HasTwoHalfLinePulses(SyncPulseKind kind) {
  return kind == SyncPulseKind::kEqualizing ||
         kind == SyncPulseKind::kVerticalSync;
}

inline double BurstPhaseRad(Standard standard, int line_1based) {
  constexpr double kPi = 3.14159265358979323846;

  if (standard == Standard::kNtsc) {
    // With absolute-time subcarrier synthesis in generation_stage, the
    // line-to-line π-radian progression arises naturally from 910 samples/line
    // at 4fsc. Keep a constant burst reference phase here so no artificial
    // per-line phase discontinuity is injected at line boundaries.
    constexpr double kNtscReferencePhase = kPi / 4.0;
    (void)line_1based;
    return kNtscReferencePhase;
  }

  if (standard == Standard::kPalM) {
    // ITU-R BT.470-6 Table 2 item 2.16: M/PAL burst phase alternates
    // ±135° relative to EU axis, line-to-line, same pattern as 625-line PAL.
    // The PAL-M colour sequence (Sequences I-IV) is managed in generation_stage
    // via PalBurstSequenceIndex; this baseline alternates ±135° per line.
    return ((line_1based % 2) == 1) ? (3.0 * kPi / 4.0) : (-3.0 * kPi / 4.0);
  }

  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f and Figure 8: PAL burst phase
  // alternates +135/-135 degrees line-to-line.
  return ((line_1based % 2) == 1) ? (3.0 * kPi / 4.0) : (-3.0 * kPi / 4.0);
}

inline bool BurstEnabledForLine(Standard standard, SyncPulseKind kind) {
  // ITU-R BT.1700 Annex 1 Part C Table 4 item 13: SECAM has no colour burst;
  // the SECAM chroma encoder emits the back-porch line identification instead.
  if (standard == Standard::kSecam) {
    return false;
  }
  // Baseline model: burst on normal horizontal lines only.
  return kind == SyncPulseKind::kHorizontal;
}

// Burst gate start and end, in samples after 0H.
inline int BurstStartSamples(Standard standard, double sample_rate_hz) {
  // ITU-R BT.470-6 Table 2 item 2.14g: burst gate start after epoch O_H.
  // M/NTSC: 5.3 µs nominal; B,G,H,I/PAL: 5.6 µs; M/PAL: 5.8 µs.
  double start_us = 5.6e-6;
  if (standard == Standard::kNtsc) {
    start_us = 5.3e-6;
  } else if (standard == Standard::kPalM) {
    start_us = 5.8e-6;
  }
  return std::max(0, static_cast<int>(std::lround(sample_rate_hz * start_us)));
}

inline int BurstEndSamples(Standard standard, double sample_rate_hz) {
  // ITU-R BT.470-6 Table 2 item 2.14g/h: burst gate end = start + duration.
  // M/NTSC: 5.3+2.67=7.97 µs; B,G,H,I/PAL: 5.6+2.25=7.85 µs (use 8.0 µs);
  // M/PAL: 5.8+2.52=8.32 µs.
  double end_us = 8.0e-6;
  if (standard == Standard::kNtsc) {
    end_us = 7.97e-6;
  } else if (standard == Standard::kPalM) {
    end_us = 8.32e-6;
  }
  return std::max(0, static_cast<int>(std::lround(sample_rate_hz * end_us)));
}

inline LineTimingPrimitive BuildLineTimingPrimitive(Standard standard,
                                                    int line_1based) {
  const SyncPulseKind sync_kind = GetSyncPulseKind(standard, line_1based);
  return LineTimingPrimitive{
      .line_number_1based = line_1based,
      .field_index_1based = GetFieldIndex(standard, line_1based),
      .sync_pulse_kind = sync_kind,
      .content_kind = GetLineContentKind(standard, line_1based),
      .has_two_half_line_pulses = HasTwoHalfLinePulses(sync_kind),
      .burst_enabled = BurstEnabledForLine(standard, sync_kind),
      .burst_phase_rad = BurstPhaseRad(standard, line_1based),
  };
}

inline std::vector<LineTimingPrimitive> BuildFrameTimingPrimitives(
    Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);

  std::vector<LineTimingPrimitive> lines;
  lines.reserve(static_cast<std::size_t>(timing.lines_per_frame));
  for (int line = 1; line <= timing.lines_per_frame; ++line) {
    lines.push_back(BuildLineTimingPrimitive(standard, line));
  }
  return lines;
}

}  // namespace videosynth
