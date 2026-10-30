/*
 * File:        source_probe_controller.h
 * Module:      gui
 * Purpose:     Background probing of progressive source files for the
 *              section editor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <optional>
#include <thread>

#include "videosynth/interfaces.h"
#include "videosynth/model.h"

namespace videosynth::gui {

// Outcome of one source probe: the resolved profile plus the validator's
// verdict on it for the project's video standard.
struct SourceProbeReport {
  // False when the probe itself failed (missing/unreadable file); the
  // failure reason is in probe_error.
  bool probe_ok = false;
  ProgressiveFrameSourceProfile profile;
  QString probe_error;
  // ProjectValidator findings for the probed profile against the requested
  // standard; empty means the source passes the HLD §8.1 profile rules.
  QStringList profile_issues;
  // ProjectValidator warnings for the probed profile (field order, sample
  // aspect); they do not make the profile incompatible.
  QStringList profile_warnings;
  // Planned colour normalisation (see FormatColourConversionSummary); empty
  // for stills.
  QString colour_conversion;

  bool profile_ok() const { return probe_ok && profile_issues.isEmpty(); }
};

// Runs the probed profile through ProjectValidator (with a probe stub that
// replays `profile`) for a minimal single-section project carrying the
// section's cadence pattern (which decides the frame rate it needs), so
// pass/fail is decided by exactly the rules generation applies rather than a
// duplicate. Returns the validator's error messages (empty = pass).
//
// Thread-safety: thread-safe (pure function); safe on worker threads.
QStringList EvaluateSourceProfile(const Section& section, Standard standard,
                                  const ProgressiveFrameSourceProfile& profile);

// As EvaluateSourceProfile, returning the validator's errors and warnings.
struct SourceProfileFindings {
  QStringList errors;
  QStringList warnings;
};
SourceProfileFindings EvaluateSourceProfileFindings(
    const Section& section, Standard standard,
    const ProgressiveFrameSourceProfile& profile);

// Describes the colour normalisation the decoder will apply to a video source
// (PlanSourceColourConversion), e.g. "Colour conversion: full range -> studio
// swing". Returns an empty string for EXR stills.
//
// Thread-safety: thread-safe (pure function).
QString FormatColourConversionSummary(
    const ProgressiveFrameSourceProfile& profile, Standard standard);

// One-line human-readable profile summary for the section editor. A
// 24000/1001 source shows its rate as "Film 24000/1001".
//
// Thread-safety: thread-safe (pure function).
QString FormatSourceProfileSummary(
    const ProgressiveFrameSourceProfile& profile);

// Multi-line detail text for the section editor's Source Profile box: the
// profile summary, the planned colour conversion, then every validator
// warning and error, one per line.
//
// Thread-safety: thread-safe (pure function).
QString FormatProbeReportDetail(const SourceProbeReport& report);

// Probes a section's source file off the GUI thread with debouncing and
// latest-wins coalescing, mirroring ValidationController: RequestProbe
// snapshots the section and (re)starts the debounce timer; when it fires the
// snapshot is probed on a dedicated worker thread, and requests arriving
// during an active run collapse to a single trailing probe.
//
// Thread-safety: all public members must be called from the owning (GUI)
// thread, which must run an event loop for results to be delivered. The
// probe function executes on a worker thread and must be safe to run there
// (the default ProgressiveFrameSourceProbe is thread-safe).
class SourceProbeController : public QObject {
  Q_OBJECT

 public:
  using ProbeFn = std::function<bool(
      const Section&, ProgressiveFrameSourceProfile*, std::string*)>;

  // probe: callable executed on the worker thread for each run; when empty,
  // a default that runs ProgressiveFrameSourceProbe (filesystem access) is
  // used.
  explicit SourceProbeController(ProbeFn probe = {}, QObject* parent = nullptr);
  ~SourceProbeController() override;

  // Debounce delay between the last RequestProbe call and the worker start.
  // Default 300 ms; use 0 in tests for immediate scheduling.
  void SetDebounceInterval(int msec);

  // Schedules a probe of a snapshot of `section` for `standard`
  // (latest-wins).
  void RequestProbe(const Section& section, Standard standard);

  // True from worker start until its report has been published.
  bool is_probing() const { return worker_active_; }

  // True once at least one probe run has completed.
  bool has_report() const { return has_report_; }

  // Report from the most recent completed run.
  const SourceProbeReport& report() const { return report_; }

 signals:
  // Worker run started (after debounce).
  void ProbeStarted();

  // report() has been replaced with the newest run's result.
  void ReportChanged();

 private:
  struct Request {
    Section section;
    Standard standard = Standard::kUnknown;
  };

  void StartWorker();
  void PublishReport(SourceProbeReport report);
  void JoinWorker();

  ProbeFn probe_;
  QTimer debounce_timer_;
  std::optional<Request> pending_request_;
  std::thread worker_;
  bool worker_active_ = false;
  bool has_report_ = false;
  SourceProbeReport report_;
};

}  // namespace videosynth::gui
