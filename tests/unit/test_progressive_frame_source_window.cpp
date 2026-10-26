/*
 * File:        test_progressive_frame_source_window.cpp
 * Module:      progressive_frame_source_tests
 * Purpose:     Unit tests for the bounded video decode window, using a fake
 *              probe and a fake sequential decoder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "videosynth/progressive_frame_source.h"

namespace videosynth {
namespace {

// Shared, thread-safe record of what the fake decoders did.
struct DecoderLog {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<VideoDecodeRequest> opens;
  int reads = 0;
  // Frames the fake stream actually holds (may differ from the probe).
  int stream_frames = 0;

  int open_count() {
    const std::lock_guard<std::mutex> lock(mutex);
    return static_cast<int>(opens.size());
  }
  int read_count() {
    const std::lock_guard<std::mutex> lock(mutex);
    return reads;
  }
  bool WaitForReads(int wanted) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(10),
                            [&] { return reads >= wanted; });
  }
};

// Emits 2x1 frames whose first luma code is the frame index, so a test can
// check which frame it was handed.
class FakeVideoFrameDecoder final : public IVideoFrameDecoder {
 public:
  explicit FakeVideoFrameDecoder(std::shared_ptr<DecoderLog> log)
      : log_(std::move(log)) {}

  bool Open(const VideoDecodeRequest& request, std::string*) override {
    const std::lock_guard<std::mutex> lock(log_->mutex);
    log_->opens.push_back(request);
    next_ = request.first_frame;
    return true;
  }

  VideoFrameReadStatus ReadFrame(FrameSourceImage* out_image,
                                 std::string*) override {
    {
      const std::lock_guard<std::mutex> lock(log_->mutex);
      if (next_ >= log_->stream_frames) {
        return VideoFrameReadStatus::kEndOfStream;
      }
      ++log_->reads;
    }
    log_->changed.notify_all();
    out_image->width = 2;
    out_image->height = 1;
    out_image->active_width = 2;
    out_image->active_height = 1;
    out_image->pixels.assign(
        2, YCbCr444Pixel{.y = static_cast<std::int16_t>(next_)});
    ++next_;
    return VideoFrameReadStatus::kFrame;
  }

 private:
  std::shared_ptr<DecoderLog> log_;
  int next_ = 0;
};

class FakeProbe final : public IProgressiveFrameSourceProbe {
 public:
  ProgressiveFrameSourceProfile profile;
  int probes = 0;

  bool Probe(const Section&, ProgressiveFrameSourceProfile* out_profile,
             std::string*) override {
    ++probes;
    *out_profile = profile;
    return true;
  }
};

ProgressiveFrameSourceProfile MakePalProfile(int frame_count) {
  ProgressiveFrameSourceProfile profile;
  profile.container = "matroska,webm";
  profile.codec = "ffv1";
  profile.pixel_format = "yuv422p10le";
  profile.color_space = "smpte170m";
  profile.color_range = "tv";
  profile.width = 720;
  profile.height = 576;
  profile.frame_rate_hz = 25.0;
  profile.frame_count = frame_count;
  return profile;
}

Section MakeVideoSection() {
  Section section;
  section.name = "Clip";
  section.type = "progressive";
  section.source = "clip.mkv";
  section.duration_frames_all = true;
  return section;
}

struct Fixture {
  explicit Fixture(std::size_t capacity, int frame_count = 100)
      : log(std::make_shared<DecoderLog>()),
        probe(std::make_shared<FakeProbe>()) {
    log->stream_frames = frame_count;
    probe->profile = MakePalProfile(frame_count);
    std::shared_ptr<DecoderLog> shared_log = log;
    source = std::make_unique<ProgressiveFrameSource>(
        probe,
        [shared_log]() {
          return std::make_unique<FakeVideoFrameDecoder>(shared_log);
        },
        capacity);
  }

  // Returns the index the delivered frame carries, or -1 on failure.
  int Fetch(int frame_index) {
    std::shared_ptr<const FrameSourceImage> image;
    std::string error;
    if (!source->GenerateFrame(section, frame_index, Standard::kPal, &image,
                               &error)) {
      last_error = error;
      return -1;
    }
    return image->PixelAt(0, 0).y;
  }

  std::shared_ptr<DecoderLog> log;
  std::shared_ptr<FakeProbe> probe;
  std::unique_ptr<ProgressiveFrameSource> source;
  Section section = MakeVideoSection();
  std::string last_error;
};

TEST(ProgressiveFrameSourceWindowTest, ForwardReadsShareOneDecoderOpen) {
  Fixture fixture(16);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(fixture.Fetch(i), i);
  }
  EXPECT_EQ(fixture.log->open_count(), 1);
  EXPECT_EQ(fixture.log->opens[0].first_frame, 0);
  EXPECT_EQ(fixture.log->read_count(), 10);
}

TEST(ProgressiveFrameSourceWindowTest, OutOfOrderRequestsInsideWindowHit) {
  Fixture fixture(32);
  EXPECT_EQ(fixture.Fetch(9), 9);
  EXPECT_EQ(fixture.Fetch(4), 4);
  EXPECT_EQ(fixture.Fetch(0), 0);
  EXPECT_EQ(fixture.Fetch(7), 7);
  // The first request opened half a window early and read 0..9 forward; the
  // rest were window hits.
  EXPECT_EQ(fixture.log->open_count(), 1);
  EXPECT_EQ(fixture.log->read_count(), 10);
}

TEST(ProgressiveFrameSourceWindowTest,
     RequestBehindWindowReopensHalfAWindowEarlier) {
  Fixture fixture(4);
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(fixture.Fetch(i), i);
  }
  // The window now holds 6..9; frame 5 reopens at 5 - 4/2 = 3.
  EXPECT_EQ(fixture.Fetch(5), 5);
  ASSERT_EQ(fixture.log->open_count(), 2);
  EXPECT_EQ(fixture.log->opens[1].first_frame, 3);
  EXPECT_EQ(fixture.log->read_count(), 13);
}

TEST(ProgressiveFrameSourceWindowTest, ReopenNeverStartsBeforeSectionStart) {
  Fixture fixture(16);
  fixture.section.start_frame = 10;
  EXPECT_EQ(fixture.Fetch(12), 12);
  ASSERT_EQ(fixture.log->open_count(), 1);
  EXPECT_EQ(fixture.log->opens[0].first_frame, 10);
}

TEST(ProgressiveFrameSourceWindowTest, FarJumpAheadReopensInsteadOfReading) {
  Fixture fixture(4);
  ASSERT_EQ(fixture.Fetch(0), 0);
  EXPECT_EQ(fixture.Fetch(50), 50);
  ASSERT_EQ(fixture.log->open_count(), 2);
  EXPECT_EQ(fixture.log->opens[1].first_frame, 48);
  EXPECT_EQ(fixture.log->read_count(), 4);
}

TEST(ProgressiveFrameSourceWindowTest, WindowHoldsAtMostCapacityFrames) {
  constexpr std::size_t kCapacity = 4;
  Fixture fixture(kCapacity);
  std::vector<std::weak_ptr<const FrameSourceImage>> delivered;
  for (int i = 0; i < 10; ++i) {
    std::shared_ptr<const FrameSourceImage> image;
    std::string error;
    ASSERT_TRUE(fixture.source->GenerateFrame(fixture.section, i,
                                              Standard::kPal, &image, &error));
    delivered.push_back(image);
  }
  // With no caller holding them, only the newest kCapacity frames survive.
  int alive = 0;
  for (std::size_t i = 0; i < delivered.size(); ++i) {
    if (!delivered[i].expired()) {
      ++alive;
      EXPECT_GE(i, delivered.size() - kCapacity) << "frame " << i;
    }
  }
  EXPECT_EQ(alive, static_cast<int>(kCapacity));
}

TEST(ProgressiveFrameSourceWindowTest, LoopBoundaryUsesSecondCursor) {
  Fixture fixture(8, 20);
  // Workers straddling the end of one pass and the start of the next.
  const std::vector<int> requests = {15, 0, 16, 1, 17, 2, 18, 3, 19, 4};
  for (int request : requests) {
    EXPECT_EQ(fixture.Fetch(request), request);
  }
  // One decode per request plus the 8/2 frames the first open looked back.
  EXPECT_EQ(fixture.log->open_count(), 2);
  EXPECT_EQ(fixture.log->read_count(), static_cast<int>(requests.size()) + 4);
}

TEST(ProgressiveFrameSourceWindowTest, ConcurrentCallersShareOneDecoder) {
  constexpr int kFrames = 50;
  constexpr int kWorkers = 8;
  Fixture fixture(32);
  // Rounds of kWorkers concurrent requests: frames inside a round arrive in
  // any order, and no request trails the newest by more than a round — the
  // in-flight bound the synthesis pool enforces (2 x threads), well inside
  // the window.
  std::atomic<int> failures{0};
  for (int round_start = 0; round_start < kFrames; round_start += kWorkers) {
    std::vector<std::thread> workers;
    for (int i = round_start; i < std::min(kFrames, round_start + kWorkers);
         ++i) {
      workers.emplace_back([&, i] {
        std::shared_ptr<const FrameSourceImage> image;
        std::string error;
        if (!fixture.source->GenerateFrame(fixture.section, i, Standard::kPal,
                                           &image, &error) ||
            image->PixelAt(0, 0).y != i) {
          ++failures;
        }
      });
    }
    for (std::thread& worker : workers) {
      worker.join();
    }
  }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(fixture.log->open_count(), 1);
  EXPECT_EQ(fixture.log->read_count(), kFrames);
}

TEST(ProgressiveFrameSourceWindowTest, PrefetchDecodesHalfAWindowAhead) {
  Fixture fixture(8);
  fixture.section.start_frame = 3;
  fixture.source->PrefetchSection(fixture.section, 3, Standard::kPal);
  ASSERT_TRUE(fixture.log->WaitForReads(4));
  // Frames 3..6 are already decoded, so these requests read nothing new.
  for (int i = 3; i < 7; ++i) {
    EXPECT_EQ(fixture.Fetch(i), i);
  }
  EXPECT_EQ(fixture.log->open_count(), 1);
  EXPECT_EQ(fixture.log->opens[0].first_frame, 3);
  EXPECT_EQ(fixture.log->read_count(), 4);
}

TEST(ProgressiveFrameSourceWindowTest, FrameCountComesFromProbeWithoutDecode) {
  Fixture fixture(8, 42);
  int frame_count = 0;
  std::string error;
  ASSERT_TRUE(fixture.source->ResolveFrameCount(fixture.section, Standard::kPal,
                                                &frame_count, &error));
  EXPECT_EQ(frame_count, 42);
  EXPECT_EQ(fixture.log->open_count(), 0);
  EXPECT_EQ(fixture.probe->probes, 1);
}

TEST(ProgressiveFrameSourceWindowTest, RequestPastProbedCountFailsFast) {
  Fixture fixture(8, 20);
  EXPECT_EQ(fixture.Fetch(20), -1);
  EXPECT_NE(fixture.last_error.find("exceeds"), std::string::npos);
  EXPECT_EQ(fixture.log->open_count(), 0);
}

TEST(ProgressiveFrameSourceWindowTest, StreamShorterThanProbeReportsWhere) {
  Fixture fixture(32, 20);
  fixture.log->stream_frames = 10;
  EXPECT_EQ(fixture.Fetch(15), -1);
  EXPECT_NE(fixture.last_error.find("ended at frame 10"), std::string::npos)
      << fixture.last_error;
  // The failed cursor is dropped; an in-range frame still decodes.
  EXPECT_EQ(fixture.Fetch(5), 5);
}

TEST(ProgressiveFrameSourceWindowTest, RasterMismatchFailsBeforeDecoding) {
  Fixture fixture(8);
  fixture.probe->profile.height = 486;
  EXPECT_EQ(fixture.Fetch(0), -1);
  EXPECT_NE(fixture.last_error.find("raster"), std::string::npos);
  EXPECT_EQ(fixture.log->open_count(), 0);
}

TEST(ProgressiveFrameSourceWindowTest, OpenCarriesColourPlanAndStartOffset) {
  Fixture fixture(8);
  fixture.section.start_frame = 2;
  fixture.probe->profile.color_range = "pc";
  fixture.probe->profile.container_start_time_seconds = 0.0;
  fixture.probe->profile.video_start_time_seconds = 0.08;
  ASSERT_EQ(fixture.Fetch(2), 2);
  ASSERT_EQ(fixture.log->open_count(), 1);
  const VideoDecodeRequest& request = fixture.log->opens[0];
  EXPECT_EQ(request.source, "clip.mkv");
  EXPECT_EQ(request.standard, Standard::kPal);
  EXPECT_EQ(request.first_frame, 2);
  EXPECT_DOUBLE_EQ(request.video_start_offset_seconds, 0.08);
  EXPECT_NE(request.filter_chain.find("in_range=pc"), std::string::npos);
}

TEST(ProgressiveFrameSourceWindowTest, ConformantSourceOpensWithoutFilter) {
  Fixture fixture(8);
  ASSERT_EQ(fixture.Fetch(0), 0);
  EXPECT_TRUE(fixture.log->opens[0].filter_chain.empty());
}

TEST(ProgressiveFrameSourceWindowTest, ClearCacheDropsWindowAndDecoders) {
  Fixture fixture(8);
  ASSERT_EQ(fixture.Fetch(0), 0);
  fixture.source->ClearCache();
  EXPECT_EQ(fixture.Fetch(0), 0);
  EXPECT_EQ(fixture.log->open_count(), 2);
}

TEST(ProgressiveFrameSourceWindowTest, Mp4AndMkvAreVideoSourcesExrIsStill) {
  EXPECT_TRUE(IsProgressiveVideoSource("a.mkv"));
  EXPECT_TRUE(IsProgressiveVideoSource("a.MP4"));
  EXPECT_FALSE(IsProgressiveVideoSource("a.exr"));
  EXPECT_TRUE(IsProgressiveStillSource("a.EXR"));
  EXPECT_FALSE(IsProgressiveStillSource("a.avi"));
  EXPECT_FALSE(IsProgressiveVideoSource("a.avi"));
}

}  // namespace
}  // namespace videosynth
