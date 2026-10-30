/*
 * File:        test_audio_track_generator.cpp
 * Module:      audio_track_generator_tests
 * Purpose:     Validates the multi-pair AudioTrackGenerator: per-pair file
 *              emission, output-position sample counts, per-section silence
 *              for undeclared pairs / channels, and file / video-soundtrack
 *              content (including 2:3 pulldown film time) read through a
 *              fake sample source.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "fake_audio_sample_source.h"
#include "videosynth/audio_sample_conversion.h"
#include "videosynth/audio_track_generator.h"
#include "videosynth/audio_wav_writer.h"
#include "videosynth/model.h"

namespace videosynth {
namespace {

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

AudioParameters ActiveTone() {
  AudioParameters channel;
  channel.enabled = true;
  channel.waveform = AudioWaveform::kSquare;  // Non-zero on the first sample.
  channel.frequency_hz = 1000.0;
  channel.amplitude = 0.5;
  return channel;
}

AudioChannelPair MakePair(int pair, bool left, bool right) {
  AudioChannelPair channel_pair;
  channel_pair.pair = pair;
  channel_pair.pair_specified = true;
  if (left) {
    channel_pair.left = ActiveTone();
  }
  if (right) {
    channel_pair.right = ActiveTone();
  }
  return channel_pair;
}

Project MakeProject(const std::filesystem::path& video_path) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = video_path.string();
  return project;
}

constexpr std::size_t kPalSamplesPerFrame = 1920;
constexpr std::size_t kStereoBytes = 6;
constexpr std::size_t kFrameBytes = kPalSamplesPerFrame * kStereoBytes;

// True if every 24-bit sample in the given output frame of `bytes` is zero.
bool FrameIsSilent(const std::vector<char>& bytes, std::size_t frame_index) {
  const std::size_t start = 44U + frame_index * kFrameBytes;
  for (std::size_t i = 0; i < kPalSamplesPerFrame * 2U; ++i) {
    if (ReadLe24Signed(bytes, start + i * 3U) != 0) {
      return false;
    }
  }
  return true;
}

std::filesystem::path TempPath(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

TEST(AudioTrackGeneratorTest, InactiveWhenNoPairsDeclared) {
  const std::filesystem::path video_path = TempPath("videosynth_atg_none.cvbs");
  const Project project = MakeProject(video_path);

  AudioTrackGenerator generator;
  std::vector<std::string> errors;
  Section section;
  const std::vector<const Section*> frames = {&section, &section};
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  EXPECT_FALSE(generator.active());
  EXPECT_TRUE(generator.channel_pairs().empty());
  // EmitFrame and Finalize are no-ops when inactive.
  EXPECT_TRUE(generator.EmitFrame(0, &errors));
  EXPECT_TRUE(generator.Finalize(&errors));
}

TEST(AudioTrackGeneratorTest, EmitsOneFilePerDeclaredPairWithSilence) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_multi.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_multi_audio_0.wav");
  const std::filesystem::path audio1 =
      TempPath("videosynth_atg_multi_audio_1.wav");
  std::filesystem::remove(audio0);
  std::filesystem::remove(audio1);

  Project project = MakeProject(video_path);

  // Section A declares pair 0 (stereo). Section B declares pair 1 (left only).
  Section section_a;
  section_a.name = "A";
  section_a.audio_channel_pairs = {MakePair(0, /*left=*/true, /*right=*/true)};
  Section section_b;
  section_b.name = "B";
  section_b.audio_channel_pairs = {MakePair(1, /*left=*/true, /*right=*/false)};
  project.sections = {section_a, section_b};

  // Output order: A, A, B, B.
  const std::vector<const Section*> frames = {
      &project.sections[0], &project.sections[0], &project.sections[1],
      &project.sections[1]};

  AudioTrackGenerator generator;
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors))
      << (errors.empty() ? "" : errors.front());
  EXPECT_TRUE(generator.active());
  ASSERT_EQ(generator.channel_pairs().size(), 2U);
  EXPECT_EQ(generator.channel_pairs()[0], 0);
  EXPECT_EQ(generator.channel_pairs()[1], 1);

  for (std::size_t k = 0; k < frames.size(); ++k) {
    ASSERT_TRUE(generator.EmitFrame(k, &errors))
        << (errors.empty() ? "" : errors.front());
  }
  ASSERT_TRUE(generator.Finalize(&errors));

  ASSERT_TRUE(std::filesystem::exists(audio0));
  ASSERT_TRUE(std::filesystem::exists(audio1));

  const std::vector<char> bytes0 = ReadFileBytes(audio0);
  const std::vector<char> bytes1 = ReadFileBytes(audio1);
  ASSERT_EQ(bytes0.size(), 44U + 4U * kFrameBytes);
  ASSERT_EQ(bytes1.size(), 44U + 4U * kFrameBytes);

  // Pair 0 is declared only by section A (output frames 0,1): frames 2,3 (B)
  // are silent, frames 0,1 are not.
  EXPECT_FALSE(FrameIsSilent(bytes0, 0));
  EXPECT_FALSE(FrameIsSilent(bytes0, 1));
  EXPECT_TRUE(FrameIsSilent(bytes0, 2));
  EXPECT_TRUE(FrameIsSilent(bytes0, 3));

  // Pair 1 is declared only by section B (output frames 2,3): frames 0,1 (A)
  // are silent, frames 2,3 are not.
  EXPECT_TRUE(FrameIsSilent(bytes1, 0));
  EXPECT_TRUE(FrameIsSilent(bytes1, 1));
  EXPECT_FALSE(FrameIsSilent(bytes1, 2));
  EXPECT_FALSE(FrameIsSilent(bytes1, 3));

  std::filesystem::remove(audio0);
  std::filesystem::remove(audio1);
}

