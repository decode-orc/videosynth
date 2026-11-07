/*
 * File:        audio_efm_writer.h
 * Module:      audio_efm_writer
 * Purpose:     Streams one audio channel pair to a LaserDisc digital audio
 *              (EFM) T-value file alongside the generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/efm/efm_stream_encoder.h"
#include "videosynth/efm/subcode_generator.h"
#include "videosynth/efm_stream_file.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"

namespace videosynth {

// Writes a single audio channel pair as a LaserDisc digital audio channel
// stream next to the CVBS output, as the two-file EFM extension defined by the
// CVBS File Format Specification (extensions/efm-extension-format.md): the
// binary stream `<basename>.efm` plus its own SQLite sidecar
// `<basename>.efm.meta` holding the per-frame index. The binary file holds one
// unsigned byte per pit or land run length (T3 to T11, IEC 60908-1999, clause
// 13), starting at the first frame sync, and this is the only core component
// that drives the EFM module.
//
// Each byte packs its run length with the producer's doubt about it, per
// efm/t_value_byte.h. The stream is synthesised, so every run length is written
// at kSynthesisedDoubt (zero doubt, maximum confidence) and the bytes are the
// plain run lengths; see that header before emitting anything less certain.
//
// The extension is self-describing: no EFM metadata is written into the core
// `<basename>.meta` database, which has no EFM concept.
//
// The first appended sample is synchronous with the first stored video frame;
// callers must append exactly the per-frame sample count
// (EfmAudioSamplesForFrame) per stored output frame, at 44 100 Hz
// (IEC 60856:1986 Amd 2, 13.2 / IEC 60857:1986 Amd 2, 13.2), so the encoded
// stream stays frame-locked. Samples arrive in the 24-bit domain of
// AudioSynthesizer and are converted to the 16-bit CD sample domain here. Left
// and right buffers must be the same length.
//
// Streaming contract: BeginWrite (opens the file and adopts the track table) ->
// repeated AppendFrameAudio -> FinalizeWrite (flushes the CIRC pipeline, closes
// the file and writes the sidecar). Both paths are derived from
// output.video_path by stripping a trailing ".cvbs" or ".cvbsy" suffix and
// appending ".efm" / ".efm.meta"; the extension requires the sidecar basename
// to match the CVBS basename, so the channel pair is not part of the name.
//
// Each AppendFrameAudio call contributes one efm_frame row: the CIRC pipeline
// latency means a frame's t-values are not the encoding of that frame's own
// samples, but the index stays contiguous and covers the whole file. The
// residue flushed by FinalizeWrite is attributed to the final frame so that the
// row set accounts for every byte written.
//
// Thread-safety: AudioEfmWriter is NOT thread-safe. Its file stream and encoder
// state are mutated by AppendFrameAudio; it must not be called concurrently
// from multiple threads.
class AudioEfmWriter {
 public:
  explicit AudioEfmWriter(ILogger* logger = nullptr);

  // Opens the derived EFM path for channel pair `channel_pair` (0-7) and
  // adopts `track_table` as the subcode layout of the stream. Creates parent
  // directories as needed. Returns false and appends a message to errors on any
  // failure, including a track table the EFM module rejects.
  bool BeginWrite(const Project& project, int channel_pair,
                  const efm::TrackTable& track_table,
                  std::vector<std::string>* errors);

  // Appends one video frame worth of 24-bit stereo samples. left and right must
  // be the same length. Returns false and appends a message to errors if the
  // session is not open, the buffers differ in length, or the stream fails.
  bool AppendFrameAudio(const std::vector<std::int32_t>& left,
                        const std::vector<std::int32_t>& right,
                        std::vector<std::string>* errors);

  // Flushes the encoder pipeline, writes the remaining T values, closes the
  // binary file and writes the `<basename>.efm.meta` sidecar. Returns false and
  // appends a message to errors on any failure.
  bool FinalizeWrite(std::vector<std::string>* errors);

  // Abandons the current write session: closes the stream and removes the
  // partially-written EFM file and any sidecar left by an earlier run. No-op
  // when no session is open.
  void AbortWrite();

  // Derives the EFM binary path from a CVBS output path: strips a trailing
  // ".cvbs" or ".cvbsy" suffix (if present) and appends ".efm".
  static std::string DeriveAudioPath(const std::string& video_path) {
    return EfmStreamFile::DeriveEfmPath(video_path);
  }

  // Derives the EFM sidecar path from a CVBS output path: as DeriveAudioPath
  // but appending ".efm.meta".
  static std::string DeriveSidecarPath(const std::string& video_path) {
    return EfmStreamFile::DeriveSidecarPath(video_path);
  }

 private:
  ILogger* logger_;
  EfmStreamFile file_;
  efm::EfmStreamEncoder encoder_;
};

}  // namespace videosynth
