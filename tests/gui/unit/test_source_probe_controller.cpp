/*
 * File:        test_source_probe_controller.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for background source probing with a mocked probe
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "source_probe_controller.h"
#include "videosynth/model.h"

namespace videosynth::gui {
namespace {

// Pumps the owning thread's event loop until `predicate` is true or the
// timeout elapses. Returns the predicate's final value.
bool PumpUntil(const std::function<bool()>& predicate, int timeout_msec) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout_msec) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

Section MakeMkvSection(const std::string& source) {
  Section section;
  section.name = "Clip";
  section.type = "progressive";
  section.source = source;
  section.duration_frames = 10;
  return section;
}

// Profile matching the validator's PAL MKV requirements (HLD §8.1).
ProgressiveFrameSourceProfile MakeGoodPalMkvProfile() {
  ProgressiveFrameSourceProfile profile;
  profile.container = "matroska";
  profile.codec = "ffv1";
  profile.pixel_format = "yuv422p10le";
  profile.field_order = "tb";
  profile.color_space = "smpte170m";
  profile.color_primaries = "bt470bg";
  profile.color_transfer = "bt709";
  profile.color_range = "tv";
  profile.bit_depth = 10;
  profile.width = 720;
  profile.height = 576;
  profile.sample_aspect_ratio = 128.0 / 117.0;
  profile.frame_rate_hz = 25.0;
  profile.average_frame_rate_hz = 25.0;
  profile.frame_count = 250;
  return profile;
}

TEST(SourceProbeControllerTest, GoodProfilePassesValidatorRules) {
  const QStringList issues = EvaluateSourceProfile(
      MakeMkvSection("clip.mkv"), Standard::kPal, MakeGoodPalMkvProfile());
  EXPECT_TRUE(issues.isEmpty()) << issues.join('\n').toStdString();
}

TEST(SourceProbeControllerTest, WrongRasterFailsValidatorRules) {
  ProgressiveFrameSourceProfile profile = MakeGoodPalMkvProfile();
  profile.height = 486;  // NTSC raster on a PAL project.
  const QStringList issues = EvaluateSourceProfile(MakeMkvSection("clip.mkv"),
                                                   Standard::kPal, profile);
  EXPECT_FALSE(issues.isEmpty());
}

TEST(SourceProbeControllerTest, WrongFieldOrderWarnsWithoutFailing) {
  ProgressiveFrameSourceProfile profile = MakeGoodPalMkvProfile();
  profile.field_order = "bt";  // PAL output is top-field-first.
  const SourceProfileFindings findings = EvaluateSourceProfileFindings(
      MakeMkvSection("clip.mkv"), Standard::kPal, profile);
  EXPECT_TRUE(findings.errors.isEmpty());
  ASSERT_EQ(findings.warnings.size(), 1);
  EXPECT_TRUE(findings.warnings[0].contains(QStringLiteral("field order")));
}

TEST(SourceProbeControllerTest, ConformantVideoReportsUnconvertedColour) {
  const QString summary =
      FormatColourConversionSummary(MakeGoodPalMkvProfile(), Standard::kPal);
  EXPECT_TRUE(summary.contains(QStringLiteral("unconverted")))
      << summary.toStdString();
}

TEST(SourceProbeControllerTest, FullRangeH264ReportsPlannedConversion) {
  ProgressiveFrameSourceProfile profile = MakeGoodPalMkvProfile();
  profile.container = "mov,mp4,m4a,3gp,3g2,mj2";
  profile.codec = "h264";
  profile.pixel_format = "yuv420p";
  profile.color_range = "pc";
  profile.color_space = "bt709";
  const QString summary =
      FormatColourConversionSummary(profile, Standard::kPal);
  EXPECT_TRUE(summary.contains(QStringLiteral("full range -> studio swing")));
  EXPECT_TRUE(summary.contains(QStringLiteral("BT.709 matrix -> BT.601")));
  EXPECT_TRUE(summary.contains(QStringLiteral("yuv420p -> yuv422p10le")));
}

TEST(SourceProbeControllerTest, StillHasNoColourConversionLine) {
  ProgressiveFrameSourceProfile profile;
  profile.container = "exr";
  EXPECT_TRUE(FormatColourConversionSummary(profile, Standard::kPal).isEmpty());
}

TEST(SourceProbeControllerTest, ReportDetailListsSummaryConversionAndFindings) {
  SourceProbeReport report;
  report.probe_ok = true;
  report.profile = MakeGoodPalMkvProfile();
  report.colour_conversion = QStringLiteral("Colour conversion: x");
  report.profile_warnings = {QStringLiteral("warning one")};
  report.profile_issues = {QStringLiteral("error one")};

  const QStringList lines =
      FormatProbeReportDetail(report).split(QLatin1Char('\n'));
  ASSERT_EQ(lines.size(), 4);
  EXPECT_TRUE(lines[0].contains(QStringLiteral("720×576")));
  EXPECT_EQ(lines[1], QStringLiteral("Colour conversion: x"));
  EXPECT_EQ(lines[2], QStringLiteral("warning one"));
  EXPECT_EQ(lines[3], QStringLiteral("error one"));
}

TEST(SourceProbeControllerTest, PublishedReportCarriesConversionAndWarnings) {
  SourceProbeController controller(
      [](const Section&, ProgressiveFrameSourceProfile* profile, std::string*) {
        *profile = MakeGoodPalMkvProfile();
        profile->field_order = "bt";
        profile->color_range = "pc";
        return true;
      });
  controller.SetDebounceInterval(0);
  controller.RequestProbe(MakeMkvSection("clip.mkv"), Standard::kPal);

  ASSERT_TRUE(PumpUntil([&] { return controller.has_report(); }, 5000));
  EXPECT_TRUE(controller.report().profile_ok());
  EXPECT_EQ(controller.report().profile_warnings.size(), 1);
  EXPECT_TRUE(controller.report().colour_conversion.contains(
      QStringLiteral("full range")));
}

TEST(SourceProbeControllerTest, SummaryListsResolvedProfile) {
  const QString summary = FormatSourceProfileSummary(MakeGoodPalMkvProfile());
  EXPECT_TRUE(summary.contains(QStringLiteral("720×576")));
  EXPECT_TRUE(summary.contains(QStringLiteral("ffv1/matroska")));
  EXPECT_TRUE(summary.contains(QStringLiteral("10-bit")));
  EXPECT_TRUE(summary.contains(QStringLiteral("250 frames")));
}

TEST(SourceProbeControllerTest, PublishesReportFromWorkerThread) {
  const std::thread::id main_thread = std::this_thread::get_id();
  std::thread::id probe_thread;

  SourceProbeController controller([&](const Section&,
                                       ProgressiveFrameSourceProfile* profile,
                                       std::string*) {
    probe_thread = std::this_thread::get_id();
    *profile = MakeGoodPalMkvProfile();
    return true;
  });
  controller.SetDebounceInterval(0);
  controller.RequestProbe(MakeMkvSection("clip.mkv"), Standard::kPal);

  ASSERT_TRUE(PumpUntil([&] { return controller.has_report(); }, 5000));
  EXPECT_NE(probe_thread, main_thread);
  EXPECT_TRUE(controller.report().probe_ok);
  EXPECT_TRUE(controller.report().profile_ok());
}

TEST(SourceProbeControllerTest, FailureReasonReachesReport) {
  SourceProbeController controller(
      [](const Section&, ProgressiveFrameSourceProfile*, std::string* error) {
        *error = "no such file";
        return false;
      });
  controller.SetDebounceInterval(0);
  controller.RequestProbe(MakeMkvSection("missing.mkv"), Standard::kPal);

  ASSERT_TRUE(PumpUntil([&] { return controller.has_report(); }, 5000));
  EXPECT_FALSE(controller.report().probe_ok);
  EXPECT_FALSE(controller.report().profile_ok());
  EXPECT_EQ(controller.report().probe_error, QStringLiteral("no such file"));
}

TEST(SourceProbeControllerTest, SlowProbeDoesNotBlockCaller) {
  constexpr int kProbeDelayMsec = 200;
  SourceProbeController controller([&](const Section&,
                                       ProgressiveFrameSourceProfile* profile,
                                       std::string*) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kProbeDelayMsec));
    *profile = MakeGoodPalMkvProfile();
    return true;
  });
  controller.SetDebounceInterval(0);

  QElapsedTimer timer;
  timer.start();
  controller.RequestProbe(MakeMkvSection("clip.mkv"), Standard::kPal);
  EXPECT_LT(timer.elapsed(), kProbeDelayMsec / 2);

  ASSERT_TRUE(PumpUntil([&] { return controller.has_report(); }, 5000));
  EXPECT_GE(timer.elapsed(), kProbeDelayMsec);
}

TEST(SourceProbeControllerTest, RapidRequestsCoalesceToLatestSection) {
  std::vector<std::string> probed_sources;
  SourceProbeController controller([&](const Section& section,
                                       ProgressiveFrameSourceProfile* profile,
                                       std::string*) {
    probed_sources.push_back(section.source);
    *profile = MakeGoodPalMkvProfile();
    return true;
  });
  controller.SetDebounceInterval(50);

  controller.RequestProbe(MakeMkvSection("first.mkv"), Standard::kPal);
  controller.RequestProbe(MakeMkvSection("second.mkv"), Standard::kPal);
  controller.RequestProbe(MakeMkvSection("third.mkv"), Standard::kPal);

  ASSERT_TRUE(PumpUntil([&] { return controller.has_report(); }, 5000));
  ASSERT_TRUE(PumpUntil([&] { return !controller.is_probing(); }, 5000));
  ASSERT_EQ(probed_sources.size(), 1U);
  EXPECT_EQ(probed_sources[0], "third.mkv");
}

}  // namespace
}  // namespace videosynth::gui