TEST(AudioTrackGeneratorTest, SilentChannelWithinDeclaredPairIsZero) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_silentchan.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_silentchan_audio_0.wav");
  std::filesystem::remove(audio0);

  Project project = MakeProject(video_path);
  Section section;
  section.name = "Mono";
  section.audio_channel_pairs = {MakePair(0, /*left=*/true, /*right=*/false)};
  project.sections = {section};
  const std::vector<const Section*> frames = {&project.sections[0]};

  AudioTrackGenerator generator;
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  ASSERT_TRUE(generator.EmitFrame(0, &errors));
  ASSERT_TRUE(generator.Finalize(&errors));

  const std::vector<char> bytes = ReadFileBytes(audio0);
  ASSERT_EQ(bytes.size(), 44U + kFrameBytes);
  bool any_left_nonzero = false;
  for (std::size_t i = 0; i < kPalSamplesPerFrame; ++i) {
    const std::size_t left_offset = 44U + i * kStereoBytes;
    const std::size_t right_offset = left_offset + 3U;
    if (ReadLe24Signed(bytes, left_offset) != 0) {
      any_left_nonzero = true;
    }
    EXPECT_EQ(ReadLe24Signed(bytes, right_offset), 0);  // Right is silent.
  }
  EXPECT_TRUE(any_left_nonzero);

  std::filesystem::remove(audio0);
}

// Runs the generator over `frames` and returns the concatenated 44.1 kHz EFM
// samples for the left and right channels of the selected pair.
struct EfmCapture {
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  std::vector<int> samples_per_frame;
};

EfmCapture RunAndCaptureEfm(const Project& project,
                            const std::vector<const Section*>& frames) {
  EfmCapture capture;
  AudioTrackGenerator generator;
  std::vector<std::string> errors;
  EXPECT_TRUE(generator.Begin(project, frames, &errors));
  EXPECT_TRUE(generator.efm_active());
  for (std::size_t k = 0; k < frames.size(); ++k) {
    EXPECT_TRUE(generator.EmitFrame(k, &errors));
    capture.samples_per_frame.push_back(generator.efm_samples_for_frame(k));
    const std::vector<std::int16_t>& left = generator.efm_frame_left();
    const std::vector<std::int16_t>& right = generator.efm_frame_right();
    capture.left.insert(capture.left.end(), left.begin(), left.end());
    capture.right.insert(capture.right.end(), right.begin(), right.end());
  }
  EXPECT_TRUE(generator.Finalize(&errors));
  return capture;
}

