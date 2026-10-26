/*
 * File:        test_media_audio.cpp
 * Module:      audio_sample_source_tests
 * Purpose:     Functional tests for file and video-soundtrack audio through
 *              ffmpeg: decode, channel duplication, resampling, seeking,
 *              probing, and sound placement against the picture in a full
 *              pipeline run, over media generated with ffmpeg at test time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "videosynth/audio_sample_source.h"
#include "videosynth/audio_track_generator.h"
#include "videosynth/audio_wav_writer.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

// Generated media goes to a scratch directory, never into the source tree.
std::filesystem::path MediaDirectory() {
  return std::filesystem::temp_directory_path() / "videosynth_media_audio";
}

// Generates `name` once per test process with `ffmpeg -v error -y <args>`,
// staged under a process-unique name and renamed into place so concurrent
// test processes never read a partial file.
std::string Media(const std::string& name, const std::string& ffmpeg_args) {
  static std::mutex mutex;
  static std::set<std::string> generated;
  const std::lock_guard<std::mutex> lock(mutex);
  const std::filesystem::path path = MediaDirectory() / name;
  if (generated.count(name) == 0U) {
    std::filesystem::create_directories(MediaDirectory());
    const std::filesystem::path staging =
        MediaDirectory() / (std::to_string(getpid()) + "_" + name);
    const std::string command = "ffmpeg -v error -y -nostdin " + ffmpeg_args +
                                " '" + staging.string() + "'";
    EXPECT_EQ(std::system(command.c_str()), 0) << command;
    std::filesystem::rename(staging, path);
    generated.insert(name);
  }
  return path.string();
}

// One second: a 1 kHz tone on the left, silence on the right.
std::string StereoWav48k() {
  return Media("tone_stereo_48k.wav",
               "-f lavfi -i \"aevalsrc=0.5*sin(2*PI*1000*t)|0:s=48000:d=1\" "
               "-c:a pcm_s16le");
}

// One second of a mono 1 kHz tone at 44.1 kHz.
std::string MonoWav44k() {
  return Media("tone_mono_44k.wav",
               "-f lavfi -i \"aevalsrc=0.5*sin(2*PI*1000*t):s=44100:d=1\" "
               "-c:a pcm_s16le");
}

// Two seconds of distinct left/right tones in FLAC.
std::string StereoFlac48k() {
  return Media("tones_stereo_48k.flac",
               "-f lavfi -i "
               "\"aevalsrc=0.5*sin(2*PI*440*t)|0.25*sin(2*PI*660*t):"
               "s=48000:d=2\" -c:a flac");
}

// Two seconds of a stereo tone at 44.1 kHz, for the resampled seek.
std::string StereoWav44k() {
  return Media("tone_stereo_44k.wav",
               "-f lavfi -i "
               "\"aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):"
               "s=44100:d=2\" -c:a pcm_s16le");
}

// A one-millisecond click at 1.2 s (source frame 30 at 25 frames/s) in an
// otherwise silent soundtrack.
std::string ClickAudioArgs() {
  return " -f lavfi -i "
         "\"aevalsrc=if(between(t\\,1.2\\,1.201)\\,0.9\\,0):s=48000:d=2\"";
}

// 50 PAL frames of H.264 with the click in an AAC soundtrack.
std::string ClickMp4() {
  return Media("click_pal.mp4",
               "-f lavfi -i testsrc2=s=720x576:r=25:d=2" + ClickAudioArgs() +
                   " -c:v libx264 -g 12 -bf 2 -pix_fmt yuv420p -c:a aac");
}

// 50 PAL frames of FFV1 with the click in a PCM soundtrack.
std::string ClickMkv() {
  return Media("click_pal.mkv",
               "-f lavfi -i testsrc2=s=720x576:r=25:d=2" + ClickAudioArgs() +
                   " -c:v ffv1 -pix_fmt yuv422p10le -color_range tv"
                   " -colorspace smpte170m -c:a pcm_s16le");
}

struct Samples {
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
};

// Reads a stream to its end in 4800-sample blocks.
Samples ReadAll(const std::string& path, double start_seconds,
                int sample_rate_hz) {
  FfmpegAudioSampleSource source;
  std::string error;
  EXPECT_TRUE(source.Open(path, 0, start_seconds, sample_rate_hz, &error))
      << error;
  Samples all;
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
  while (true) {
    const int read = source.Read(4800, &left, &right, &error);
    EXPECT_GE(read, 0) << error;
    all.left.insert(all.left.end(), left.begin(), left.end());
    all.right.insert(all.right.end(), right.begin(), right.end());
    if (read < 4800) {
      break;
    }
  }
  return all;
}

// Tone frequency from positive-going zero crossings.
double MeasuredFrequencyHz(const std::vector<std::int32_t>& samples,
                           int sample_rate_hz) {
  int crossings = 0;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    if (samples[i - 1] < 0 && samples[i] >= 0) {
      ++crossings;
    }
  }
  return crossings * static_cast<double>(sample_rate_hz) /
         static_cast<double>(samples.size());
}

std::int32_t PeakMagnitude(const std::vector<std::int32_t>& samples) {
  std::int32_t peak = 0;
  for (const std::int32_t sample : samples) {
    peak = std::max(peak, std::abs(sample));
  }
  return peak;
}

// --- Sample source --------------------------------------------------------

TEST(MediaAudioTest, StereoWavDecodesEverySampleOnItsOwnChannel) {
  const Samples samples = ReadAll(StereoWav48k(), 0.0, 48000);
  ASSERT_EQ(samples.left.size(), 48000U);
  ASSERT_EQ(samples.right.size(), 48000U);
  EXPECT_NEAR(MeasuredFrequencyHz(samples.left, 48000), 1000.0, 2.0);
  // 0.5 of full scale in 16 bits, widened to 24 bits.
  EXPECT_NEAR(PeakMagnitude(samples.left), 0.5 * 8388607, 0.01 * 8388607);
  EXPECT_EQ(PeakMagnitude(samples.right), 0);
}

TEST(MediaAudioTest, MonoFileIsDuplicatedToBothChannelsAndResampled) {
  const Samples samples = ReadAll(MonoWav44k(), 0.0, 48000);
  EXPECT_NEAR(static_cast<double>(samples.left.size()), 48000.0, 64.0);
  EXPECT_EQ(samples.left, samples.right);
  EXPECT_NEAR(MeasuredFrequencyHz(samples.left, 48000), 1000.0, 2.0);
}

TEST(MediaAudioTest, FlacDecodesDistinctChannels) {
  const Samples samples = ReadAll(StereoFlac48k(), 0.0, 48000);
  ASSERT_EQ(samples.left.size(), 96000U);
  EXPECT_NEAR(MeasuredFrequencyHz(samples.left, 48000), 440.0, 2.0);
  EXPECT_NEAR(MeasuredFrequencyHz(samples.right, 48000), 660.0, 2.0);
}

TEST(MediaAudioTest, EfmRateReadResamplesToTheCompactDiscGrid) {
  const Samples samples = ReadAll(StereoWav48k(), 0.0, 44100);
  EXPECT_NEAR(static_cast<double>(samples.left.size()), 44100.0, 64.0);
  EXPECT_NEAR(MeasuredFrequencyHz(samples.left, 44100), 1000.0, 2.0);
}

TEST(MediaAudioTest, SeekMatchesTheTailOfAFullRead) {
  // No resampling: the seek is sample-exact.
  const Samples full = ReadAll(StereoFlac48k(), 0.0, 48000);
  const Samples tail = ReadAll(StereoFlac48k(), 0.5, 48000);
  ASSERT_EQ(tail.left.size(), full.left.size() - 24000U);
  for (std::size_t i = 0; i < tail.left.size(); ++i) {
    ASSERT_EQ(tail.left[i], full.left[24000U + i]) << "sample " << i;
    ASSERT_EQ(tail.right[i], full.right[24000U + i]) << "sample " << i;
  }

  // Resampled: equal once the resampler has settled.
  const Samples full_44k = ReadAll(StereoWav44k(), 0.0, 48000);
  const Samples tail_44k = ReadAll(StereoWav44k(), 0.5, 48000);
  ASSERT_NEAR(static_cast<double>(tail_44k.left.size()),
              static_cast<double>(full_44k.left.size()) - 24000.0, 64.0);
  constexpr std::size_t kSettle = 64;
  const std::int32_t tolerance = 8388607 / 100;
  for (std::size_t i = kSettle; i < 4800; ++i) {
    ASSERT_NEAR(tail_44k.left[i], full_44k.left[24000U + i], tolerance)
        << "sample " << i;
  }
}

TEST(MediaAudioTest, ReadsTheSoundtrackOfAVideo) {
  const Samples samples = ReadAll(ClickMkv(), 0.0, 48000);
  EXPECT_EQ(samples.left.size(), 96000U);
  EXPECT_EQ(samples.left, samples.right);
}

// --- Audio file probe -----------------------------------------------------

TEST(MediaAudioTest, ProbeReportsChannelsRateAndDuration) {
  FfprobeAudioFileProbe probe;
  AudioFileProfile profile;
  std::string error;
  ASSERT_TRUE(probe.Probe(MonoWav44k(), &profile, &error)) << error;
  EXPECT_EQ(profile.channels, 1);
  EXPECT_EQ(profile.sample_rate_hz, 44100);
  EXPECT_NEAR(profile.duration_seconds, 1.0, 0.01);

  ASSERT_TRUE(probe.Probe(StereoFlac48k(), &profile, &error)) << error;
  EXPECT_EQ(profile.channels, 2);
  EXPECT_EQ(profile.sample_rate_hz, 48000);
  EXPECT_NEAR(profile.duration_seconds, 2.0, 0.01);
}

TEST(MediaAudioTest, ProbeRejectsMissingFilesAndFilesWithoutAudio) {
  FfprobeAudioFileProbe probe;
  AudioFileProfile profile;
  std::string error;
  EXPECT_FALSE(probe.Probe((MediaDirectory() / "absent.wav").string(), &profile,
                           &error));
  EXPECT_EQ(error, "the file does not exist.");

  const std::string silent_video = Media(
      "no_audio.mkv", "-f lavfi -i testsrc2=s=720x576:r=25:d=0.2 -c:v ffv1");
  error.clear();
  EXPECT_FALSE(probe.Probe(silent_video, &profile, &error));
  EXPECT_EQ(error, "the file has no audio stream.");
}

// --- Placement in a pipeline run -----------------------------------------

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string& message) override { errors.push_back(message); }
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
  std::vector<std::string> errors;
};

// A PAL project showing `source` from `start_frame` for `duration_frames`
// frames, with its soundtrack on channel pair 0.
Project MakeSoundtrackProject(const std::string& source, int start_frame,
                              int duration_frames, const std::string& tag) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  std::filesystem::create_directories(MediaDirectory());
  project.output.video_path =
      (MediaDirectory() / ("soundtrack_" + tag + ".cvbs")).string();
  project.output.metadata_path =
      (MediaDirectory() / ("soundtrack_" + tag + ".meta")).string();

  Section section;
  section.name = "Clip";
  section.type = "progressive";
  section.source = source;
  section.start_frame = start_frame;
  section.duration_frames = duration_frames;
  AudioChannelPair soundtrack;
  soundtrack.pair = 0;
  soundtrack.pair_specified = true;
  soundtrack.content = AudioPairContent::kVideo;
  section.audio_channel_pairs = {soundtrack};
  project.sections = {section};
  return project;
}

// Runs the project as the CLI does and returns pair 0's left channel.
std::vector<std::int32_t> RunAndReadPairZero(const Project& project) {
  NullLogger logger;
  YamlProjectParser parser(&logger);
  ProgressiveFrameSourceProbe probe;
  FfprobeAudioFileProbe audio_file_probe;
  ProjectValidator validator(&probe, &logger, &audio_file_probe);
  GenerationStage generation(&logger);
  NoiseInjectionStage noise_injection(&logger);
  DropoutInjectionStage dropout_injection(&logger);
  OutputStage output(&logger);
  AudioTrackGenerator audio_generator(&logger);
  VideoSynthPipeline pipeline(&parser, &validator, &generation,
                              &noise_injection, &dropout_injection, &output,
                              &logger, &audio_generator);
  EXPECT_TRUE(pipeline.RunProject(project, RunOptions{}))
      << (logger.errors.empty() ? "" : logger.errors.front());

  const std::filesystem::path wav =
      AudioWavWriter::DeriveAudioPath(project.output.video_path, 0);
  std::ifstream stream(wav, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
  std::vector<std::int32_t> left;
  for (std::size_t offset = 44U; offset + 6U <= bytes.size(); offset += 6U) {
    std::uint32_t raw =
        static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(bytes[offset + 1]))
         << 8) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(bytes[offset + 2]))
         << 16);
    if ((raw & 0x800000U) != 0U) {
      raw |= 0xFF000000U;
    }
    left.push_back(static_cast<std::int32_t>(raw));
  }
  std::filesystem::remove(wav);
  std::filesystem::remove(project.output.video_path);
  std::filesystem::remove(project.output.metadata_path);
  return left;
}

// Output samples whose magnitude exceeds a quarter of full scale, collapsed
// to the first sample of each click.
std::vector<std::size_t> ClickPositions(
    const std::vector<std::int32_t>& samples) {
  std::vector<std::size_t> clicks;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (std::abs(samples[i]) > 8388607 / 4 &&
        (clicks.empty() || i > clicks.back() + 480U)) {
      clicks.push_back(i);
    }
  }
  return clicks;
}

constexpr std::size_t kPalSamplesPerFrame = 1920;
// ±1 ms at 48 kHz.
constexpr double kPlacementTolerance = 48.0;

TEST(MediaAudioTest, Mp4ClickLandsOnTheOutputFrameShowingItsSourceFrame) {
  // Source frame 30 holds the click; starting at 25 puts it on output frame 5.
  const std::vector<std::int32_t> left =
      RunAndReadPairZero(MakeSoundtrackProject(ClickMp4(), 25, 10, "mp4"));
  ASSERT_EQ(left.size(), 10U * kPalSamplesPerFrame);
  const std::vector<std::size_t> clicks = ClickPositions(left);
  ASSERT_EQ(clicks.size(), 1U);
  EXPECT_NEAR(static_cast<double>(clicks[0]), 5.0 * kPalSamplesPerFrame,
              kPlacementTolerance);
}

TEST(MediaAudioTest, LoopingRangeReplaysTheSoundWithThePicture) {
  // Frames 25-34 played twice: the click plays on output frames 5 and 15.
  Project project = MakeSoundtrackProject(ClickMkv(), 25, 0, "loop");
  project.sections[0].end_frame = 34;
  project.sections[0].end_frame_specified = true;
  project.sections[0].duration_frames_all = true;
  project.sections[0].duration_frames_repeat = 2;
  const std::vector<std::int32_t> left = RunAndReadPairZero(project);
  ASSERT_EQ(left.size(), 20U * kPalSamplesPerFrame);
  const std::vector<std::size_t> clicks = ClickPositions(left);
  ASSERT_EQ(clicks.size(), 2U);
  EXPECT_NEAR(static_cast<double>(clicks[0]), 5.0 * kPalSamplesPerFrame,
              kPlacementTolerance);
  EXPECT_NEAR(static_cast<double>(clicks[1]), 15.0 * kPalSamplesPerFrame,
              kPlacementTolerance);
}

}  // namespace
}  // namespace videosynth
