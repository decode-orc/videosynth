/*
 * File:        test_preview_frame_service.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for the preview frame service: worker threading,
 *              request coalescing, LRU caching, and schedule frame indexing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "preview_frame_service.h"
#include "videosynth/generation_stage.h"
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

Section MakeProgressiveSection(const std::string& name, int duration_frames) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.source = "assets/" + name + ".exr";
  section.duration_frames = duration_frames;
  return section;
}

// Structurally valid PAL project (passes ProjectValidator without a probe);
// the placeholder sources are never decoded because tests inject a fake
// synthesizer.
Project MakeValidProject(const std::vector<int>& section_durations) {
  Project project;
  project.name = "preview-test";
  project.version = "1.0";
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = "out/video.cvbs";
  project.output.metadata_path = "out/metadata.meta";
  for (std::size_t i = 0; i < section_durations.size(); ++i) {
    project.sections.push_back(MakeProgressiveSection(
        "section" + std::to_string(i), section_durations[i]));
  }
  return project;
}

// Fake synthesizer that records calls without touching the filesystem.
struct RecordingSynthesizer {
  std::atomic<int>* call_count = nullptr;
  std::vector<std::size_t>* disc_frames = nullptr;
  std::vector<std::string>* section_names = nullptr;
  std::vector<int>* source_frame_indices = nullptr;
  std::thread::id* synthesis_thread = nullptr;

  bool operator()(const PreviewFrameService::SynthesisContext& context,
                  std::size_t disc_frame, const PreviewOptions& options,
                  PreviewFrameData* out_frame, std::string* error) const {
    Q_UNUSED(options);
    Q_UNUSED(out_frame);
    Q_UNUSED(error);
    if (call_count != nullptr) {
      ++(*call_count);
    }
    if (disc_frames != nullptr) {
      disc_frames->push_back(disc_frame);
    }
    const IGenerationStage::FrameScheduleItem& item =
        (*context.schedule)[disc_frame];
    if (section_names != nullptr && item.section != nullptr) {
      section_names->push_back(item.section->name);
    }
    if (source_frame_indices != nullptr) {
      source_frame_indices->push_back(item.source_frame_index);
    }
    if (synthesis_thread != nullptr) {
      *synthesis_thread = std::this_thread::get_id();
    }
    return true;
  }
};

TEST(PreviewFrameServiceTest, SynthesisRunsOffTheUiThreadWithoutBlocking) {
  std::atomic<int> calls{0};
  std::thread::id synthesis_thread;
  std::atomic<bool> release{false};

  PreviewFrameService service([&](const PreviewFrameService::SynthesisContext&,
                                  std::size_t, const PreviewOptions&,
                                  PreviewFrameData*, std::string*) {
    synthesis_thread = std::this_thread::get_id();
    ++calls;
    while (!release) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  });

  service.SetProject(MakeValidProject({4}));

  QElapsedTimer timer;
  timer.start();
  service.RequestFrame(0, {});
  // Scheduling must return immediately even though synthesis is blocked.
  EXPECT_LT(timer.elapsed(), 100);

  ASSERT_TRUE(PumpUntil([&] { return calls > 0; }, 5000));
  release = true;
  ASSERT_TRUE(PumpUntil([&] { return !service.is_working(); }, 5000));
  EXPECT_NE(synthesis_thread, std::this_thread::get_id());
}

TEST(PreviewFrameServiceTest, CacheHitDeliversWithoutResynthesis) {
  std::atomic<int> calls{0};
  PreviewFrameService service(RecordingSynthesizer{&calls});
  service.SetProject(MakeValidProject({4}));

  int frames_delivered = 0;
  QObject::connect(
      &service, &PreviewFrameService::FrameReady,
      [&frames_delivered](std::shared_ptr<const PreviewFrameData>) {
        ++frames_delivered;
      });

  service.RequestFrame(1, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 1; }, 5000));
  EXPECT_EQ(calls, 1);

  // Same frame and options: served synchronously from the cache.
  service.RequestFrame(1, {});
  EXPECT_EQ(frames_delivered, 2);
  EXPECT_EQ(calls, 1);

  // Different options are a different cache entry.
  PreviewOptions with_noise;
  with_noise.apply_noise = true;
  service.RequestFrame(1, with_noise);
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 3; }, 5000));
  EXPECT_EQ(calls, 2);
}

TEST(PreviewFrameServiceTest, SetProjectInvalidatesCache) {
  std::atomic<int> calls{0};
  PreviewFrameService service(RecordingSynthesizer{&calls});
  const Project project = MakeValidProject({4});
  service.SetProject(project);

  int frames_delivered = 0;
  QObject::connect(
      &service, &PreviewFrameService::FrameReady,
      [&frames_delivered](std::shared_ptr<const PreviewFrameData>) {
        ++frames_delivered;
      });

  service.RequestFrame(0, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 1; }, 5000));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(service.cached_frame_count(), 1);

  // A document change (even to identical content) invalidates the cache.
  service.SetProject(project);
  EXPECT_EQ(service.cached_frame_count(), 0);
  service.RequestFrame(0, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 2; }, 5000));
  EXPECT_EQ(calls, 2);
}

TEST(PreviewFrameServiceTest, LruEvictionDropsLeastRecentlyUsedFrame) {
  std::atomic<int> calls{0};
  PreviewFrameService service(RecordingSynthesizer{&calls},
                              /*cache_capacity=*/2);
  service.SetProject(MakeValidProject({4}));

  int frames_delivered = 0;
  QObject::connect(
      &service, &PreviewFrameService::FrameReady,
      [&frames_delivered](std::shared_ptr<const PreviewFrameData>) {
        ++frames_delivered;
      });

  service.RequestFrame(0, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 1; }, 5000));
  service.RequestFrame(1, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 2; }, 5000));
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(service.cached_frame_count(), 2);

  // Touch frame 0 so frame 1 becomes least recently used, then request a
  // third frame to evict it.
  service.RequestFrame(0, {});
  EXPECT_EQ(frames_delivered, 3);
  service.RequestFrame(2, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 4; }, 5000));
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(service.cached_frame_count(), 2);

  // Frame 0 survived; frame 1 was evicted and needs resynthesis.
  service.RequestFrame(0, {});
  EXPECT_EQ(frames_delivered, 5);
  EXPECT_EQ(calls, 3);
  service.RequestFrame(1, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 6; }, 5000));
  EXPECT_EQ(calls, 4);
}