TEST(AudioTrackGeneratorTest, SynthesisesSelectedPairAt44100Hz) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_efm_pal.cvbs");
  const std::filesystem::path audio1 =
      TempPath("videosynth_atg_efm_pal_audio_1.wav");
  const std::filesystem::path efm1 = TempPath("videosynth_atg_efm_pal.efm");
  std::filesystem::remove(audio1);
  std::filesystem::remove(efm1);

  Project project = MakeProject(video_path);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 1;
  Section lead_in;
  lead_in.name = "LeadIn";
  lead_in.section_type = SectionType::kLeadIn;
  lead_in.audio_channel_pairs = {MakePair(1, /*left=*/true, /*right=*/true)};
  Section programme;
  programme.name = "Tone";
  programme.section_type = SectionType::kProgrammeArea;
  programme.audio_channel_pairs = {MakePair(1, /*left=*/true, /*right=*/true)};
  project.sections = {lead_in, programme};
  const std::vector<const Section*> frames = {
      &project.sections[0], &project.sections[0], &project.sections[1],
      &project.sections[1]};

  const EfmCapture capture = RunAndCaptureEfm(project, frames);

  // IEC 60856-1986 Amd 2 13.2: PAL carries 1764 EFM samples per video frame.
  EXPECT_EQ(capture.samples_per_frame,
            (std::vector<int>{1764, 1764, 1764, 1764}));
  EXPECT_EQ(capture.left.size(), std::size_t{4} * 1764U);
  EXPECT_EQ(capture.right.size(), std::size_t{4} * 1764U);

  // The lead-in carries the tone; the programme area opens with the mandatory
  // pause preceding track 1, which is digital silence in the EFM stream
  // (IEC 60908-1999, 17.5.1).
  const auto any_nonzero = [&capture](std::ptrdiff_t first,
                                      std::ptrdiff_t last) {
    return std::any_of(capture.left.begin() + first,
                       capture.left.begin() + last,
                       [](std::int16_t sample) { return sample != 0; });
  };
  constexpr std::ptrdiff_t kEfmSamplesPerPalFrame = 1764;
  EXPECT_TRUE(any_nonzero(0, 2 * kEfmSamplesPerPalFrame));
  EXPECT_FALSE(
      any_nonzero(2 * kEfmSamplesPerPalFrame, 4 * kEfmSamplesPerPalFrame));

  // The 48 kHz WAV path is unaffected and still written for the pair, and the
  // EFM T-value file is written alongside it.
  ASSERT_TRUE(std::filesystem::exists(audio1));
  const std::vector<char> bytes = ReadFileBytes(audio1);
  EXPECT_EQ(bytes.size(), 44U + (std::size_t{4} * kFrameBytes));
  EXPECT_TRUE(std::filesystem::exists(efm1));
  EXPECT_GT(std::filesystem::file_size(efm1), 0U);

  std::filesystem::remove(audio1);
  std::filesystem::remove(efm1);
}

TEST(AudioTrackGeneratorTest, AbortRemovesThePartialEfmTrack) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_efm_abort.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_efm_abort_audio_0.wav");
  const std::filesystem::path efm0 = TempPath("videosynth_atg_efm_abort.efm");
  std::filesystem::remove(audio0);
  std::filesystem::remove(efm0);

  Project project = MakeProject(video_path);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 0;
  Section section;
  section.name = "Tone";
  section.section_type = SectionType::kProgrammeArea;
  section.audio_channel_pairs = {MakePair(0, /*left=*/true, /*right=*/true)};
  project.sections = {section};
  const std::vector<const Section*> frames = {&project.sections[0],
                                              &project.sections[0]};

  AudioTrackGenerator generator;
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  ASSERT_TRUE(generator.EmitFrame(0, &errors));
  ASSERT_TRUE(std::filesystem::exists(efm0));
  generator.Abort();

  EXPECT_FALSE(std::filesystem::exists(efm0));
  EXPECT_FALSE(std::filesystem::exists(audio0));
}

