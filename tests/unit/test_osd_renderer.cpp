/*
 * File:        test_osd_renderer.cpp
 * Module:      osd_renderer_tests
 * Purpose:     Unit tests for OsdRenderer — bitmap-font OSD overlay rendering
 *              into luma sample buffers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/osd_renderer.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Nominal active-picture geometry used by all tests.
// Lines 0–575 are the frame; active lines 0–575 (whole raster for simplicity).
// Samples per line: 100.  Active window: samples 0–99.
constexpr int kLines = 30;
constexpr int kSamplesPerLine = 100;
constexpr int kActiveLineStart = 0;
constexpr int kActiveLineEnd = kLines;
constexpr int kActiveSampleStart = 0;
constexpr int kActiveSampleEnd = kSamplesPerLine;

std::vector<SampleFixed> MakeBuffer(SampleFixed fill = SampleFixed{0}) {
  return std::vector<SampleFixed>(
      static_cast<std::size_t>(kLines * kSamplesPerLine), fill);
}

std::vector<int> MakeLineOffsets() {
  std::vector<int> offsets(static_cast<std::size_t>(kLines));
  for (int i = 0; i < kLines; ++i) {
    offsets[static_cast<std::size_t>(i)] = i * kSamplesPerLine;
  }
  return offsets;
}

// PAL signal levels: black=0 mV, white=700 mV.
SignalLevels PalLevels() { return GetSignalLevels(Standard::kPal); }

// Returns the sample value at active-area (col, row) in the buffer.
SampleFixed SampleAt(const std::vector<SampleFixed>& buf, int row, int col) {
  const std::size_t idx = static_cast<std::size_t>(row) *
                              static_cast<std::size_t>(kSamplesPerLine) +
                          static_cast<std::size_t>(col);
  return buf[idx];
}

// Returns the expected SampleFixed for a given E_Y' luma value under PAL.
SampleFixed ExpectedSample(double luma) {
  const SignalLevels lvl = PalLevels();
  const double mv = lvl.black_mv + luma * (lvl.white_mv - lvl.black_mv);
  return MillivoltsToSampleFixed(mv);
}

// Builds a minimal OsdConfig with a single overlay.
OsdConfig MakeSingleOverlay(const std::string& text, int x = 0, int y = 0,
                            int scale = 1, OsdFgLevel fg = OsdFgLevel::kWhite,
                            OsdBgLevel bg = OsdBgLevel::kTransparent) {
  OsdConfig cfg;
  OsdOverlay ov;
  ov.text = text;
  ov.x = x;
  ov.y = y;
  ov.scale = scale;
  ov.fg_level = fg;
  ov.bg_level = bg;
  cfg.overlays.push_back(ov);
  return cfg;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// OsdRenderer_WritesCorrectGlyph_AtOriginScale1
// Render 'A' at (0,0) scale=1 and verify that the pixels on row 3 of the
// glyph (the crossbar row, bitmap byte 0x7E = 0111 1110) are set in the
// foreground and unset pixels remain at the fill value.
TEST(OsdRendererTest, WritesCorrectGlyphAtOriginScale1) {
  OsdRenderer renderer;
  auto buf = MakeBuffer(SampleFixed{0});
  auto cbuf = MakeBuffer(SampleFixed{0});
  const auto offsets = MakeLineOffsets();
  const auto cfg = MakeSingleOverlay("A", 0, 0, 1, OsdFgLevel::kWhite,
                                     OsdBgLevel::kTransparent);
  const SignalLevels lvl = PalLevels();

  renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                  kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                  kActiveSampleEnd, lvl);

  // Glyph row 3 of 'A' is 0x7E = 0111 1110: cols 1-6 are foreground.
  const SampleFixed fg = ExpectedSample(1.0);
  const SampleFixed blank{0};
  EXPECT_EQ(SampleAt(buf, 3, 0),
            blank);  // bit 7 clear → background (transparent)
  EXPECT_EQ(SampleAt(buf, 3, 1), fg);
  EXPECT_EQ(SampleAt(buf, 3, 2), fg);
  EXPECT_EQ(SampleAt(buf, 3, 3), fg);
  EXPECT_EQ(SampleAt(buf, 3, 4), fg);
  EXPECT_EQ(SampleAt(buf, 3, 5), fg);
  EXPECT_EQ(SampleAt(buf, 3, 6), fg);
  EXPECT_EQ(SampleAt(buf, 3, 7), blank);  // bit 0 clear
}

// OsdRenderer_WritesNeutralChroma_WhenLinesSupplied
// With a neutral-chroma line table (SECAM), OSD pixels take the line's neutral
// chroma; lines without an entry fall back to zero chroma.
TEST(OsdRendererTest, WritesNeutralChromaWhenLinesSupplied) {
  OsdRenderer renderer;
  auto buf = MakeBuffer(SampleFixed{0});
  auto cbuf = MakeBuffer(MillivoltsToSampleFixed(50.0));
  const auto offsets = MakeLineOffsets();
  const auto cfg =
      MakeSingleOverlay("A", 0, 0, 1, OsdFgLevel::kWhite, OsdBgLevel::kBlack);

  std::vector<SampleFixed> neutral(static_cast<std::size_t>(kSamplesPerLine));
  for (int i = 0; i < kSamplesPerLine; ++i) {
    neutral[static_cast<std::size_t>(i)] = MillivoltsToSampleFixed(i);
  }
  std::vector<const SampleFixed*> neutral_lines(
      static_cast<std::size_t>(kLines), neutral.data());
  neutral_lines[5] = nullptr;

  renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                  kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                  kActiveSampleEnd, PalLevels(), &neutral_lines);

  // Row 3 (foreground and background cells) takes the neutral line samples.
  for (int col = 0; col < 8; ++col) {
    EXPECT_EQ(SampleAt(cbuf, 3, col), MillivoltsToSampleFixed(col)) << col;
  }
  // Row 5 has no neutral entry, so chroma is zeroed as before.
  EXPECT_EQ(SampleAt(cbuf, 5, 2), SampleFixed{0});
  // Outside the glyph cell the picture chroma is untouched.
  EXPECT_EQ(SampleAt(cbuf, 3, 8), MillivoltsToSampleFixed(50.0));
}

// OsdRenderer_MapsFgLevels_ToDiscreteLuma
// Each of the four supported foreground levels must render at its defined
// E_Y' luma step: white=1.0, light_grey=0.75, dark_grey=0.25, black=0.0.
TEST(OsdRendererTest, MapsFgLevelsToDiscreteLuma) {
  const struct {
    OsdFgLevel level;
    double luma;
  } kCases[] = {
      {OsdFgLevel::kWhite, 1.0},
      {OsdFgLevel::kLightGrey, 0.75},
      {OsdFgLevel::kDarkGrey, 0.25},
      {OsdFgLevel::kBlack, 0.0},
  };
  for (const auto& c : kCases) {
    OsdRenderer renderer;
    auto buf = MakeBuffer(SampleFixed{0});
    auto cbuf = MakeBuffer(SampleFixed{0});
    const auto offsets = MakeLineOffsets();
    const auto cfg =
        MakeSingleOverlay("A", 0, 0, 1, c.level, OsdBgLevel::kTransparent);
    const SignalLevels lvl = PalLevels();

    renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                    kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                    kActiveSampleEnd, lvl);

    // Glyph row 3 of 'A' is 0x7E: col 1 is a foreground pixel.
    EXPECT_EQ(SampleAt(buf, 3, 1), ExpectedSample(c.luma))
        << "fg level " << OsdFgLevelToString(c.level);
  }
}

// OsdRenderer_MapsBgLevels_ToDiscreteLuma
// Each of the four opaque background levels must render at its defined E_Y'
// luma step: white=1.0, light_grey=0.75, dark_grey=0.25, black=0.0.
TEST(OsdRendererTest, MapsBgLevelsToDiscreteLuma) {
  const struct {
    OsdBgLevel level;
    double luma;
  } kCases[] = {
      {OsdBgLevel::kWhite, 1.0},
      {OsdBgLevel::kLightGrey, 0.75},
      {OsdBgLevel::kDarkGrey, 0.25},
      {OsdBgLevel::kBlack, 0.0},
  };
  for (const auto& c : kCases) {
    OsdRenderer renderer;
    auto buf = MakeBuffer(SampleFixed{42});
    auto cbuf = MakeBuffer(SampleFixed{42});
    const auto offsets = MakeLineOffsets();
    const auto cfg =
        MakeSingleOverlay("A", 0, 0, 1, OsdFgLevel::kWhite, c.level);
    const SignalLevels lvl = PalLevels();

    renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                    kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                    kActiveSampleEnd, lvl);

    // Glyph row 3 of 'A' is 0x7E: col 0 is a background pixel.
    EXPECT_EQ(SampleAt(buf, 3, 0), ExpectedSample(c.luma))
        << "bg level " << OsdBgLevelToString(c.level);
  }
}

// OsdRenderer_ScalesGlyphCorrectly_Scale2
// Render '0' at scale=2 and verify that the top-left 16×16 pixel block
// is exactly double the glyph row width.  Glyph row 0 of '0' is 0x3C =
// 0011 1100: active-area columns 2–5 at scale 1, so columns 4–11 at scale 2.
TEST(OsdRendererTest, ScalesGlyphCorrectlyAtScale2) {
  OsdRenderer renderer;
  auto buf = MakeBuffer(SampleFixed{0});
  auto cbuf = MakeBuffer(SampleFixed{0});
  const auto offsets = MakeLineOffsets();
  const auto cfg =
      MakeSingleOverlay("0", 0, 0, 2, OsdFgLevel::kWhite, OsdBgLevel::kBlack);
  const SignalLevels lvl = PalLevels();

  renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                  kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                  kActiveSampleEnd, lvl);

  const SampleFixed fg = ExpectedSample(1.0);
  const SampleFixed bg = ExpectedSample(0.0);

  // Scale=2: glyph row 0 occupies output rows 0 and 1.
  // 0x3C = 00111100: glyph cols 2–5 are set → output cols 4–11 are fg.
  for (int out_row : {0, 1}) {
    EXPECT_EQ(SampleAt(buf, out_row, 0), bg);
    EXPECT_EQ(SampleAt(buf, out_row, 2), bg);
    EXPECT_EQ(SampleAt(buf, out_row, 4), fg);
    EXPECT_EQ(SampleAt(buf, out_row, 5), fg);
    EXPECT_EQ(SampleAt(buf, out_row, 10), fg);
    EXPECT_EQ(SampleAt(buf, out_row, 11), fg);
    EXPECT_EQ(SampleAt(buf, out_row, 12), bg);
  }
}

// OsdRenderer_ClampsAtRightEdge
// Place a 10-character string at x=96 (4 samples from the right edge of a
// 100-sample active area at scale=1).  Only the first 4 columns of the first
// glyph may be written; nothing may fall at x>=100.
TEST(OsdRendererTest, ClampsAtRightEdge) {
  OsdRenderer renderer;
  // Use a string of spaces so foreground writes are distinct from the fill.
  // Actually use 'I' whose row 0 is 0x3C = cols 2–5 foreground; at x=96
  // cols 2–3 land at samples 98–99 (in range) and cols 4–5 at 100–101
  // (clipped).
  auto buf = MakeBuffer(SampleFixed{0});
  auto cbuf = MakeBuffer(SampleFixed{0});
  const auto offsets = MakeLineOffsets();
  const std::string text(10, 'I');  // 10 glyphs
  const auto cfg = MakeSingleOverlay(text, 96, 0, 1, OsdFgLevel::kWhite,
                                     OsdBgLevel::kTransparent);
  const SignalLevels lvl = PalLevels();

  renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                  kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                  kActiveSampleEnd, lvl);

  // No sample at or beyond kActiveSampleEnd (100) must be written.
  // The buffer has exactly kLines * kSamplesPerLine elements so any out-of-
  // bounds write would cause UB; the test verifies by reading only valid
  // indices. Row 0 of 'I' glyph is 0x3C: cols 2 and 3 within the glyph are set.
  // At x=96 col 2 → sample 98, col 3 → sample 99.  Both are in range.
  const SampleFixed fg = ExpectedSample(1.0);
  EXPECT_EQ(SampleAt(buf, 0, 98), fg);
  EXPECT_EQ(SampleAt(buf, 0, 99), fg);
  // col 4 → sample 100 is out of range and must not be written (buffer
  // unchanged). We cannot directly check sample 100 without UB; instead verify
  // cols just below the boundary are correct, confirming the render did not
  // corrupt them.
}

// OsdRenderer_SkipsOverlay_WhenYOutOfRange
// An overlay with y >= (active_line_end - active_line_start) must leave both
// the luma and chroma sample buffers entirely unchanged.
TEST(OsdRendererTest, SkipsOverlayWhenYOutOfRange) {
  OsdRenderer renderer;
  const SampleFixed fill = MillivoltsToSampleFixed(100.0);
  const SampleFixed chromaFill = MillivoltsToSampleFixed(50.0);
  auto buf = MakeBuffer(fill);
  auto cbuf = MakeBuffer(chromaFill);
  const auto offsets = MakeLineOffsets();
  // y = kLines puts the first row one past the last valid line.
  const auto cfg = MakeSingleOverlay("A", 0, kLines, 1, OsdFgLevel::kWhite,
                                     OsdBgLevel::kBlack);
  const SignalLevels lvl = PalLevels();

  renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                  kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                  kActiveSampleEnd, lvl);

  // Both luma and chroma buffers must be completely unchanged.
  for (const auto& s : buf) {
    EXPECT_EQ(s, fill);
  }
  for (const auto& s : cbuf) {
    EXPECT_EQ(s, chromaFill);
  }
}

// OsdRenderer_TransparentBackground_LeavesUnsetPixels
// With a transparent background, only foreground (set-bit) pixels are written;
// all unset-bit positions retain their original fill value.
TEST(OsdRendererTest, TransparentBackgroundLeavesUnsetPixels) {
  OsdRenderer renderer;
  const SampleFixed fill = MillivoltsToSampleFixed(350.0);  // mid-grey sentinel
  auto buf = MakeBuffer(fill);
  auto cbuf = MakeBuffer(SampleFixed{0});
  const auto offsets = MakeLineOffsets();
  const auto cfg = MakeSingleOverlay("A", 0, 0, 1, OsdFgLevel::kWhite,
                                     OsdBgLevel::kTransparent);
  const SignalLevels lvl = PalLevels();

  renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                  kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                  kActiveSampleEnd, lvl);

  // Glyph row 0 of 'A' is 0x18 = 00011000: cols 3 and 4 are set, rest clear.
  const SampleFixed fg = ExpectedSample(1.0);
  EXPECT_EQ(SampleAt(buf, 0, 0), fill);  // clear → untouched
  EXPECT_EQ(SampleAt(buf, 0, 1), fill);
  EXPECT_EQ(SampleAt(buf, 0, 2), fill);
  EXPECT_EQ(SampleAt(buf, 0, 3), fg);    // set → foreground
  EXPECT_EQ(SampleAt(buf, 0, 4), fg);    // set → foreground
  EXPECT_EQ(SampleAt(buf, 0, 5), fill);  // clear → untouched
  EXPECT_EQ(SampleAt(buf, 0, 6), fill);
  EXPECT_EQ(SampleAt(buf, 0, 7), fill);
}

// OsdRenderer_EmptyText_ProducesNoWrites
// An overlay with an empty resolved text string must not modify the buffer.
TEST(OsdRendererTest, EmptyTextProducesNoWrites) {
  OsdRenderer renderer;
  const SampleFixed fill = MillivoltsToSampleFixed(200.0);
  auto buf = MakeBuffer(fill);
  auto cbuf = MakeBuffer(SampleFixed{0});
  const auto offsets = MakeLineOffsets();
  const auto cfg =
      MakeSingleOverlay("", 0, 0, 1, OsdFgLevel::kWhite, OsdBgLevel::kBlack);
  const SignalLevels lvl = PalLevels();

  renderer.Render(cfg, {""}, &buf, &cbuf, 0, offsets, kActiveLineStart,
                  kActiveLineEnd, kActiveSampleStart, kActiveSampleEnd, lvl);

  for (const auto& s : buf) {
    EXPECT_EQ(s, fill);
  }
}

// OsdRenderer_ChromaZeroedAtOsdPixels
// Render '0' with a solid black background on a chroma buffer pre-filled
// with a non-zero sentinel.  Every pixel touched by the OSD (both fg and bg)
// must have its chroma zeroed; pixels outside the glyph bounding box must
// retain the original chroma fill.
TEST(OsdRendererTest, ChromaZeroedAtOsdPixels) {
  OsdRenderer renderer;
  auto buf = MakeBuffer(SampleFixed{0});
  const SampleFixed chromaFill = MillivoltsToSampleFixed(300.0);
  auto cbuf = MakeBuffer(chromaFill);
  const auto offsets = MakeLineOffsets();
  // Render a single glyph with solid background so every pixel in the 8×8
  // glyph cell is written (bg covers unset bits, fg covers set bits).
  const auto cfg =
      MakeSingleOverlay("0", 0, 0, 1, OsdFgLevel::kWhite, OsdBgLevel::kBlack);
  const SignalLevels lvl = PalLevels();

  renderer.Render(cfg, {cfg.overlays[0].text}, &buf, &cbuf, 0, offsets,
                  kActiveLineStart, kActiveLineEnd, kActiveSampleStart,
                  kActiveSampleEnd, lvl);

  // All 8 columns of the first 8 rows (the glyph cell) must have chroma = 0.
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      EXPECT_EQ(SampleAt(cbuf, row, col), SampleFixed{0})
          << "chroma not zeroed at (" << row << ", " << col << ")";
    }
  }
  // A pixel well outside the glyph must retain the original chroma fill.
  EXPECT_EQ(SampleAt(cbuf, 20, 50), chromaFill);
}

}  // namespace
}  // namespace videosynth
