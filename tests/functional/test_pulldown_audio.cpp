/*
 * File:        test_pulldown_audio.cpp
 * Module:      audio_track_generator_tests
 * Purpose:     Checks that a 2:3 pulldown section's video soundtrack plays in
 *              real time, on both the 48 kHz and the 44.1 kHz EFM paths, over
 *              a film clip generated with ffmpeg at test time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "videosynth/audio_sample_conversion.h"
#include "videosynth/audio_sample_source.h"
#include "videosynth/audio_track_generator.h"
#include "videosynth/generation_stage.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// 480 film frames at 24000/1001 last 20.02 s, exactly 600 NTSC frames.
constexpr int kFilmFrames = 480;
constexpr std::size_t kOutputFrames = 600;
constexpr std::size_t kWavSamples = 960960;  // 20.02 s at 48 kHz.
constexpr std::size_t kEfmSamples = 882882;  // 20.02 s at 44.1 kHz.

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

// Generated media and output go to a scratch directory, never into the source
// tree.
std::filesystem::path ScratchDirectory() {
  return std::filesystem::temp_directory_path() / "videosynth_pulldown_audio";
}

// A conformant 525-line 24000/1001 film clip lasting 21 s, longer than the
// kFilmFrames the test plays, with a seeded white-noise stereo soundtrack, so
// any skipped or repeated sample shows. Generated once per test process, staged
// under a process-unique name and renamed into place so concurrent test
// processes never read a partial file.
std::string FilmClipWithSound() {
  static std::mutex mutex;
  static bool generated = false;
  const std::lock_guard<std::mutex> lock(mutex);
  const std::filesystem::path path = ScratchDirectory() / "film_sound.mkv";
  if (!generated) {
    std::filesystem::create_directories(ScratchDirectory());
    const std::filesystem::path staging =
        ScratchDirectory() / (std::to_string(getpid()) + "_film_sound.mkv");
    const std::string command =
        "ffmpeg -v error -y -nostdin"
        " -f lavfi -i color=c=gray:s=720x486:r=24000/1001:d=21"
        " -f lavfi -i anoisesrc=r=48000:c=white:seed=7:a=0.5:d=21"
        " -c:v ffv1 -pix_fmt yuv422p10le -color_range tv"
        " -colorspace smpte170m -color_primaries smpte170m -color_trc bt709"
        " -ac 2 -c:a pcm_s24le '" +
        staging.string() + "'";
    EXPECT_EQ(std::system(command.c_str()), 0) << command;
    std::filesystem::rename(staging, path);
    generated = true;
  }
  return path.string();
}

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

std::int32_t ReadLe24Signed(const std::vector<char>& bytes,
                            std::size_t offset) {
  std::uint32_t raw =
      static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1]))
       << 8) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2]))
       << 16);
  if ((raw & 0x800000U) != 0U) {
    raw |= 0xFF000000U;
  }
  return static_cast<std::int32_t>(raw);
}

// Left channel of a written 24-bit stereo channel-pair file.
std::vector<std::int32_t> ReadWavLeft(const std::filesystem::path& path) {
  constexpr std::size_t kHeaderBytes = 44;
  constexpr std::size_t kStereoBytes = 6;
  const std::vector<char> bytes = ReadFileBytes(path);
  std::vector<std::int32_t> left;
  for (std::size_t offset = kHeaderBytes; offset + kStereoBytes <= bytes.size();
       offset += kStereoBytes) {
    left.push_back(ReadLe24Signed(bytes, offset));
  }
  return left;
}

// The left channel of the clip's soundtrack decoded in one uninterrupted
// read from its start.
std::vector<std::int32_t> ReferenceLeft(const std::string& path,
                                        int sample_rate_hz, std::size_t count) {
  FfmpegAudioSampleSource source;
  std::string error;
  EXPECT_TRUE(source.Open(path, 0, 0.0, sample_rate_hz, &error)) << error;
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
  EXPECT_EQ(source.Read(static_cast<int>(count), &left, &right, &error),
            static_cast<int>(count))
      << error;
  return left;
}

TEST(PulldownAudioTest, FilmSoundtrackPlaysUnbrokenAtBothSampleRates) {
  const std::string film = FilmClipWithSound();

  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = (ScratchDirectory() / "film.cvbs").string();
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 0;

  Section section;
  section.name = "Film";
  section.type = "progressive";
  section.source = film;
  // Lead-in audio is not muted by the EFM track-1 pause.
  section.section_type = SectionType::kLeadIn;
  section.duration_frames_all = true;
  section.start_frame = 0;
  section.end_frame = kFilmFrames - 1;
  section.end_frame_specified = true;
  section.cadence.pattern = CadencePattern::kPulldown23;
  AudioChannelPair channel_pair;
  channel_pair.pair = 0;
  channel_pair.pair_specified = true;
  channel_pair.content = AudioPairContent::kVideo;
  section.audio_channel_pairs = {channel_pair};
  project.sections = {section};

  NullLogger logger;
  GenerationStage generation(&logger);
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.BuildFrameSchedule(project, &schedule, &errors))
      << (errors.empty() ? "" : errors.front());
  ASSERT_EQ(schedule.size(), kOutputFrames);

  AudioTrackGenerator generator(&logger);
  ASSERT_TRUE(
      generator.Begin(project, AudioOutputFramesForSchedule(schedule), &errors))
      << (errors.empty() ? "" : errors.front());
  ASSERT_TRUE(generator.efm_active());
  std::vector<std::int16_t> efm_left;
  for (std::size_t k = 0; k < schedule.size(); ++k) {
    ASSERT_TRUE(generator.EmitFrame(k, &errors)) << errors.back();
    efm_left.insert(efm_left.end(), generator.efm_frame_left().begin(),
                    generator.efm_frame_left().end());
  }
  ASSERT_TRUE(generator.Finalize(&errors));

  // The 48 kHz track is the soundtrack sample for sample.
  const std::vector<std::int32_t> wav_left =
      ReadWavLeft(ScratchDirectory() / "film_audio_0.wav");
  ASSERT_EQ(wav_left.size(), kWavSamples);
  const std::vector<std::int32_t> reference =
      ReferenceLeft(film, 48000, kWavSamples);
  ASSERT_EQ(reference.size(), kWavSamples);
  for (std::size_t i = 0; i < kWavSamples; ++i) {
    ASSERT_EQ(wav_left[i], reference[i]) << "48 kHz sample " << i;
  }

  // The EFM path carries one output frame's share of 44.1 kHz samples per
  // frame (SMPTE 272M-1994 Table 1), likewise unbroken.
  std::size_t efm_expected = 0;
  for (std::size_t k = 0; k < kOutputFrames; ++k) {
    efm_expected += static_cast<std::size_t>(
        EfmAudioSamplesForFrame(Standard::kNtsc, static_cast<std::int64_t>(k)));
  }
  EXPECT_EQ(efm_expected, kEfmSamples);
  ASSERT_EQ(efm_left.size(), kEfmSamples);
  const std::vector<std::int16_t> efm_reference =
      ConvertSamples24To16(ReferenceLeft(film, 44100, kEfmSamples));
  for (std::size_t i = 0; i < kEfmSamples; ++i) {
    ASSERT_EQ(efm_left[i], efm_reference[i]) << "44.1 kHz sample " << i;
  }

  std::error_code ec;
  std::filesystem::remove(ScratchDirectory() / "film_audio_0.wav", ec);
  std::filesystem::remove(ScratchDirectory() / "film.efm", ec);
  std::filesystem::remove(ScratchDirectory() / "film.efm.meta", ec);
}

}  // namespace
}  // namespace videosynth