TEST(AudioTrackGeneratorTest, EfmSynthesisIsDeterministicAcrossRuns) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_efm_repeat.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_efm_repeat_audio_0.wav");
  const std::filesystem::path efm0 = TempPath("videosynth_atg_efm_repeat.efm");
  std::filesystem::remove(audio0);
  std::filesystem::remove(efm0);

  Project project = MakeProject(video_path);
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 0;
  // Lead-in audio is not muted by the track-1 pause, so the compared samples
  // carry the synthesised tone.
  Section section;
  section.name = "Tone";
  section.section_type = SectionType::kLeadIn;
  section.audio_channel_pairs = {MakePair(0, /*left=*/true, /*right=*/true)};
  project.sections = {section};
  const std::vector<const Section*> frames = {
      &project.sections[0], &project.sections[0], &project.sections[0]};

  const EfmCapture first = RunAndCaptureEfm(project, frames);
  const EfmCapture second = RunAndCaptureEfm(project, frames);

  // SMPTE 272M-1994 Table 1: 44.1 kHz NTSC frames 1, 2, 3 of the sequence.
  EXPECT_EQ(first.samples_per_frame, (std::vector<int>{1472, 1471, 1472}));
  EXPECT_EQ(first.left, second.left);
  EXPECT_EQ(first.right, second.right);
  EXPECT_TRUE(std::any_of(first.left.begin(), first.left.end(),
                          [](std::int16_t sample) { return sample != 0; }));

  std::filesystem::remove(audio0);
  std::filesystem::remove(efm0);
}

TEST(AudioTrackGeneratorTest,
     EfmInactiveForUndeclaredPairOrUnsupportedStandard) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_efm_inactive.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_efm_inactive_audio_0.wav");
  std::filesystem::remove(audio0);

  Project project = MakeProject(video_path);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 3;  // Not declared by any section.
  Section section;
  section.name = "Tone";
  section.audio_channel_pairs = {MakePair(0, /*left=*/true, /*right=*/true)};
  project.sections = {section};
  const std::vector<const Section*> frames = {&project.sections[0]};

  {
    AudioTrackGenerator generator;
    std::vector<std::string> errors;
    ASSERT_TRUE(generator.Begin(project, frames, &errors));
    EXPECT_FALSE(generator.efm_active());
    ASSERT_TRUE(generator.EmitFrame(0, &errors));
    EXPECT_TRUE(generator.efm_frame_left().empty());
    EXPECT_EQ(generator.efm_samples_for_frame(0), 0);
    ASSERT_TRUE(generator.Finalize(&errors));
  }

  // SECAM has no LaserDisc digital audio specification, so a valid pair
  // selection still leaves the EFM path inactive.
  project.output.efm_audio.pair = 0;
  project.cvbs_presets.video_standard_preset = Standard::kSecam;
  {
    AudioTrackGenerator generator;
    std::vector<std::string> errors;
    ASSERT_TRUE(generator.Begin(project, frames, &errors));
    EXPECT_FALSE(generator.efm_active());
    ASSERT_TRUE(generator.EmitFrame(0, &errors));
    ASSERT_TRUE(generator.Finalize(&errors));
  }

  std::filesystem::remove(audio0);
}

// Replays one profile for every section, as the real probe would for a video
// with the given audio streams.
class FakeVideoProbe final : public IProgressiveFrameSourceProbe {
 public:
  ProgressiveFrameSourceProfile profile;
  bool Probe(const Section&, ProgressiveFrameSourceProfile* out_profile,
             std::string*) override {
    *out_profile = profile;
    return true;
  }
};

std::shared_ptr<FakeVideoProbe> MakeVideoProbeWithAudio() {
  auto probe = std::make_shared<FakeVideoProbe>();
  ProgressiveAudioStreamProfile audio;
  audio.channels = 2;
  audio.sample_rate_hz = 48000;
  probe->profile.audio_streams.push_back(audio);
  return probe;
}

AudioChannelPair MakeMediaPair(int pair, AudioPairContent content) {
  AudioChannelPair channel_pair;
  channel_pair.pair = pair;
  channel_pair.pair_specified = true;
  channel_pair.content = content;
  channel_pair.file.path = "narration.wav";
  return channel_pair;
}

// 24-bit left/right samples of a written channel-pair file.
void ReadWavSamples(const std::filesystem::path& path,
                    std::vector<std::int32_t>* left,
                    std::vector<std::int32_t>* right) {
  const std::vector<char> bytes = ReadFileBytes(path);
  left->clear();
  right->clear();
  for (std::size_t offset = 44U; offset + kStereoBytes <= bytes.size();
       offset += kStereoBytes) {
    left->push_back(ReadLe24Signed(bytes, offset));
    right->push_back(ReadLe24Signed(bytes, offset + 3U));
  }
}

