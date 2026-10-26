/*
 * File:        progressive_frame_source.h
 * Module:      progressive_frame_source
 * Purpose:     Defines fixed-format frame-source buffers for frame-based
 * content.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "videosynth/interfaces.h"
#include "videosynth/model.h"
#include "videosynth/video_frame_decoder.h"

namespace videosynth {

struct YCbCr444Pixel {
  std::int16_t y = 64;
  std::int16_t cb = 512;
  std::int16_t cr = 512;
};

struct FrameSourceImage {
  int width = 0;
  int height = 0;
  int active_x = 0;
  int active_y = 0;
  int active_width = 0;
  int active_height = 0;
  std::vector<YCbCr444Pixel> pixels;

  const YCbCr444Pixel& PixelAt(int x, int y) const;
};

// Source-path families accepted by progressive sections, by extension (case
// insensitive): EXR stills, and MKV/MP4 video clips decoded through ffmpeg.
bool IsProgressiveStillSource(const std::string& source);
bool IsProgressiveVideoSource(const std::string& source);

// Thread-safety: ProgressiveFrameSource IS thread-safe, satisfying the
// IProgressiveFrameProvider contract. GenerateFrame, ResolveFrameCount,
// PrefetchSection, and ClearCache may be called concurrently from multiple
// threads. The cache table is guarded by cache_mutex_, which is held only for
// the lookup; each entry carries its own lock that is held across its decode.
// Two requests for the same source therefore share one decoder, while a decode
// of one source never blocks requests for another — which is what lets a
// prefetch of the next section run alongside the current section's synthesis.
//
// Video sources are streamed, never decoded whole: each cached source keeps a
// bounded window of at most window_capacity decoded frames (least recently
// used evicted first) fed by up to kDecodeCursorsPerSource sequential
// decoders. A request inside the window is a cache hit; one a short way ahead
// of a decoder reads forward; anything else (a loop back to the start, a jump)
// reopens the least recently used decoder half a window before the requested
// frame (but not before the section's start_frame), so workers arriving out of
// order are served by the same decoder. Two cursors
// keep a loop boundary — where workers straddle the end of one pass and the
// start of the next — from reopening a decoder on every request.
//
// Memory bound: kMaxCachedSources × window_capacity decoded frames, each
// width × height × sizeof(YCbCr444Pixel) bytes (~2.4 MiB for a 625-line
// raster), plus frames callers still hold.
//
// Decoded frames are immutable and handed out as shared_ptr<const ...>, so a
// cache hit costs a reference count instead of an image copy and workers may
// keep using a frame after the cache evicts it.
class ProgressiveFrameSource final : public IProgressiveFrameProvider {
 public:
  // Uses ProgressiveFrameSourceProbe, FfmpegVideoFrameDecoder and
  // DefaultWindowCapacity().
  ProgressiveFrameSource();

  // Injects the probe (shared; must be thread-safe), the decoder factory, and
  // the per-source window capacity in frames (clamped to at least 1).
  ProgressiveFrameSource(std::shared_ptr<IProgressiveFrameSourceProbe> probe,
                         VideoFrameDecoderFactory decoder_factory,
                         std::size_t window_capacity);

  // Stops and joins the prefetch thread, if one was started.
  ~ProgressiveFrameSource() override;

  // Owns a worker thread and a decode cache; neither copying nor moving is
  // meaningful.
  ProgressiveFrameSource(const ProgressiveFrameSource&) = delete;
  ProgressiveFrameSource& operator=(const ProgressiveFrameSource&) = delete;
  ProgressiveFrameSource(ProgressiveFrameSource&&) = delete;
  ProgressiveFrameSource& operator=(ProgressiveFrameSource&&) = delete;

  // Window capacity covering the frame-synthesis pool's in-flight span
  // (2 × hardware threads, see FrameSynthesisPool::RunOrdered) plus a margin
  // for the batch path and prefetch.
  static std::size_t DefaultWindowCapacity();

  std::size_t window_capacity() const { return window_capacity_; }

  // Checks if this source supports the given section.
  //
  // Args:
  //   section: The project section to check.
  //
  // Returns:
  //   true if the section is supported, false otherwise.
  bool SupportsSection(const Section& section) const;

  // Requests a background decode of the start of the section's source, so
  // that a later GenerateFrame for that section finds its first frames
  // already decoded instead of stalling every caller at a section boundary.
  //
  // Returns immediately. The request is dropped when the source is already
  // queued or unsupported, and decode failures are ignored here: the real
  // GenerateFrame call repeats the decode and reports the error.
  //
  // Args:
  //   section: The section whose source should be decoded ahead of time.
  //   frame_index: First source frame index that section will request; for a
  //     video source, decoding covers half a window starting there.
  //   standard: The video standard the decode targets.
  void PrefetchSection(const Section& section, int frame_index,
                       Standard standard) const;

  // Clears any cached frame data and closes every decoder.
  void ClearCache() const;

  // Determines the frame count for a section: 1 for a still, the probed
  // frame count for a video (no decode).
  //
  // Ownership: out_frame_count and error are output parameters. The caller owns
  // the pointed-to memory and must ensure the pointers are valid (non-null).
  // The implementation writes to these locations but does not take ownership.
  //
  // Args:
  //   section: The project section.
  //   standard: The video standard.
  //   out_frame_count: Output pointer for the resolved frame count.
  //   error: Output pointer for any error message.
  //
  // Returns:
  //   true on success, false on any error.
  bool ResolveFrameCount(const Section& section, Standard standard,
                         int* out_frame_count, std::string* error) const;

  // Provides a frame of image data for the given section and frame index.
  //
  // Ownership: out_image and error are output parameters. The caller owns
  // the pointed-to memory and must ensure the pointers are valid (non-null).
  // The decoded image is shared with the internal cache and must not be
  // mutated; it stays alive for as long as the caller holds its reference.
  //
  // Args:
  //   section: The project section.
  //   frame_index: Index of the frame to generate.
  //   standard: The video standard.
  //   out_image: Output pointer receiving the shared decoded frame.
  //   error: Output pointer for any error message.
  //
  // Returns:
  //   true on success, false on any error.
  bool GenerateFrame(const Section& section, int frame_index, Standard standard,
                     std::shared_ptr<const FrameSourceImage>* out_image,
                     std::string* error) const override;

 private:
  // Sequential decoders per video source; see the class comment.
  static constexpr std::size_t kDecodeCursorsPerSource = 2;

  // One sequential decoder and the index its next read returns.
  struct DecodeCursor {
    std::unique_ptr<IVideoFrameDecoder> decoder;
    int next_index = 0;
    std::uint64_t last_used = 0;
  };

  // A decoded frame in a video source's window.
  struct WindowFrame {
    std::shared_ptr<const FrameSourceImage> image;
    std::uint64_t last_used = 0;
  };

  // One source held by the cache: an EXR still, or a video's decode window.
  //
  // The entry is created empty under cache_mutex_ and filled under its own
  // decode_mutex, which every reader also holds while copying a frame
  // reference out. Entries are shared_ptr-owned, so an entry evicted from the
  // table stays valid for whoever is still decoding from or reading it.
  struct DecodedSource {
    std::string source;
    Standard standard = Standard::kUnknown;
    // Held across this entry's decode and across every read of the fields
    // below. Never taken while cache_mutex_ is held.
    std::mutex decode_mutex;
    std::shared_ptr<const FrameSourceImage> still;
    // Video sources: probe result and the decode request derived from it.
    bool video_ready = false;
    int frame_count = 0;
    VideoDecodeRequest request;
    std::map<int, WindowFrame> window;
    std::array<DecodeCursor, kDecodeCursorsPerSource> cursors;
    // Monotonic use counter for LRU ordering of window frames and cursors.
    std::uint64_t use_clock = 0;
  };

  using DecodedSourcePtr = std::shared_ptr<DecodedSource>;

  // Cache depth in distinct sources: the section being rendered, the previous
  // section a straddling worker may still be finishing, and the next section
  // a prefetch has decoded ahead of time.
  static constexpr std::size_t kMaxCachedSources = 3;

  // Returns the cache entry for (source, standard), creating an empty one when
  // absent, and promotes it to the front of the most-recently-used order.
  // Complexity: O(kMaxCachedSources). Takes cache_mutex_ internally.
  DecodedSourcePtr AcquireCacheEntry(const std::string& source,
                                     Standard standard) const;

  // Probes a video entry and plans its decode, once. Caller must hold
  // entry->decode_mutex.
  bool EnsureVideoReady(DecodedSource* entry, const Section& section,
                        std::string* error) const;

  // Returns frame_index of a video entry from its window, decoding forward or
  // reopening a cursor as needed. A reopened cursor starts up to half a window
  // before frame_index but never before first_section_frame. Caller must hold
  // entry->decode_mutex.
  // Complexity: O(window_capacity) per decoded frame for LRU eviction.
  bool FetchVideoFrame(DecodedSource* entry, int frame_index,
                       int first_section_frame,
                       std::shared_ptr<const FrameSourceImage>* out_image,
                       std::string* error) const;

  // Inserts a decoded frame into the window, evicting the least recently used
  // frame beyond window_capacity_. Caller must hold entry->decode_mutex.
  void InsertWindowFrame(DecodedSource* entry, int frame_index,
                         std::shared_ptr<const FrameSourceImage> image) const;

  // Decodes the start of the section's source into the cache, ignoring
  // failures. Runs on the prefetch thread.
  void DecodeSectionIntoCache(const Section& section, int frame_index,
                              Standard standard) const;

  // Prefetch thread body: decodes queued requests until the source is
  // destroyed.
  void PrefetchLoop() const;

  std::shared_ptr<IProgressiveFrameSourceProbe> probe_;
  VideoFrameDecoderFactory decoder_factory_;
  std::size_t window_capacity_ = 1;

  // Guards the cache table only; never held across a decode.
  mutable std::mutex cache_mutex_;

  // Most-recently-used first; at most kMaxCachedSources entries.
  mutable std::vector<DecodedSourcePtr> cached_sources_;

  // Guards the prefetch queue and thread lifecycle below.
  mutable std::mutex prefetch_mutex_;
  mutable std::condition_variable prefetch_cv_;
  // Queued prefetches: the section, the first frame index it will request,
  // and the target standard.
  struct PrefetchRequest {
    Section section;
    int frame_index = 0;
    Standard standard = Standard::kUnknown;
  };
  mutable std::vector<PrefetchRequest> prefetch_queue_;
  mutable std::thread prefetch_thread_;
  mutable bool prefetch_stopping_ = false;
};

}  // namespace videosynth