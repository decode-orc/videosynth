/*
 * File:        video_frame_decoder.cpp
 * Module:      video_frame_decoder
 * Purpose:     Sequential video frame decoding into BT.601 4:4:4 frame
 *              images through an external ffmpeg process.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/video_frame_decoder.h"

#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>

#include "videosynth/progressive_frame_source.h"

namespace videosynth {
namespace {

// The decoded sample word carries a 10-bit code in its low bits.
constexpr std::uint16_t kTenBitCodeMask = 0x03FFu;

// Half a frame period: an input seek placed this far before the wanted frame
// lands between it and its predecessor, so timestamp rounding in the container
// time base (1 ms for Matroska) can neither drop the wanted frame nor keep the
// one before it.
constexpr double kSeekLeadFrames = 0.5;

std::string EscapeForSingleQuotedShell(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char c : value) {
    if (c == '\'') {
      escaped += "'\\''";
      continue;
    }
    escaped.push_back(c);
  }
  return escaped;
}

void RasterForStandard(Standard standard, int* width, int* height) {
  // ITU-R BT.601-5 Annex 1 Table 2: 720 active samples per line; 576 active
  // lines for 625-line systems and 486 for 525-line systems (SMPTE 125M).
  *width = 720;
  *height = Is625LineStandard(standard) ? 576 : 486;
}

double FrameRateForStandard(Standard standard) {
  return Is625LineStandard(standard) ? 25.0 : (30000.0 / 1001.0);
}

}  // namespace

bool UnpackYuv422p10leFrame(const std::uint16_t* samples,
                            std::size_t sample_count, int width, int height,
                            FrameSourceImage* out_image, std::string* error) {
  if (out_image == nullptr || samples == nullptr || width <= 0 || height <= 0 ||
      (width % 2) != 0) {
    if (error != nullptr) {
      *error = "Invalid yuv422p10le frame geometry for unpacking.";
    }
    return false;
  }

  const std::size_t luma_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const std::size_t chroma_width = static_cast<std::size_t>(width / 2);
  const std::size_t chroma_count =
      chroma_width * static_cast<std::size_t>(height);
  if (sample_count != luma_count + (2U * chroma_count)) {
    if (error != nullptr) {
      *error = "yuv422p10le frame payload size does not match its raster.";
    }
    return false;
  }

  out_image->width = width;
  out_image->height = height;
  out_image->active_x = 0;
  out_image->active_y = 0;
  out_image->active_width = width;
  out_image->active_height = height;
  out_image->pixels.resize(luma_count);

  const std::uint16_t* y_plane = samples;
  const std::uint16_t* cb_plane = y_plane + luma_count;
  const std::uint16_t* cr_plane = cb_plane + chroma_count;
  for (int y = 0; y < height; ++y) {
    const std::size_t row = static_cast<std::size_t>(y);
    const std::uint16_t* y_row =
        y_plane + (row * static_cast<std::size_t>(width));
    const std::uint16_t* cb_row = cb_plane + (row * chroma_width);
    const std::uint16_t* cr_row = cr_plane + (row * chroma_width);
    YCbCr444Pixel* out_row =
        out_image->pixels.data() + (row * static_cast<std::size_t>(width));
    for (int x = 0; x < width; ++x) {
      const std::size_t chroma_x = static_cast<std::size_t>(x / 2);
      out_row[x] = YCbCr444Pixel{
          .y = static_cast<std::int16_t>(y_row[x] & kTenBitCodeMask),
          .cb = static_cast<std::int16_t>(cb_row[chroma_x] & kTenBitCodeMask),
          .cr = static_cast<std::int16_t>(cr_row[chroma_x] & kTenBitCodeMask),
      };
    }
  }
  return true;
}

FfmpegVideoFrameDecoder::~FfmpegVideoFrameDecoder() { Close(); }

int FfmpegVideoFrameDecoder::Close() {
  if (pipe_ == nullptr) {
    return 0;
  }
  // Closing the read end first makes a still-running ffmpeg fail its next
  // write and exit, so pclose does not wait for a whole-file decode.
  const int status = pclose(pipe_);
  pipe_ = nullptr;
  return status;
}

// static
std::string FfmpegVideoFrameDecoder::BuildCommand(
    const VideoDecodeRequest& request) {
  std::ostringstream command;
  command << "ffmpeg -v error -nostdin";
  if (request.first_frame > 0) {
    const double seek_seconds =
        request.video_start_offset_seconds +
        ((static_cast<double>(request.first_frame) - kSeekLeadFrames) /
         FrameRateForStandard(request.standard));
    command << " -ss " << std::fixed << std::setprecision(6) << seek_seconds;
  }
  command << " -i '" << EscapeForSingleQuotedShell(request.source) << "'"
          << " -map 0:v:0 -an -sn -dn";
  if (!request.filter_chain.empty()) {
    command << " -vf '" << EscapeForSingleQuotedShell(request.filter_chain)
            << "'";
  }
  command << " -pix_fmt yuv422p10le -fps_mode passthrough -f rawvideo -"
          << " 2>/dev/null";
  return command.str();
}

bool FfmpegVideoFrameDecoder::Open(const VideoDecodeRequest& request,
                                   std::string* error) {
  Close();
  if (request.first_frame < 0) {
    if (error != nullptr) {
      *error = "Video decode start frame must be non-negative.";
    }
    return false;
  }

  RasterForStandard(request.standard, &width_, &height_);
  frame_samples_.resize(static_cast<std::size_t>(width_) *
                        static_cast<std::size_t>(height_) * 2U);

  const std::string command = BuildCommand(request);
  pipe_ = popen(command.c_str(), "r");
  if (pipe_ == nullptr) {
    if (error != nullptr) {
      *error = "Unable to run ffmpeg for video source decoding.";
    }
    return false;
  }
  return true;
}

VideoFrameReadStatus FfmpegVideoFrameDecoder::ReadFrame(
    FrameSourceImage* out_image, std::string* error) {
  if (pipe_ == nullptr) {
    if (error != nullptr) {
      *error = "Video decoder read before a successful open.";
    }
    return VideoFrameReadStatus::kError;
  }

  const std::size_t wanted = frame_samples_.size();
  const std::size_t read =
      std::fread(frame_samples_.data(), sizeof(std::uint16_t), wanted, pipe_);
  if (read == 0U) {
    // A clean end of stream needs ffmpeg itself to have succeeded; a non-zero
    // status means the file failed to decode rather than simply ended.
    const int status = Close();
    if (status != 0) {
      if (error != nullptr) {
        *error = "ffmpeg failed while decoding video source.";
      }
      return VideoFrameReadStatus::kError;
    }
    return VideoFrameReadStatus::kEndOfStream;
  }
  if (read != wanted) {
    Close();
    if (error != nullptr) {
      *error =
          "Decoded video payload size is not aligned to yuv422p10le frame "
          "boundaries.";
    }
    return VideoFrameReadStatus::kError;
  }

  if (!UnpackYuv422p10leFrame(frame_samples_.data(), frame_samples_.size(),
                              width_, height_, out_image, error)) {
    return VideoFrameReadStatus::kError;
  }
  return VideoFrameReadStatus::kFrame;
}

}  // namespace videosynth