TEST(PreviewFrameServiceTest, RapidScrubbingCoalescesToLatestRequest) {
  std::atomic<int> calls{0};
  std::atomic<bool> release{false};
  std::vector<std::size_t> synthesised_frames;

  PreviewFrameService service([&](const PreviewFrameService::SynthesisContext&,
                                  std::size_t disc_frame, const PreviewOptions&,
                                  PreviewFrameData*, std::string*) {
    synthesised_frames.push_back(disc_frame);
    if (++calls == 1) {
      // Block the first synthesis so scrub requests pile up behind it.
      while (!release) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return true;
  });
  service.SetProject(MakeValidProject({8}));

  std::vector<std::size_t> delivered;
  QObject::connect(&service, &PreviewFrameService::FrameReady,
                   [&delivered](std::shared_ptr<const PreviewFrameData> frame) {
                     delivered.push_back(frame->output_frame_index);
                   });

  service.RequestFrame(0, {});
  ASSERT_TRUE(PumpUntil([&] { return calls > 0; }, 5000));

  // Scrub while the worker is busy: only the newest request may survive.
  service.RequestFrame(1, {});
  service.RequestFrame(2, {});
  service.RequestFrame(3, {});
  service.RequestFrame(4, {});
  release = true;

  ASSERT_TRUE(PumpUntil(
      [&] { return delivered.size() == 2 && !service.is_working(); }, 5000));
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(synthesised_frames, (std::vector<std::size_t>{0U, 4U}));
  EXPECT_EQ(delivered, (std::vector<std::size_t>{0U, 4U}));
}

TEST(PreviewFrameServiceTest, FrameIndexingMatchesBuildFrameSchedule) {
  std::vector<std::size_t> disc_frames;
  std::vector<std::string> section_names;
  std::vector<int> source_frame_indices;
  PreviewFrameService service(RecordingSynthesizer{
      nullptr, &disc_frames, &section_names, &source_frame_indices});

  const Project project = MakeValidProject({3, 2});
  service.SetProject(project);

  int frames_delivered = 0;
  QObject::connect(
      &service, &PreviewFrameService::FrameReady,
      [&frames_delivered](std::shared_ptr<const PreviewFrameData>) {
        ++frames_delivered;
      });

  // Frame 3 is the second section's first frame.
  service.RequestFrame(3, {});
  ASSERT_TRUE(PumpUntil([&] { return frames_delivered == 1; }, 5000));

  const PreviewScheduleInfo info =
      service.schedule_info().value_or(PreviewScheduleInfo{});
  ASSERT_GT(info.output_frame_count, 0U);
  EXPECT_EQ(info.output_frame_count, 5U);
  EXPECT_EQ(info.standard, Standard::kPal);
  EXPECT_EQ(info.samples_per_line, 1135);
  EXPECT_EQ(info.lines_per_frame, 625);
  ASSERT_EQ(info.section_first_output_frame.size(), 2U);
  EXPECT_EQ(info.section_first_output_frame[0], 0);
  EXPECT_EQ(info.section_first_output_frame[1], 3);

  // The synthesised item must match BuildFrameSchedule for the same project.
  GenerationStage reference_stage;
  std::vector<IGenerationStage::FrameScheduleItem> reference_schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(reference_stage.BuildFrameSchedule(project, &reference_schedule,
                                                 &errors));
  ASSERT_EQ(reference_schedule.size(), 5U);
  ASSERT_EQ(disc_frames.size(), 1U);
  EXPECT_EQ(disc_frames[0], 3U);
  EXPECT_EQ(section_names[0], reference_schedule[3].section->name);
  EXPECT_EQ(section_names[0], "section1");
  EXPECT_EQ(source_frame_indices[0], reference_schedule[3].source_frame_index);
}

TEST(PreviewFrameServiceTest, InvalidProjectReportsFailureWithoutSynthesis) {
  std::atomic<int> calls{0};
  PreviewFrameService service(RecordingSynthesizer{&calls});

  // A default-constructed project is structurally invalid (unknown
  // standard, no output paths, no sections).
  service.SetProject(Project{});

  int failures = 0;
  int frames_delivered = 0;
  QObject::connect(&service, &PreviewFrameService::PreviewFailed,
                   [&failures](quint64, const QString&) { ++failures; });
  QObject::connect(
      &service, &PreviewFrameService::FrameReady,
      [&frames_delivered](std::shared_ptr<const PreviewFrameData>) {
        ++frames_delivered;
      });

  service.RequestFrame(0, {});
  ASSERT_TRUE(PumpUntil([&] { return failures == 1; }, 5000));
  EXPECT_EQ(frames_delivered, 0);
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(service.schedule_info().has_value());
}

TEST(PreviewFrameServiceTest, OutOfRangeRequestFailsButPublishesSchedule) {
  std::atomic<int> calls{0};
  PreviewFrameService service(RecordingSynthesizer{&calls});
  service.SetProject(MakeValidProject({2}));

  int failures = 0;
  QObject::connect(&service, &PreviewFrameService::PreviewFailed,
                   [&failures](quint64, const QString&) { ++failures; });

  service.RequestFrame(99, {});
  ASSERT_TRUE(PumpUntil([&] { return failures == 1; }, 5000));
  EXPECT_EQ(calls, 0);
  // The schedule was still built and published so navigators can clamp.
  const PreviewScheduleInfo info =
      service.schedule_info().value_or(PreviewScheduleInfo{});
  EXPECT_EQ(info.output_frame_count, 2U);
}

TEST(PreviewFrameServiceTest, FieldCadenceNamesEachFieldsFilmFrame) {
  std::array<IGenerationStage::FieldPicture, 2> fields{};
  fields[0] = {.source_frame_index = 37,
               .starts_picture = false,
               .cadence_letter = 'B',
               .cadence_field_in_letter = 3};
  fields[1] = {.source_frame_index = 38,
               .starts_picture = true,
               .cadence_letter = 'C',
               .cadence_field_in_letter = 1};
  EXPECT_EQ(FormatFieldCadence(fields),
            QStringLiteral("F1: film 37 (B3) · F2: film 38 (C1)"));
}

TEST(PreviewFrameServiceTest, FieldCadenceIsEmptyForVideoFrames) {
  std::array<IGenerationStage::FieldPicture, 2> fields{};
  fields[0] = {.source_frame_index = 5, .starts_picture = true};
  fields[1] = {.source_frame_index = 5};
  EXPECT_TRUE(FormatFieldCadence(fields).isEmpty());
}

}  // namespace
}  // namespace videosynth::gui
