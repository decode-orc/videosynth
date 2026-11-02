/*
 * File:        frame_synthesis_pool.h
 * Module:      frame_synthesis_pool
 * Purpose:     Worker-thread pool that synthesises frames out of order and an
 *              ordered reassembly buffer that delivers results in frame order
 *              with bounded memory.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "videosynth/dropout_injection_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/interfaces.h"

namespace videosynth {

// Result of synthesising one frame on a pool worker.
struct SynthesizedFrame {
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  // Output sample codes for this frame, encoded on the worker so the consumer
  // thread only issues the write. Empty when the job does not encode.
  EncodedFrame encoded;
  // The frame's picture truth (one entry) when the run writes output.truth;
  // empty otherwise.
  std::vector<PictureTruthFrame> truth;
  // Dropout sidecar rows produced inside the frame job; committed in frame
  // order at the reassembly point so the sidecar stays byte-identical to a
  // sequential run.
  std::vector<DropoutSidecarRow> dropout_rows;
  bool ok = true;
  std::vector<std::string> errors;
};

// Free list that recycles SynthesizedFrame buffers between the pool workers
// and the consumer.
//
// Each in-flight frame owns two whole-frame fixed-point sample buffers
// (~11.35 MB for PAL) plus its encoded output codes; without recycling, every
// frame allocates and page-faults that memory afresh and releases it again.
// Acquire resets only the logical fields, so a recycled frame arrives with its
// buffers already sized and resident, and the synthesis path overwrites every
// sample it uses.
//
// The free list is capped at the reassembly window size, so peak memory is
// bounded by the same window that bounds in-flight frames.
//
// Thread-safety: all methods ARE thread-safe (guarded by an internal mutex).
class FrameBufferPool {
 public:
  // capacity: maximum number of recycled frames retained; clamped to at
  //   least 1. Frames released beyond it are freed.
  explicit FrameBufferPool(std::size_t capacity);

  // Returns a frame with ok = true and its per-frame result fields cleared.
  // Sample and code buffers keep their previous capacity.
  SynthesizedFrame Acquire();

  // Returns a consumed frame to the free list, or frees it when the list is
  // full.
  void Release(SynthesizedFrame&& frame);

  // Frees every recycled frame. Used when a run ends or is aborted.
  void Clear();

 private:
  std::mutex mutex_;
  std::vector<SynthesizedFrame> free_frames_;
  const std::size_t capacity_;
};

// Bounded reassembly buffer that restores frame order between out-of-order
// producers and a single in-order consumer.
//
// Producers call BeginFrame(index) before starting work on a frame — the call
// blocks until the frame index falls inside the bounded window
// [next unconsumed frame, next unconsumed frame + max_in_flight) — then
// Push(index, frame) when done. The consumer calls Pop repeatedly and
// receives frames in strictly increasing index order. Memory is therefore
// bounded: at most max_in_flight frames exist between synthesis start and
// consumption, independent of the total frame count.
//
// Abort() wakes all blocked producers and the consumer, drops stored frames,
// and makes all subsequent calls return false immediately; used for
// cancellation and error unwinding.
//
// Thread-safety: all methods ARE thread-safe (internally synchronised with a
// mutex and condition variables). Intended usage is N producer threads and
// exactly one consumer thread.
class OrderedFrameReassemblyBuffer {
 public:
  // total_frames: number of frames the consumer expects (Pop returns false
  //   after that many frames were delivered).
  // max_in_flight: window size; clamped to at least 1.
  OrderedFrameReassemblyBuffer(std::size_t total_frames,
                               std::size_t max_in_flight);

  // Blocks until frame_index is inside the bounded window or the buffer is
  // aborted. Returns false when aborted.
  bool BeginFrame(std::size_t frame_index);

  // Stores a completed frame for in-order delivery. Returns false when
  // aborted (the frame is discarded).
  bool Push(std::size_t frame_index, SynthesizedFrame&& frame);

  // Blocks until the next frame in index order is available and moves it to
  // out_frame (out_frame_index receives its index). Returns false when all
  // total_frames frames have been delivered or the buffer is aborted.
  bool Pop(std::size_t* out_frame_index, SynthesizedFrame* out_frame);

  // Aborts the exchange: wakes all waiters, discards stored frames, and makes
  // every subsequent call return false. Idempotent.
  void Abort();

  // Returns true once Abort() has been called.
  bool aborted() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable producer_cv_;
  std::condition_variable consumer_cv_;
  std::map<std::size_t, SynthesizedFrame> completed_frames_;
  const std::size_t total_frames_;
  const std::size_t max_in_flight_;
  std::size_t next_consume_index_ = 0;
  bool aborted_ = false;
};

// Executes per-frame synthesis jobs on a pool of worker threads and delivers
// the results to a consumer callback in frame order on the calling thread.
//
// Thread-safety: a FrameSynthesisPool instance is single-owner; RunOrdered
// must not be called concurrently on the same instance. The job callback runs
// on pool worker threads, potentially concurrently with itself for different
// frame indices, and must be safe for that. The consumer callback always runs
// on the thread that called RunOrdered, in strictly increasing frame order,
// so single-owner collaborators (output stage, WAV writer, sidecar database)
// may be used from it without synchronisation.
class FrameSynthesisPool {
 public:
  // Synthesises one frame. Implementations report failure by setting
  // out_frame->ok = false and appending to out_frame->errors; exceptions are
  // caught by the pool and converted to a failed frame.
  using FrameJob =
      std::function<void(std::size_t frame_index, SynthesizedFrame* out_frame)>;

  // Consumes one frame in order. Returning false stops the run (treated as an
  // error the consumer has already reported through its own channels). The
  // frame stays owned by the pool, which recycles its buffers once the
  // consumer returns, so consumers must copy out anything they need to keep
  // rather than moving from it.
  using FrameConsumer =
      std::function<bool(std::size_t frame_index, SynthesizedFrame& frame)>;

  // Maps the RunOptions::threads convention to an effective worker count:
  // 0 → std::thread::hardware_concurrency() (at least 1), otherwise the
  // requested value (at least 1).
  static unsigned ResolveThreadCount(int requested_threads);

  // thread_count is used as-is (clamped to at least 1); pass the result of
  // ResolveThreadCount to honour the RunOptions convention.
  explicit FrameSynthesisPool(unsigned thread_count);

  unsigned thread_count() const { return thread_count_; }

  // Runs job(frame_index) for every frame in [0, frame_count) across the
  // worker threads and invokes consumer in frame order on the calling thread.
  // In-flight frames are bounded to 2 × thread_count.
  //
  // Stops early and returns false when:
  //   - cancellation is requested (no error message is appended),
  //   - a job reports failure or throws (job errors appended to errors),
  //   - the consumer returns false (no error message is appended).
  // Returns true when all frames were consumed successfully. Worker threads
  // are always joined before returning.
  bool RunOrdered(std::size_t frame_count, const FrameJob& job,
                  const FrameConsumer& consumer,
                  CancellationToken* cancellation,
                  std::vector<std::string>* errors);

 private:
  unsigned thread_count_;
};

}  // namespace videosynth
