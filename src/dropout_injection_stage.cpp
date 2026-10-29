/*
 * File:        dropout_injection_stage.cpp
 * Module:      dropout_injection
 * Purpose:     Applies per-section random and scratch dropout events to
 *              fixed-point mV Y/C buffers before output quantisation, and
 *              writes a conformant dropout sidecar SQLite file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/dropout_injection_stage.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "videosynth/dropout_scale.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/impairment_seed.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

// Sidecar schema version per dropout-extension-format.md §Versioning.
constexpr int kSidecarSchemaVersion = 5;

// Severity values per dropout-injection-design.md §Sidecar Format.
constexpr int kSeverityNonVisible = 25;
constexpr int kSeverityVisible = 75;

// Per-standard active-picture sample ranges at 4fsc.
// Derived in dropout-injection-design.md §Active-Picture Sample Ranges.
// PAL: lines 23–622 active (fields 1 and 2 combined).
//   First active sample: line 22 * 1135 = 24 970. (0-based: line index 22)
//   Last  active sample: end of line 622 = line 622 * 1135 + 1134 = 706 404.
// These are used for severity assignment and boundary splitting.
constexpr int kPalActivePictureStart = 22 * 1135;        // 24970
constexpr int kPalActivePictureEnd = 622 * 1135 + 1134;  // 706404

// NTSC: lines 22–520 active.
//   First active sample: line 21 * 910 = 19110. (0-based)
//   Last  active sample: end of line 520 = line 520 * 910 + 909 = 474109.
constexpr int kNtscActivePictureStart = 21 * 910;       // 19110
constexpr int kNtscActivePictureEnd = 520 * 910 + 909;  // 474109

// PAL-M: same line range as NTSC (System M 525-line frame) with 909 spl at
// 4fsc.
//   First active sample: line 21 * 909 = 19089. (0-based)
//   Last  active sample: end of line 520 = line 520 * 909 + 908 = 472988.
constexpr int kPalMActivePictureStart = 21 * 909;       // 19089
constexpr int kPalMActivePictureEnd = 520 * 909 + 908;  // 472988

// Luma clamping bounds matching those used by NoiseInjectionStage.
constexpr double kPalClampMinMv = -300.006;
constexpr double kPalClampMaxMv = 908.452;
constexpr double kNtscClampMinMv = -285.712;
constexpr double kNtscClampMaxMv = 993.984;

// Derives the per-frame RNG seed. Mirrors the scheme in NoiseInjectionStage
// and follows dropout-injection-design.md §RNG Seeding.
inline uint64_t FrameSeed(uint64_t base_seed, std::size_t section_index,
                          std::size_t global_frame) {
  return (base_seed ^ (static_cast<uint64_t>(section_index) * 2654435761ULL)) ^
         (static_cast<uint64_t>(global_frame) * 2246822519ULL);
}

// Derives the per-scratch-event RNG seed.
inline uint64_t EventSeed(uint64_t base_seed, std::size_t section_index,
                          std::size_t event_index) {
  return (base_seed ^ (static_cast<uint64_t>(section_index) * 2654435761ULL)) ^
         (static_cast<uint64_t>(event_index) * 1000000007ULL);
}

// Draws N from a Poisson distribution.
int DrawPoisson(double mean, std::mt19937_64& rng) {
  std::poisson_distribution<int> dist(mean);
  return dist(rng);
}

// Draws a run duration uniformly from [min_dur, max_dur].
int DrawDuration(int min_dur, int max_dur, std::mt19937_64& rng) {
  if (min_dur >= max_dur) {
    return min_dur;
  }
  std::uniform_int_distribution<int> dist(min_dur, max_dur);
  return dist(rng);
}

// Clips the random run list against the sorted scratch intervals.
// Returns a de-overlapped, sorted list of (start, count) pairs.
// Each surviving sub-run inherits from its originating event but that
// is not needed for the caller — direction/push are applied per sub-run.
struct RandomEvent {
  int start;
  int count;
  int direction;
  double push_fraction;
};

std::vector<RandomEvent> ClipAgainstScratch(
    const std::vector<RandomEvent>& events,
    const std::vector<std::pair<int, int>>& scratch_intervals) {
  std::vector<RandomEvent> result;
  result.reserve(events.size());

  for (const RandomEvent& ev : events) {
    int lo = ev.start;
    const int hi = ev.start + ev.count;  // exclusive

    // Clip against each scratch interval in order.
    for (const auto& [s_lo, s_hi] : scratch_intervals) {
      if (s_lo >= hi || s_hi <= lo) {
        continue;  // no overlap
      }
      // Left remainder: [lo, s_lo)
      if (lo < s_lo) {
        RandomEvent sub = ev;
        sub.start = lo;
        sub.count = s_lo - lo;
        result.push_back(sub);
      }
      lo = s_hi;
      if (lo >= hi) {
        break;
      }
    }

    // Right remainder: [lo, hi)
    if (lo < hi) {
      RandomEvent sub = ev;
      sub.start = lo;
      sub.count = hi - lo;
      result.push_back(sub);
    }
  }

  // Sort and merge adjacent/overlapping sub-runs with the same direction and
  // push. Since events may have different parameters, only merge runs that
  // are adjacent with the same direction/push_fraction (no inter-event merge
  // is needed by the spec — de-duplication of overlapping sub-runs suffices).
  // Sort by start position.
  std::sort(result.begin(), result.end(),
            [](const RandomEvent& a, const RandomEvent& b) {
              return a.start < b.start;
            });

  // Merge overlapping/adjacent sub-runs derived from different events.
  // The merged run inherits from the first (lower-start) sub-run.
  std::vector<RandomEvent> merged;
  merged.reserve(result.size());
  for (const RandomEvent& ev : result) {
    if (!merged.empty()) {
      RandomEvent& back = merged.back();
      const int back_end = back.start + back.count;
      if (ev.start <= back_end) {
        // Extend the last merged run.
        back.count = std::max(back_end, ev.start + ev.count) - back.start;
        continue;
      }
    }
    merged.push_back(ev);
  }

  return merged;
}

}  // namespace

// ---------------------------------------------------------------------------
// DropoutInjectionStage implementation
// ---------------------------------------------------------------------------

DropoutInjectionStage::DropoutInjectionStage(ILogger* logger)
    : logger_(logger), run_base_seed_(std::random_device{}()) {}

DropoutInjectionStage::~DropoutInjectionStage() {
  if (insert_stmt_ != nullptr) {
    sqlite3_finalize(insert_stmt_);
    insert_stmt_ = nullptr;
  }
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

// Schedule items point into the project's contiguous section vector, so the
// index is recoverable by pointer arithmetic in O(1) — a linear scan here runs
// once per frame on every dropout-enabled project.
// static
std::size_t DropoutInjectionStage::FindSectionIndex(const Project& project,
                                                    const Section* section) {
  const Section* first = project.sections.data();
  if (first == nullptr || section < first ||
      section >= (first + project.sections.size())) {
    return 0;
  }
  return static_cast<std::size_t>(section - first);
}

// static
std::string DropoutInjectionStage::DeriveSidecarPath(
    const std::string& metadata_path) {
  const std::string suffix = ".meta";
  if (metadata_path.size() >= suffix.size() &&
      metadata_path.compare(metadata_path.size() - suffix.size(), suffix.size(),
                            suffix) == 0) {
    return metadata_path.substr(0, metadata_path.size() - suffix.size()) +
           ".dropouts.meta";
  }
  return metadata_path + ".dropouts.meta";
}

// static
bool DropoutInjectionStage::AnyDropoutEnabled(const Project& project) {
  for (const Section& s : project.sections) {
    if ((s.dropouts.random.enabled && s.dropouts.random.scale > 0) ||
        (s.dropouts.scratch.enabled && s.dropouts.scratch.scale > 0)) {
      return true;
    }
  }
  return false;
}

bool DropoutInjectionStage::Begin(const Project& project,
                                  std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }

  if (!AnyDropoutEnabled(project)) {
    if (logger_ != nullptr) {
      logger_->Debug(
          "DropoutInjectionStage: no dropout injection enabled; sidecar "
          "will not be created.");
    }
    return true;
  }

  const std::string sidecar_path =
      DeriveSidecarPath(project.output.metadata_path);

  if (logger_ != nullptr) {
    logger_->Info("DropoutInjectionStage: opening sidecar at " + sidecar_path);
  }

  int rc = sqlite3_open(sidecar_path.c_str(), &db_);
  if (rc != SQLITE_OK) {
    errors->push_back("DropoutInjectionStage: failed to open sidecar '" +
                      sidecar_path + "': " + std::string(sqlite3_errmsg(db_)));
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  // Set schema version and recreate schema from scratch so that repeated
  // pipeline runs on the same output path never accumulate stale rows from a
  // previous invocation.  The video file is overwritten each run; the sidecar
  // must match it exactly.
  const std::string schema =
      "PRAGMA user_version = " + std::to_string(kSidecarSchemaVersion) +
      ";"
      "DROP TABLE IF EXISTS dropout_run;"
      "DROP INDEX IF EXISTS idx_dropout_run_frame;"
      "CREATE TABLE dropout_run ("
      "  cvbs_file_id  INTEGER NOT NULL,"
      "  frame_id      INTEGER NOT NULL CHECK (frame_id >= 0),"
      "  sample_start  INTEGER NOT NULL CHECK (sample_start >= 0),"
      "  sample_count  INTEGER NOT NULL CHECK (sample_count > 0),"
      "  severity      INTEGER NOT NULL CHECK (severity >= 0 AND severity <= "
      "100),"
      "  PRIMARY KEY (cvbs_file_id, frame_id, sample_start)"
      ");"
      "CREATE INDEX idx_dropout_run_frame"
      "  ON dropout_run (cvbs_file_id, frame_id);"
      "BEGIN;";

  char* errmsg = nullptr;
  rc = sqlite3_exec(db_, schema.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    const std::string msg = errmsg != nullptr ? errmsg : "unknown error";
    sqlite3_free(errmsg);
    errors->push_back(
        "DropoutInjectionStage: failed to create sidecar schema: " + msg);
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  // Prepare the insert statement.
  const char* insert_sql =
      "INSERT INTO dropout_run "
      "(cvbs_file_id, frame_id, sample_start, sample_count, severity) "
      "VALUES (1, ?, ?, ?, ?);";
  rc = sqlite3_prepare_v2(db_, insert_sql, -1, &insert_stmt_, nullptr);
  if (rc != SQLITE_OK) {
    errors->push_back(
        "DropoutInjectionStage: failed to prepare insert statement: " +
        std::string(sqlite3_errmsg(db_)));
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  sidecar_path_ = sidecar_path;
  return true;
}

void DropoutInjectionStage::Abort() {
  if (db_ == nullptr) {
    return;
  }

  if (insert_stmt_ != nullptr) {
    sqlite3_finalize(insert_stmt_);
    insert_stmt_ = nullptr;
  }

  // Discard the open transaction; errors are ignored because the file is
  // removed immediately afterwards.
  sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
  sqlite3_close(db_);
  db_ = nullptr;

  std::error_code ec;
  std::filesystem::remove(sidecar_path_, ec);
  sidecar_path_.clear();

  if (logger_ != nullptr) {
    logger_->Info(
        "DropoutInjectionStage: sidecar session aborted; removed "
        "in-progress sidecar file.");
  }
}

bool DropoutInjectionStage::Finalize(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (db_ == nullptr) {
    return true;
  }

  if (insert_stmt_ != nullptr) {
    sqlite3_finalize(insert_stmt_);
    insert_stmt_ = nullptr;
  }

  char* errmsg = nullptr;
  const int rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    const std::string msg = errmsg != nullptr ? errmsg : "unknown error";
    sqlite3_free(errmsg);
    errors->push_back(
        "DropoutInjectionStage: failed to commit sidecar transaction: " + msg);
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  sqlite3_close(db_);
  db_ = nullptr;

  if (logger_ != nullptr) {
    logger_->Info("DropoutInjectionStage: sidecar finalised successfully.");
  }
  return true;
}

void DropoutInjectionStage::WriteSidecarRow(int64_t frame_id,
                                            int64_t sample_start,
                                            int64_t sample_count,
                                            int severity) {
  if (insert_stmt_ == nullptr) {
    return;
  }
  sqlite3_reset(insert_stmt_);
  sqlite3_bind_int64(insert_stmt_, 1, frame_id);
  sqlite3_bind_int64(insert_stmt_, 2, sample_start);
  sqlite3_bind_int64(insert_stmt_, 3, sample_count);
  sqlite3_bind_int(insert_stmt_, 4, severity);
  sqlite3_step(insert_stmt_);
}

// static
std::vector<ScratchEvent> DropoutInjectionStage::ComputeScratchEvents(
    const Section& section, std::size_t section_index,
    std::uint64_t section_disc_start, int lines_per_frame, int samples_per_line,
    uint64_t run_base_seed) {
  std::vector<ScratchEvent> scratch_events;

  if (!section.dropouts.scratch.enabled ||
      section.dropouts.scratch.scale == 0) {
    return scratch_events;
  }

  const ScratchDropoutDerivedParams sp =
      DeriveScratchDropoutParams(section.dropouts.scratch.scale);

  const uint64_t scratch_base =
      section.dropouts.scratch.seed_specified
          ? static_cast<uint64_t>(section.dropouts.scratch.seed)
          : run_base_seed;

  scratch_events.reserve(static_cast<std::size_t>(sp.count));

  // Disc-anchored scratches are keyed by the disc frame the section starts
  // on, so a replay from the same picture repeats them.
  const bool disc_anchored =
      section.dropouts.scratch.anchor == DropoutAnchor::kDisc;
  const uint64_t disc_base = DeriveImpairmentSeed(
      scratch_base, kScratchDropoutSeedDomain, section_disc_start);
  for (int e = 0; e < sp.count; ++e) {
    const uint64_t seed_e =
        disc_anchored ? SplitMix64(disc_base ^ static_cast<uint64_t>(e))
                      : EventSeed(scratch_base, section_index,
                                  static_cast<std::size_t>(e));
    std::mt19937_64 rng_e(seed_e);

    std::uniform_int_distribution<int> line_dist(0, lines_per_frame - 1);
    std::uniform_int_distribution<int> offset_dist(0, samples_per_line - 1);
    std::uniform_int_distribution<int> dur_dist(1, sp.max_dur_frames);
    std::uniform_int_distribution<int> width_dist(1, sp.max_width_samples);
    std::uniform_int_distribution<int> dir_dist(0, 1);
    std::uniform_real_distribution<double> push_dist(0.5, 1.0);

    ScratchEvent ev;
    ev.anchor_line = line_dist(rng_e);
    ev.anchor_offset = offset_dist(rng_e);
    ev.duration_frames = dur_dist(rng_e);
    ev.peak_width = width_dist(rng_e);
    ev.direction = dir_dist(rng_e) == 0 ? -1 : 1;
    ev.max_push_fraction = push_dist(rng_e);
    scratch_events.push_back(ev);
  }

  return scratch_events;
}

// Applies the directional signal push and appends sidecar row(s), splitting
// at the active-picture boundary when the run straddles it.
// static
void DropoutInjectionStage::ApplyAndRecordRun(
    int sample_start, int sample_count, int direction, double push_fraction,
    int64_t frame_id, int active_picture_start, int active_picture_end,
    const SignalLevels& levels, SampleFixed clamp_min, SampleFixed clamp_max,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv,
    std::size_t frame_buffer_offset, std::vector<DropoutSidecarRow>* out_rows) {
  if (sample_count <= 0) {
    return;
  }

  auto AppendRow = [out_rows, frame_id](int64_t row_sample_start,
                                        int64_t row_sample_count,
                                        int severity) {
    out_rows->push_back(DropoutSidecarRow{frame_id, row_sample_start,
                                          row_sample_count, severity});
  };

  // The dropout shifts the signal as a DC offset (preserving peak-to-peak
  // amplitude) rather than lerping to a fixed target.  Maximum shift amplitude
  // is push_fraction × luminance range; direction selects up (+1) or down (−1).
  const double signal_range = levels.white_mv - levels.blanking_mv;
  const double max_shift_mv =
      push_fraction * signal_range * static_cast<double>(direction);

  // Tukey window (α = 0.5): cosine ramp over the first and last 25% of the
  // run, flat at 1.0 in the central 50%.  Ramp is twice as rapid as a full
  // Hanning window while still giving a smooth analogue onset/recovery.
  // Sample centres are at half-integer positions so that single-sample runs
  // fall in the flat region and receive the full shift.
  const double n = static_cast<double>(sample_count);
  const double ramp_n = n * 0.25;

  for (int s = sample_start; s < sample_start + sample_count; ++s) {
    const int local = s - sample_start;
    const double x = static_cast<double>(local) + 0.5;
    double envelope;
    if (x < ramp_n) {
      envelope = 0.5 * (1.0 - std::cos(std::acos(-1.0) * x / ramp_n));
    } else if (x > n - ramp_n) {
      envelope = 0.5 * (1.0 - std::cos(std::acos(-1.0) * (n - x) / ramp_n));
    } else {
      envelope = 1.0;
    }
    const double shift_mv = max_shift_mv * envelope;
    const std::size_t idx = frame_buffer_offset + static_cast<std::size_t>(s);
    const double y_orig = SampleFixedToMillivolts((*y_mv)[idx]);
    const double c_orig = SampleFixedToMillivolts((*c_mv)[idx]);
    (*y_mv)[idx] = std::clamp(MillivoltsToSampleFixed(y_orig + shift_mv),
                              clamp_min, clamp_max);
    // The bounds are composite limits; zero-centred C is left for the output
    // stage, which clamps Y + C.
    (*c_mv)[idx] = MillivoltsToSampleFixed(c_orig + shift_mv);
  }

  // Unflagged runs leave no trace in the sidecar.
  if (out_rows == nullptr) {
    return;
  }

  // Write sidecar row(s), splitting at the active-picture boundary.
  const int run_end = sample_start + sample_count;

  // Case 1: run is entirely before the active picture.
  if (run_end <= active_picture_start) {
    AppendRow(sample_start, sample_count, kSeverityNonVisible);
    return;
  }

  // Case 2: run is entirely within the active picture.
  if (sample_start >= active_picture_start &&
      run_end <= active_picture_end + 1) {
    AppendRow(sample_start, sample_count, kSeverityVisible);
    return;
  }

  // Case 3: run is entirely after the active picture.
  if (sample_start > active_picture_end) {
    AppendRow(sample_start, sample_count, kSeverityNonVisible);
    return;
  }

  // Case 4: run straddles the start of the active picture.
  if (sample_start < active_picture_start && run_end > active_picture_start) {
    const int before = active_picture_start - sample_start;
    const int after = run_end - active_picture_start;
    AppendRow(sample_start, before, kSeverityNonVisible);
    AppendRow(active_picture_start, after, kSeverityVisible);
    return;
  }

  // Case 5: run straddles the end of the active picture.
  if (sample_start <= active_picture_end && run_end > active_picture_end + 1) {
    const int inside = active_picture_end + 1 - sample_start;
    const int after = run_end - (active_picture_end + 1);
    AppendRow(sample_start, inside, kSeverityVisible);
    AppendRow(active_picture_end + 1, after, kSeverityNonVisible);
    return;
  }

  // Fallback: treat as non-visible.
  AppendRow(sample_start, sample_count, kSeverityNonVisible);
}

// static
void DropoutInjectionStage::ProcessScratchDropouts(
    const std::vector<ScratchEvent>& scratch_events, int samples_per_line,
    int64_t frame_id, int frame_index_in_section,
    std::vector<std::pair<int, int>>* out_scratch, const SignalLevels& levels,
    SampleFixed clamp_min, SampleFixed clamp_max,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv,
    std::size_t frame_buffer_offset, int samples_per_frame,
    std::vector<DropoutSidecarRow>* out_rows) {
  out_scratch->clear();

  if (scratch_events.empty()) {
    return;
  }

  const int active_picture_start =
      (samples_per_line == 1135)  ? kPalActivePictureStart
      : (samples_per_line == 909) ? kPalMActivePictureStart
                                  : kNtscActivePictureStart;
  const int active_picture_end =
      (samples_per_line == 1135)  ? kPalActivePictureEnd
      : (samples_per_line == 909) ? kPalMActivePictureEnd
                                  : kNtscActivePictureEnd;

  for (const ScratchEvent& ev : scratch_events) {
    const int max_frame = ev.duration_frames - 1;
    if (frame_index_in_section < 0 || frame_index_in_section > max_frame) {
      continue;
    }

    const double progress = (max_frame > 0)
                                ? static_cast<double>(frame_index_in_section) /
                                      static_cast<double>(max_frame)
                                : 1.0;
    const double triangle = 1.0 - std::abs(2.0 * progress - 1.0);

    const int width = static_cast<int>(
        std::round(static_cast<double>(ev.peak_width) * triangle));
    if (width <= 0) {
      continue;
    }

    const double current_push = ev.max_push_fraction * triangle;

    // Scratch position is fixed for the event's entire lifespan; anchor_line
    // is constant so the dropout stays on the same line as it grows/shrinks.
    int sample_start = ev.anchor_line * samples_per_line + ev.anchor_offset;
    sample_start = std::clamp(sample_start, 0, samples_per_frame - width);

    out_scratch->emplace_back(sample_start, sample_start + width);

    ApplyAndRecordRun(sample_start, width, ev.direction, current_push, frame_id,
                      active_picture_start, active_picture_end, levels,
                      clamp_min, clamp_max, y_mv, c_mv, frame_buffer_offset,
                      out_rows);
  }

  // Sort scratch intervals for overlap clipping.
  std::sort(out_scratch->begin(), out_scratch->end());
}

void DropoutInjectionStage::ProcessRandomDropouts(
    const Section& section, std::size_t section_index, std::size_t global_frame,
    std::uint64_t disc_frame, int samples_per_frame, int lines_per_frame,
    int samples_per_line, int64_t frame_id,
    const std::vector<std::pair<int, int>>& scratch_intervals,
    const SignalLevels& levels, SampleFixed clamp_min, SampleFixed clamp_max,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv,
    std::size_t frame_buffer_offset,
    std::vector<DropoutSidecarRow>* out_rows) const {
  (void)lines_per_frame;

  const uint64_t random_base =
      section.dropouts.random.seed_specified
          ? static_cast<uint64_t>(section.dropouts.random.seed)
          : run_base_seed_;
  const uint64_t frame_seed =
      section.dropouts.random.anchor == DropoutAnchor::kDisc
          ? DeriveImpairmentSeed(random_base, kRandomDropoutSeedDomain,
                                 disc_frame)
          : FrameSeed(random_base, section_index, global_frame);
  std::mt19937_64 rng(frame_seed);

  const RandomDropoutDerivedParams rp =
      DeriveRandomDropoutParams(section.dropouts.random.scale);

  const int n = DrawPoisson(rp.frequency, rng);

  std::uniform_int_distribution<int> pos_dist(0, samples_per_frame - 1);
  std::bernoulli_distribution up_dist(0.2);  // 20% up, 80% down
  std::uniform_real_distribution<double> push_dist(0.5, 1.0);

  const int active_picture_start =
      (samples_per_line == 1135)  ? kPalActivePictureStart
      : (samples_per_line == 909) ? kPalMActivePictureStart
                                  : kNtscActivePictureStart;
  const int active_picture_end =
      (samples_per_line == 1135)  ? kPalActivePictureEnd
      : (samples_per_line == 909) ? kPalMActivePictureEnd
                                  : kNtscActivePictureEnd;

  std::vector<RandomEvent> raw_events;
  raw_events.reserve(static_cast<std::size_t>(n));

  for (int i = 0; i < n; ++i) {
    RandomEvent ev;
    ev.start = pos_dist(rng);
    const int dur = DrawDuration(rp.min_duration, rp.max_duration, rng);
    ev.count = std::min(dur, samples_per_frame - ev.start);
    ev.direction = up_dist(rng) ? 1 : -1;
    ev.push_fraction = push_dist(rng);
    if (ev.count > 0) {
      raw_events.push_back(ev);
    }
  }

  // Sort raw events by start position before clipping.
  std::sort(raw_events.begin(), raw_events.end(),
            [](const RandomEvent& a, const RandomEvent& b) {
              return a.start < b.start;
            });

  const std::vector<RandomEvent> resolved =
      ClipAgainstScratch(raw_events, scratch_intervals);

  for (const RandomEvent& ev : resolved) {
    ApplyAndRecordRun(ev.start, ev.count, ev.direction, ev.push_fraction,
                      frame_id, active_picture_start, active_picture_end,
                      levels, clamp_min, clamp_max, y_mv, c_mv,
                      frame_buffer_offset, out_rows);
  }
}

void DropoutInjectionStage::ComputeFrameDropouts(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t global_frame, std::vector<SampleFixed>* y_mv,
    std::vector<SampleFixed>* c_mv, std::size_t frame_buffer_offset,
    std::vector<DropoutSidecarRow>* out_rows) const {
  if (y_mv == nullptr || c_mv == nullptr || out_rows == nullptr ||
      global_frame >= schedule.size()) {
    return;
  }

  const Standard standard = project.cvbs_presets.video_standard_preset;
  const int samples_per_frame = SamplesPerFrame4fsc(standard);
  if (samples_per_frame <= 0) {
    return;
  }

  const Section* section = schedule[global_frame].section;
  if (section == nullptr) {
    return;
  }

  const bool random_active =
      section->dropouts.random.enabled && section->dropouts.random.scale > 0;
  const bool scratch_active =
      section->dropouts.scratch.enabled && section->dropouts.scratch.scale > 0;

  if (!random_active && !scratch_active) {
    return;
  }

  const TimingConstants timing = GetTimingConstants(standard);
  const int lines_per_frame = timing.lines_per_frame;
  const int samples_per_line = timing.samples_per_line_4fsc;

  const SignalLevels levels = GetSignalLevels(project.cvbs_presets);

  const SampleFixed clamp_min = MillivoltsToSampleFixed(
      Is625LineStandard(standard) ? kPalClampMinMv : kNtscClampMinMv);
  const SampleFixed clamp_max = MillivoltsToSampleFixed(
      Is625LineStandard(standard) ? kPalClampMaxMv : kNtscClampMaxMv);

  const std::size_t section_index = FindSectionIndex(project, section);
  const int64_t frame_id = static_cast<int64_t>(global_frame);
  // The scratch envelope runs over the frame's position within the section,
  // not the (possibly offset or looping) source frame it shows.
  const IGenerationStage::FrameScheduleItem& item = schedule[global_frame];
  const int frame_index_in_section = item.section_frame_index;
  // Disc position keys disc-anchored dropouts. Disc frames run on one per
  // frame within a section, so the section starts at disc_frame minus its
  // position (picture_number start_value - 1 on CAV).
  const std::uint64_t disc_frame =
      item.enrichment != nullptr
          ? static_cast<std::uint64_t>(item.enrichment->disc_frame_index)
          : static_cast<std::uint64_t>(global_frame);
  const std::uint64_t section_disc_start =
      disc_frame - static_cast<std::uint64_t>(frame_index_in_section);

  // 1. Process scratch dropouts first; collect their covered intervals.
  // Scratch events are recomputed deterministically per frame so this method
  // needs no mutable cache and stays order-independent.
  std::vector<std::pair<int, int>> scratch_intervals;
  if (scratch_active) {
    const std::vector<ScratchEvent> scratch_events =
        ComputeScratchEvents(*section, section_index, section_disc_start,
                             lines_per_frame, samples_per_line, run_base_seed_);
    ProcessScratchDropouts(
        scratch_events, samples_per_line, frame_id, frame_index_in_section,
        &scratch_intervals, levels, clamp_min, clamp_max, y_mv, c_mv,
        frame_buffer_offset, samples_per_frame,
        section->dropouts.scratch.flagged ? out_rows : nullptr);
  }

  // 2. Process random dropouts, clipping against scratch intervals.
  if (random_active) {
    ProcessRandomDropouts(
        *section, section_index, global_frame, disc_frame, samples_per_frame,
        lines_per_frame, samples_per_line, frame_id, scratch_intervals, levels,
        clamp_min, clamp_max, y_mv, c_mv, frame_buffer_offset,
        section->dropouts.random.flagged ? out_rows : nullptr);
  }
}

void DropoutInjectionStage::CommitSidecarRows(
    const std::vector<DropoutSidecarRow>& rows) {
  for (const DropoutSidecarRow& row : rows) {
    WriteSidecarRow(row.frame_id, row.sample_start, row.sample_count,
                    row.severity);
  }
}

void DropoutInjectionStage::InjectDropouts(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) {
  if (y_mv == nullptr || c_mv == nullptr) {
    return;
  }

  const Standard standard = project.cvbs_presets.video_standard_preset;
  const int samples_per_frame = SamplesPerFrame4fsc(standard);
  if (samples_per_frame <= 0) {
    return;
  }

  std::vector<DropoutSidecarRow> rows;
  for (std::size_t i = 0; i < frame_count; ++i) {
    const std::size_t global_frame = frame_offset + i;
    if (global_frame >= schedule.size()) {
      break;
    }
    const std::size_t frame_buffer_offset =
        i * static_cast<std::size_t>(samples_per_frame);
    rows.clear();
    ComputeFrameDropouts(project, schedule, global_frame, y_mv, c_mv,
                         frame_buffer_offset, &rows);
    CommitSidecarRows(rows);
  }
}

}  // namespace videosynth
