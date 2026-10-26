/*
 * File:        progressive_frame_source.cpp
 * Module:      progressive_frame_source
 * Purpose:     Generates fixed-format 10-bit 4:4:4 BT.601 frame data for
 * frame-based sources.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/progressive_frame_source.h"

#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFloatAttribute.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <OpenEXR/ImfRationalAttribute.h>
#include <OpenEXR/ImfStringAttribute.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/source_colour_conversion.h"

namespace videosynth {

namespace {

namespace Imf = OPENEXR_IMF_NAMESPACE;
namespace ImathNs = IMATH_NAMESPACE;

constexpr int kPalWidth = 720;
constexpr int kPalHeight = 576;
constexpr int kPalActiveX = 0;
constexpr int kPalActiveY = 0;
constexpr int kPalActiveWidth = 720;
constexpr int kPalActiveHeight = 576;

constexpr int kNtscWidth = 720;
constexpr int kNtscHeight = 486;
constexpr int kNtscActiveX = 0;
constexpr int kNtscActiveY = 0;
constexpr int kNtscActiveWidth = 720;
constexpr int kNtscActiveHeight = 486;

int ClampCode(int code, int lo, int hi) {
  return std::max(lo, std::min(code, hi));
}

std::int16_t ClampYCode(int code) {
  return static_cast<std::int16_t>(ClampCode(code, 64, 940));
}

std::int16_t ClampChromaCode(int code) {
  return static_cast<std::int16_t>(ClampCode(code, 64, 960));
}

YCbCr444Pixel MakePixel(int y_code, int cb_code, int cr_code) {
  return YCbCr444Pixel{
      .y = ClampYCode(y_code),
      .cb = ClampChromaCode(cb_code),
      .cr = ClampChromaCode(cr_code),
  };
}

YCbCr444Pixel MakeRawPixel(std::uint16_t y_code, std::uint16_t cb_code,
                           std::uint16_t cr_code) {
  return YCbCr444Pixel{
      .y = static_cast<std::int16_t>(
          ClampCode(static_cast<int>(y_code), 0, 1023)),
      .cb = static_cast<std::int16_t>(
          ClampCode(static_cast<int>(cb_code), 0, 1023)),
      .cr = static_cast<std::int16_t>(
          ClampCode(static_cast<int>(cr_code), 0, 1023)),
  };
}

bool EndsWithLowercase(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
         0;
}

std::string Lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

void SetFrameGeometryForStandard(Standard standard, FrameSourceImage* image) {
  if (Is625LineStandard(standard)) {
    image->width = kPalWidth;
    image->height = kPalHeight;
    image->active_x = kPalActiveX;
    image->active_y = kPalActiveY;
    image->active_width = kPalActiveWidth;
    image->active_height = kPalActiveHeight;
    return;
  }

  image->width = kNtscWidth;
  image->height = kNtscHeight;
  image->active_x = kNtscActiveX;
  image->active_y = kNtscActiveY;
  image->active_width = kNtscActiveWidth;
  image->active_height = kNtscActiveHeight;
}

YCbCr444Pixel ConvertRgbFloatToBt601(std::float_t r, std::float_t g,
                                     std::float_t b) {
  const double r_norm = static_cast<double>(r);
  const double g_norm = static_cast<double>(g);
  const double b_norm = static_cast<double>(b);

  const double y = (0.299 * r_norm) + (0.587 * g_norm) + (0.114 * b_norm);
  const double cb = (-0.168736 * r_norm) - (0.331264 * g_norm) + (0.5 * b_norm);
  const double cr = (0.5 * r_norm) - (0.418688 * g_norm) - (0.081312 * b_norm);

  const int y_code = static_cast<int>(std::lround(64.0 + (876.0 * y)));
  const int cb_code = static_cast<int>(std::lround(512.0 + (896.0 * cb)));
  const int cr_code = static_cast<int>(std::lround(512.0 + (896.0 * cr)));
  return MakeRawPixel(static_cast<std::uint16_t>(ClampCode(y_code, 0, 1023)),
                      static_cast<std::uint16_t>(ClampCode(cb_code, 0, 1023)),
                      static_cast<std::uint16_t>(ClampCode(cr_code, 0, 1023)));
}

bool ValidateExrMetadata(const Imf::Header& header, int width, int height,
                         Standard standard, std::string* error) {
  if (!Is625LineStandard(standard) && standard != Standard::kNtsc &&
      standard != Standard::kPalM) {
    if (error != nullptr) {
      *error = "Unsupported video standard for progressive EXR source.";
    }
    return false;
  }

  const IMATH_NAMESPACE::Box2i data_window = header.dataWindow();
  const IMATH_NAMESPACE::Box2i display_window = header.displayWindow();
  if (data_window.min.x != 0 || data_window.min.y != 0 ||
      data_window.max.x != width - 1 || data_window.max.y != height - 1) {
    if (error != nullptr) {
      *error = "Progressive EXR dataWindow must match full raster bounds.";
    }
    return false;
  }
  if (display_window.min.x != data_window.min.x ||
      display_window.min.y != data_window.min.y ||
      display_window.max.x != data_window.max.x ||
      display_window.max.y != data_window.max.y) {
    if (error != nullptr) {
      *error = "Progressive EXR displayWindow must match dataWindow.";
    }
    return false;
  }

  if (header.compression() != Imf::NO_COMPRESSION) {
    if (error != nullptr) {
      *error = "Progressive EXR must use no compression.";
    }
    return false;
  }

  if (header.lineOrder() != Imf::INCREASING_Y) {
    if (error != nullptr) {
      *error = "Progressive EXR must use increasing-y line order.";
    }
    return false;
  }

  const Imf::FloatAttribute* gamma =
      header.findTypedAttribute<Imf::FloatAttribute>("gamma");
  if (gamma == nullptr || std::abs(gamma->value() - 1.0F) > 1.0e-6F) {
    if (error != nullptr) {
      *error = "Progressive EXR must define gamma metadata equal to 1.";
    }
    return false;
  }

  const float expected_pixel_aspect =
      Is625LineStandard(standard) ? (128.0F / 117.0F) : (108.0F / 119.0F);
  if (std::abs(header.pixelAspectRatio() - expected_pixel_aspect) > 2.0e-3F) {
    if (error != nullptr) {
      *error =
          "Progressive EXR pixelAspectRatio metadata is outside supported "
          "tolerance.";
    }
    return false;
  }

  const Imf::RationalAttribute* fps =
      header.findTypedAttribute<Imf::RationalAttribute>("framesPerSecond");
  if (fps == nullptr) {
    if (error != nullptr) {
      *error = "Progressive EXR is missing framesPerSecond metadata.";
    }
    return false;
  }

  const auto fps_value = fps->value();
  if ((Is625LineStandard(standard) &&
       !(fps_value.n == 25 && fps_value.d == 1)) ||
      ((standard == Standard::kNtsc || standard == Standard::kPalM) &&
       !(fps_value.n == 30000 && fps_value.d == 1001))) {
    if (error != nullptr) {
      *error =
          "Progressive EXR framesPerSecond metadata does not match selected "
          "standard.";
    }
    return false;
  }

  return true;
}

bool LoadExrFrame(const std::string& source, Standard standard,
                  FrameSourceImage* out_image, std::string* error) {
  if (out_image == nullptr) {
    if (error != nullptr) {
      *error = "Frame source output image pointer must not be null.";
    }
    return false;
  }

  // Guard against a missing file before OpenEXR opens it: its C core writes an
  // "Unable to open file for read" line directly to stderr that never reaches
  // the logger. A cheap existence check keeps the failure logged and clean.
  std::error_code exists_ec;
  if (!std::filesystem::exists(source, exists_ec)) {
    if (error != nullptr) {
      *error = "Progressive EXR source not found: " + source;
    }
    return false;
  }

  try {
    Imf::InputFile input_file(source.c_str());
    const Imf::Header& header = input_file.header();
    const ImathNs::Box2i data_window = header.dataWindow();
    const int width = (data_window.max.x - data_window.min.x) + 1;
    const int height = (data_window.max.y - data_window.min.y) + 1;

    const bool is_625_line = Is625LineStandard(standard);
    const int expected_height = is_625_line ? kPalHeight : kNtscHeight;
    if (height != expected_height || width != 720) {
      if (error != nullptr) {
        *error =
            "Progressive EXR raster must be 720x576 for PAL/SECAM and 720x486 "
            "for "
            "NTSC or PAL-M.";
      }
      return false;
    }

    const Imf::ChannelList& channels = header.channels();
    const Imf::Channel* r_channel = channels.findChannel("R");
    const Imf::Channel* g_channel = channels.findChannel("G");
    const Imf::Channel* b_channel = channels.findChannel("B");
    if (r_channel == nullptr || g_channel == nullptr || b_channel == nullptr) {
      if (error != nullptr) {
        *error = "Progressive EXR must provide R, G, and B channels.";
      }
      return false;
    }

    if (r_channel->type != g_channel->type ||
        r_channel->type != b_channel->type) {
      if (error != nullptr) {
        *error =
            "Progressive EXR channels R/G/B must use a single shared channel "
            "type.";
      }
      return false;
    }

    const Imf::PixelType channel_type = r_channel->type;
    if (channel_type != Imf::FLOAT) {
      if (error != nullptr) {
        *error = "Progressive EXR channels R/G/B must use 32-bit FLOAT type.";
      }
      return false;
    }

    if (!ValidateExrMetadata(header, width, height, standard, error)) {
      return false;
    }

    SetFrameGeometryForStandard(standard, out_image);
    out_image->pixels.assign(static_cast<std::size_t>(out_image->width) *
                                 static_cast<std::size_t>(out_image->height),
                             MakePixel(64, 512, 512));

    const int source_x_offset = 0;
    const std::ptrdiff_t pixel_offset =
        static_cast<std::ptrdiff_t>(data_window.min.x) +
        (static_cast<std::ptrdiff_t>(data_window.min.y) * width);
    Imf::FrameBuffer frame_buffer;

    if (channel_type == Imf::FLOAT) {
      std::vector<float> red(
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
          0.0F);
      std::vector<float> green(
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
          0.0F);
      std::vector<float> blue(
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
          0.0F);

      frame_buffer.insert(
          "R", Imf::Slice(Imf::FLOAT,
                          reinterpret_cast<char*>(red.data() - pixel_offset),
                          sizeof(float),
                          static_cast<std::size_t>(sizeof(float) * width)));
      frame_buffer.insert(
          "G", Imf::Slice(Imf::FLOAT,
                          reinterpret_cast<char*>(green.data() - pixel_offset),
                          sizeof(float),
                          static_cast<std::size_t>(sizeof(float) * width)));
      frame_buffer.insert(
          "B", Imf::Slice(Imf::FLOAT,
                          reinterpret_cast<char*>(blue.data() - pixel_offset),
                          sizeof(float),
                          static_cast<std::size_t>(sizeof(float) * width)));

      input_file.setFrameBuffer(frame_buffer);
      input_file.readPixels(data_window.min.y, data_window.max.y);

      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const std::size_t source_index =
              static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
              static_cast<std::size_t>(x);
          const int dst_x = x + source_x_offset;
          out_image->pixels[static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(out_image->width) +
                            static_cast<std::size_t>(dst_x)] =
              ConvertRgbFloatToBt601(red[source_index], green[source_index],
                                     blue[source_index]);
        }
      }
      return true;
    }

    if (error != nullptr) {
      *error =
          "Progressive EXR decode path currently supports FLOAT channels only.";
    }
    return false;
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = std::string("Failed while decoding progressive EXR source: ") +
               ex.what();
    }
    return false;
  }
}

}  // namespace

const YCbCr444Pixel& FrameSourceImage::PixelAt(int x, int y) const {
  return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x)];
}

bool IsProgressiveStillSource(const std::string& source) {
  return EndsWithLowercase(Lowercase(source), ".exr");
}

bool IsProgressiveVideoSource(const std::string& source) {
  const std::string lowered = Lowercase(source);
  return EndsWithLowercase(lowered, ".mkv") ||
         EndsWithLowercase(lowered, ".mp4");
}

ProgressiveFrameSource::ProgressiveFrameSource()
    : ProgressiveFrameSource(
          std::make_shared<ProgressiveFrameSourceProbe>(),
          []() { return std::make_unique<FfmpegVideoFrameDecoder>(); },
          DefaultWindowCapacity()) {}

ProgressiveFrameSource::ProgressiveFrameSource(
    std::shared_ptr<IProgressiveFrameSourceProbe> probe,
    VideoFrameDecoderFactory decoder_factory, std::size_t window_capacity)
    : probe_(std::move(probe)),
      decoder_factory_(std::move(decoder_factory)),
      window_capacity_(std::max<std::size_t>(1U, window_capacity)) {}

// static
std::size_t ProgressiveFrameSource::DefaultWindowCapacity() {
  // Frames the batch path and the prefetch may hold beyond the pool span.
  constexpr std::size_t kWindowMarginFrames = 16;
  // Floor for hosts that report no or very few hardware threads.
  constexpr std::size_t kMinimumWindowFrames = 32;
  const std::size_t threads =
      std::max<std::size_t>(1U, std::thread::hardware_concurrency());
  return std::max(kMinimumWindowFrames, (2U * threads) + kWindowMarginFrames);
}

bool ProgressiveFrameSource::SupportsSection(const Section& section) const {
  if (section.type != "progressive" || section.source.empty()) {
    return false;
  }
  return IsProgressiveStillSource(section.source) ||
         IsProgressiveVideoSource(section.source);
}

ProgressiveFrameSource::~ProgressiveFrameSource() {
  {
    const std::lock_guard<std::mutex> lock(prefetch_mutex_);
    prefetch_stopping_ = true;
    prefetch_queue_.clear();
  }
  prefetch_cv_.notify_all();
  if (prefetch_thread_.joinable()) {
    prefetch_thread_.join();
  }
}

void ProgressiveFrameSource::ClearCache() const {
  const std::lock_guard<std::mutex> lock(cache_mutex_);
  cached_sources_.clear();
}

ProgressiveFrameSource::DecodedSourcePtr
ProgressiveFrameSource::AcquireCacheEntry(const std::string& source,
                                          Standard standard) const {
  const std::lock_guard<std::mutex> lock(cache_mutex_);
  for (std::size_t i = 0; i < cached_sources_.size(); ++i) {
    if (cached_sources_[i]->source != source ||
        cached_sources_[i]->standard != standard) {
      continue;
    }
    if (i != 0) {
      std::rotate(cached_sources_.begin(),
                  cached_sources_.begin() + static_cast<std::ptrdiff_t>(i),
                  cached_sources_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    }
    return cached_sources_.front();
  }

  auto entry = std::make_shared<DecodedSource>();
  entry->source = source;
  entry->standard = standard;
  // Evicting only drops the table's reference: a thread that is decoding from
  // or reading the evicted entry keeps it alive through its own reference.
  if (cached_sources_.size() >= kMaxCachedSources) {
    cached_sources_.resize(kMaxCachedSources - 1U);
  }
  cached_sources_.insert(cached_sources_.begin(), entry);
  return entry;
}

void ProgressiveFrameSource::PrefetchSection(const Section& section,
                                             int frame_index,
                                             Standard standard) const {
  if (!SupportsSection(section)) {
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(prefetch_mutex_);
    if (prefetch_stopping_) {
      return;
    }
    for (const PrefetchRequest& queued : prefetch_queue_) {
      if (queued.section.source == section.source &&
          queued.standard == standard) {
        return;
      }
    }
    prefetch_queue_.push_back(PrefetchRequest{section, frame_index, standard});
    if (!prefetch_thread_.joinable()) {
      prefetch_thread_ = std::thread([this]() { PrefetchLoop(); });
    }
  }
  prefetch_cv_.notify_one();
}

void ProgressiveFrameSource::PrefetchLoop() const {
  for (;;) {
    PrefetchRequest request;
    {
      std::unique_lock<std::mutex> lock(prefetch_mutex_);
      prefetch_cv_.wait(lock, [this]() {
        return prefetch_stopping_ || !prefetch_queue_.empty();
      });
      if (prefetch_stopping_) {
        return;
      }
      request = prefetch_queue_.front();
      prefetch_queue_.erase(prefetch_queue_.begin());
    }
    DecodeSectionIntoCache(request.section, request.frame_index,
                           request.standard);
  }
}

void ProgressiveFrameSource::DecodeSectionIntoCache(const Section& section,
                                                    int frame_index,
                                                    Standard standard) const {
  // A prefetch is an optimisation, so failures are dropped here; the frame
  // request that follows repeats the decode and reports the error. Each frame
  // is a separate request, so the entry lock is released between frames and
  // a worker that reaches the section early is served as soon as its frame
  // is decoded rather than after the whole prefetch.
  const int prefetch_frames = IsProgressiveVideoSource(section.source)
                                  ? static_cast<int>(window_capacity_ / 2U)
                                  : 1;
  for (int i = 0; i < std::max(1, prefetch_frames); ++i) {
    {
      const std::lock_guard<std::mutex> lock(prefetch_mutex_);
      if (prefetch_stopping_) {
        return;
      }
    }
    std::shared_ptr<const FrameSourceImage> image;
    std::string ignored_error;
    if (!GenerateFrame(section, frame_index + i, standard, &image,
                       &ignored_error)) {
      return;
    }
  }
}

bool ProgressiveFrameSource::EnsureVideoReady(DecodedSource* entry,
                                              const Section& section,
                                              std::string* error) const {
  if (entry->video_ready) {
    return true;
  }
  if (probe_ == nullptr) {
    if (error != nullptr) {
      *error = "Progressive video source has no probe configured.";
    }
    return false;
  }

  ProgressiveFrameSourceProfile profile;
  std::string probe_error;
  if (!probe_->Probe(section, &profile, &probe_error)) {
    if (error != nullptr) {
      *error = probe_error.empty() ? "Failed to probe progressive video source."
                                   : probe_error;
    }
    return false;
  }

  FrameSourceImage geometry;
  SetFrameGeometryForStandard(entry->standard, &geometry);
  if (profile.width != geometry.width || profile.height != geometry.height) {
    if (error != nullptr) {
      *error =
          "Progressive video raster must be 720x576 for PAL/SECAM and 720x486 "
          "for NTSC or PAL-M.";
    }
    return false;
  }
  if (profile.frame_count <= 0) {
    if (error != nullptr) {
      *error = "Progressive video source does not contain decodable frames.";
    }
    return false;
  }

  entry->frame_count = profile.frame_count;
  entry->request.source = entry->source;
  entry->request.standard = entry->standard;
  entry->request.video_start_offset_seconds =
      std::max(0.0, profile.video_start_time_seconds -
                        profile.container_start_time_seconds);
  entry->request.filter_chain =
      PlanSourceColourConversion(profile, entry->standard).filter_chain;
  entry->video_ready = true;
  return true;
}

void ProgressiveFrameSource::InsertWindowFrame(
    DecodedSource* entry, int frame_index,
    std::shared_ptr<const FrameSourceImage> image) const {
  entry->window[frame_index] =
      WindowFrame{std::move(image), ++entry->use_clock};
  while (entry->window.size() > window_capacity_) {
    auto oldest = entry->window.begin();
    for (auto it = entry->window.begin(); it != entry->window.end(); ++it) {
      if (it->second.last_used < oldest->second.last_used) {
        oldest = it;
      }
    }
    entry->window.erase(oldest);
  }
}

bool ProgressiveFrameSource::FetchVideoFrame(
    DecodedSource* entry, int frame_index, int first_section_frame,
    std::shared_ptr<const FrameSourceImage>* out_image,
    std::string* error) const {
  const auto cached = entry->window.find(frame_index);
  if (cached != entry->window.end()) {
    cached->second.last_used = ++entry->use_clock;
    *out_image = cached->second.image;
    return true;
  }

  // Prefer the cursor that reaches the frame with the least forward reading,
  // as long as that reading stays within one window.
  DecodeCursor* cursor = nullptr;
  for (DecodeCursor& candidate : entry->cursors) {
    if (candidate.decoder == nullptr || candidate.next_index > frame_index ||
        static_cast<std::size_t>(frame_index - candidate.next_index) >=
            window_capacity_) {
      continue;
    }
    if (cursor == nullptr || candidate.next_index > cursor->next_index) {
      cursor = &candidate;
    }
  }

  if (cursor == nullptr) {
    // Reopen the least recently used cursor (an unused one first) at the
    // requested frame.
    cursor = &entry->cursors.front();
    for (DecodeCursor& candidate : entry->cursors) {
      if (candidate.decoder == nullptr) {
        cursor = &candidate;
        break;
      }
      if (candidate.last_used < cursor->last_used) {
        cursor = &candidate;
      }
    }
    if (!decoder_factory_) {
      if (error != nullptr) {
        *error = "Progressive video source has no decoder configured.";
      }
      return false;
    }
    // Open half a window early (never before the section's first frame):
    // concurrent workers reach a section start or a jump slightly out of
    // order, and the frames just behind the first request are about to be
    // asked for too. Decoding them now costs a few reads; missing them would
    // cost a reopen each.
    const int lookback = static_cast<int>(window_capacity_ / 2U);
    const int open_index = std::max(std::min(first_section_frame, frame_index),
                                    frame_index - lookback);
    cursor->decoder = decoder_factory_();
    VideoDecodeRequest request = entry->request;
    request.first_frame = open_index;
    std::string open_error;
    if (cursor->decoder == nullptr ||
        !cursor->decoder->Open(request, &open_error)) {
      cursor->decoder.reset();
      if (error != nullptr) {
        *error = open_error.empty() ? "Failed to open progressive video source."
                                    : open_error;
      }
      return false;
    }
    cursor->next_index = open_index;
  }
  cursor->last_used = ++entry->use_clock;

  while (cursor->next_index <= frame_index) {
    auto image = std::make_shared<FrameSourceImage>();
    std::string read_error;
    const VideoFrameReadStatus status =
        cursor->decoder->ReadFrame(image.get(), &read_error);
    if (status != VideoFrameReadStatus::kFrame) {
      cursor->decoder.reset();
      if (error != nullptr) {
        if (status == VideoFrameReadStatus::kError) {
          *error = read_error.empty()
                       ? "Failed to decode progressive video source."
                       : read_error;
        } else {
          *error = "Progressive video source ended at frame " +
                   std::to_string(cursor->next_index) +
                   ", before the probed frame count of " +
                   std::to_string(entry->frame_count) + ".";
        }
      }
      return false;
    }
    const int decoded_index = cursor->next_index++;
    if (decoded_index == frame_index) {
      *out_image = image;
    }
    InsertWindowFrame(entry, decoded_index, std::move(image));
  }
  return true;
}

bool ProgressiveFrameSource::ResolveFrameCount(const Section& section,
                                               Standard standard,
                                               int* out_frame_count,
                                               std::string* error) const {
  if (out_frame_count == nullptr) {
    if (error != nullptr) {
      *error = "Progressive frame count output pointer must not be null.";
    }
    return false;
  }

  if (!SupportsSection(section)) {
    if (error != nullptr) {
      *error = "Progressive section source family is not supported.";
    }
    return false;
  }

  if (IsProgressiveStillSource(section.source)) {
    *out_frame_count = 1;
    return true;
  }

  const DecodedSourcePtr entry = AcquireCacheEntry(section.source, standard);
  const std::lock_guard<std::mutex> decode_lock(entry->decode_mutex);
  if (!EnsureVideoReady(entry.get(), section, error)) {
    return false;
  }
  *out_frame_count = entry->frame_count;
  return true;
}

bool ProgressiveFrameSource::GenerateFrame(
    const Section& section, int frame_index, Standard standard,
    std::shared_ptr<const FrameSourceImage>* out_image,
    std::string* error) const {
  if (!SupportsSection(section)) {
    if (error != nullptr) {
      *error = "Progressive section source family is not supported.";
    }
    return false;
  }

  if (out_image == nullptr) {
    if (error != nullptr) {
      *error = "Frame source output image pointer must not be null.";
    }
    return false;
  }

  if (IsProgressiveStillSource(section.source)) {
    const DecodedSourcePtr entry = AcquireCacheEntry(section.source, standard);
    const std::lock_guard<std::mutex> decode_lock(entry->decode_mutex);
    if (entry->still == nullptr) {
      auto decoded = std::make_shared<FrameSourceImage>();
      std::string decode_error;
      if (!LoadExrFrame(section.source, standard, decoded.get(),
                        &decode_error)) {
        if (error != nullptr) {
          *error = decode_error.empty()
                       ? "Failed to decode progressive EXR source."
                       : decode_error;
        }
        return false;
      }
      entry->still = std::move(decoded);
    }

    *out_image = entry->still;
    return true;
  }

  if (frame_index < 0) {
    if (error != nullptr) {
      *error = "Progressive frame index must be non-negative.";
    }
    return false;
  }

  const DecodedSourcePtr entry = AcquireCacheEntry(section.source, standard);
  const std::lock_guard<std::mutex> decode_lock(entry->decode_mutex);
  if (!EnsureVideoReady(entry.get(), section, error)) {
    return false;
  }
  if (frame_index >= entry->frame_count) {
    if (error != nullptr) {
      *error =
          "Requested frame index exceeds progressive video source length (" +
          std::to_string(entry->frame_count) + " frames).";
    }
    return false;
  }
  return FetchVideoFrame(entry.get(), frame_index,
                         std::max(0, section.start_frame), out_image, error);
}

}  // namespace videosynth
