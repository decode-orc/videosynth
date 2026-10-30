/*
 * File:        audio_track_generator.h
 * Module:      audio_track_generator
 * Purpose:     Orchestrates synthesis of up to eight stereo audio channel pairs
 *              frame-locked to the output video stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/audio_efm_writer.h"
#include "videosynth/audio_media_reader.h"
#include "videosynth/audio_sample_source.h"
#include "videosynth/audio_synthesizer.h"
#include "videosynth/audio_wav_writer.h"
#include "videosynth/efm_track_layout.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"

namespace videosynth {

// One stored output frame as the audio tracks see it: the section it belongs
// to and where its time span starts in the source, the start of
// `source_frame_index` plus `fields_after` output fields.
struct AudioOutputFrame {
  const Section* section = nullptr;
  int source_frame_index = 0;
  std::int64_t fields_after = 0;
};

// The audio view of `schedule`, index for index: a video-cadence frame starts
// at the source frame it shows, a 2:3 pulldown frame at its source time
// (FrameScheduleItem::source_time).
//
// Thread-safety: thread-safe (pure function).
std::vector<AudioOutputFrame> AudioOutputFramesForSchedule(
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule);

// Generates every audio channel pair declared by a project and streams each as
// a separate stereo 24-bit RIFF/WAVE file (`_audio_<pair>.wav`) frame-locked to
// the output video stream, per the CVBS File Format Specification (Audio Data).
//
// When the project selects one pair for LaserDisc digital audio
// (`output.efm_audio`), that pair is additionally synthesised on the 44.1 kHz
// grid of IEC 60856/60857:1986 Amd 2, 13.2 and streamed to a `.efm` T-value
// file (plus its `.efm.meta` frame-index sidecar) by an AudioEfmWriter. The WAV
// file of that pair is unchanged.
//
// The set of emitted channel pairs is the union of the pair numbers declared
// across all sections (ProjectAudioChannelPairs). Every pair file spans the
// whole output; sections that omit a pair emit silence for their frames, and a
// channel with `enabled == false` within a declared pair emits all-zero silence
// (SMPTE 272M §6.4).
//
// Frame k carries AudioSamplesForFrame(standard, k) samples per channel
// (SMPTE 272M §14.3) whatever its content. Each contiguous run of output frames
// sharing one section starts afresh: tone oscillators reset their phase, an
// audio-file pair plays its file from the start, and a video-soundtrack pair
// follows the source time of each frame (AudioMediaReader). The caller emits
// output frames strictly in output order.
//
// Usage: Begin(project, output_frames) opens the writers and computes
// the per-frame plan; EmitFrame(k) must then be called for k = 0, 1, ... in
// increasing output order; Finalize() closes the files. Abort() discards a
// partially-written set (e.g. on cancellation).
//
// Thread-safety: NOT thread-safe. Its synthesiser and writer state is mutated
// by EmitFrame; it must not be called concurrently from multiple threads.
class AudioTrackGenerator {
 public:
  // The probe supplies a video's soundtrack timing and the factory reads
  // file and video audio; either may be null to use the ffprobe/ffmpeg
  // implementations. Neither is used by a tone-only project.
  explicit AudioTrackGenerator(
      ILogger* logger = nullptr,
      std::shared_ptr<IProgressiveFrameSourceProbe> probe = nullptr,
      AudioSampleSourceFactory sample_source_factory = nullptr);

  // Prepares the plan and opens one WAV writer per declared channel pair.
  // output_frames lists, in output order, the section and source time of
  // each stored output frame (its size is the total output frame count).
  // Returns false and appends messages to errors on any failure; on failure
  // any opened writers are aborted. When the project declares no audio,
  // Begin() succeeds, active() stays false, and EmitFrame/Finalize are
  // no-ops.
  bool Begin(const Project& project,
             const std::vector<AudioOutputFrame>& output_frames,
             std::vector<std::string>* errors);

  // As above for callers without a schedule: each section run is taken to
  // show source frames 0, 1, 2, ... in order.
  bool Begin(const Project& project,
             const std::vector<const Section*>& output_frame_sections,
             std::vector<std::string>* errors);

  // Synthesises and appends output frame `output_index` (0-based) across all
  // channel pairs. Must be called once per output frame in increasing order.
  // Returns false and appends to errors on any writer failure or if called out
  // of range.
  bool EmitFrame(std::size_t output_index, std::vector<std::string>* errors);

  // Closes every channel-pair file, back-patching its RIFF sizes.
  bool Finalize(std::vector<std::string>* errors);

  // Discards every in-progress channel-pair file. No-op when inactive.
  void Abort();

  // True when at least one channel pair is being written.
  bool active() const { return active_; }

  // Channel-pair numbers being written, sorted ascending.
  const std::vector<int>& channel_pairs() const { return pair_numbers_; }

  // True when the project selects a declared channel pair for LaserDisc
  // digital audio (EFM) output on a standard that defines it, so the second
  // 44.1 kHz synthesis path is running alongside the 48 kHz WAV path.
  bool efm_active() const { return efm_active_; }

  // Channel-pair number carrying EFM output; meaningful only when
  // efm_active() is true.
  int efm_pair() const { return efm_pair_; }

  // Number of 44.1 kHz EFM samples per channel carried by output frame
  // `output_index` (IEC 60856/60857 Amd 2 13.2, SMPTE 272M-1994 Table 1).
  // Zero when EFM output is inactive or the index is out of range.
  int efm_samples_for_frame(std::size_t output_index) const;

  // Subcode layout of the EFM stream, valid after a successful Begin(). Its
  // track table is empty when EFM output is inactive.
  const EfmTrackLayout& efm_layout() const { return efm_layout_; }

  // 16-bit 44.1 kHz samples written to the EFM stream for the most recent
  // EmitFrame() call (IEC 60908-1999 clause 12 sample format), including the
  // digital silence of the track-1 pause. Empty until the first frame is
  // emitted, and empty throughout when EFM output is inactive.
  const std::vector<std::int16_t>& efm_frame_left() const {
    return efm_frame_left_;
  }
  const std::vector<std::int16_t>& efm_frame_right() const {
    return efm_frame_right_;
  }

 private:
  // Per channel pair: its number, WAV writer, the two channel synthesisers
  // and the media reader. The EFM-selected pair additionally owns a second
  // set running on the 44.1 kHz grid; they are null for every other pair.
  struct PairState {
    int pair = 0;
    std::unique_ptr<AudioWavWriter> writer;
    std::unique_ptr<AudioSynthesizer> left;
    std::unique_ptr<AudioSynthesizer> right;
    std::unique_ptr<AudioMediaReader> media;
    std::unique_ptr<AudioSynthesizer> efm_left;
    std::unique_ptr<AudioSynthesizer> efm_right;
    std::unique_ptr<AudioMediaReader> efm_media;
    // True while the current run reads file or video audio rather than
    // synthesising tones.
    bool media_run = false;
  };

  // Reconfigures every pair for the section run that starts at output frame
  // `output_index`. Returns false and appends to errors when a video
  // soundtrack cannot be located.
  bool BeginRun(std::size_t output_index, std::vector<std::string>* errors);

  // Reads one frame of a pair's media audio into left/right.
  bool ReadMediaFrame(AudioMediaReader* reader, int sample_count,
                      std::size_t output_index, std::vector<std::int32_t>* left,
                      std::vector<std::int32_t>* right,
                      std::vector<std::string>* errors);

  // Zeroes the samples of one output frame that fall in the mandatory pause
  // preceding track 1 (IEC 60908-1999, 17.5.1). The buffers hold the samples
  // starting at stream position efm_sample_position_.
  void MuteTrackOnePause(std::vector<std::int32_t>* left,
                         std::vector<std::int32_t>* right);

  ILogger* logger_;
  std::shared_ptr<IProgressiveFrameSourceProbe> probe_;
  AudioSampleSourceFactory sample_source_factory_;
  bool active_ = false;
  Standard standard_ = Standard::kUnknown;
  std::vector<int> pair_numbers_;
  std::vector<PairState> pairs_;

  // Per-output-frame plan (indexed by output frame).
  std::vector<const Section*> frame_sections_;
  std::vector<int> frame_source_indices_;
  std::vector<std::int64_t> frame_source_fields_;
  std::vector<int> frame_sample_counts_;
  std::vector<char> is_run_start_;
  std::vector<std::int64_t> run_total_samples_;

  // Parallel 44.1 kHz plan for the EFM-selected pair, empty when inactive.
  bool efm_active_ = false;
  int efm_pair_ = 0;
  std::vector<int> efm_frame_sample_counts_;
  std::vector<std::int64_t> efm_run_total_samples_;
  std::vector<std::int16_t> efm_frame_left_;
  std::vector<std::int16_t> efm_frame_right_;
  EfmTrackLayout efm_layout_;
  std::unique_ptr<AudioEfmWriter> efm_writer_;
  // 44.1 kHz sampling periods appended to the EFM stream so far, i.e. the
  // stream position of the next appended sample.
  std::size_t efm_sample_position_ = 0;
};

}  // namespace videosynth
