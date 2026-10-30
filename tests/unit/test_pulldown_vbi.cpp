/*
 * File:        test_pulldown_vbi.cpp
 * Module:      biphase_injection_manager_tests
 * Purpose:     Checks the per-field laserdisc VBI codes of 2:3 pulldown
 *              frames against IEC 60857 clause 10: picture numbers, picture
 *              stop, chapter numbers, white flag and the 40-bit FM picture
 *              number on CAV, and the CLV picture-field codes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "videosynth/biphase_injection_manager.h"
#include "videosynth/biphase_types.h"
#include "videosynth/cav_code_generator.h"
#include "videosynth/film_cadence.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/model.h"

namespace videosynth {
namespace {

constexpr double kSampleRateHz = 4.0 * 315.0e6 / 88.0;
constexpr int kActiveWindowStart = 150;
constexpr uint32_t kPictureStopCode = 0x82CFFFU;
constexpr uint32_t kProgrammeStatusCode = 0x8DC000U;

// NTSC lines of field 1 and field 2 that carry the codes under test.
struct FieldLines {
  int fm;          // 40-bit FM picture number / programme time
  int white_flag;  // white flag
  int line16;      // picture stop / programme status (CAV), CLV PN (CLV)
  int line17;      // picture number / picture stop / chapter (CAV)
  int line18;      // picture number / chapter (CAV)
};
constexpr std::array<FieldLines, 2> kFieldLines = {
    FieldLines{
        .fm = 10, .white_flag = 11, .line16 = 16, .line17 = 17, .line18 = 18},
    FieldLines{.fm = 273,
               .white_flag = 274,
               .line16 = 279,
               .line17 = 280,
               .line18 = 281}};

Section::LineInjectionCode Code(const std::string& code_type) {
  Section::LineInjectionCode code;
  code.code_type = code_type;
  return code;
}

Section MakeSection(CadencePattern pattern,
                    const std::vector<Section::LineInjectionCode>& codes) {
  Section section;
  section.name = "Programme";
  section.type = "progressive";
  section.section_type = SectionType::kProgrammeArea;
  section.duration_frames = 1;
  section.cadence.pattern = pattern;
  Section::LineInjection injection;
  injection.type = "laserdisc";
  injection.codes = codes;
  section.line_injections.push_back(injection);
  return section;
}

std::vector<Section::LineInjectionCode> CavCodes() {
  Section::LineInjectionCode picture_number = Code("picture_number");
  picture_number.start_value = 1;
  picture_number.start_value_specified = true;
  Section::LineInjectionCode chapter = Code("chapter_number");
  chapter.chapter = 1;
  chapter.chapter_specified = true;
  Section::LineInjectionCode status = Code("programme_status");
  status.programme_status = "8DC000";
  status.programme_status_specified = true;
  return {picture_number, Code("picture_stop"),      chapter,
          status,         Code("fm_picture_number"), Code("fm_white_flag")};
}

// Which fields of each frame start a picture, for `frames` frames of a 2:3
// section whose first field sits in `start_slot`.
std::vector<std::array<bool, 2>> PictureStarts(int start_slot, int frames) {
  std::vector<std::array<bool, 2>> starts;
  for (int frame = 0; frame < frames; ++frame) {
    starts.push_back(
        {CadenceFieldAt(start_slot, 2LL * frame).starts_picture,
         CadenceFieldAt(start_slot, (2LL * frame) + 1).starts_picture});
  }
  return starts;
}

// One field's resolved codes, keyed by line.
struct FieldCodes {
  std::map<int, VbiLineInjection> lines;

  bool Has(int line) const { return lines.count(line) > 0; }
  uint32_t Biphase(int line) const { return lines.at(line).biphase_code; }
  bool IsWhiteFlag(int line) const {
    return Has(line) &&
           lines.at(line).kind == VbiLineInjection::Kind::kWhiteFlag;
  }
  int FmValue(int line) const {
    const FmData& data = lines.at(line).fm_data;
    return (data.x1 * 10000) + (data.x2 * 1000) + (data.x3 * 100) +
           (data.x4 * 10) + data.x5;
  }
};

// Resolves the frames in order and splits their codes into fields.
std::vector<FieldCodes> ResolveFields(
    BiphaseInjectionManager* manager, const Section& section,
    const std::vector<std::array<bool, 2>>& starts,
    std::vector<FrameEnrichment>* enrichments = nullptr) {
  std::vector<FieldCodes> fields;
  for (const std::array<bool, 2>& frame_starts : starts) {
    FrameEnrichment enrichment;
    std::vector<std::string> errors;
    EXPECT_TRUE(manager->ResolveFrame(section, Standard::kNtsc, kSampleRateHz,
                                      kActiveWindowStart, frame_starts,
                                      &enrichment, &errors));
    std::array<FieldCodes, 2> frame_fields;
    for (const VbiLineInjection& injection : enrichment.vbi_lines) {
      const std::size_t field = injection.line_number_1based <= 262 ? 0 : 1;
      frame_fields[field].lines[injection.line_number_1based] = injection;
    }
    fields.push_back(frame_fields[0]);
    fields.push_back(frame_fields[1]);
    if (enrichments != nullptr) {
      enrichments->push_back(enrichment);
    }
  }
  return fields;
}

bool IsChapterCode(uint32_t code) { return (code & 0xF00FFFU) == 0x800DDDU; }

BiphaseInjectionManager MakeManager(DiscType disc_type) {
  BiphaseInjectionManager manager;
  manager.Reset();
  manager.SetProjectDiscType(disc_type);
  return manager;
}

// ---------------------------------------------------------------------------
// CAV per-field code tables
// ---------------------------------------------------------------------------

// Phase A over one 10-field cycle (A A B B B C C D D D): pictures 1-4 start
// on fields 0, 2, 5 and 7.
TEST(PulldownVbiTest, CavCodeTableForOneCycleFromPhaseA) {
  BiphaseInjectionManager manager = MakeManager(DiscType::kCAV);
  const Section section = MakeSection(CadencePattern::kPulldown23, CavCodes());
  const std::vector<FieldCodes> fields =
      ResolveFields(&manager, section, PictureStarts(0, 5));
  ASSERT_EQ(fields.size(), 10U);

  // 'P' picture number, 'S' picture stop, '-' neither.
  const std::string roles = "PSPS-PSPS-";
  const std::array<int, 10> picture = {1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
  // IEC 60857 §10.2.3: the FM number changes on each picture's second field.
  const std::array<int, 10> fm_value = {1, 1, 1, 2, 2, 2, 3, 3, 4, 4};

  for (std::size_t i = 0; i < fields.size(); ++i) {
    SCOPED_TRACE("field " + std::to_string(i));
    const FieldCodes& field = fields[i];
    const FieldLines& lines = kFieldLines[i % 2];
    switch (roles[i]) {
      case 'P':
        EXPECT_EQ(field.Biphase(lines.line17),
                  CavPictureNumberGenerator::EncodePictureNumber(picture[i]));
        EXPECT_EQ(field.Biphase(lines.line18),
                  CavPictureNumberGenerator::EncodePictureNumber(picture[i]));
        EXPECT_EQ(field.Biphase(lines.line16), kProgrammeStatusCode);
        EXPECT_TRUE(field.IsWhiteFlag(lines.white_flag));
        break;
      case 'S':
        EXPECT_EQ(field.Biphase(lines.line16), kPictureStopCode);
        EXPECT_EQ(field.Biphase(lines.line17), kPictureStopCode);
        EXPECT_TRUE(IsChapterCode(field.Biphase(lines.line18)));
        EXPECT_FALSE(field.Has(lines.white_flag));
        break;
      default:
        EXPECT_EQ(field.Biphase(lines.line16), kProgrammeStatusCode);
        EXPECT_TRUE(IsChapterCode(field.Biphase(lines.line17)));
        EXPECT_TRUE(IsChapterCode(field.Biphase(lines.line18)));
        EXPECT_FALSE(field.Has(lines.white_flag));
        break;
    }
    EXPECT_EQ(field.FmValue(lines.fm), fm_value[i]);
  }
}

// Every phase follows the same rules, wherever its pictures start, and the
// frame with no picture start (one in five) carries no picture number and no
// white flag.
TEST(PulldownVbiTest, EveryPhasePlacesCodesByPictureStart) {
  for (const CadencePhase phase : {CadencePhase::kA, CadencePhase::kB,
                                   CadencePhase::kC, CadencePhase::kD}) {
    const int start_slot = CadencePhaseStartSlot(phase);
    SCOPED_TRACE("start slot " + std::to_string(start_slot));
    BiphaseInjectionManager manager = MakeManager(DiscType::kCAV);
    const Section section =
        MakeSection(CadencePattern::kPulldown23, CavCodes());
    const std::vector<std::array<bool, 2>> starts =
        PictureStarts(start_slot, 10);
    const std::vector<FieldCodes> fields =
        ResolveFields(&manager, section, starts);

    int next_picture = 1;
    int frames_without_picture = 0;
    for (std::size_t i = 0; i < fields.size(); ++i) {
      SCOPED_TRACE("field " + std::to_string(i));
      const bool starts_picture = starts[i / 2][i % 2];
      const bool follows_start = i > 0 && starts[(i - 1) / 2][(i - 1) % 2];
      const FieldLines& lines = kFieldLines[i % 2];
      const FieldCodes& field = fields[i];
      EXPECT_EQ(field.IsWhiteFlag(lines.white_flag), starts_picture);
      if (starts_picture) {
        EXPECT_EQ(field.Biphase(lines.line17),
                  CavPictureNumberGenerator::EncodePictureNumber(next_picture));
        ++next_picture;
      } else if (follows_start) {
        EXPECT_EQ(field.Biphase(lines.line17), kPictureStopCode);
      } else {
        EXPECT_TRUE(IsChapterCode(field.Biphase(lines.line17)));
      }
      EXPECT_EQ(field.Biphase(lines.line16),
                follows_start ? kPictureStopCode : kProgrammeStatusCode);
      if (i % 2 == 1 && !starts[i / 2][0] && !starts[i / 2][1]) {
        ++frames_without_picture;
      }
    }
    // Ten frames show eight pictures; two frames start none.
    EXPECT_EQ(next_picture, 9);
    EXPECT_EQ(frames_without_picture, 2);
  }
}

// A section that continues the cadence from A's second field opens with a
// one-field picture: both fields of its first frame start pictures. Field 2
// carries its own picture number on lines 280/281 and the stop code for field
// 1's picture on line 279 only, and both fields carry a white flag.
TEST(PulldownVbiTest, OrphanFieldPictureKeepsItsNumberAndStop) {
  BiphaseInjectionManager manager = MakeManager(DiscType::kCAV);
  const Section section = MakeSection(CadencePattern::kPulldown23, CavCodes());
  const std::vector<std::array<bool, 2>> starts = PictureStarts(1, 2);
  ASSERT_TRUE(starts[0][0]);
  ASSERT_TRUE(starts[0][1]);
  const std::vector<FieldCodes> fields =
      ResolveFields(&manager, section, starts);

  EXPECT_EQ(fields[0].Biphase(17),
            CavPictureNumberGenerator::EncodePictureNumber(1));
  EXPECT_TRUE(fields[0].IsWhiteFlag(11));
  EXPECT_EQ(fields[1].Biphase(279), kPictureStopCode);
  EXPECT_EQ(fields[1].Biphase(280),
            CavPictureNumberGenerator::EncodePictureNumber(2));
  EXPECT_EQ(fields[1].Biphase(281),
            CavPictureNumberGenerator::EncodePictureNumber(2));
  EXPECT_TRUE(fields[1].IsWhiteFlag(274));
  // The next field follows picture 2's first field: its stop code.
  EXPECT_EQ(fields[2].Biphase(16), kPictureStopCode);
  EXPECT_EQ(fields[2].Biphase(17), kPictureStopCode);
}

// A picture that starts on field 2 has its stop code in field 1 of the next
// frame, even when the next frame belongs to a video section.
TEST(PulldownVbiTest, PictureStopCrossesIntoTheNextFrame) {
  BiphaseInjectionManager manager = MakeManager(DiscType::kCAV);
  const Section film = MakeSection(CadencePattern::kPulldown23, CavCodes());
  // Frame 3 of phase A: C2 then D1, which starts picture 4 on field 2.
  const std::vector<std::array<bool, 2>> starts = PictureStarts(0, 4);
  ResolveFields(&manager, film, starts);

  Section::LineInjectionCode chapter = Code("chapter_number");
  const Section video =
      MakeSection(CadencePattern::kVideo,
                  {Code("picture_number"), Code("picture_stop"), chapter,
                   Code("fm_picture_number"), Code("fm_white_flag")});
  const std::vector<FieldCodes> next =
      ResolveFields(&manager, video, {{true, false}});
  // Field 1 starts picture 5, so line 17 keeps its number; line 16 carries
  // picture 4's stop.
  EXPECT_EQ(next[0].Biphase(16), kPictureStopCode);
  EXPECT_EQ(next[0].Biphase(17),
            CavPictureNumberGenerator::EncodePictureNumber(5));
  EXPECT_EQ(next[1].Biphase(279), kPictureStopCode);
}

// A video-cadence section keeps one picture per frame: the white flag on
// line 11 of every frame and the picture number on field 1. The FM number
// follows the same rule as film: it changes on each picture's second field,
// so field 1 still carries the previous frame's number (the first programme
// picture, with none before it, carries its own).
TEST(PulldownVbiTest, VideoSectionKeepsOnePicturePerFrame) {
  BiphaseInjectionManager manager = MakeManager(DiscType::kCAV);
  const Section section = MakeSection(CadencePattern::kVideo, CavCodes());
  std::vector<std::array<bool, 2>> starts(4, {true, false});
  const std::vector<FieldCodes> fields =
      ResolveFields(&manager, section, starts);
  for (std::size_t frame = 0; frame < 4; ++frame) {
    SCOPED_TRACE("frame " + std::to_string(frame));
    const int picture = static_cast<int>(frame) + 1;
    const FieldCodes& field1 = fields[2 * frame];
    const FieldCodes& field2 = fields[(2 * frame) + 1];
    EXPECT_TRUE(field1.IsWhiteFlag(11));
    EXPECT_FALSE(field2.Has(274));
    EXPECT_EQ(field1.Biphase(17),
              CavPictureNumberGenerator::EncodePictureNumber(picture));
    EXPECT_EQ(field2.Biphase(280), kPictureStopCode);
    EXPECT_EQ(field1.FmValue(10), frame == 0 ? 1 : picture - 1);
    EXPECT_EQ(field2.FmValue(273), picture);
  }
}

// Picture numbers count pictures across a video → 2:3 → video chain, and
// each field reports the picture it shows for the OSD.
TEST(PulldownVbiTest, PictureNumbersContinueAcrossCadenceChanges) {
  BiphaseInjectionManager manager = MakeManager(DiscType::kCAV);
  const Section video_in = MakeSection(CadencePattern::kVideo, CavCodes());
  Section::LineInjectionCode picture_number = Code("picture_number");
  const Section film = MakeSection(
      CadencePattern::kPulldown23,
      {picture_number, Code("fm_picture_number"), Code("fm_white_flag")});
  const Section video_out = MakeSection(
      CadencePattern::kVideo,
      {picture_number, Code("fm_picture_number"), Code("fm_white_flag")});

  std::vector<FrameEnrichment> enrichments;
  ResolveFields(&manager, video_in, {{true, false}, {true, false}},
                &enrichments);
  const std::vector<FieldCodes> film_fields =
      ResolveFields(&manager, film, PictureStarts(0, 5), &enrichments);
  ResolveFields(&manager, video_out, {{true, false}}, &enrichments);

  // Video pictures 1-2, film pictures 3-6, then video picture 7.
  const std::vector<std::array<int, 2>> expected = {
      {1, 1}, {2, 2}, {3, 3}, {4, 4}, {4, 5}, {5, 6}, {6, 6}, {7, 7}};
  ASSERT_EQ(enrichments.size(), expected.size());
  for (std::size_t frame = 0; frame < expected.size(); ++frame) {
    EXPECT_EQ(enrichments[frame].field_picture_numbers[0], expected[frame][0])
        << "frame " << frame;
    EXPECT_EQ(enrichments[frame].field_picture_numbers[1], expected[frame][1])
        << "frame " << frame;
    EXPECT_EQ(enrichments[frame].context.picture_number, expected[frame][0]);
  }
  // The first film picture's first field still shows video picture 2's FM
  // number.
  EXPECT_EQ(film_fields[0].FmValue(10), 2);
  EXPECT_EQ(film_fields[1].FmValue(273), 3);
}

// 2:3 frames take the colour-frame index from the frame count, not from
// picture numbers that advance four times in five frames.
TEST(PulldownVbiTest, ColourFrameIndexFollowsFramesNotPictures) {
  BiphaseInjectionManager manager = MakeManager(DiscType::kCAV);
  const Section section = MakeSection(CadencePattern::kPulldown23, CavCodes());
  std::vector<FrameEnrichment> enrichments;
  ResolveFields(&manager, section, PictureStarts(0, 10), &enrichments);
  for (std::size_t frame = 0; frame < enrichments.size(); ++frame) {
    EXPECT_EQ(enrichments[frame].context.colour_frame_index,
              static_cast<int>(frame % 2))
        << "frame " << frame;
  }
}

// ---------------------------------------------------------------------------
// CLV
// ---------------------------------------------------------------------------

// On CLV the picture-field codes (CLV picture number, programme time code)
// follow film pictures like CAV picture numbers do. Their values stay those
// of the video frame the picture starts in, so the CLV picture number skips
// the one frame in five that starts no picture and the colour time error
// rule (IEC 60857 Amendment 2 §10.1.10), which counts video frames, still
// applies. The other fields carry clv_code, programme_status and chapter.
TEST(PulldownVbiTest, ClvPictureCodesFollowFilmPictures) {
  BiphaseInjectionManager manager = MakeManager(DiscType::kCLV);
  Section::LineInjectionCode chapter = Code("chapter_number");
  chapter.chapter = 1;
  chapter.chapter_specified = true;
  const Section section =
      MakeSection(CadencePattern::kPulldown23,
                  {Code("programme_time_code"), Code("clv_picture_number"),
                   Code("clv_code"), chapter, Code("fm_programme_time"),
                   Code("fm_white_flag")});
  const std::vector<std::array<bool, 2>> starts = PictureStarts(0, 5);
  const std::vector<FieldCodes> fields =
      ResolveFields(&manager, section, starts);

  constexpr uint32_t kClvCode = 0x87FFFFU;
  std::vector<int> clv_picture_frames;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    SCOPED_TRACE("field " + std::to_string(i));
    const bool starts_picture = starts[i / 2][i % 2];
    const FieldLines& lines = kFieldLines[i % 2];
    const FieldCodes& field = fields[i];
    EXPECT_EQ(field.IsWhiteFlag(lines.white_flag), starts_picture);
    if (starts_picture) {
      const uint32_t clv_picture = field.Biphase(lines.line16);
      EXPECT_EQ(clv_picture & 0xF0F000U, 0x80E000U);
      clv_picture_frames.push_back(static_cast<int>(
          ((clv_picture >> 4U) & 0xFU) * 10U + (clv_picture & 0xFU)));
      EXPECT_EQ(field.Biphase(lines.line17) & 0xF0FF00U, 0xF0DD00U);
      EXPECT_EQ(field.Biphase(lines.line18) & 0xF0FF00U, 0xF0DD00U);
    } else {
      EXPECT_EQ(field.Biphase(lines.line17), kClvCode);
      EXPECT_TRUE(IsChapterCode(field.Biphase(lines.line18)));
    }
    EXPECT_TRUE(field.Has(lines.fm));
  }
  // Pictures start in frames 0, 1, 2 (field 2) and 3 (field 2); frame 4
  // starts none.
  EXPECT_EQ(clv_picture_frames, (std::vector<int>{0, 1, 2, 3}));
}

}  // namespace
}  // namespace videosynth
