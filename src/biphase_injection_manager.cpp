/*
 * File:        biphase_injection_manager.cpp
 * Module:      biphase_injection_manager
 * Purpose:     Orchestrates biphase and 40-bit FM code injection into VBI
 *              lines of the video signal for LaserDisc authoring per
 *              IEC 60856 (PAL) and IEC 60857 (NTSC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/biphase_injection_manager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "videosynth/biphase_utils.h"
#include "videosynth/cav_code_generator.h"
#include "videosynth/clv_code_generator.h"
#include "videosynth/signal_shaping.h"

namespace videosynth {

namespace {

// PAL biphase: zero level is at blanking (0 mV) per IEC 60856 Figure 14.
// The "30%–100%" in §10.1 refers to the allowed range of the high level.
constexpr double kPalBiphaseBaselineMv = 0.0;

// NTSC biphase: 0 IRE = 0 mV per IEC 60857.
constexpr double kNtscBiphaseBaselineMv = 0.0;

// 0.160 H horizontal start offset for NTSC white flag per IEC 60857 Figure 12.
constexpr double k160hFraction = 0.160;

// 0.172 H horizontal start offset fraction per IEC 60856 Figure 14 /
// IEC 60857 Figure 11.
constexpr double k172hFraction = 0.172;

// 0.215 H horizontal start offset for NTSC 40-bit FM codes per
// IEC 60857 Figure 13.
constexpr double k215hFraction = 0.215;

// 0.790 H white flag pulse length per IEC 60857 Figure 12.
constexpr double k790hFraction = 0.790;

double BiphaseBaselineMv(Standard standard) {
  return (standard == Standard::kPal) ? kPalBiphaseBaselineMv
                                      : kNtscBiphaseBaselineMv;
}

std::string Lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Writes a 24-bit biphase waveform into the Y buffer for one VBI line.
// Overwrites samples from start_sample up to (not including) active_end with
// the biphase signal (baseline for the post-signal tail, waveform at front).
void WriteBiphaseWaveform(std::vector<SampleFixed>* out_y_mv, int line_base,
                          int active_end, uint32_t code_value,
                          Standard standard, const SignalLevels& levels,
                          int start_sample, const BiphaseEncoder& encoder) {
  const double baseline_mv = BiphaseBaselineMv(standard);
  const double peak_mv = levels.white_mv;

  const auto waveform =
      encoder.Generate24BitCode(code_value, baseline_mv, peak_mv);
  const int waveform_count = static_cast<int>(waveform.size());

  const SampleFixed baseline_fixed = MillivoltsToSampleFixed(baseline_mv);

  // Overwrite the VBI data region with the biphase baseline, then overlay the
  // waveform. Stops at active_end to avoid writing into the front porch.
  for (int i = start_sample; i < active_end; ++i) {
    (*out_y_mv)[static_cast<std::size_t>(line_base) +
                static_cast<std::size_t>(i)] = baseline_fixed;
  }
  for (int i = 0; i < waveform_count && (start_sample + i) < active_end; ++i) {
    (*out_y_mv)[static_cast<std::size_t>(line_base) +
                static_cast<std::size_t>(start_sample) +
                static_cast<std::size_t>(i)] =
        waveform[static_cast<std::size_t>(i)];
  }
}

// Writes a 40-bit FM waveform into the Y buffer for one VBI line (NTSC).
// Writes from start_sample up to (not including) active_end.
void WriteFmWaveform(std::vector<SampleFixed>* out_y_mv, int line_base,
                     int active_end, const FmData& fm_data,
                     const SignalLevels& levels, int start_sample,
                     const FmEncoder& encoder) {
  // 40-bit FM uses 0 IRE baseline regardless of standard (NTSC only).
  const double baseline_mv = kNtscBiphaseBaselineMv;
  const double peak_mv = levels.white_mv;

  const auto waveform =
      encoder.Generate40BitWaveform(fm_data, baseline_mv, peak_mv);
  const int waveform_count = static_cast<int>(waveform.size());

  const SampleFixed baseline_fixed = MillivoltsToSampleFixed(baseline_mv);

  for (int i = start_sample; i < active_end; ++i) {
    (*out_y_mv)[static_cast<std::size_t>(line_base) +
                static_cast<std::size_t>(i)] = baseline_fixed;
  }
  for (int i = 0; i < waveform_count && (start_sample + i) < active_end; ++i) {
    (*out_y_mv)[static_cast<std::size_t>(line_base) +
                static_cast<std::size_t>(start_sample) +
                static_cast<std::size_t>(i)] =
        waveform[static_cast<std::size_t>(i)];
  }
}

// Writes a pre-rendered white flag pulse into a VBI line (NTSC only).
// Fills blanking from start_sample to active_end, then overlays the shaped
// 100 IRE pulse (135 ns rise/fall per IEC 60857 Figure 12).
void WriteWhiteFlagPulse(std::vector<SampleFixed>* out_y_mv, int line_base,
                         int active_end, const SignalLevels& levels,
                         int start_sample,
                         const std::vector<SampleFixed>& flag_pulse) {
  const SampleFixed baseline_fixed =
      MillivoltsToSampleFixed(levels.blanking_mv);

  // Clear the region from start_sample to active_end with blanking.
  for (int i = start_sample; i < active_end; ++i) {
    (*out_y_mv)[static_cast<std::size_t>(line_base) +
                static_cast<std::size_t>(i)] = baseline_fixed;
  }

  const int pulse_count = static_cast<int>(flag_pulse.size());
  const int flag_end = std::min(start_sample + pulse_count, active_end);
  for (int i = start_sample; i < flag_end; ++i) {
    (*out_y_mv)[static_cast<std::size_t>(line_base) +
                static_cast<std::size_t>(i)] =
        flag_pulse[static_cast<std::size_t>(i - start_sample)];
  }
}

}  // namespace

VbiWaveformRenderer::VbiWaveformRenderer(Standard standard,
                                         double sample_rate_hz)
    : standard_(standard),
      sample_rate_hz_(sample_rate_hz),
      levels_(GetSignalLevels(standard)),
      biphase_encoder_(sample_rate_hz) {}

const FmEncoder& VbiWaveformRenderer::GetFmEncoder() const {
  if (fm_encoder_ == nullptr) {
    fm_encoder_ = std::make_unique<FmEncoder>(sample_rate_hz_);
  }
  return *fm_encoder_;
}

const std::vector<SampleFixed>& VbiWaveformRenderer::GetWhiteFlagPulse(
    int flag_length_samples) const {
  if (white_flag_pulse_length_ == flag_length_samples) {
    return white_flag_pulse_;
  }

  const int ramp_samples = GetFmEncoder().ramp_samples();
  white_flag_pulse_.assign(
      static_cast<std::size_t>(std::max(0, flag_length_samples)), 0);
  for (int i = 0; i < flag_length_samples; ++i) {
    const double level =
        ShapedPulseLevel(i, flag_length_samples, ramp_samples,
                         levels_.blanking_mv, levels_.white_mv);
    white_flag_pulse_[static_cast<std::size_t>(i)] =
        MillivoltsToSampleFixed(level);
  }
  white_flag_pulse_length_ = flag_length_samples;
  return white_flag_pulse_;
}

void VbiWaveformRenderer::Render(const FrameEnrichment& enrichment,
                                 std::vector<SampleFixed>* out_y_mv,
                                 int frame_sample_base,
                                 const std::vector<int>& line_sample_offsets,
                                 const std::vector<int>& line_sample_counts,
                                 int active_window_end_samples) const {
  if (out_y_mv == nullptr || enrichment.vbi_lines.empty()) {
    return;
  }

  for (const VbiLineInjection& injection : enrichment.vbi_lines) {
    const auto line_index =
        static_cast<std::size_t>(injection.line_number_1based - 1);
    if (line_index >= line_sample_offsets.size() ||
        line_index >= line_sample_counts.size()) {
      continue;
    }

    const int line_base = frame_sample_base + line_sample_offsets[line_index];
    const int line_samples = line_sample_counts[line_index];

    // Cap the active end at the line boundary to prevent out-of-bounds writes.
    const int active_end = std::min(active_window_end_samples, line_samples);

    switch (injection.kind) {
      case VbiLineInjection::Kind::kBiphase24:
        WriteBiphaseWaveform(out_y_mv, line_base, active_end,
                             injection.biphase_code, standard_, levels_,
                             injection.start_sample, biphase_encoder_);
        break;
      case VbiLineInjection::Kind::kFm40:
        WriteFmWaveform(out_y_mv, line_base, active_end, injection.fm_data,
                        levels_, injection.start_sample, GetFmEncoder());
        break;
      case VbiLineInjection::Kind::kWhiteFlag:
        WriteWhiteFlagPulse(out_y_mv, line_base, active_end, levels_,
                            injection.start_sample,
                            GetWhiteFlagPulse(injection.flag_length_samples));
        break;
    }
  }
}

void InjectResolvedVbiLines(const FrameEnrichment& enrichment,
                            std::vector<SampleFixed>* out_y_mv,
                            int frame_sample_base,
                            const std::vector<int>& line_sample_offsets,
                            const std::vector<int>& line_sample_counts,
                            Standard standard, double sample_rate_hz,
                            int active_window_end_samples) {
  if (out_y_mv == nullptr || enrichment.vbi_lines.empty()) {
    return;
  }

  const VbiWaveformRenderer renderer(standard, sample_rate_hz);
  renderer.Render(enrichment, out_y_mv, frame_sample_base, line_sample_offsets,
                  line_sample_counts, active_window_end_samples);
}

void BiphaseInjectionManager::Reset() {
  current_section_ = nullptr;
  generators_.clear();
  fm_picture_number_generator_.reset();
  fm_programme_time_generator_.reset();
  placement_engine_.reset();
  biphase_encoder_.reset();
  fm_encoder_.reset();
  line_offsets_ = {};
  has_laserdisc_ = false;
  project_disc_type_ = DiscType::kUnknown;
  disc_type_ = DiscType::kUnknown;
  section_type_ = SectionType::kUnknown;
  last_context_ = {};
  frame_count_ = 0;
}

void BiphaseInjectionManager::SetInitialFrameCount(int initial_count) {
  frame_count_ = initial_count;
}

void BiphaseInjectionManager::SetProjectDiscType(DiscType disc_type) {
  project_disc_type_ = disc_type;
}

const PerFrameContext& BiphaseInjectionManager::GetLastFrameContext() const {
  return last_context_;
}

bool BiphaseInjectionManager::ResolveFrame(const Section& section,
                                           Standard standard,
                                           double sample_rate_hz,
                                           int active_window_start_samples,
                                           FrameEnrichment* out_enrichment,
                                           std::vector<std::string>* errors) {
  if (out_enrichment == nullptr || errors == nullptr) {
    return false;
  }

  out_enrichment->vbi_lines.clear();

  // Initialize section generators first so the picture_number generator already
  // reflects the new section's start_value when deriving colour_frame_index.
  if (&section != current_section_) {
    if (!InitializeSection(section, standard, sample_rate_hz, errors)) {
      return false;
    }
    current_section_ = &section;
  }

  // Derive colour_frame_index from the disc picture number when a CAV
  // picture_number generator is active. This produces disc-accurate phase
  // even after section transitions with non-contiguous picture numbers
  // (backward-skip replay sections or post-gap forward-skip sections).
  // Fall back to the monotonic frame_count_ for sections without a PN
  // generator (lead-in, lead-out, or sections with no laserdisc injection).
  constexpr int kPalColourPeriod = 4;
  constexpr int kNtscColourPeriod = 2;
  const int colour_period =
      (standard == Standard::kPal || standard == Standard::kPalM)
          ? kPalColourPeriod
          : kNtscColourPeriod;
  last_context_ = {};
  if (const CodeGenerator* pn_gen = GetGenerator("picture_number")) {
    const uint32_t code = pn_gen->CurrentCode();
    const int pn = static_cast<int>(
        ((code >> 16U) & 0xFU) * 10000U + ((code >> 12U) & 0xFU) * 1000U +
        ((code >> 8U) & 0xFU) * 100U + ((code >> 4U) & 0xFU) * 10U +
        (code & 0xFU));
    last_context_.colour_frame_index = (pn - 1) % colour_period;
  } else {
    last_context_.colour_frame_index = frame_count_ % colour_period;
  }
  ++frame_count_;

  // Note: the {timecode} OSD token is a continuous per-frame CLV timecode
  // driven by the output frame position and disc type, not by the injected VBI
  // generators (which only advance in sections that carry the relevant codes).
  // It is populated by GenerationStage; see ClvTimecodeForFrame.

  if (!has_laserdisc_) {
    out_enrichment->context = last_context_;
    return true;
  }

  // Horizontal offsets are pure functions of the standard's line length and
  // are resolved once per section in InitializeSection.
  const int offset_160h_samples = line_offsets_.white_flag_start_samples;
  const int offset_172h_samples = line_offsets_.offset_172h_samples;
  const int offset_215h_samples = line_offsets_.fm_start_samples;
  const int flag_length_samples = line_offsets_.white_flag_length_samples;

  for (int line_num = 1; line_num <= line_offsets_.lines_per_frame;
       ++line_num) {
    if (!LinePlacementEngine::IsInBiphaseReservedRange(standard, line_num)) {
      continue;
    }

    const bool field_one = LinePlacementEngine::IsFieldOne(standard, line_num);
    const LineCodeAssignment assignment =
        placement_engine_->GetAssignment(line_num, field_one);

    if (!assignment.assigned) {
      continue;
    }

    VbiLineInjection injection;
    injection.line_number_1based = line_num;
    injection.start_sample =
        assignment.is_white_flag
            ? offset_160h_samples
            : (assignment.uses_172h_offset
                   ? offset_172h_samples
                   : (assignment.is_fm ? offset_215h_samples
                                       : active_window_start_samples));

    if (assignment.is_white_flag) {
      injection.kind = VbiLineInjection::Kind::kWhiteFlag;
      injection.flag_length_samples = flag_length_samples;
    } else if (assignment.is_fm) {
      if (fm_encoder_ == nullptr) {
        continue;
      }
      if (assignment.code_type == "fm_picture_number" &&
          fm_picture_number_generator_) {
        injection.fm_data =
            fm_picture_number_generator_->CurrentData(field_one, section_type_);
      } else if (assignment.code_type == "fm_programme_time" &&
                 fm_programme_time_generator_) {
        injection.fm_data =
            fm_programme_time_generator_->CurrentData(field_one, section_type_);
      } else {
        continue;
      }
      injection.kind = VbiLineInjection::Kind::kFm40;
    } else {
      const CodeGenerator* gen = GetGenerator(assignment.code_type);
      if (gen == nullptr || biphase_encoder_ == nullptr) {
        continue;
      }
      injection.kind = VbiLineInjection::Kind::kBiphase24;
      injection.biphase_code = gen->CurrentCode();
    }

    out_enrichment->vbi_lines.push_back(injection);
  }

  // Capture current code values before advancing so OSD token resolver can
  // read the biphase words and picture number that were written this frame.
  if (const CodeGenerator* pn_gen = GetGenerator("picture_number")) {
    const uint32_t code = pn_gen->CurrentCode();
    last_context_.picture_number = static_cast<int>(
        ((code >> 16U) & 0xFU) * 10000U + ((code >> 12U) & 0xFU) * 1000U +
        ((code >> 8U) & 0xFU) * 100U + ((code >> 4U) & 0xFU) * 10U +
        (code & 0xFU));
  }
  for (const auto& [ct, gen] : generators_) {
    last_context_.biphase_words.push_back(gen->CurrentCode());
  }
  out_enrichment->context = last_context_;

  AdvanceGenerators();
  return true;
}

bool BiphaseInjectionManager::ProcessFrame(
    std::vector<SampleFixed>* out_y_mv, int frame_sample_base,
    const std::vector<int>& line_sample_offsets,
    const std::vector<int>& line_sample_counts, const Section& section,
    Standard standard, double sample_rate_hz,
    const std::vector<LineTimingPrimitive>& frame_lines,
    int active_window_start_samples, int active_window_end_samples,
    std::vector<std::string>* errors) {
  // frame_lines is retained for API compatibility; resolution covers every
  // line of the frame via the standard's timing constants.
  (void)frame_lines;

  if (out_y_mv == nullptr || errors == nullptr) {
    return false;
  }

  FrameEnrichment enrichment;
  if (!ResolveFrame(section, standard, sample_rate_hz,
                    active_window_start_samples, &enrichment, errors)) {
    return false;
  }

  InjectResolvedVbiLines(enrichment, out_y_mv, frame_sample_base,
                         line_sample_offsets, line_sample_counts, standard,
                         sample_rate_hz, active_window_end_samples);
  return true;
}

bool BiphaseInjectionManager::InitializeSection(
    const Section& section, Standard standard, double sample_rate_hz,
    std::vector<std::string>* errors) {
  // Preserve disc-global timekeeping generators across section boundaries.
  // programme_time_code and clv_picture_number accumulate from the start of
  // the disc and must not reset on chapter/section transitions; only Reset()
  // starts them over. The CAV picture_number counter is preserved the same way
  // so it numbers frames continuously across all sections by default; a section
  // that specifies an explicit start_value re-anchors it (handled below).
  // The chapter_number generator is likewise preserved so a section with no
  // explicit chapter continues the previous section's chapter (and its
  // stop-bit track counter); an explicit chapter re-anchors it.
  std::unique_ptr<CodeGenerator> saved_ptc;
  std::unique_ptr<CodeGenerator> saved_cpn;
  std::unique_ptr<CodeGenerator> saved_pn;
  std::unique_ptr<CodeGenerator> saved_ch;
  {
    auto it = generators_.find("programme_time_code");
    if (it != generators_.end()) {
      saved_ptc = std::move(it->second);
    }
    it = generators_.find("clv_picture_number");
    if (it != generators_.end()) {
      saved_cpn = std::move(it->second);
    }
    it = generators_.find("picture_number");
    if (it != generators_.end()) {
      saved_pn = std::move(it->second);
    }
    it = generators_.find("chapter_number");
    if (it != generators_.end()) {
      saved_ch = std::move(it->second);
    }
  }

  generators_.clear();
  // fm_picture_number_generator_ and fm_programme_time_generator_ are NOT
  // reset here — they persist across section boundaries like the biphase
  // timekeeping generators above.
  placement_engine_.reset();
  biphase_encoder_.reset();
  fm_encoder_.reset();
  line_offsets_ = {};
  has_laserdisc_ = false;
  disc_type_ = DiscType::kUnknown;
  section_type_ = section.section_type;

  if (saved_ptc) {
    generators_["programme_time_code"] = std::move(saved_ptc);
  }
  if (saved_cpn) {
    generators_["clv_picture_number"] = std::move(saved_cpn);
  }
  if (saved_pn) {
    generators_["picture_number"] = std::move(saved_pn);
  }
  if (saved_ch) {
    generators_["chapter_number"] = std::move(saved_ch);
  }

  const Section::LineInjection* injection = nullptr;
  for (const Section::LineInjection& inj : section.line_injections) {
    if (Lowercase(inj.type) == "laserdisc") {
      injection = &inj;
      break;
    }
  }

  if (injection == nullptr) {
    return true;
  }

  // The disc format is a project-wide setting supplied via SetProjectDiscType.
  disc_type_ = project_disc_type_;
  if (disc_type_ == DiscType::kUnknown) {
    errors->push_back(
        "Biphase injection: a section declares laserdisc codes but no "
        "project disc_type (CAV/CLV) was set.");
    return false;
  }

  has_laserdisc_ = true;
  biphase_encoder_ = std::make_unique<BiphaseEncoder>(sample_rate_hz);
  if (!Is625LineStandard(standard)) {
    fm_encoder_ = std::make_unique<FmEncoder>(sample_rate_hz);
  }

  // Horizontal placement is frame-invariant: resolve it once here rather than
  // recomputing it for every frame of the section.
  const TimingConstants timing = GetTimingConstants(standard);
  line_offsets_.lines_per_frame = timing.lines_per_frame;
  // Per IEC 60857 Figure 12: NTSC white flag starts at 0.160 H and runs for
  // 0.790 H.
  line_offsets_.white_flag_start_samples = static_cast<int>(
      std::round(k160hFraction * timing.samples_per_line_4fsc));
  line_offsets_.white_flag_length_samples = static_cast<int>(
      std::round(k790hFraction * timing.samples_per_line_4fsc));
  // Per IEC 60856 Figure 14 / IEC 60857 Figure 11: programme_status and NTSC
  // clv_code start at 0.172 H from the start of the line.
  line_offsets_.offset_172h_samples = static_cast<int>(
      std::round(k172hFraction * timing.samples_per_line_4fsc));
  // Per IEC 60857 Figure 13: NTSC 40-bit FM codes start at 0.215 H.
  line_offsets_.fm_start_samples = static_cast<int>(
      std::round(k215hFraction * timing.samples_per_line_4fsc));

  std::vector<std::string> codes_present;

  for (const Section::LineInjectionCode& code : injection->codes) {
    codes_present.push_back(code.code_type);

    if (code.code_type == "lead_in") {
      generators_["lead_in"] = std::make_unique<LeadInCodeGenerator>();

    } else if (code.code_type == "lead_out") {
      generators_["lead_out"] = std::make_unique<LeadOutCodeGenerator>();

    } else if (code.code_type == "picture_number") {
      if (code.start_value_specified) {
        // Explicit anchor: (re)start numbering from start_value at this
        // section, overriding any continued counter.
        generators_["picture_number"] =
            std::make_unique<CavPictureNumberGenerator>(code.start_value,
                                                        standard);
      } else if (generators_.find("picture_number") == generators_.end()) {
        // No counter carried over from an earlier section (this is the first
        // section to number): begin at 1. When a counter was preserved above,
        // leave it running so the number continues across the boundary.
        generators_["picture_number"] =
            std::make_unique<CavPictureNumberGenerator>(1, standard);
      }

    } else if (code.code_type == "picture_stop") {
      generators_["picture_stop"] =
          std::make_unique<PictureStopCodeGenerator>();

    } else if (code.code_type == "chapter_number") {
      if (code.chapter_specified) {
        // Explicit chapter: (re)start the chapter code at this section,
        // resetting the IEC 60856/60857 §10.1.5 stop-bit track counter for
        // the new chapter, overriding any continued chapter.
        generators_["chapter_number"] =
            std::make_unique<ChapterNumberGenerator>(code.chapter);
      } else if (generators_.find("chapter_number") == generators_.end()) {
        // No chapter carried over from an earlier section (this is the first
        // section to carry a chapter): begin at 0. When a chapter was
        // preserved above, leave it running so the chapter number and its
        // stop-bit state continue across the boundary.
        generators_["chapter_number"] =
            std::make_unique<ChapterNumberGenerator>(0);
      }

    } else if (code.code_type == "programme_status") {
      if (code.programme_status_specified) {
        const auto maybe_val = ParseBiphaseHexCode(code.programme_status);
        if (!maybe_val) {
          errors->push_back(
              "Biphase injection: invalid programme_status hex '" +
              code.programme_status + "'.");
          return false;
        }
        generators_["programme_status"] =
            std::make_unique<ProgrammeStatusCodeGenerator>(*maybe_val);
      }

    } else if (code.code_type == "users_code") {
      if (code.users_code_specified) {
        const auto maybe_val = ParseBiphaseHexCode(code.users_code);
        if (!maybe_val) {
          errors->push_back("Biphase injection: invalid users_code hex '" +
                            code.users_code + "'.");
          return false;
        }
        generators_["users_code"] =
            std::make_unique<UsersCodeGenerator>(*maybe_val);
      }

    } else if (code.code_type == "clv_code") {
      generators_["clv_code"] = std::make_unique<ClvCodeGenerator>();

    } else if (code.code_type == "programme_time_code") {
      if (generators_.find("programme_time_code") == generators_.end()) {
        generators_["programme_time_code"] =
            std::make_unique<ProgrammeTimeCodeGenerator>(0, 0, standard);
      }

    } else if (code.code_type == "clv_picture_number") {
      if (generators_.find("clv_picture_number") == generators_.end()) {
        generators_["clv_picture_number"] =
            std::make_unique<ClvPictureNumberGenerator>(standard);
      }

    } else if (code.code_type == "fm_picture_number") {
      if (!fm_picture_number_generator_) {
        const int start = code.start_value_specified ? code.start_value : 1;
        fm_picture_number_generator_ =
            std::make_unique<FmPictureNumberGenerator>(start);
      }

    } else if (code.code_type == "fm_programme_time") {
      if (!fm_programme_time_generator_) {
        fm_programme_time_generator_ =
            std::make_unique<FmProgrammeTimeGenerator>();
      }
    }
  }

  placement_engine_ = std::make_unique<LinePlacementEngine>(
      standard, disc_type_, section_type_, codes_present);

  return true;
}

void BiphaseInjectionManager::AdvanceGenerators() {
  for (auto& [code_type, gen] : generators_) {
    gen->Advance();
  }
  if (fm_picture_number_generator_) {
    fm_picture_number_generator_->Advance(section_type_);
  }
  if (fm_programme_time_generator_) {
    fm_programme_time_generator_->Advance(section_type_);
  }
}

CodeGenerator* BiphaseInjectionManager::GetGenerator(
    const std::string& code_type) const {
  const auto it = generators_.find(code_type);
  if (it == generators_.end()) {
    return nullptr;
  }
  return it->second.get();
}

}  // namespace videosynth
