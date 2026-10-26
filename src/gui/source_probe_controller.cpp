/*
 * File:        source_probe_controller.cpp
 * Module:      gui
 * Purpose:     Background probing of progressive source files for the
 *              section editor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "source_probe_controller.h"

#include <string>
#include <utility>

#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/source_colour_conversion.h"

namespace videosynth::gui {

namespace {

constexpr int kDefaultDebounceMsec = 300;

// Replays an already-resolved profile into ProjectValidator so the profile
// rules run without touching the filesystem.
class FixedProfileProbe final : public IProgressiveFrameSourceProbe {
 public:
  explicit FixedProfileProbe(const ProgressiveFrameSourceProfile& profile)
      : profile_(profile) {}

  bool Probe(const Section& /*section*/,
             ProgressiveFrameSourceProfile* out_profile,
             std::string* error) override {
    *out_profile = profile_;
    error->clear();
    return true;
  }

 private:
  ProgressiveFrameSourceProfile profile_;
};

// Fresh probe per run: ProgressiveFrameSourceProbe is thread-safe, but a
// worker-local instance keeps the default path free of shared state — and it
// keeps the probe's per-source memo scoped to this request, so re-probing after
// the user edits or replaces a source file always reads the file again.
bool RunFilesystemProbe(const Section& section,
                        ProgressiveFrameSourceProfile* out_profile,
                        std::string* error) {
  ProgressiveFrameSourceProbe probe;
  return probe.Probe(section, out_profile, error);
}

}  // namespace

SourceProfileFindings EvaluateSourceProfileFindings(
    const Section& section, Standard standard,
    const ProgressiveFrameSourceProfile& profile) {
  // Minimal single-section project: only source/profile rules can fire, so
  // every reported finding concerns the probed file.
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.output.video_path = "probe.cvbs";
  project.output.metadata_path = "probe.meta";

  Section probe_section;
  probe_section.name = section.name;
  probe_section.type = section.type;
  probe_section.source = section.source;
  probe_section.duration_frames = 1;
  project.sections.push_back(std::move(probe_section));

  FixedProfileProbe fixed_probe(profile);
  ProjectValidator validator(&fixed_probe);
  const ValidationResult result = validator.Validate(project);

  SourceProfileFindings findings;
  for (const std::string& error : result.errors) {
    findings.errors.append(QString::fromStdString(error));
  }
  for (const std::string& warning : result.warnings) {
    findings.warnings.append(QString::fromStdString(warning));
  }
  return findings;
}

QStringList EvaluateSourceProfile(
    const Section& section, Standard standard,
    const ProgressiveFrameSourceProfile& profile) {
  return EvaluateSourceProfileFindings(section, standard, profile).errors;
}

QString FormatColourConversionSummary(
    const ProgressiveFrameSourceProfile& profile, Standard standard) {
  if (profile.container == "exr") {
    return QString();
  }
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, standard);
  if (plan.IsPassThrough()) {
    return QObject::tr("Colour: BT.601 studio swing, used unconverted");
  }
  QStringList conversions;
  for (const std::string& conversion : plan.conversions) {
    conversions.append(QString::fromStdString(conversion));
  }
  return QObject::tr("Colour conversion: %1")
      .arg(conversions.join(QStringLiteral(", ")));
}

