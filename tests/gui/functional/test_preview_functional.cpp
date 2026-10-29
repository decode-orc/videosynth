/*
 * File:        test_preview_functional.cpp
 * Module:      gui_tests
 * Purpose:     Functional tests: the preview service's default synthesizer
 *              reproduces the pipeline's frame output for real colour-bar
 *              content with correct raster geometry, including the channel,
 *              chroma error and luma bandwidth impairments
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "preview_frame_service.h"
#include "preview_render.h"
#include "project_document.h"
#include "project_settings_presenter.h"
#include "section_block_presenters.h"
#include "videosynth/channel_stage.h"
#include "videosynth/chroma_error_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth::gui {
namespace {

bool PumpUntil(const std::function<bool()>& predicate, int timeout_msec) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout_msec) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

std::string AssetPath(const std::string& relative) {
  return (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) / relative).string();
}

Project MakeColourBarProject(Standard standard, const std::string& source) {
  Project project;
  project.name = "preview-functional";
  project.version = "1.0";
  project.cvbs_presets.video_standard_preset = standard;
  project.output.video_path = "out/preview.cvbs";
  project.output.metadata_path = "out/preview.meta";

  Section section;
  section.name = "Bars";
  section.type = "progressive";
  section.source = source;
  section.duration_frames = 2;
  project.sections.push_back(section);
  return project;
}

TEST(PreviewFunctionalTest, DefaultSynthesizerMatchesGenerationStageOutput) {
  const Project project = MakeColourBarProject(
      Standard::kPal,
      AssetPath("videosynth-assets/assets/exr/720x576/75_BARS.exr"));

  PreviewFrameService service;
  service.SetProject(project);

  std::shared_ptr<const PreviewFrameData> frame;
  QObject::connect(&service, &PreviewFrameService::FrameReady,
                   [&frame](std::shared_ptr<const PreviewFrameData> ready) {
                     frame = std::move(ready);
                   });
  QString failure_message;
  QObject::connect(&service, &PreviewFrameService::PreviewFailed,
                   [&failure_message](quint64, const QString& message) {
                     failure_message = message;
                   });

  service.RequestFrame(0, {});
  ASSERT_TRUE(PumpUntil(
      [&] { return frame != nullptr || !failure_message.isEmpty(); }, 30000))
      << failure_message.toStdString();
  ASSERT_TRUE(failure_message.isEmpty()) << failure_message.toStdString();
  ASSERT_NE(frame, nullptr);

  // The preview must be exactly what the generation stage would hand to the
  // output stage for this frame.
  GenerationStage reference_stage;
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(reference_stage.BuildFrameSchedule(project, &schedule, &errors));
  std::vector<SampleFixed> reference_y;
  std::vector<SampleFixed> reference_c;
  ASSERT_TRUE(reference_stage.GenerateFrameBatch(
      project, schedule, 0, 1, &reference_y, &reference_c, &errors));
  EXPECT_EQ(frame->y_mv, reference_y);
  EXPECT_EQ(frame->c_mv, reference_c);

  // Decoded source image accompanies progressive sections.
  ASSERT_TRUE(frame->has_source_image);
  const QImage source = RenderSourceImage(frame->source_image);
  ASSERT_FALSE(source.isNull());
  EXPECT_EQ(source.width(), 720);
  EXPECT_EQ(source.height(), 576);

  // Rendered field rasters carry full-field PAL geometry (VBI included).
  const QImage field1 =
      RenderEncodedFieldImage(frame->y_mv, frame->c_mv, Standard::kPal, 1,
                              EncodedImageMode::kComposite);
  const QImage field2 =
      RenderEncodedFieldImage(frame->y_mv, frame->c_mv, Standard::kPal, 2,
                              EncodedImageMode::kComposite);
  ASSERT_FALSE(field1.isNull());
  ASSERT_FALSE(field2.isNull());
  EXPECT_EQ(field1.width(), 1135);
  EXPECT_EQ(field1.height(), 312);
  EXPECT_EQ(field2.height(), 313);

  // Colour bars: the first active line's leading bar (white, 700 mV) must
  // render markedly brighter than the VBI blanking region above it.
  // PAL active picture starts at line 23; active window starts at sample
  // 177 (see GetActiveRasterGeometry).
  const int active_row = 23 - 1;  // line 23 in field 1, 0-based row
  const int bar_x = 250;          // inside the first (white) bar
  const int white_gray = qGray(field1.pixel(bar_x, active_row + 5));
  const int blanking_gray = qGray(field1.pixel(bar_x, 17));  // VBI line 18
  EXPECT_GT(white_gray, blanking_gray + 80);

  // A second frame of the same section reuses the schedule and must also
  // synthesise (colour-bar content is static, buffers differ only by
  // subcarrier phase continuity rules).
  std::shared_ptr<const PreviewFrameData> second_frame;
  QObject::connect(&service, &PreviewFrameService::FrameReady,
                   [&second_frame](std::shared_ptr<const PreviewFrameData> f) {
                     second_frame = std::move(f);
                   });
  service.RequestFrame(1, {});
  ASSERT_TRUE(PumpUntil([&] { return second_frame != nullptr; }, 30000));
  EXPECT_EQ(second_frame->output_frame_index, 1U);
  EXPECT_EQ(second_frame->y_mv.size(), reference_y.size());
}

// Requests output frame 0 and waits for it.
std::shared_ptr<const PreviewFrameData> PreviewFirstFrame(
    const Project& project) {
  PreviewFrameService service;
  service.SetProject(project);
  std::shared_ptr<const PreviewFrameData> frame;
  QString failure_message;
  QObject::connect(&service, &PreviewFrameService::FrameReady,
                   [&frame](std::shared_ptr<const PreviewFrameData> ready) {
                     frame = std::move(ready);
                   });
  QObject::connect(&service, &PreviewFrameService::PreviewFailed,
                   [&failure_message](quint64, const QString& message) {
                     failure_message = message;
                   });
  service.RequestFrame(0, {});
  EXPECT_TRUE(PumpUntil(
      [&] { return frame != nullptr || !failure_message.isEmpty(); }, 30000));
  EXPECT_TRUE(failure_message.isEmpty()) << failure_message.toStdString();
  return frame;
}

// Edits made through the editors' document commands reach the preview: each
// fires DocumentChanged (which drives the preview pane's debounced refresh),
// and the refreshed frame carries the channel, chroma error and luma
// bandwidth exactly as the pipeline applies them.
TEST(PreviewFunctionalTest, ImpairmentEditsAreReflectedInThePreview) {
  const Project clean = MakeColourBarProject(
      Standard::kPal,
      AssetPath("videosynth-assets/assets/exr/720x576/75_BARS.exr"));
  ProjectDocument document;
  document.ResetProject(clean, QString());
  int document_changes = 0;
  QObject::connect(&document, &ProjectDocument::DocumentChanged,
                   [&document_changes] { ++document_changes; });

  ChannelParameters channel = document.project().channel;
  SetChannelEnabled(&channel, true);
  channel.timing_offset_samples = 0.25;
  SetEdgeSmearEnabled(&channel, true);
  SetFmBeatEnabled(&channel, true);
  ASSERT_TRUE(document.SetChannel(channel));
  ChromaErrorParameters chroma_error = document.project().chroma_error;
  SetChromaErrorEnabled(&chroma_error, true);
  chroma_error.gain = 0.9;
  chroma_error.phase_deg = 5.0;
  ASSERT_TRUE(document.SetChromaError(chroma_error));
  Section section = document.project().sections[0];
  SetLumaBandwidthEnabled(&section, true);
  ASSERT_TRUE(document.SetSection(0, section));
  EXPECT_EQ(document_changes, 3);

  const Project& impaired = document.project();
  const std::shared_ptr<const PreviewFrameData> clean_frame =
      PreviewFirstFrame(clean);
  const std::shared_ptr<const PreviewFrameData> frame =
      PreviewFirstFrame(impaired);
  ASSERT_NE(clean_frame, nullptr);
  ASSERT_NE(frame, nullptr);
  EXPECT_NE(frame->y_mv, clean_frame->y_mv);
  EXPECT_NE(frame->c_mv, clean_frame->c_mv);

  GenerationStage generation;
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.BuildFrameSchedule(impaired, &schedule, &errors));
  std::vector<SampleFixed> reference_y;
  std::vector<SampleFixed> reference_c;
  ASSERT_TRUE(generation.GenerateFrameBatch(
      impaired, schedule, 0, 1, &reference_y, &reference_c, &errors));
  ChannelStage().ApplyChannel(impaired, schedule, 0, 1, &reference_y,
                              &reference_c);
  ChromaErrorStage().ApplyChromaError(impaired, schedule, 0, 1, &reference_c);
  EXPECT_EQ(frame->y_mv, reference_y);
  EXPECT_EQ(frame->c_mv, reference_c);
}

}  // namespace
}  // namespace videosynth::gui
