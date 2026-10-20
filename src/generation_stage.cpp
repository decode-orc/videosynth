/*
 * File:        generation_stage.cpp
 * Module:      generation_stage
 * Purpose:     Generates CVBS-domain Y and C sample buffers from frame-based
 * source data.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/generation_stage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "videosynth/active_sample_mapping.h"
#include "videosynth/biphase_injection_manager.h"
#include "videosynth/chroma_encoder.h"
#include "videosynth/clv_code_generator.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/osd_renderer.h"
#include "videosynth/osd_token_resolver.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/secam_chroma_encoder.h"
#include "videosynth/signal_shaping.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

// Sampled-domain synthesis detail types. They live in a named namespace rather
// than the file's anonymous namespace so that GenerationStage::
// SynthesisResources — a member of an externally-linked class — can hold them.
namespace generation_detail {

struct ActiveRasterGeometry {
  int first_active_line_field1 = 0;
  int first_active_line_field2 = 0;
  int active_lines_per_field = 0;
  int active_window_start_samples = 0;
  int active_window_end_samples = 0;
  int active_width_pixels = 720;
};

struct LinePulseSegment {
  int offset_samples = 0;
  SyncPulseKind kind = SyncPulseKind::kHorizontal;
};

// No line of any supported standard carries more than two sync pulses: one at
// the line start and, on half-line lines, one at the half-line point.
constexpr int kMaxLinePulseSegments = 2;

// The sync pulses of one frame line. The schedule is a pure function of
// (standard, line), so a run resolves all of them once instead of building a
// fresh vector for every line of every frame.
struct LinePulsePlan {
  int segment_count = 0;
  std::array<LinePulseSegment, kMaxLinePulseSegments> segments{};
};

// One shaped sync pulse, rendered once per (standard, pulse kind, levels).
// A pulse truncated by the end of its line has a different S-curve to the
// nominal one, so the sample loop only copies the table when the widths agree.
struct SyncPulseWaveform {
  int width_samples = 0;
  std::vector<SampleFixed> levels;
};

// One rendered LaserDisc pilot burst, as fixed-point additions to the sync tip.
//
// 17,734,475 = 25 × 709,379, so the pilot advances a whole number of cycles per
// PAL frame and its waveform repeats on every frame; each burst is therefore
// rendered once per run from its offset within the frame.
struct PilotBurstSegment {
  int offset_in_line = 0;
  std::vector<SampleFixed> samples;
};

struct PilotBurstLine {
  int segment_count = 0;
  std::array<PilotBurstSegment, kMaxLinePulseSegments> segments;
};

// Count of SyncPulseKind enumerators, used to size the pulse waveform table.
constexpr int kSyncPulseKindCount = 3;

struct SampledSynthesisContext {
  Standard standard = Standard::kUnknown;
  double sample_rate_hz = 0.0;
  int frame_samples = 0;
  std::vector<int> line_sample_counts;
  std::vector<int> line_sample_offsets;
  int max_line_samples = 0;
  std::vector<LineTimingPrimitive> frame_lines;
  std::vector<LinePulsePlan> line_pulse_plans;
  int burst_start_samples = 0;
  int burst_end_samples = 0;
  int sync_rise_samples = 0;
  int burst_rise_samples = 0;
  ActiveRasterGeometry active;
};

// Maps each active-window sample position to its source pixel column.
//
// MapActiveSampleToSourcePixel performs an integer division per call, and the
// mapping only depends on the active window width and the source's active
// raster, so the whole line's worth of positions is resolved once per source
// geometry rather than once per sample of every active line.
//
// The mapping is monotonically non-decreasing, so samples whose pixel column
// falls outside the source raster form a suffix; in_range_sample_count records
// where that suffix starts and lets the sample loop drop its bounds check.
struct ActiveSampleColumnMap {
  int active_width = -1;
  int active_x = -1;
  int source_width = -1;
  int in_range_sample_count = 0;
  std::vector<int> pixel_x;

  bool MatchesSource(const FrameSourceImage& source) const {
    return active_width == source.active_width && active_x == source.active_x &&
           source_width == source.width;
  }

  void Rebuild(const FrameSourceImage& source, int active_window_samples) {
    active_width = source.active_width;
    active_x = source.active_x;
    source_width = source.width;
    pixel_x.assign(static_cast<std::size_t>(active_window_samples), 0);
    in_range_sample_count = active_window_samples;
    for (int x_sample = 0; x_sample < active_window_samples; ++x_sample) {
      const int mapped =
          MapActiveSampleToSourcePixel(x_sample, active_window_samples,
                                       source.active_width, source.active_x);
      pixel_x[static_cast<std::size_t>(x_sample)] = mapped;
      if (mapped >= source.width && x_sample < in_range_sample_count) {
        in_range_sample_count = x_sample;
      }
    }
  }
};

}  // namespace generation_detail

namespace {

using generation_detail::ActiveRasterGeometry;
using generation_detail::ActiveSampleColumnMap;
using generation_detail::LinePulsePlan;
using generation_detail::LinePulseSegment;
using generation_detail::SampledSynthesisContext;
using generation_detail::SyncPulseWaveform;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kQuarterWaveRad = kPi / 2.0;

// Samples per subcarrier cycle at 4fsc. The carrier advances exactly π/2 per
// sample, so its phase is a pure function of the sample index modulo this
// value; see docs-tech/design/high-level-design.md, "Subcarrier Lock
// Implementation".
constexpr std::size_t kSubcarrierLatticeSamples = 4U;

// Number of frames after which a clean synthesised frame repeats exactly for
// identical source content. The subcarrier lattice position advances by
// (samples per frame mod 4) each frame — period 4 for PAL (709,379 ≡ 3) and
// PAL-M (477,225 ≡ 1), period 2 for NTSC (477,750 ≡ 2) — and the PAL/PAL-M
// burst meander and V-switch have period 2, which divides 4. The LaserDisc
// pilot burst is period 1 (17,734,475 = 25 × 709,379). SECAM chroma is
// rendered from 0H of every line, independent of the lattice position, and
// its D'R/D'B sequence and phase inversion repeat every 2 frames (ITU-R
// BT.1700 Annex 1 Part C Figure 16, Table 4 Note 1). Every use of the disc
// frame index in clean synthesis therefore depends only on the index modulo
// this period.
std::size_t ColourSequencePeriodFrames(Standard standard) {
  return (standard == Standard::kNtsc || standard == Standard::kSecam) ? 2U
                                                                       : 4U;
}

// Reduces a phase argument into [0, 2π).
//
// Every phase in the synthesiser is periodic, so this is mathematically a
// no-op; it exists so the arguments handed to sin/cos stay bounded no matter
// how long the render is, instead of growing with the absolute sample index and
// losing low-order precision.
double WrapPhaseRad(double phase_rad) {
  double wrapped = std::fmod(phase_rad, kTwoPi);
  if (wrapped < 0.0) {
    wrapped += kTwoPi;
  }
  return wrapped;
}

// Phase of the colour subcarrier at an absolute sample index, exactly.
//
// Reducing the index onto the 4-sample lattice before the multiply keeps the
// result exact for the life of the render: sin/cos see one of four bounded
// arguments rather than (π/2) × an index that reaches 1.9 × 10^11 on a
// three-hour disc.
double SubcarrierPhaseRad(std::size_t absolute_sample_index) {
  return kQuarterWaveRad *
         static_cast<double>(absolute_sample_index % kSubcarrierLatticeSamples);
}

// IEC 60856 §9.1.2: pilot burst frequency is 240 × fH = 3.75 MHz and
// amplitude is 6/7 of (white - blanking) = 6/7 × 700 mV = 600 mV p-p,
// centred on sync tip (−300 mV), giving ±300 mV swing.
constexpr double kPilotBurstFreqHz = 3.75e6;
constexpr double kPilotBurstAmplitudeMv = 300.0;

// ITU-R BT.1700 Annex 1 Part B Figure 8 with Table 1 item 10f: the 8-field
// PAL sequence pairs each burst-blanking meander position with a specific
// subcarrier-to-frame phase. Rotating the subcarrier lattice by 270° anchors
// disc frame 0 (meander parity 0, fields I/II) to the subcarrier phase that
// decoders identify as colour fields 1/2. Validated against the
// ld-decode/decode-orc field-phase detection: without this anchor the two
// fields of a frame decode as non-consecutive field IDs. The anchor applies
// to 625-line PAL only; PAL-M keeps a zero anchor.
constexpr double kPalSubcarrierAnchorRad = 3.0 * kPi / 2.0;

int BurstStartSamples(Standard standard, double sample_rate_hz);
int BurstEndSamples(Standard standard, double sample_rate_hz);
double SyncEdgeRiseTimeSeconds(Standard standard);
double BurstEnvelopeRiseTimeSeconds(Standard standard);
std::vector<generation_detail::LinePulsePlan> BuildLinePulsePlans(
    Standard standard, const std::vector<LineTimingPrimitive>& frame_lines,
    const std::vector<int>& line_sample_counts);

std::string Lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// A .exr source decodes to one image that is returned for every requested
// frame index (progressive_frame_source.cpp), so all schedule positions of a
// still section share a single source-frame identity for template caching.
bool SourceIgnoresFrameIndex(const std::string& source) {
  const std::string lowered = Lowercase(source);
  constexpr std::size_t kSuffixLength = 4;
  return lowered.size() >= kSuffixLength &&
         lowered.compare(lowered.size() - kSuffixLength, kSuffixLength,
                         ".exr") == 0;
}

ActiveRasterGeometry GetActiveRasterGeometry(Standard standard,
                                             double sample_rate_hz) {
  if (Is625LineStandard(standard)) {
    // ITU-R BT.1700 Annex 1 Part B Table 3: PAL line timing, mapped through
    // BT.601's 13.5 MHz sampling model. Keep the established PAL line start
    // anchor at +177 4fsc samples, but limit active-picture synthesis to
    // the 52.0 us visible-aperture duration.
    const int active_window_start = 177;
    const int active_window_end =
        active_window_start +
        std::max(1, static_cast<int>(std::lround(sample_rate_hz * 52.0e-6)));
    return ActiveRasterGeometry{
        .first_active_line_field1 = 23,
        .first_active_line_field2 = 335,
        .active_lines_per_field = 288,
        .active_window_start_samples = active_window_start,
        .active_window_end_samples = active_window_end,
        .active_width_pixels = 720,
    };
  }

  const int active_window_start =
      std::max(0, static_cast<int>(std::lround(sample_rate_hz * 10.5e-6)));
  const int active_window_end =
      std::max(active_window_start + 1,
               static_cast<int>(std::lround(sample_rate_hz * 62.5e-6)));

  return ActiveRasterGeometry{
      .first_active_line_field1 = 22,
      .first_active_line_field2 = 284,
      .active_lines_per_field = 240,
      .active_window_start_samples = active_window_start,
      .active_window_end_samples = active_window_end,
      .active_width_pixels = 720,
  };
}

SampledSynthesisContext BuildSampledSynthesisContext(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  SampledSynthesisContext context;
  context.standard = standard;
  context.sample_rate_hz = timing.sample_rate_4fsc_hz;
  context.frame_samples = SamplesPerFrame4fsc(standard);
  context.line_sample_counts = BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc);
  context.line_sample_offsets =
      BuildLineSampleOffsets(context.line_sample_counts);
  context.max_line_samples = MaxLineSamples(context.line_sample_counts);
  context.frame_lines = BuildFrameTimingPrimitives(standard);
  context.line_pulse_plans = BuildLinePulsePlans(standard, context.frame_lines,
                                                 context.line_sample_counts);
  context.burst_start_samples =
      BurstStartSamples(standard, context.sample_rate_hz);
  context.burst_end_samples = BurstEndSamples(standard, context.sample_rate_hz);
  context.sync_rise_samples = RiseTimeToRampSamples(
      SyncEdgeRiseTimeSeconds(standard), context.sample_rate_hz);
  context.burst_rise_samples = RiseTimeToRampSamples(
      BurstEnvelopeRiseTimeSeconds(standard), context.sample_rate_hz);
  context.active = GetActiveRasterGeometry(standard, context.sample_rate_hz);
  return context;
}

// Returns the picture_number start_value from a section's laserdisc injection,
// or 0 if the section has no picture_number code.
int FindSectionPictureNumberStart(const Section& section) {
  for (const Section::LineInjection& inj : section.line_injections) {
    if (Lowercase(inj.type) != "laserdisc") {
      continue;
    }
    for (const Section::LineInjectionCode& code : inj.codes) {
      if (code.code_type == "picture_number" && code.start_value_specified) {
        return code.start_value;
      }
    }
  }
  return 0;
}

bool BuildFramePatternSchedule(
    const Project& project, const ProgressiveFrameSource& progressive_source,
    Standard standard,
    std::vector<std::pair<const Section*, int>>* out_frame_sections,
    std::string* error) {
  if (out_frame_sections == nullptr) {
    return false;
  }

  out_frame_sections->clear();
  for (const Section& section : project.sections) {
    if (section.type == "progressive") {
      if (!progressive_source.SupportsSection(section)) {
        if (error != nullptr) {
          *error = "Unsupported progressive source family in section schedule.";
        }
        return false;
      }

      if (section.duration_frames_all) {
        int resolved_frame_count = 0;
        std::string count_error;
        if (!progressive_source.ResolveFrameCount(
                section, standard, &resolved_frame_count, &count_error)) {
          if (error != nullptr) {
            *error = count_error.empty()
                         ? "Failed to resolve progressive section frame count "
                           "for duration_frames='all'."
                         : count_error;
          }
          return false;
        }

        if (resolved_frame_count <= 0) {
          if (error != nullptr) {
            *error =
                "Progressive section duration_frames='all' resolved to zero "
                "frames.";
          }
          return false;
        }

        // Replay the whole resolved source duration_frames_repeat times. Each
        // pass re-emits source frames 0..N-1 (a loop), so the source frame
        // index never exceeds the source length regardless of the multiplier.
        const int repeat = section.duration_frames_repeat > 0
                               ? section.duration_frames_repeat
                               : 1;
        for (int pass = 0; pass < repeat; ++pass) {
          for (int i = 0; i < resolved_frame_count; ++i) {
            out_frame_sections->push_back(
                std::make_pair(&section, i + section.start_frame));
          }
        }
        continue;
      }

      if (section.duration_frames <= 0) {
        if (error != nullptr) {
          *error =
              "Progressive sections must define duration_frames > 0 or 'all'.";
        }
        return false;
      }

      for (int i = 0; i < section.duration_frames; ++i) {
        out_frame_sections->push_back(
            std::make_pair(&section, i + section.start_frame));
      }
      continue;
    }

    if (error != nullptr) {
      *error = "Unsupported section type in generation schedule.";
    }
    return false;
  }

  return !out_frame_sections->empty();
}

int ActiveFrameLineIndex(const ActiveRasterGeometry& geometry,
                         [[maybe_unused]] Standard standard, int line_1based) {
  if (line_1based >= geometry.first_active_line_field1 &&
      line_1based < (geometry.first_active_line_field1 +
                     geometry.active_lines_per_field)) {
    return line_1based - geometry.first_active_line_field1;
  }
  if (line_1based >= geometry.first_active_line_field2 &&
      line_1based < (geometry.first_active_line_field2 +
                     geometry.active_lines_per_field)) {
    return geometry.active_lines_per_field +
           (line_1based - geometry.first_active_line_field2);
  }

  return -1;
}

int InvertCenteredChromaCode(int code) { return 1024 - code; }

double LumaMillivoltsFromCode(int y_code, const SignalLevels& levels) {
  const double y_norm = static_cast<double>(y_code - 64) / 876.0;
  return levels.black_mv + (y_norm * (levels.white_mv - levels.black_mv));
}

// Number of entries in the luma code lookup table: the whole 10-bit code space
// the frame sources clamp their samples into (progressive_frame_source.cpp).
constexpr int kLumaCodeTableSize = 1024;

// Builds the luma code -> fixed-point millivolt table. The mapping is a pure
// function of the signal levels, so a run resolves it once instead of doing a
// divide, a multiply-add and an llround on every active sample.
std::vector<SampleFixed> BuildLumaCodeTable(const SignalLevels& levels) {
  std::vector<SampleFixed> table(static_cast<std::size_t>(kLumaCodeTableSize));
  for (int code = 0; code < kLumaCodeTableSize; ++code) {
    table[static_cast<std::size_t>(code)] =
        MillivoltsToSampleFixed(LumaMillivoltsFromCode(code, levels));
  }
  return table;
}

double PulseWidthSeconds(SyncPulseKind kind, Standard standard) {
  if (kind == SyncPulseKind::kHorizontal) {
    return 4.7e-6;
  }
  if (kind == SyncPulseKind::kEqualizing) {
    return 2.3e-6;
  }
  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // SMPTE 170M-2004 Table 1-2 item q: broad pulses 27.1 µs for System M.
    // PAL-M uses the same System M sync structure as M/NTSC.
    return 27.1e-6;
  }
  return 27.3e-6;
}

int PulseWidthSamples(SyncPulseKind kind, Standard standard,
                      double sample_rate_hz) {
  const double pulse_seconds = PulseWidthSeconds(kind, standard);
  return std::max(
      1, static_cast<int>(std::lround(sample_rate_hz * pulse_seconds)));
}

int BurstStartSamples(Standard standard, double sample_rate_hz) {
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

int BurstEndSamples(Standard standard, double sample_rate_hz) {
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

LinePulsePlan OnePulse(SyncPulseKind kind) {
  return LinePulsePlan{
      .segment_count = 1,
      .segments = {LinePulseSegment{.offset_samples = 0, .kind = kind},
                   LinePulseSegment{}},
  };
}

LinePulsePlan TwoPulses(SyncPulseKind first_kind, SyncPulseKind second_kind,
                        int half_line_samples) {
  return LinePulsePlan{
      .segment_count = 2,
      .segments = {LinePulseSegment{.offset_samples = 0, .kind = first_kind},
                   LinePulseSegment{.offset_samples = half_line_samples,
                                    .kind = second_kind}},
  };
}

LinePulsePlan BuildLinePulseSchedule(const LineTimingPrimitive& line,
                                     Standard standard, int half_line_samples) {
  if (Is625LineStandard(standard)) {
    const int line_1based = line.line_number_1based;
    if (line_1based == 1 || line_1based == 2 || line_1based == 314 ||
        line_1based == 315) {
      return TwoPulses(SyncPulseKind::kVerticalSync,
                       SyncPulseKind::kVerticalSync, half_line_samples);
    }
    if (line_1based == 3) {
      return TwoPulses(SyncPulseKind::kVerticalSync, SyncPulseKind::kEqualizing,
                       half_line_samples);
    }
    if (line_1based == 313) {
      return TwoPulses(SyncPulseKind::kEqualizing, SyncPulseKind::kVerticalSync,
                       half_line_samples);
    }
    if (line_1based == 6 || line_1based == 318) {
      return OnePulse(SyncPulseKind::kEqualizing);
    }
    if (line_1based == 4 || line_1based == 5 || line_1based == 311 ||
        line_1based == 312 || line_1based == 316 || line_1based == 317 ||
        line_1based == 623 || line_1based == 624 || line_1based == 625) {
      return TwoPulses(SyncPulseKind::kEqualizing, SyncPulseKind::kEqualizing,
                       half_line_samples);
    }
  }

  if ((standard == Standard::kNtsc || standard == Standard::kPalM) &&
      line.line_number_1based == 263) {
    // SMPTE 170M-2004 / ITU-R BT.470-6 System M: line 263 is the field-2
    // transition half-line in System M (NTSC and PAL-M share this structure).
    return TwoPulses(SyncPulseKind::kHorizontal, SyncPulseKind::kEqualizing,
                     half_line_samples);
  }

  if (!line.has_two_half_line_pulses) {
    return OnePulse(line.sync_pulse_kind);
  }

  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // Align the line-granular model with the SMPTE 170M field-2 transition
    // shape seen in 4fsc reference material, where mixed half-line pulse kinds
    // occur around frame lines 263/266/269/272. PAL-M uses the same System M
    // sync structure as M/NTSC.
    if (line.line_number_1based == 266) {
      return TwoPulses(SyncPulseKind::kEqualizing, SyncPulseKind::kVerticalSync,
                       half_line_samples);
    }
    if (line.line_number_1based == 269) {
      return TwoPulses(SyncPulseKind::kVerticalSync, SyncPulseKind::kEqualizing,
                       half_line_samples);
    }
    if (line.line_number_1based == 272) {
      return OnePulse(SyncPulseKind::kEqualizing);
    }
  }

  return TwoPulses(line.sync_pulse_kind, line.sync_pulse_kind,
                   half_line_samples);
}

// Where one scheduled sync pulse starts and ends within its line.
//
// Shared by frame synthesis and the pilot burst table builder so the two can
// never disagree about where a pulse sits.
struct PulseSegmentBounds {
  int start_in_line = 0;
  int end_in_line = 0;
};

PulseSegmentBounds ResolvePulseSegmentBounds(const LinePulseSegment& segment,
                                             int nominal_width_samples,
                                             int line_samples) {
  const int start = std::min(segment.offset_samples, line_samples - 1);
  return PulseSegmentBounds{
      .start_in_line = start,
      .end_in_line = std::min(start + nominal_width_samples, line_samples),
  };
}

// Resolves the sync pulse schedule of every frame line once per run.
std::vector<LinePulsePlan> BuildLinePulsePlans(
    Standard standard, const std::vector<LineTimingPrimitive>& frame_lines,
    const std::vector<int>& line_sample_counts) {
  std::vector<LinePulsePlan> plans;
  plans.reserve(frame_lines.size());
  for (const LineTimingPrimitive& line : frame_lines) {
    const auto line_index =
        static_cast<std::size_t>(line.line_number_1based - 1);
    const int line_samples = (line_index < line_sample_counts.size())
                                 ? line_sample_counts[line_index]
                                 : 0;
    plans.push_back(
        BuildLinePulseSchedule(line, standard, (line_samples + 1) / 2));
  }
  return plans;
}

// Renders every LaserDisc pilot burst of a frame once per run.
//
// IEC 60856 §9.1.2 fig 6: a triangle burst rides on each sync pulse, capped at
// q = 13.5 periods for line and field sync and r = 6 periods for equalizing
// pulses; the remainder of a broad field-sync pulse stays at sync tip.
//
// The pilot is period-1 in frames (17,734,475 = 25 × 709,379), so each burst
// depends only on its offset within the frame. Deriving the phase from that
// offset rather than from the absolute sample index makes every frame's pilot
// identical by construction and keeps the phase bounded, where the absolute
// index both drifted and — held in an int — overflowed past ~3,000 frames.
std::vector<generation_detail::PilotBurstLine> BuildPilotBurstLines(
    const SampledSynthesisContext& synth,
    const std::array<SyncPulseWaveform, generation_detail::kSyncPulseKindCount>&
        pulse_waveforms,
    double pilot_omega, int pilot_q_samples, int pilot_r_samples) {
  std::vector<generation_detail::PilotBurstLine> pilot_lines(
      synth.line_sample_counts.size());

  for (const LineTimingPrimitive& line : synth.frame_lines) {
    const auto line_index =
        static_cast<std::size_t>(line.line_number_1based - 1);
    if (line_index >= pilot_lines.size()) {
      continue;
    }

    const int line_samples = synth.line_sample_counts[line_index];
    const int line_offset = synth.line_sample_offsets[line_index];
    const LinePulsePlan& plan = synth.line_pulse_plans[line_index];
    generation_detail::PilotBurstLine& pilot_line = pilot_lines[line_index];

    for (int segment_index = 0; segment_index < plan.segment_count;
         ++segment_index) {
      const LinePulseSegment& segment =
          plan.segments[static_cast<std::size_t>(segment_index)];
      const SyncPulseWaveform& waveform =
          pulse_waveforms[static_cast<std::size_t>(segment.kind)];
      const PulseSegmentBounds bounds = ResolvePulseSegmentBounds(
          segment, waveform.width_samples, line_samples);

      const int max_burst_samples = (segment.kind == SyncPulseKind::kEqualizing)
                                        ? pilot_r_samples
                                        : pilot_q_samples;
      const int flat_start = bounds.start_in_line + synth.sync_rise_samples;
      const int flat_end =
          std::min(bounds.end_in_line - synth.sync_rise_samples,
                   flat_start + max_burst_samples);
      if (flat_start >= flat_end) {
        continue;
      }

      generation_detail::PilotBurstSegment& pilot_segment =
          pilot_line
              .segments[static_cast<std::size_t>(pilot_line.segment_count)];
      pilot_segment.offset_in_line = flat_start;
      pilot_segment.samples.clear();
      pilot_segment.samples.reserve(
          static_cast<std::size_t>(flat_end - flat_start));

      double phase = WrapPhaseRad(
          pilot_omega * static_cast<double>(line_offset + flat_start));
      for (int i = flat_start; i < flat_end; ++i) {
        const double phase_frac = phase / kTwoPi;
        const double triangle = (phase_frac < 0.5) ? (4.0 * phase_frac - 1.0)
                                                   : (3.0 - 4.0 * phase_frac);
        pilot_segment.samples.push_back(
            MillivoltsToSampleFixed(kPilotBurstAmplitudeMv * triangle));

        // One subtraction suffices: the pilot advances well under a full cycle
        // per sample (3.75 MHz against a 17.734475 MHz sample rate).
        phase += pilot_omega;
        if (phase >= kTwoPi) {
          phase -= kTwoPi;
        }
      }

      ++pilot_line.segment_count;
    }
  }

  return pilot_lines;
}

double SyncEdgeRiseTimeSeconds(Standard standard) {
  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // SMPTE 170M-2004 Table 2 plus Note 1: sync pulse rise/fall 140 ns ± 20 ns
    // for System M. PAL-M uses System M sync structure.
    return 140.0e-9;
  }
  // ITU-R BT.1700 Annex 1 Part B Table 2 item f and Table 3 item s: 625 PAL
  // sync/equalizing edge rise/fall 200 ns ± 100 ns measured 10%-90%.
  return 200.0e-9;
}

double BurstEnvelopeRiseTimeSeconds(Standard standard) {
  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // SMPTE 170M-2004 Table 2: burst envelope rise 300 ns (+200/-100) 10%-90%
    // for System M. PAL-M uses the same M-system burst envelope shape.
    return 300.0e-9;
  }
  // ITU-R BT.1700 Annex 1 Part B Table 2 items g/h: define PAL burst placement
  // and duration, and item e defines line-blanking edge rise of 300 ns ±
  // 100 ns. This model uses the same time constant to apply a finite PAL burst
  // gate envelope.
  return 300.0e-9;
}

int PalBurstSequenceIndex(std::size_t frame_index, int field_index_1based) {
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: defines sequence I/II/III/IV
  // repeating over colour fields. A frame contains two consecutive fields.
  return static_cast<int>(
      ((2U * frame_index) + static_cast<std::size_t>(field_index_1based - 1)) %
      4U);
}

bool PalBurstPositiveOnOddLine(int burst_sequence_index) {
  // Sequence I/II: odd lines use +135 deg, even lines use -135 deg.
  // Sequence III/IV: odd lines use -135 deg, even lines use +135 deg.
  return burst_sequence_index == 0 || burst_sequence_index == 1;
}

bool IsPalBurstBlankedLine(int line_1based, int colour_frame_parity) {
  // ITU-R BT.1700 Annex 1 Part B Figure 8: the four 9-line burst-blanking
  // windows are defined in frame-line numbers and span frame boundaries:
  //   I:   lines 623-006    III: lines 622-005
  //   II:  lines 310-318    IV:  lines 311-319
  // An even-parity colour frame carries fields I/II, so it takes window I's
  // head (lines 1-6), window II (310-318), and window III's start (622-625,
  // preceding the next frame's field III). An odd-parity frame carries fields
  // III/IV and takes lines 1-5, 311-319, and window I's start (623-625).
  if (colour_frame_parity == 0) {
    return line_1based <= 6 || (line_1based >= 310 && line_1based <= 318) ||
           line_1based >= 622;
  }
  return line_1based <= 5 || (line_1based >= 311 && line_1based <= 319) ||
         line_1based >= 623;
}

bool IsPalMBurstBlankedLine(int line_1based, int colour_frame_parity) {
  // ITU-R BT.1700 Annex 1 Part B Figure 9: the four 11-line burst-blanking
  // windows for 525-line M/PAL, defined in frame-line numbers and spanning
  // frame boundaries:
  //   I:   lines 523-008    III: lines 522-007
  //   II:  lines 260-270    IV:  lines 259-269
  // An even-parity colour frame carries fields I/II, so it takes window I's
  // head (lines 1-8), window II (260-270), and window III's start (522-525,
  // preceding the next frame's field III). An odd-parity frame carries fields
  // III/IV and takes lines 1-7, 259-269, and window I's start (523-525).
  if (colour_frame_parity == 0) {
    return line_1based <= 8 || (line_1based >= 260 && line_1based <= 270) ||
           line_1based >= 522;
  }
  return line_1based <= 7 || (line_1based >= 259 && line_1based <= 269) ||
         line_1based >= 523;
}

double PalBurstPhaseRadForLine(std::size_t frame_index,
                               const LineTimingPrimitive& line) {
  const int burst_sequence_index =
      PalBurstSequenceIndex(frame_index, line.field_index_1based);
  const bool line_is_odd = (line.line_number_1based % 2) == 1;
  const bool positive_on_odd = PalBurstPositiveOnOddLine(burst_sequence_index);
  const bool positive_phase = line_is_odd ? positive_on_odd : !positive_on_odd;
  return positive_phase ? (3.0 * kPi / 4.0) : (-3.0 * kPi / 4.0);
}

bool PalBurstEnabledForLine(std::size_t frame_index,
                            const LineTimingPrimitive& line) {
  if (line.sync_pulse_kind != SyncPulseKind::kHorizontal) {
    return false;
  }
  const int colour_frame_parity = static_cast<int>(frame_index % 2U);
  return !IsPalBurstBlankedLine(line.line_number_1based, colour_frame_parity);
}

bool PalMBurstEnabledForLine(std::size_t frame_index,
                             const LineTimingPrimitive& line) {
  if (line.sync_pulse_kind != SyncPulseKind::kHorizontal) {
    return false;
  }
  const int colour_frame_parity = static_cast<int>(frame_index % 2U);
  return !IsPalMBurstBlankedLine(line.line_number_1based, colour_frame_parity);
}

bool PalInvertVAxisForLine(std::size_t frame_index,
                           const LineTimingPrimitive& line) {
  return PalBurstPhaseRadForLine(frame_index, line) < 0.0;
}

using RenderedVitsLineMap =
    std::unordered_map<int, std::shared_ptr<const VitsRenderedLine>>;

// Builds the project-wide VITS render state: every targeted frame line is
// mapped to its rendered sample buffer. VITS are a project-level setting, so
// the same set is applied to every frame regardless of section. A rendered
// line is a pure function of (vits_type, line_samples, standard), so lines
// sharing both share a single rendering and the whole map is frame-invariant.
bool BuildRenderedVitsLines(
    const std::vector<VitsInjection>& vits_set, Standard standard,
    double sample_rate_hz, const std::vector<int>& line_sample_counts,
    const IVitsDefinitionProvider& vits_definition_provider,
    const IVitsGenerator& vits_generator, RenderedVitsLineMap* out_lines,
    std::string* error) {
  if (out_lines == nullptr) {
    if (error != nullptr) {
      *error = "Internal generation error: null VITS state output.";
    }
    return false;
  }

  out_lines->clear();

  std::unordered_map<std::string, VitsSynthesisPlan> plans;
  // Keyed by "<vits_type>@<line_samples>" so a rendering is reused by every
  // targeted line of the same length.
  std::unordered_map<std::string, std::shared_ptr<const VitsRenderedLine>>
      renderings;

  for (const VitsInjection& injection : vits_set) {
    VitsSynthesisPlan* cached_plan = nullptr;
    const auto existing_plan = plans.find(injection.vits_type);
    if (existing_plan == plans.end()) {
      VitsDefinition definition;
      std::string definition_error;
      if (!vits_definition_provider.TryGetDefinition(
              standard, injection.vits_type, &definition, &definition_error)) {
        if (error != nullptr) {
          *error = definition_error.empty()
                       ? "Failed to resolve VITS definition for line injection."
                       : definition_error;
        }
        return false;
      }

      VitsSynthesisPlan plan;
      std::string plan_error;
      if (!vits_generator.BuildSynthesisPlan(definition, &plan, &plan_error)) {
        if (error != nullptr) {
          *error = plan_error.empty() ? "Failed to build VITS synthesis plan."
                                      : plan_error;
        }
        return false;
      }

      if (plan.primitives.empty() && plan.render_order.empty()) {
        if (error != nullptr) {
          *error = "VITS type '" + injection.vits_type +
                   "' has no renderable primitives in the current runtime.";
        }
        return false;
      }

      cached_plan =
          &plans.emplace(injection.vits_type, std::move(plan)).first->second;
    } else {
      cached_plan = &existing_plan->second;
    }

    for (int target_line : injection.target_lines) {
      const auto line_index = static_cast<std::size_t>(target_line - 1);
      if (target_line < 1 || line_index >= line_sample_counts.size()) {
        continue;
      }

      const int line_samples = line_sample_counts[line_index];
      const std::string rendering_key =
          injection.vits_type + "@" + std::to_string(line_samples);
      auto rendering = renderings.find(rendering_key);
      if (rendering == renderings.end()) {
        auto rendered_line = std::make_shared<VitsRenderedLine>();
        std::string render_error;
        if (!vits_generator.RenderLine(*cached_plan, sample_rate_hz,
                                       line_samples, rendered_line.get(),
                                       &render_error)) {
          if (error != nullptr) {
            *error = render_error.empty()
                         ? "Failed to render VITS line injection."
                         : render_error;
          }
          return false;
        }
        rendering =
            renderings.emplace(rendering_key, std::move(rendered_line)).first;
      }

      (*out_lines)[target_line] = rendering->second;
    }
  }

  return true;
}

}  // namespace

// Frame-invariant synthesis resources for a single worker thread.
//
// Everything here is a pure function of the project's CVBS presets and VITS
// set: the sampled timing context, the chroma encoder (which owns mutable
// per-line workspaces and therefore cannot be shared between threads), the
// rendered VITS lines, and the VBI waveform renderer. A worker builds the set
// once and reuses it for every frame it synthesises.
struct GenerationStage::SynthesisResources {
  // Configuration the resources were built from; used to detect a project
  // change and rebuild.
  CvbsPresets presets;
  std::vector<VitsInjection> vits;

  TimingConstants timing;
  SignalLevels levels;
  generation_detail::SampledSynthesisContext synth;

  int active_window_start = 0;
  int active_window_end = 0;
  int active_window_samples = 0;
  SampleFixed blanking_fixed = 0;
  double burst_amplitude_mv = 0.0;

  // Luma code -> fixed-point millivolts, indexed by 10-bit source code.
  std::vector<SampleFixed> luma_code_table;

  // Active-sample to source-column mapping for the source most recently
  // synthesised by this worker. Held here rather than per call because the pool
  // invokes GenerateFrameBatch once per frame, so a per-call table would be
  // rebuilt every frame instead of only when a section change brings in a
  // source with a different active raster.
  generation_detail::ActiveSampleColumnMap column_map;

  // Shaped sync pulses, indexed by SyncPulseKind, and the burst gate envelope
  // pre-scaled by the burst amplitude. Both are pure functions of the standard
  // and the signal levels.
  std::array<generation_detail::SyncPulseWaveform,
             generation_detail::kSyncPulseKindCount>
      pulse_waveforms;
  int burst_width_samples = 0;
  std::vector<double> burst_envelope_mv;

  bool pal_pilot_burst = false;
  double pilot_omega = 0.0;
  int pilot_q_samples = 0;
  int pilot_r_samples = 0;
  // Pre-rendered pilot bursts per frame line; empty unless the pilot is on.
  std::vector<generation_detail::PilotBurstLine> pilot_burst_lines;

  // Exactly one chroma encoder is set: the quadrature encoder for PAL, NTSC
  // and PAL-M, or the SECAM encoder for SECAM.
  std::unique_ptr<IChromaEncoder> chroma_encoder;
  std::unique_ptr<ISecamChromaEncoder> secam_encoder;
  bool secam_field_identification = false;
  // SECAM only: colourless (undeviated) chroma of a nominal-length line for
  // each D'R/D'B component and 0H phase, indexed [component][phase is π], and
  // per frame parity a per-frame-line pointer into them. The OSD renderer
  // writes these under its text, where a SECAM signal still carries the
  // undeviated subcarrier.
  std::array<std::array<std::vector<SampleFixed>, 2>, 2> secam_neutral_chroma;
  std::array<std::vector<const SampleFixed*>, 2> secam_neutral_chroma_lines;
  RenderedVitsLineMap vits_lines;
  std::unique_ptr<VbiWaveformRenderer> vbi_renderer;

  // Per-line workspaces for clean-frame synthesis, sized to the active window
  // at build time and reused for every line of every frame. Worker-private,
  // like the chroma encoder's internal buffers.
  std::vector<YCbCr444Pixel> line_source_samples;
  std::vector<int> active_sample_indices;
  std::vector<SampleFixed> encoded_line_chroma;
  std::vector<SampleFixed> secam_line_chroma;
};

// Per-worker cache of the frame-invariant synthesis resources.
//
// Thread-safety: Acquire() may be called concurrently. Each calling thread
// receives its own SynthesisResources instance, so only the lookup table is
// mutex-guarded; the returned resources are never touched by another thread
// and entries are never erased, so the returned pointer stays valid for the
// lifetime of the cache.
class GenerationStage::SynthesisResourceCache {
 public:
  // Returns the calling thread's resources, building them on first use and
  // rebuilding them if the project's synthesis configuration has changed.
  // Returns nullptr and appends to errors when the VITS set cannot be built.
  SynthesisResources* Acquire(
      const Project& project,
      const IVitsDefinitionProvider& vits_definition_provider,
      const IVitsGenerator& vits_generator, std::vector<std::string>* errors);

 private:
  static bool Build(const Project& project,
                    const IVitsDefinitionProvider& vits_definition_provider,
                    const IVitsGenerator& vits_generator,
                    SynthesisResources* out, std::vector<std::string>* errors);

  std::mutex mutex_;
  std::unordered_map<std::thread::id, std::unique_ptr<SynthesisResources>>
      per_thread_;
};

GenerationStage::SynthesisResources*
GenerationStage::SynthesisResourceCache::Acquire(
    const Project& project,
    const IVitsDefinitionProvider& vits_definition_provider,
    const IVitsGenerator& vits_generator, std::vector<std::string>* errors) {
  SynthesisResources* resources = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<SynthesisResources>& slot =
        per_thread_[std::this_thread::get_id()];
    if (slot == nullptr) {
      slot = std::make_unique<SynthesisResources>();
    }
    resources = slot.get();
  }

  if ((resources->chroma_encoder != nullptr ||
       resources->secam_encoder != nullptr) &&
      resources->presets == project.cvbs_presets &&
      resources->vits == project.line_injections.vits) {
    return resources;
  }

  if (!Build(project, vits_definition_provider, vits_generator, resources,
             errors)) {
    return nullptr;
  }
  return resources;
}

bool GenerationStage::SynthesisResourceCache::Build(
    const Project& project,
    const IVitsDefinitionProvider& vits_definition_provider,
    const IVitsGenerator& vits_generator, SynthesisResources* out,
    std::vector<std::string>* errors) {
  const Standard standard = project.cvbs_presets.video_standard_preset;

  // Leave the cache invalid until the build completes, so a failed build is
  // never mistaken for a usable set on the next call.
  out->chroma_encoder.reset();
  out->secam_encoder.reset();
  out->timing = GetTimingConstants(standard);
  out->levels = GetSignalLevels(project.cvbs_presets);
  out->synth = BuildSampledSynthesisContext(standard);

  out->active_window_start =
      std::max(0, std::min(out->synth.active.active_window_start_samples,
                           out->synth.max_line_samples - 1));
  out->active_window_end =
      std::max(out->active_window_start + 1,
               std::min(out->synth.active.active_window_end_samples,
                        out->synth.max_line_samples));
  out->active_window_samples =
      out->active_window_end - out->active_window_start;
  out->blanking_fixed = MillivoltsToSampleFixed(out->levels.blanking_mv);
  out->luma_code_table = BuildLumaCodeTable(out->levels);

  // SMPTE 170M-2004 Table 1: NTSC burst amplitude = 40 IRE p-p = 20 IRE peak.
  // ITU-R BT.1700 Part B Table 2 item 5: PAL burst amplitude = 300 mV p-p.
  // ITU-R BT.470-6 Table 2 item 2.15: M/PAL burst = 3/7 × (white−blanking).
  //   PAL: 3/7 × 700 mV = 300 mV p-p → 150.0 mV peak.
  //   M/PAL: 3/7 × 714.3 mV = 306.1 mV p-p → 153.05 mV peak.
  out->burst_amplitude_mv = 150.0;
  if (standard == Standard::kNtsc) {
    out->burst_amplitude_mv = 20.0 * 1000.0 / 140.0;
  } else if (standard == Standard::kPalM) {
    out->burst_amplitude_mv = (3.0 / 7.0) * 714.3 / 2.0;
  }

  // Sync pulses are pure functions of (standard, pulse kind, signal levels), so
  // each shape is rendered once here and copied into place per line instead of
  // being evaluated sample by sample (~52 k S-curve evaluations per frame).
  for (int kind_index = 0; kind_index < generation_detail::kSyncPulseKindCount;
       ++kind_index) {
    const auto kind = static_cast<SyncPulseKind>(kind_index);
    const int width =
        PulseWidthSamples(kind, standard, out->timing.sample_rate_4fsc_hz);
    generation_detail::SyncPulseWaveform& waveform =
        out->pulse_waveforms[static_cast<std::size_t>(kind_index)];
    waveform.width_samples = width;
    waveform.levels.assign(static_cast<std::size_t>(width), 0);
    for (int index = 0; index < width; ++index) {
      waveform.levels[static_cast<std::size_t>(index)] =
          MillivoltsToSampleFixed(ShapedPulseLevel(
              index, width, out->synth.sync_rise_samples,
              out->levels.blanking_mv, out->levels.sync_tip_mv));
    }
  }

  // The burst gate envelope is likewise frame-invariant; folding the burst
  // amplitude into the table keeps the multiply order — and so the rounding —
  // identical to evaluating the envelope inline.
  out->burst_width_samples = std::max(
      0, out->synth.burst_end_samples - out->synth.burst_start_samples);
  out->burst_envelope_mv.assign(
      static_cast<std::size_t>(out->burst_width_samples), 0.0);
  for (int index = 0; index < out->burst_width_samples; ++index) {
    out->burst_envelope_mv[static_cast<std::size_t>(index)] =
        out->burst_amplitude_mv *
        ShapedGateEnvelope(index, out->burst_width_samples,
                           out->synth.burst_rise_samples);
  }

  out->pal_pilot_burst = project.cvbs_presets.pal_laserdisc_pilot_burst &&
                         standard == Standard::kPal;
  out->pilot_omega =
      out->pal_pilot_burst
          ? (2.0 * kPi * kPilotBurstFreqHz / out->timing.sample_rate_4fsc_hz)
          : 0.0;
  // IEC 60856 §9.1.2 fig 6: q = 13.5 periods for line/field sync pulses,
  // r = 6 periods for equalizing pulses. Broad field-sync pulses use q here
  // (not the optional 100-period extension); the rest stays at sync tip.
  out->pilot_q_samples =
      out->pal_pilot_burst
          ? static_cast<int>(std::lround(
                13.5 * out->timing.sample_rate_4fsc_hz / kPilotBurstFreqHz))
          : 0;
  out->pilot_r_samples =
      out->pal_pilot_burst
          ? static_cast<int>(std::lround(6.0 * out->timing.sample_rate_4fsc_hz /
                                         kPilotBurstFreqHz))
          : 0;
  out->pilot_burst_lines =
      out->pal_pilot_burst
          ? BuildPilotBurstLines(out->synth, out->pulse_waveforms,
                                 out->pilot_omega, out->pilot_q_samples,
                                 out->pilot_r_samples)
          : std::vector<generation_detail::PilotBurstLine>{};

  std::string vits_error;
  if (!BuildRenderedVitsLines(project.line_injections.vits, standard,
                              out->timing.sample_rate_4fsc_hz,
                              out->synth.line_sample_counts,
                              vits_definition_provider, vits_generator,
                              &out->vits_lines, &vits_error)) {
    errors->push_back(vits_error.empty()
                          ? "Unable to prepare section VITS state."
                          : vits_error);
    return false;
  }

  out->vbi_renderer = std::make_unique<VbiWaveformRenderer>(
      standard, out->timing.sample_rate_4fsc_hz);

  // ITU-R BT.1700 Annex 1 Part C: SECAM replaces the quadrature encoder and
  // the colour burst with line-sequential FM chroma. The encoder itself is
  // only published below, once the remaining build steps have succeeded.
  std::unique_ptr<ISecamChromaEncoder> secam_encoder;
  out->secam_field_identification = false;
  out->secam_line_chroma.clear();
  std::unique_ptr<IChromaEncoder> chroma_encoder;
  if (standard == Standard::kSecam) {
    secam_encoder =
        std::make_unique<SecamChromaEncoder>(out->timing.sample_rate_4fsc_hz);
    out->secam_field_identification =
        project.cvbs_presets.secam_field_identification;
    out->secam_line_chroma.reserve(
        static_cast<std::size_t>(out->synth.max_line_samples));
    // OSD text lies inside the active window, which ends well before the
    // front porch, so a nominal-length line serves the two longer lines too.
    const int nominal_line_samples = out->timing.samples_per_line_4fsc;
    for (int component = 0; component < 2; ++component) {
      for (int inverted = 0; inverted < 2; ++inverted) {
        secam_encoder->EncodeLine(
            component == 0 ? SecamLineComponent::kDr : SecamLineComponent::kDb,
            inverted == 0 ? 0.0 : kPi, {}, 0, nominal_line_samples,
            &out->secam_neutral_chroma[static_cast<std::size_t>(component)]
                                      [static_cast<std::size_t>(inverted)]);
      }
    }
    for (int parity = 0; parity < 2; ++parity) {
      std::vector<const SampleFixed*>& lines =
          out->secam_neutral_chroma_lines[static_cast<std::size_t>(parity)];
      lines.assign(static_cast<std::size_t>(out->timing.lines_per_frame),
                   nullptr);
      for (int line = 1; line <= out->timing.lines_per_frame; ++line) {
        const int component =
            SecamComponentForLine(line, parity) == SecamLineComponent::kDr ? 0
                                                                           : 1;
        const int inverted =
            SecamInitialPhaseRadForLine(line, parity) > 0.0 ? 1 : 0;
        lines[static_cast<std::size_t>(line - 1)] =
            out->secam_neutral_chroma[static_cast<std::size_t>(component)]
                                     [static_cast<std::size_t>(inverted)]
                                         .data();
      }
    }
  } else {
    chroma_encoder =
        CreateChromaEncoder(standard, out->timing.sample_rate_4fsc_hz);
    if (chroma_encoder == nullptr) {
      errors->push_back("Unsupported video standard for chroma encoding.");
      return false;
    }
  }

  out->line_source_samples.assign(
      static_cast<std::size_t>(out->active_window_samples), YCbCr444Pixel{});
  out->active_sample_indices.assign(
      static_cast<std::size_t>(out->active_window_samples), 0);
  out->encoded_line_chroma.assign(
      static_cast<std::size_t>(out->active_window_samples),
      MillivoltsToSampleFixed(0.0));

  out->presets = project.cvbs_presets;
  out->vits = project.line_injections.vits;
  out->chroma_encoder = std::move(chroma_encoder);
  out->secam_encoder = std::move(secam_encoder);
  return true;
}

// One cached clean frame: everything except the per-frame VBI and OSD
// patches. Immutable once published by the template cache.
struct GenerationStage::FrameTemplate {
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
};

// Bounded cache of clean frame templates, shared by all worker threads.
//
// Key: (source path, source frame identity, colour sequence phase, section
// type). The clean frame depends on the section only through its decoded
// source and its progressive/interlaced row pairing, so sections sharing a
// source share templates. Everything else the clean frame depends on — the
// CVBS presets (standard, levels, pilot burst) and the project VITS set — is
// held as the cache configuration: a lookup against a different configuration
// clears the cache first.
//
// Admission: a template is built and stored only on a key's second request
// (misses record the key in a ghost set and synthesise directly), so sources
// whose keys never repeat — a clip played once — bypass the cache instead of
// filling it with templates nothing will ever read.
//
// Sizing: entries are never evicted. When admitting a new template would
// exceed the byte capacity the lookup returns nullptr and the caller
// synthesises directly, so sources with more distinct frames than the cache
// can hold degrade to the uncached path instead of thrashing.
//
// Thread-safety: Acquire may be called concurrently. The lookup table is
// mutex-guarded; each entry is built exactly once under a per-key
// std::once_flag (single-flight), so concurrent requests for the same key
// serialise on that one build while requests for other keys proceed. A
// published template is immutable and handed out as shared_ptr-to-const.
class GenerationStage::TemplateCache {
 public:
  explicit TemplateCache(std::size_t capacity_bytes)
      : capacity_bytes_(capacity_bytes) {}

  void SetCapacityBytes(std::size_t capacity_bytes) {
    const std::lock_guard<std::mutex> lock(mutex_);
    capacity_bytes_ = capacity_bytes;
    slots_.clear();
    requested_once_.clear();
    reserved_bytes_ = 0;
  }

  void Clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
    requested_once_.clear();
    reserved_bytes_ = 0;
  }

  // Returns the template for the key, building it via synthesise (which must
  // fill the template completely and cannot fail) when absent. Returns
  // nullptr when the cache is disabled, when the key has not been requested
  // before (admission on second request), or when admitting the template
  // would exceed the capacity; the caller then synthesises directly into its
  // own buffers.
  std::shared_ptr<const FrameTemplate> Acquire(
      const Project& project, const Section& section, int source_frame_index,
      std::size_t sequence_phase, std::size_t template_bytes,
      const std::function<void(FrameTemplate*)>& synthesise) {
    std::shared_ptr<Slot> slot;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (capacity_bytes_ == 0U) {
        return nullptr;
      }

      if (!configuration_valid_ || !(presets_ == project.cvbs_presets) ||
          !(vits_ == project.line_injections.vits)) {
        slots_.clear();
        requested_once_.clear();
        reserved_bytes_ = 0;
        presets_ = project.cvbs_presets;
        vits_ = project.line_injections.vits;
        configuration_valid_ = true;
      }

      SlotKey key{section.source, section.type, source_frame_index,
                  sequence_phase};
      auto it = slots_.find(key);
      if (it == slots_.end()) {
        // Admission on second request: a template is only worth building and
        // storing when its key recurs. A clip source played once produces
        // every key exactly once, so it bypasses the cache entirely instead
        // of filling it with templates nothing will ever read; still frames
        // and duration_repeat passes revisit their keys and are admitted from
        // the second request on.
        if (requested_once_.insert(key).second) {
          return nullptr;
        }
        if (reserved_bytes_ + template_bytes > capacity_bytes_) {
          return nullptr;
        }
        // The ghost set only needs keys that have not been admitted yet; a
        // defensive bound keeps it from growing without limit on very long
        // single-pass sources (clearing it merely delays later admissions).
        if (requested_once_.size() > kMaxGhostKeys) {
          requested_once_.clear();
        } else {
          requested_once_.erase(key);
        }
        it = slots_.emplace(std::move(key), std::make_shared<Slot>()).first;
        reserved_bytes_ += template_bytes;
      }
      slot = it->second;
    }

    // Single-flight: the first requester synthesises, concurrent requesters
    // for the same key block here until the template is published, and
    // call_once's synchronisation makes the write to slot->ready visible.
    std::call_once(slot->once, [&]() {
      auto built = std::make_shared<FrameTemplate>();
      synthesise(built.get());
      slot->ready = std::move(built);
    });
    return slot->ready;
  }

 private:
  struct SlotKey {
    std::string source;
    std::string section_type;
    int source_frame_index = 0;
    std::size_t sequence_phase = 0;

    bool operator==(const SlotKey& other) const {
      return source_frame_index == other.source_frame_index &&
             sequence_phase == other.sequence_phase && source == other.source &&
             section_type == other.section_type;
    }
  };

  struct SlotKeyHash {
    std::size_t operator()(const SlotKey& key) const {
      std::size_t hash = std::hash<std::string>{}(key.source);
      hash ^= std::hash<std::string>{}(key.section_type) +
              0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
      hash ^= std::hash<int>{}(key.source_frame_index) + 0x9e3779b97f4a7c15ULL +
              (hash << 6) + (hash >> 2);
      hash ^= std::hash<std::size_t>{}(key.sequence_phase) +
              0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
      return hash;
    }
  };

  struct Slot {
    std::once_flag once;
    std::shared_ptr<const FrameTemplate> ready;
  };

  // Bound on the second-request admission ghost set (~100 bytes per key).
  static constexpr std::size_t kMaxGhostKeys = 65536;

  std::mutex mutex_;
  std::size_t capacity_bytes_;
  std::size_t reserved_bytes_ = 0;
  // Configuration the cached templates were built under; a mismatch clears.
  bool configuration_valid_ = false;
  CvbsPresets presets_;
  std::vector<VitsInjection> vits_;
  std::unordered_map<SlotKey, std::shared_ptr<Slot>, SlotKeyHash> slots_;
  // Keys requested exactly once and not (yet) admitted.
  std::unordered_set<SlotKey, SlotKeyHash> requested_once_;
};

GenerationStage::GenerationStage(
    ILogger* logger, const IVitsDefinitionProvider* vits_definition_provider,
    const IVitsGenerator* vits_generator)
    : logger_(logger),
      resource_cache_(std::make_unique<SynthesisResourceCache>()),
      template_cache_(
          std::make_unique<TemplateCache>(kDefaultTemplateCacheCapacityBytes)),
      vits_definition_provider_(vits_definition_provider != nullptr
                                    ? vits_definition_provider
                                    : &default_vits_definition_provider_),
      vits_generator_(vits_generator != nullptr ? vits_generator
                                                : &default_vits_generator_) {}

GenerationStage::~GenerationStage() = default;

void GenerationStage::SetTemplateCacheCapacityBytes(
    std::size_t capacity_bytes) {
  template_cache_->SetCapacityBytes(capacity_bytes);
}

bool GenerationStage::BuildFrameSchedule(
    const Project& project, std::vector<FrameScheduleItem>* out_schedule,
    std::vector<std::string>* errors) {
  if (out_schedule == nullptr || errors == nullptr) {
    return false;
  }

  out_schedule->clear();
  progressive_source_.ClearCache();
  template_cache_->Clear();
  biphase_manager_.Reset();
  biphase_manager_.SetProjectDiscType(
      DiscTypeFromString(project.line_injections.disc_type));

  // Compute disc_frame_offset = (first_PN - 1) from the first CAV
  // picture_number code. This offset is added to global_frame_index when
  // computing the colour-subcarrier phase so that sources not starting at PN 1
  // still produce disc-accurate colour sequences ((PN-1) % colour_period).
  std::size_t initial_frame_offset = 0;
  for (const Section& sec : project.sections) {
    bool found = false;
    for (const Section::LineInjection& inj : sec.line_injections) {
      if (Lowercase(inj.type) != "laserdisc") {
        continue;
      }
      for (const Section::LineInjectionCode& code : inj.codes) {
        if (code.code_type == "picture_number" && code.start_value_specified &&
            code.start_value > 1) {
          initial_frame_offset = static_cast<std::size_t>(code.start_value - 1);
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (found) break;
  }
  biphase_manager_.SetInitialFrameCount(static_cast<int>(initial_frame_offset));

  std::vector<std::pair<const Section*, int>> frame_sections;
  std::string schedule_error;
  if (!BuildFramePatternSchedule(project, progressive_source_,
                                 project.cvbs_presets.video_standard_preset,
                                 &frame_sections, &schedule_error)) {
    errors->push_back(schedule_error.empty()
                          ? "Unable to build section frame schedule."
                          : schedule_error);
    return false;
  }

  // Build the output schedule, computing the per-frame disc picture number so
  // that colour-subcarrier phase is derived from disc position (PN - 1) rather
  // than file position even when sections have non-contiguous start_values
  // (backward-skip replay or post-gap forward-skip sections).
  out_schedule->reserve(frame_sections.size());
  const Section* prev_schedule_section = nullptr;
  int section_pn_start = 0;
  int section_frame_offset = 0;
  for (const std::pair<const Section*, int>& frame_section : frame_sections) {
    const Section* sec = frame_section.first;
    if (sec != prev_schedule_section) {
      section_pn_start = FindSectionPictureNumberStart(*sec);
      section_frame_offset = 0;
      prev_schedule_section = sec;
    }
    const int disc_pn =
        (section_pn_start > 0) ? (section_pn_start + section_frame_offset) : 0;
    out_schedule->push_back(FrameScheduleItem{
        .section = sec,
        .source_frame_index = frame_section.second,
        .disc_picture_number = disc_pn,
    });
    ++section_frame_offset;
  }

  // Record what follows each section so synthesis can start the next
  // section's decode in the background while the current one still renders.
  // Sections occupy contiguous runs of the schedule, so one entry per section
  // describes the whole sequence.
  next_section_start_index_.clear();
  const Section* previous_run_section = nullptr;
  for (std::size_t index = 0; index < out_schedule->size(); ++index) {
    const Section* item_section = (*out_schedule)[index].section;
    if (item_section == previous_run_section) {
      continue;
    }
    if (previous_run_section != nullptr) {
      next_section_start_index_[previous_run_section] = index;
    }
    previous_run_section = item_section;
  }

  // Sequential enrichment pass: resolve the per-frame VBI payload, colour
  // context, and OSD token strings once, in schedule order, by advancing the
  // biphase generators over the whole schedule. With the payload attached,
  // GenerateFrameBatch needs no mutable cross-frame state and frames can be
  // synthesised out of order.
  const Standard standard = project.cvbs_presets.video_standard_preset;
  const SampledSynthesisContext synth = BuildSampledSynthesisContext(standard);
  const int active_window_start =
      std::max(0, std::min(synth.active.active_window_start_samples,
                           synth.max_line_samples - 1));
  // The {timecode} OSD token is a continuous CLV programme timecode; it is only
  // meaningful on CLV discs, where it runs from the start of the output.
  const bool disc_is_clv =
      DiscTypeFromString(project.line_injections.disc_type) == DiscType::kCLV;
  for (std::size_t frame_index = 0; frame_index < out_schedule->size();
       ++frame_index) {
    FrameScheduleItem& item = (*out_schedule)[frame_index];
    auto enrichment = std::make_shared<FrameEnrichment>();

    // disc_frame_index represents (PN - 1) for the frame, anchoring
    // colour-subcarrier and pilot-burst phase to the disc picture number.
    enrichment->disc_frame_index =
        (item.disc_picture_number > 0)
            ? static_cast<std::size_t>(item.disc_picture_number - 1)
            : (frame_index + initial_frame_offset);

    if (!biphase_manager_.ResolveFrame(
            *item.section, standard, synth.sample_rate_hz, active_window_start,
            enrichment.get(), errors)) {
      return false;
    }

    // The {frame_number} OSD token is the 1-based position of this frame in the
    // whole generated output; it is independent of biphase codes and cannot be
    // re-anchored by a section, so it is derived from the schedule position
    // here rather than in the biphase manager.
    enrichment->context.frame_number = static_cast<int>(frame_index) + 1;

    // The {timecode} OSD token is a continuous CLV programme timecode driven by
    // the 0-based output frame position, so it advances on every frame of a CLV
    // disc regardless of which VBI codes (if any) a section injects. It renders
    // a placeholder on non-CLV discs where has_clv_timecode stays false.
    if (disc_is_clv) {
      const ClvTimecode tc = ClvTimecodeForFrame(frame_index, standard);
      enrichment->context.has_clv_timecode = true;
      enrichment->context.clv_hours = tc.hours;
      enrichment->context.clv_minutes = tc.minutes;
      enrichment->context.clv_seconds = tc.seconds;
      enrichment->context.clv_frames = tc.frames;
    }

    enrichment->osd_texts.reserve(item.section->osd.overlays.size());
    for (const OsdOverlay& overlay : item.section->osd.overlays) {
      enrichment->osd_texts.push_back(osd_token_resolver_.Resolve(
          overlay.text, enrichment->context, item.section->name));
    }

    item.enrichment = std::move(enrichment);
  }

  if (logger_ != nullptr) {
    logger_->Debug("Built frame schedule for " +
                   std::to_string(out_schedule->size()) + " frame(s).");
  }

  return true;
}

// Synthesises one clean frame — sync, pilot burst, colour burst, active
// picture, VITS — into y_out/c_out. Everything that genuinely varies per
// frame beyond the colour sequence phase (VBI code words, OSD text, noise,
// dropouts) is applied afterwards as a localised patch, so for constant
// source content the result is an exact function of
// (source, source frame, sequence_phase).
void GenerationStage::SynthesiseTemplate(SynthesisResources& resources,
                                         const Section& section,
                                         const FrameSourceImage& source_frame,
                                         std::size_t sequence_phase,
                                         SampleFixed* y_out,
                                         SampleFixed* c_out) {
  const SignalLevels& levels = resources.levels;
  const generation_detail::SampledSynthesisContext& synth = resources.synth;
  const Standard video_standard = synth.standard;
  const double burst_amplitude_mv = resources.burst_amplitude_mv;
  const IChromaEncoder* chroma_encoder = resources.chroma_encoder.get();
  const ISecamChromaEncoder* secam_encoder = resources.secam_encoder.get();
  std::vector<SampleFixed>& secam_line_chroma = resources.secam_line_chroma;
  // ITU-R BT.1700 Annex 1 Part C Figure 16 / Table 4 Note 1: the SECAM D'R/D'B
  // line sequence and the subcarrier phase inversion both repeat every two
  // frames, which divides the 4-frame colour sequence period.
  const int secam_frame_parity = static_cast<int>(sequence_phase % 2U);
  const int active_window_start = resources.active_window_start;
  const int active_window_samples = resources.active_window_samples;
  const bool pal_pilot_burst = resources.pal_pilot_burst;
  const SampleFixed blanking_fixed = resources.blanking_fixed;
  const SampleFixed* const luma_code_table = resources.luma_code_table.data();

  std::fill_n(y_out, static_cast<std::size_t>(synth.frame_samples),
              blanking_fixed);
  std::fill_n(c_out, static_cast<std::size_t>(synth.frame_samples),
              SampleFixed{0});

  // Active luma comes from the precomputed code table. Sources clamp their
  // samples into the 10-bit code space, so the guard only covers hand-built
  // images and costs a predictable compare rather than a divide and an
  // llround.
  auto LumaFixedFromCode = [&](int y_code) -> SampleFixed {
    if (y_code >= 0 && y_code < kLumaCodeTableSize) {
      return luma_code_table[y_code];
    }
    return MillivoltsToSampleFixed(LumaMillivoltsFromCode(y_code, levels));
  };

  // Per-line workspaces and the active-sample to source-column mapping are
  // worker-private members of the resources; the mapping is rebuilt only when
  // a section change brings in a source with a different active raster.
  std::vector<YCbCr444Pixel>& line_source_samples =
      resources.line_source_samples;
  std::vector<int>& active_sample_indices = resources.active_sample_indices;
  std::vector<SampleFixed>& encoded_line_chroma = resources.encoded_line_chroma;
  ActiveSampleColumnMap& column_map = resources.column_map;
  if (!column_map.MatchesSource(source_frame)) {
    column_map.Rebuild(source_frame, active_window_samples);
  }

  // Progressive imports use field-2-dominant row pairing; the section type
  // decides it once per frame rather than once per active sample.
  const bool progressive_section = section.type == "progressive";

  // The clean frame repeats exactly every colour sequence period, so the
  // absolute sample base derives from the reduced phase: the subcarrier
  // lattice (mod 4) and the PAL burst meander parity (mod 2) see exactly the
  // residues the full disc frame index would produce.
  const std::size_t absolute_frame_base =
      sequence_phase * static_cast<std::size_t>(synth.frame_samples);

  for (const LineTimingPrimitive& line : synth.frame_lines) {
    const int line_index = line.line_number_1based - 1;
    const int line_base =
        synth.line_sample_offsets[static_cast<std::size_t>(line_index)];
    const std::size_t absolute_line_base =
        absolute_frame_base +
        static_cast<std::size_t>(
            synth.line_sample_offsets[static_cast<std::size_t>(line_index)]);
    const int line_samples =
        synth.line_sample_counts[static_cast<std::size_t>(line_index)];
    const int line_end = line_base + line_samples;

    const generation_detail::LinePulsePlan& pulse_plan =
        synth.line_pulse_plans[static_cast<std::size_t>(line_index)];

    for (int segment_index = 0; segment_index < pulse_plan.segment_count;
         ++segment_index) {
      const LinePulseSegment& segment =
          pulse_plan.segments[static_cast<std::size_t>(segment_index)];
      const generation_detail::SyncPulseWaveform& pulse_waveform =
          resources.pulse_waveforms[static_cast<std::size_t>(segment.kind)];
      const int pulse_offset = segment.offset_samples;
      const int pulse_start =
          line_base + std::min(pulse_offset, line_samples - 1);
      const int pulse_end =
          std::min(pulse_start + pulse_waveform.width_samples, line_end);
      const int pulse_width_samples = pulse_end - pulse_start;

      if (pulse_width_samples == pulse_waveform.width_samples) {
        std::copy(pulse_waveform.levels.begin(), pulse_waveform.levels.end(),
                  y_out + pulse_start);
      } else {
        // A pulse clipped by the end of its line has a different S-curve to
        // the nominal shape, so it is still shaped sample by sample.
        for (int i = pulse_start; i < pulse_end; ++i) {
          const int relative_index = i - pulse_start;
          y_out[i] = MillivoltsToSampleFixed(ShapedPulseLevel(
              relative_index, pulse_width_samples, synth.sync_rise_samples,
              levels.blanking_mv, levels.sync_tip_mv));
        }
      }
    }

    if (pal_pilot_burst) {
      // The pilot burst repeats on every frame, so each line's bursts are
      // added straight from the table rendered once per run.
      const generation_detail::PilotBurstLine& pilot_line =
          resources.pilot_burst_lines[static_cast<std::size_t>(line_index)];
      for (int segment_index = 0; segment_index < pilot_line.segment_count;
           ++segment_index) {
        const generation_detail::PilotBurstSegment& pilot_segment =
            pilot_line.segments[static_cast<std::size_t>(segment_index)];
        SampleFixed* const destination =
            y_out +
            static_cast<std::size_t>(line_base + pilot_segment.offset_in_line);
        for (std::size_t i = 0; i < pilot_segment.samples.size(); ++i) {
          destination[i] += pilot_segment.samples[i];
        }
      }
    }

    const bool is_pal = video_standard == Standard::kPal;
    const bool is_pal_m = video_standard == Standard::kPalM;
    bool burst_enabled = line.burst_enabled;
    double burst_phase_rad = line.burst_phase_rad;
    if (is_pal) {
      burst_enabled = PalBurstEnabledForLine(sequence_phase, line);
      burst_phase_rad = PalBurstPhaseRadForLine(sequence_phase, line);
    } else if (is_pal_m) {
      // ITU-R BT.470-6 Table 2 item 2.17: PAL-M burst blanking uses
      // System M line numbers (IsPalMBurstBlankedLine). Burst phase
      // follows the same ±135° alternating pattern as 625-line PAL.
      burst_enabled = PalMBurstEnabledForLine(sequence_phase, line);
      burst_phase_rad = PalBurstPhaseRadForLine(sequence_phase, line);
    }

    if (burst_enabled) {
      const int burst_sample_start =
          std::min(line_base + synth.burst_start_samples,
                   line_end > 0 ? line_end - 1 : line_base);
      const int burst_sample_end =
          std::min(line_base + synth.burst_end_samples, line_end);
      const int burst_width_samples = burst_sample_end - burst_sample_start;
      const std::size_t burst_sample_start_absolute =
          absolute_line_base +
          static_cast<std::size_t>(burst_sample_start - line_base);

      // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: PAL/PAL-M burst at
      // ±135° from EU axis, alternating per line per PalBurstPhaseRadForLine.
      // Item 10h: V-switching direction is carried in the EV' component of
      // the burst; this requires burst_phase_rad to remain ±135° so decoders
      // can extract the sign. NTSC burst_phase_rad is kept at π/4 (45°) per
      // the constant-reference convention in signal_timing_model.h.
      const double subcarrier_anchor_rad =
          is_pal ? kPalSubcarrierAnchorRad : 0.0;
      const double burst_start_phase_rad =
          WrapPhaseRad(SubcarrierPhaseRad(burst_sample_start_absolute) +
                       subcarrier_anchor_rad + burst_phase_rad);
      double burst_sin = std::sin(burst_start_phase_rad);
      double burst_cos = std::cos(burst_start_phase_rad);

      // The gate envelope is frame-invariant, so the pre-scaled table is read
      // directly whenever the burst has its nominal width; a burst clipped by
      // the end of its line keeps the sample-by-sample shaping.
      const bool burst_is_nominal_width =
          burst_width_samples == resources.burst_width_samples;

      for (int i = burst_sample_start; i < burst_sample_end; ++i) {
        const int relative_index = i - burst_sample_start;
        const double envelope_mv =
            burst_is_nominal_width
                ? resources.burst_envelope_mv[static_cast<std::size_t>(
                      relative_index)]
                : (burst_amplitude_mv *
                   ShapedGateEnvelope(relative_index, burst_width_samples,
                                      synth.burst_rise_samples));
        c_out[i] = MillivoltsToSampleFixed(envelope_mv * burst_sin);

        const double next_sin = burst_cos;
        const double next_cos = -burst_sin;
        burst_sin = next_sin;
        burst_cos = next_cos;
      }
    }

    if (line.sync_pulse_kind == SyncPulseKind::kHorizontal &&
        line.content_kind == LineContentKind::kActivePicture) {
      const int active_y = ActiveFrameLineIndex(synth.active, video_standard,
                                                line.line_number_1based);
      if (active_y >= 0) {
        const bool invert_pal_v_axis =
            (is_pal || is_pal_m) && PalInvertVAxisForLine(sequence_phase, line);
        const std::size_t active_window_line_start_absolute =
            absolute_line_base + static_cast<std::size_t>(active_window_start);
        // SMPTE 170M-2004 Section 10: defines active chroma with burst+180
        // deg reference for NTSC. PAL and PAL-M use 0 phase offset; PAL
        // additionally carries the subcarrier anchor so picture chroma stays
        // phase-coherent with the anchored burst.
        const double phase_offset =
            (video_standard == Standard::kNtsc)
                ? (line.burst_phase_rad + kPi)
                : (is_pal ? kPalSubcarrierAnchorRad : 0.0);
        const double phase_start =
            WrapPhaseRad(SubcarrierPhaseRad(active_window_line_start_absolute) +
                         phase_offset);

        // Map field lines onto progressive source rows by interleaving
        // fields. Progressive imports use field-2-dominant row pairing, so
        // field 1 consumes odd rows and field 2 consumes even rows. The row
        // is the same for every sample of the line.
        const int field_line = active_y % synth.active.active_lines_per_field;
        const int source_row =
            source_frame.active_y +
            ((line.field_index_1based == 1)
                 ? (2 * field_line + (progressive_section ? 1 : 0))
                 : (2 * field_line + (progressive_section ? 0 : 1)));

        // Samples that used to be skipped one at a time form a suffix of the
        // active window: the sample index leaves the line, or the mapped
        // source column leaves the raster, and neither condition can revert
        // as x_sample grows. Taking the smallest such bound as the loop limit
        // removes both per-sample checks while leaving the skipped tail with
        // exactly the placeholder values it had before.
        const int line_sample_limit = line_samples - active_window_start;
        const int painted_sample_count =
            (source_row < source_frame.height)
                ? std::max(0,
                           std::min({active_window_samples, line_sample_limit,
                                     column_map.in_range_sample_count}))
                : 0;

        const int active_line_sample_base = line_base + active_window_start;
        const YCbCr444Pixel* const source_row_pixels =
            (painted_sample_count > 0) ? &source_frame.PixelAt(0, source_row)
                                       : nullptr;

        for (int x_sample = 0; x_sample < painted_sample_count; ++x_sample) {
          const int sample_index = active_line_sample_base + x_sample;
          const std::size_t sample_slot = static_cast<std::size_t>(x_sample);
          const YCbCr444Pixel& pixel =
              source_row_pixels[column_map.pixel_x[sample_slot]];

          active_sample_indices[sample_slot] = sample_index;
          line_source_samples[sample_slot] = pixel;

          if (invert_pal_v_axis) {
            // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: PAL V-axis
            // switching follows the burst-sequence-dependent odd/even
            // polarity map.
            line_source_samples[sample_slot].cr =
                static_cast<std::int16_t>(InvertCenteredChromaCode(pixel.cr));
          }

          // Preserve any sync-domain sample already placed for this line;
          // only paint active luma where the waveform is at/above blanking
          // level.
          SampleFixed& luma_sample =
              y_out[static_cast<std::size_t>(sample_index)];
          if (luma_sample >= blanking_fixed) {
            luma_sample = LumaFixedFromCode(pixel.y);
          }
        }

        // Restore the placeholders for the tail the loop above did not paint,
        // so the chroma encoder sees neutral pixels there and the accumulate
        // step folds their output onto the line's first sample, exactly as
        // the per-sample skip used to.
        std::fill(line_source_samples.begin() + painted_sample_count,
                  line_source_samples.end(), YCbCr444Pixel{});
        std::fill(active_sample_indices.begin() + painted_sample_count,
                  active_sample_indices.end(), line_base);

        if (secam_encoder != nullptr) {
          // The SECAM encoder emits the whole line from 0H: subcarrier
          // blanking, the undeviated back-porch reference and the
          // frequency-modulated picture chroma. Neutral placeholder pixels in
          // the unpainted tail encode as the undeviated subcarrier.
          secam_encoder->EncodeLine(
              SecamComponentForLine(line.line_number_1based,
                                    secam_frame_parity),
              SecamInitialPhaseRadForLine(line.line_number_1based,
                                          secam_frame_parity),
              line_source_samples, active_window_start, line_samples,
              &secam_line_chroma);
          for (int i = 0; i < line_samples; ++i) {
            c_out[static_cast<std::size_t>(line_base + i)] +=
                secam_line_chroma[static_cast<std::size_t>(i)];
          }
        } else {
          chroma_encoder->EncodeLineFromPhaseStart(
              line_source_samples, phase_start, &encoded_line_chroma);
          for (int x_sample = 0; x_sample < active_window_samples; ++x_sample) {
            c_out[static_cast<std::size_t>(
                active_sample_indices[static_cast<std::size_t>(x_sample)])] +=
                encoded_line_chroma[static_cast<std::size_t>(x_sample)];
          }
        }
      }
    }

    if (secam_encoder != nullptr && resources.secam_field_identification &&
        IsSecamFieldIdentificationLine(line.line_number_1based)) {
      // ITU-R BT.1700 Annex 1 Part C Table 4 item 13 / Figure 16: field
      // identification trapezoids on lines 7-15 and 320-328.
      secam_encoder->EncodeIdentificationLine(
          SecamComponentForLine(line.line_number_1based, secam_frame_parity),
          SecamInitialPhaseRadForLine(line.line_number_1based,
                                      secam_frame_parity),
          line_samples, &secam_line_chroma);
      for (int i = 0; i < line_samples; ++i) {
        c_out[static_cast<std::size_t>(line_base + i)] +=
            secam_line_chroma[static_cast<std::size_t>(i)];
      }
    }

    const auto vits_line = resources.vits_lines.find(line.line_number_1based);
    if (vits_line != resources.vits_lines.end()) {
      const VitsRenderedLine& rendered_line = *vits_line->second;
      for (int sample_offset = 0; sample_offset < line_samples;
           ++sample_offset) {
        const std::size_t frame_sample_index =
            static_cast<std::size_t>(line_base) +
            static_cast<std::size_t>(sample_offset);
        y_out[frame_sample_index] +=
            rendered_line.y_samples_mv[static_cast<std::size_t>(sample_offset)];
        c_out[frame_sample_index] +=
            rendered_line.c_samples_mv[static_cast<std::size_t>(sample_offset)];
      }
    }
  }
}

bool GenerationStage::GenerateFrameBatch(
    const Project& project, const std::vector<FrameScheduleItem>& schedule,
    std::size_t start_frame, std::size_t frame_count,
    std::vector<SampleFixed>* out_y_mv, std::vector<SampleFixed>* out_c_mv,
    std::vector<std::string>* errors) {
  if (out_y_mv == nullptr || out_c_mv == nullptr || errors == nullptr) {
    return false;
  }

  if (start_frame > schedule.size() ||
      (start_frame + frame_count) > schedule.size()) {
    errors->push_back("Requested frame batch is out of schedule bounds.");
    return false;
  }

  if (frame_count == 0U) {
    out_y_mv->clear();
    out_c_mv->clear();
    return true;
  }

  // Frame-invariant setup (sampled timing context, chroma encoder, rendered
  // VITS lines, VBI encoders) is built once per worker thread and reused for
  // every frame that worker synthesises.
  // Non-const: the resources are this worker's alone, and the source-column
  // map is updated in place as sections change.
  SynthesisResources* resources = resource_cache_->Acquire(
      project, *vits_definition_provider_, *vits_generator_, errors);
  if (resources == nullptr) {
    return false;
  }

  const generation_detail::SampledSynthesisContext& synth = resources->synth;
  const int frame_samples = synth.frame_samples;
  const Standard video_standard = project.cvbs_presets.video_standard_preset;

  const std::size_t frame_span = static_cast<std::size_t>(frame_samples);
  const std::size_t sample_count = frame_count * frame_span;

  const int active_window_start = resources->active_window_start;
  const int active_window_end = resources->active_window_end;

  // Every sample of every frame is written below — by a template copy or by
  // direct synthesis, both of which fill the whole frame — so the buffers are
  // sized without a value-fill pass. Single-frame requests (the pool and
  // skip paths) defer sizing to the per-frame step: a template hit there
  // copy-assigns the buffers, which avoids zero-filling freshly allocated
  // memory only to overwrite it.
  const bool single_frame_request = frame_count == 1U;
  if (!single_frame_request) {
    out_y_mv->resize(sample_count);
    out_c_mv->resize(sample_count);
  }

  const std::size_t sequence_period =
      ColourSequencePeriodFrames(video_standard);
  const std::size_t template_bytes = frame_span * sizeof(SampleFixed) * 2U;

  for (std::size_t local_frame_index = 0; local_frame_index < frame_count;
       ++local_frame_index) {
    const std::size_t global_frame_index = start_frame + local_frame_index;
    const FrameScheduleItem& frame_item = schedule[global_frame_index];
    const Section* section = frame_item.section;
    const int source_frame_index = frame_item.source_frame_index;
    if (section == nullptr) {
      errors->push_back(
          "Internal generation error: null section in frame schedule.");
      return false;
    }

    // The enrichment payload carries the resolved VBI code words and OSD
    // token strings. Hand-built schedules without a payload are accepted only
    // for sections that need neither.
    const FrameEnrichment* enrichment = frame_item.enrichment.get();
    if (enrichment == nullptr) {
      bool has_laserdisc_injection = false;
      for (const Section::LineInjection& injection : section->line_injections) {
        if (Lowercase(injection.type) == "laserdisc") {
          has_laserdisc_injection = true;
          break;
        }
      }
      if (has_laserdisc_injection || !section->osd.overlays.empty()) {
        errors->push_back(
            "Frame schedule item is missing its enrichment payload; build "
            "the schedule with BuildFrameSchedule for sections using "
            "laserdisc line injections or OSD overlays.");
        return false;
      }
    }

    // At the first frame of a section run, ask the frame source to decode the
    // next section's source in the background. Section-start decodes (EXR
    // convert, whole-clip MKV decode) otherwise stall every worker at the
    // boundary; the frame source decodes each source under its own lock, so
    // the prefetch overlaps this section's synthesis instead of blocking it.
    if (global_frame_index == 0U ||
        schedule[global_frame_index - 1U].section != section) {
      const auto next = next_section_start_index_.find(section);
      if (next != next_section_start_index_.end() &&
          next->second < schedule.size()) {
        const FrameScheduleItem& next_item = schedule[next->second];
        if (next_item.section != nullptr &&
            next_item.section->source != section->source) {
          progressive_source_.PrefetchSection(
              *next_item.section, next_item.source_frame_index, video_standard);
        }
      }
    }

    if (logger_ != nullptr && schedule.size() <= 120U) {
      logger_->Trace("Generating frame " +
                     std::to_string(global_frame_index + 1U) + " of " +
                     std::to_string(schedule.size()) + " from section '" +
                     section->name + "' (" + section->type + ").");
    }

    // Shared, immutable decoded source image: delivery is a reference count,
    // not a per-frame copy of the ~2.4 MiB raster.
    std::shared_ptr<const FrameSourceImage> source_frame_image;
    std::string frame_error;
    const bool generated = progressive_source_.GenerateFrame(
        *section, source_frame_index,
        project.cvbs_presets.video_standard_preset, &source_frame_image,
        &frame_error);

    if (!generated || source_frame_image == nullptr) {
      errors->push_back(frame_error.empty()
                            ? "Unable to generate frame-based source data."
                            : frame_error);
      return false;
    }
    const FrameSourceImage& source_frame = *source_frame_image;

    const int local_frame_base =
        static_cast<int>(local_frame_index * frame_span);
    // disc_frame_index represents (PN - 1) for the current frame, anchoring
    // colour-subcarrier phase and pilot-burst phase to the disc picture
    // number. Resolved during the BuildFrameSchedule enrichment pass; for
    // hand-built schedules without a payload, fall back to the schedule
    // position (or the picture number when populated).
    const std::size_t disc_frame_index =
        (enrichment != nullptr) ? enrichment->disc_frame_index
                                : ((frame_item.disc_picture_number > 0)
                                       ? static_cast<std::size_t>(
                                             frame_item.disc_picture_number - 1)
                                       : global_frame_index);
    // The clean frame depends on the disc frame index only through this
    // residue (see ColourSequencePeriodFrames), so the reduction is exact.
    const std::size_t sequence_phase = disc_frame_index % sequence_period;

    // A still source delivers the same image for every schedule position, so
    // its schedule indices collapse onto one cached identity per phase.
    const int source_frame_identity =
        SourceIgnoresFrameIndex(section->source) ? 0 : source_frame_index;

    const std::shared_ptr<const FrameTemplate> frame_template =
        template_cache_->Acquire(
            project, *section, source_frame_identity, sequence_phase,
            template_bytes, [&](FrameTemplate* out) {
              out->y_mv.resize(frame_span);
              out->c_mv.resize(frame_span);
              SynthesiseTemplate(*resources, *section, source_frame,
                                 sequence_phase, out->y_mv.data(),
                                 out->c_mv.data());
            });
    if (frame_template != nullptr) {
      // Cached clean frame: delivery is a copy, no line-loop work.
      if (single_frame_request) {
        // Copy-assignment writes each destination sample exactly once,
        // avoiding the zero-fill a resize of a fresh buffer would perform
        // before the copy overwrote it.
        *out_y_mv = frame_template->y_mv;
        *out_c_mv = frame_template->c_mv;
      } else {
        std::copy(frame_template->y_mv.begin(), frame_template->y_mv.end(),
                  out_y_mv->data() + local_frame_base);
        std::copy(frame_template->c_mv.begin(), frame_template->c_mv.end(),
                  out_c_mv->data() + local_frame_base);
      }
    } else {
      // Cache disabled or full: synthesise the clean frame in place.
      if (single_frame_request) {
        out_y_mv->resize(sample_count);
        out_c_mv->resize(sample_count);
      }
      SynthesiseTemplate(*resources, *section, source_frame, sequence_phase,
                         out_y_mv->data() + local_frame_base,
                         out_c_mv->data() + local_frame_base);
    }

    if (enrichment != nullptr) {
      resources->vbi_renderer->Render(
          *enrichment, out_y_mv, local_frame_base, synth.line_sample_offsets,
          synth.line_sample_counts, active_window_end);
    }

    if (!section->osd.overlays.empty()) {
      // Non-null and correctly sized: enforced by the enrichment guard above
      // and the BuildFrameSchedule enrichment pass.
      const std::vector<std::string>& resolved_texts = enrichment->osd_texts;
      const SignalLevels osd_levels =
          GetSignalLevels(project.cvbs_presets.video_standard_preset);
      // SECAM text keeps the undeviated subcarrier rather than blanking it.
      const std::vector<const SampleFixed*>* neutral_chroma_lines =
          resources->secam_encoder != nullptr
              ? &resources->secam_neutral_chroma_lines[sequence_phase % 2U]
              : nullptr;
      const int field1_start = synth.active.first_active_line_field1 - 1;
      const int field1_end = field1_start + synth.active.active_lines_per_field;
      osd_renderer_.Render(section->osd, resolved_texts, out_y_mv, out_c_mv,
                           local_frame_base, synth.line_sample_offsets,
                           field1_start, field1_end, active_window_start,
                           active_window_end, osd_levels, neutral_chroma_lines);
      const int field2_start = synth.active.first_active_line_field2 - 1;
      const int field2_end = field2_start + synth.active.active_lines_per_field;
      osd_renderer_.Render(section->osd, resolved_texts, out_y_mv, out_c_mv,
                           local_frame_base, synth.line_sample_offsets,
                           field2_start, field2_end, active_window_start,
                           active_window_end, osd_levels, neutral_chroma_lines);
    }
  }

  return true;
}

bool GenerationStage::Generate(const Project& project,
                               std::vector<SampleFixed>* out_y_mv,
                               std::vector<SampleFixed>* out_c_mv,
                               std::vector<std::string>* errors) {
  if (out_y_mv == nullptr || out_c_mv == nullptr || errors == nullptr) {
    return false;
  }

  if (logger_ != nullptr) {
    logger_->Debug("Generating full frame buffer for " +
                   std::to_string(project.sections.size()) + " section(s).");
  }

  std::vector<FrameScheduleItem> schedule;
  if (!BuildFrameSchedule(project, &schedule, errors)) {
    return false;
  }

  if (!GenerateFrameBatch(project, schedule, 0, schedule.size(), out_y_mv,
                          out_c_mv, errors)) {
    return false;
  }

  if (logger_ != nullptr) {
    logger_->Info("Generated " + std::to_string(schedule.size()) +
                  " frame(s) of signal data.");
  }

  return true;
}

}  // namespace videosynth