QString FormatSourceProfileSummary(
    const ProgressiveFrameSourceProfile& profile) {
  QStringList parts;
  if (profile.width > 0 && profile.height > 0) {
    parts.append(
        QStringLiteral("%1×%2").arg(profile.width).arg(profile.height));
  }
  if (profile.frame_rate_hz > 0.0) {
    parts.append(
        QStringLiteral("%1 fps").arg(profile.frame_rate_hz, 0, 'f', 3));
  }
  if (!profile.codec.empty()) {
    QString codec = QString::fromStdString(profile.codec);
    if (!profile.container.empty()) {
      codec +=
          QStringLiteral("/%1").arg(QString::fromStdString(profile.container));
    }
    parts.append(codec);
  }
  if (!profile.pixel_format.empty()) {
    QString format = QString::fromStdString(profile.pixel_format);
    if (profile.bit_depth > 0) {
      format += QStringLiteral(" %1-bit").arg(profile.bit_depth);
    }
    parts.append(format);
  } else if (profile.bit_depth > 0) {
    parts.append(QStringLiteral("%1-bit").arg(profile.bit_depth));
  }
  if (!profile.color_space.empty()) {
    parts.append(QString::fromStdString(profile.color_space));
  }
  if (!profile.field_order.empty()) {
    parts.append(QStringLiteral("field order %1")
                     .arg(QString::fromStdString(profile.field_order)));
  }
  if (profile.frame_count > 0) {
    parts.append(QStringLiteral("%1 frames").arg(profile.frame_count));
  }
  return parts.join(QStringLiteral(", "));
}

QString FormatProbeReportDetail(const SourceProbeReport& report) {
  QStringList lines;
  lines.append(FormatSourceProfileSummary(report.profile));
  if (!report.colour_conversion.isEmpty()) {
    lines.append(report.colour_conversion);
  }
  lines.append(report.profile_warnings);
  lines.append(report.profile_issues);
  return lines.join(QStringLiteral("\n"));
}

SourceProbeController::SourceProbeController(ProbeFn probe, QObject* parent)
    : QObject(parent), probe_(std::move(probe)) {
  if (!probe_) {
    probe_ = RunFilesystemProbe;
  }
  debounce_timer_.setSingleShot(true);
  debounce_timer_.setInterval(kDefaultDebounceMsec);
  connect(&debounce_timer_, &QTimer::timeout, this,
          &SourceProbeController::StartWorker);
}

SourceProbeController::~SourceProbeController() { JoinWorker(); }

void SourceProbeController::SetDebounceInterval(int msec) {
  debounce_timer_.setInterval(msec);
}

void SourceProbeController::RequestProbe(const Section& section,
                                         Standard standard) {
  pending_request_ = Request{section, standard};
  debounce_timer_.start();
}

void SourceProbeController::StartWorker() {
  if (worker_active_ || !pending_request_.has_value()) {
    // An active run publishes via PublishReport, which re-enters here to
    // pick up the pending snapshot.
    return;
  }

  Request request = std::move(*pending_request_);
  pending_request_.reset();
  JoinWorker();
  worker_active_ = true;
  emit ProbeStarted();

  worker_ = std::thread([this, request = std::move(request)] {
    SourceProbeReport report;
    std::string error;
    report.probe_ok = probe_(request.section, &report.profile, &error);
    if (!report.probe_ok) {
      report.probe_error = error.empty() ? tr("Source profile probing failed.")
                                         : QString::fromStdString(error);
    } else {
      SourceProfileFindings findings = EvaluateSourceProfileFindings(
          request.section, request.standard, report.profile);
      report.profile_issues = std::move(findings.errors);
      report.profile_warnings = std::move(findings.warnings);
      report.colour_conversion =
          FormatColourConversionSummary(report.profile, request.standard);
    }

    // Marshal back to the owning thread; if the controller is destroyed
    // before delivery, Qt drops the queued call with the context object.
    QMetaObject::invokeMethod(
        this,
        [this, report = std::move(report)]() mutable {
          PublishReport(std::move(report));
        },
        Qt::QueuedConnection);
  });
}

void SourceProbeController::PublishReport(SourceProbeReport report) {
  worker_active_ = false;
  has_report_ = true;
  report_ = std::move(report);
  emit ReportChanged();

  // A request arrived while the run was active: probe the newest snapshot
  // immediately (the debounce already elapsed for it).
  if (pending_request_.has_value()) {
    StartWorker();
  }
}

void SourceProbeController::JoinWorker() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

}  // namespace videosynth::gui