TEST(AudioTrackGeneratorTest, FilePairFollowsTheNtscFiveFrameSequence) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_file_ntsc.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_file_ntsc_audio_0.wav");
  testing::FakeAudioLibrary library;
  library.media["narration.wav"] = {0.0, 10.0};

  Project project = MakeProject(video_path);
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  Section section;
  section.name = "Narrated";
  section.audio_channel_pairs = {MakeMediaPair(0, AudioPairContent::kFile)};
  project.sections = {section};
  const std::vector<const Section*> frames(5, &project.sections[0]);

  AudioTrackGenerator generator(nullptr, nullptr,
                                testing::MakeFakeAudioFactory(&library));
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  for (std::size_t k = 0; k < frames.size(); ++k) {
    ASSERT_TRUE(generator.EmitFrame(k, &errors)) << errors.back();
  }
  ASSERT_TRUE(generator.Finalize(&errors));

  // SMPTE 272M-1994 §14.3: five NTSC frames carry 8008 samples, read from the
  // file without gaps or repeats.
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
  ReadWavSamples(audio0, &left, &right);
  ASSERT_EQ(left.size(), 8008U);
  for (std::size_t i = 0; i < left.size(); ++i) {
    ASSERT_EQ(left[i], static_cast<std::int32_t>(i + 1)) << "sample " << i;
    ASSERT_EQ(right[i], -static_cast<std::int32_t>(i + 1)) << "sample " << i;
  }
  EXPECT_EQ(library.opens.size(), 1U);
  std::filesystem::remove(audio0);
}

TEST(AudioTrackGeneratorTest, VideoPairFollowsTheScheduledSourceFrames) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_video_loop.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_video_loop_audio_0.wav");
  testing::FakeAudioLibrary library;
  library.media["clip.mp4"] = {0.0, 60.0};

  Project project = MakeProject(video_path);
  Section section;
  section.name = "Clip";
  section.source = "clip.mp4";
  section.audio_channel_pairs = {MakeMediaPair(0, AudioPairContent::kVideo)};
  project.sections = {section};
  // A range starting at source frame 10 that loops after three frames.
  const Section* clip = &project.sections[0];
  const std::vector<AudioOutputFrame> frames = {
      {clip, 10}, {clip, 11}, {clip, 12}, {clip, 10}};

  AudioTrackGenerator generator(nullptr, MakeVideoProbeWithAudio(),
                                testing::MakeFakeAudioFactory(&library));
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  for (std::size_t k = 0; k < frames.size(); ++k) {
    ASSERT_TRUE(generator.EmitFrame(k, &errors)) << errors.back();
  }
  ASSERT_TRUE(generator.Finalize(&errors));

  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
  ReadWavSamples(audio0, &left, &right);
  ASSERT_EQ(left.size(), 4U * kPalSamplesPerFrame);
  for (std::size_t k = 0; k < frames.size(); ++k) {
    const double seconds = frames[k].source_frame_index / 25.0;
    EXPECT_EQ(left[k * kPalSamplesPerFrame],
              testing::FakeSampleAt(seconds, 48000))
        << "output frame " << k;
  }
  ASSERT_EQ(library.opens.size(), 2U);
  EXPECT_DOUBLE_EQ(library.opens[0].start_seconds, 0.4);
  EXPECT_DOUBLE_EQ(library.opens[1].start_seconds, 0.4);
  std::filesystem::remove(audio0);
}

