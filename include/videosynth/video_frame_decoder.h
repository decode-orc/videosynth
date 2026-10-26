/*
 * File:        video_frame_decoder.h
 * Module:      video_frame_decoder
 * Purpose:     Sequential video frame decoding into BT.601 4:4:4 frame
 *              images, behind an interface so callers can substitute fakes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/model.h"
#include "videosynth/source_colour_conversion.h"

namespace videosynth {

struct FrameSourceImage;

// What a decoder should open and where it should start.
struct VideoDecodeRequest {
  std::string source;
  Standard standard = Standard::kUnknown;
  // 0-based index of the first frame the next ReadFrame returns.
  int first_frame = 0;
  // Video stream start minus container start, in seconds (see
  // ProgressiveFrameSourceProfile::container_start_time_seconds).
  double video_start_offset_seconds = 0.0;
  // Normalisation to BT.601 yuv422p10le studio swing; empty for pass-through.
  std::string filter_chain;
};

enum class VideoFrameReadStatus {
  kFrame,
  kEndOfStream,
  kError,
};

// Decodes a video source sequentially, one frame per ReadFrame call, starting
// at the frame named in the request.
//
// Thread-safety: implementations are NOT thread-safe. A decoder belongs to a
// single caller (or a caller-held lock); concurrent use of one instance is
// undefined. Distinct instances may run concurrently.
class IVideoFrameDecoder {
 public:
  virtual ~IVideoFrameDecoder() = default;

  // Opens the source, closing any previous stream first. Returns false and
  // sets error when the decode cannot be started.
  virtual bool Open(const VideoDecodeRequest& request, std::string* error) = 0;

  // Writes the next frame into out_image (full raster for the request's
  // standard, active area = whole raster). Returns kEndOfStream after the
  // last frame and kError, with error set, on a decode failure.
  virtual VideoFrameReadStatus ReadFrame(FrameSourceImage* out_image,
                                         std::string* error) = 0;
};

// Creates a fresh, unopened decoder. Must be safe to call from any thread.
using VideoFrameDecoderFactory =
    std::function<std::unique_ptr<IVideoFrameDecoder>()>;

// Unpacks one planar yuv422p10le frame (Y plane, then Cb, then Cr, each
// sample a little-endian 16-bit word with the code in its low 10 bits) into
// a 4:4:4 image. Each colour-difference sample is co-sited with the even luma
// sample of its pair (ITU-R BT.601-5 Annex 1 Table 2 item 3: CR and CB
// co-sited with the 1st, 3rd, 5th... Y samples) and repeated for the odd
// one. Sets out_image's geometry to the full width x height raster.
//
// Returns false (and sets error) when width is odd, a dimension is not
// positive, or sample_count is not exactly 2 * width * height.
// Complexity: O(width * height).
bool UnpackYuv422p10leFrame(const std::uint16_t* samples,
                            std::size_t sample_count, int width, int height,
                            FrameSourceImage* out_image, std::string* error);

// Decodes through an external `ffmpeg` process: one rawvideo pipe held open
// per Open, reading one frame-sized block per ReadFrame. A first_frame > 0
// becomes an input-side `-ss` placed half a frame before the wanted frame;
// ffmpeg decodes from the preceding keyframe and drops frames before that
// time, so the seek is frame-accurate for intra and long-GOP codecs alike.
//
// Thread-safety: NOT thread-safe (see IVideoFrameDecoder).
class FfmpegVideoFrameDecoder final : public IVideoFrameDecoder {
 public:
  FfmpegVideoFrameDecoder() = default;
  ~FfmpegVideoFrameDecoder() override;

  // Owns a child process; neither copying nor moving is meaningful.
  FfmpegVideoFrameDecoder(const FfmpegVideoFrameDecoder&) = delete;
  FfmpegVideoFrameDecoder& operator=(const FfmpegVideoFrameDecoder&) = delete;
  FfmpegVideoFrameDecoder(FfmpegVideoFrameDecoder&&) = delete;
  FfmpegVideoFrameDecoder& operator=(FfmpegVideoFrameDecoder&&) = delete;

  bool Open(const VideoDecodeRequest& request, std::string* error) override;
  VideoFrameReadStatus ReadFrame(FrameSourceImage* out_image,
                                 std::string* error) override;

  // Builds the ffmpeg command line for a request. Exposed for unit tests.
  static std::string BuildCommand(const VideoDecodeRequest& request);

 private:
  // Closes the pipe; returns the child's exit status from pclose.
  int Close();

  FILE* pipe_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  // One frame of planar yuv422p10le samples, reused across reads.
  std::vector<std::uint16_t> frame_samples_;
};

}  // namespace videosynth