TEST(AudioTrackGeneratorTest, PulldownVideoPairPlaysFilmInRealTime) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_video_film.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_video_film_audio_0.wav");
  testing::FakeAudioLibrary library;
  library.media["film.mkv"] = {0.0, 60.0};

  Project project = MakeProject(video_path);
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  Section section;
  section.name = "Film";
  section.source = "film.mkv";
  section.cadence.pattern = CadencePattern::kPulldown23;
  section.audio_channel_pairs = {MakeMediaPair(0, AudioPairContent::kVideo)};
  project.sections = {section};
  // Five output frames from film frame 24, two fields apart.
  const Section* film = &project.sections[0];
  const std::vector<AudioOutputFrame> frames = {{film, 24, 0},
                                                {film, 24, 2},
                                                {film, 24, 4},
                                                {film, 24, 6},
                                                {film, 24, 8}};

  AudioTrackGenerator generator(nullptr, MakeVideoProbeWithAudio(),
                                testing::MakeFakeAudioFactory(&library));
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  for (std::size_t k = 0; k < frames.size(); ++k) {
    ASSERT_TRUE(generator.EmitFrame(k, &errors)) << errors.back();
  }
  ASSERT_TRUE(generator.Finalize(&errors));

  // Film frame 24 starts 1.001 s in; the five frames read 8008 samples
  // (SMPTE 272M-1994 §14.3) from there without a break.
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
  ReadWavSamples(audio0, &left, &right);
  ASSERT_EQ(left.size(), 8008U);
  for (std::size_t i = 0; i < left.size(); ++i) {
    ASSERT_EQ(left[i],
              testing::FakeSampleAt(1.001, 48000, static_cast<std::int64_t>(i)))
        << "sample " << i;
  }
  ASSERT_EQ(library.opens.size(), 1U);
  EXPECT_DOUBLE_EQ(library.opens[0].start_seconds, 1.001);
  std::filesystem::remove(audio0);
}

TEST(AudioTrackGeneratorTest, EfmPairReadsItsMediaAgainAt44100Hz) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_media_efm.cvbs");
  const std::filesystem::path audio0 =
      TempPath("videosynth_atg_media_efm_audio_0.wav");
  const std::filesystem::path efm0 = TempPath("videosynth_atg_media_efm.efm");
  testing::FakeAudioLibrary library;
  library.media["clip.mp4"] = {0.0, 60.0};

  Project project = MakeProject(video_path);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 0;
  // Lead-in audio is not muted by the track-1 pause.
  Section section;
  section.name = "Clip";
  section.source = "clip.mp4";
  section.section_type = SectionType::kLeadIn;
  section.audio_channel_pairs = {MakeMediaPair(0, AudioPairContent::kVideo)};
  project.sections = {section};
  const std::vector<AudioOutputFrame> frames = {{&project.sections[0], 50}};

  AudioTrackGenerator generator(nullptr, MakeVideoProbeWithAudio(),
                                testing::MakeFakeAudioFactory(&library));
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  ASSERT_TRUE(generator.efm_active());
  ASSERT_TRUE(generator.EmitFrame(0, &errors)) << errors.back();

  ASSERT_EQ(library.opens.size(), 2U);
  EXPECT_EQ(library.opens[0].sample_rate_hz, 48000);
  EXPECT_EQ(library.opens[1].sample_rate_hz, 44100);
  EXPECT_DOUBLE_EQ(library.opens[1].start_seconds, 2.0);
  // The EFM path carries the 44.1 kHz read, converted to 16 bits.
  ASSERT_EQ(generator.efm_frame_left().size(), 1764U);
  EXPECT_EQ(generator.efm_frame_left().back(),
            ConvertSample24To16(testing::FakeSampleAt(2.0, 44100, 1763)));
  generator.Abort();
  std::filesystem::remove(audio0);
  std::filesystem::remove(efm0);
}

TEST(AudioTrackGeneratorTest, VideoPairWithoutTheAudioStreamFails) {
  const std::filesystem::path video_path =
      TempPath("videosynth_atg_video_nostream.cvbs");
  testing::FakeAudioLibrary library;
  Project project = MakeProject(video_path);
  Section section;
  section.name = "Clip";
  section.source = "clip.mp4";
  AudioChannelPair channel_pair = MakeMediaPair(0, AudioPairContent::kVideo);
  channel_pair.video_audio.stream = 1;  // The probe reports one stream.
  section.audio_channel_pairs = {channel_pair};
  project.sections = {section};
  const std::vector<const Section*> frames = {&project.sections[0]};

  AudioTrackGenerator generator(nullptr, MakeVideoProbeWithAudio(),
                                testing::MakeFakeAudioFactory(&library));
  std::vector<std::string> errors;
  ASSERT_TRUE(generator.Begin(project, frames, &errors));
  EXPECT_FALSE(generator.EmitFrame(0, &errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.back().find("no audio stream 1"), std::string::npos)
      << errors.back();
  generator.Abort();
}

}  // namespace
}  // namespace videosynth
