/*
 * File:        validation_issues_model.h
 * Module:      gui
 * Purpose:     List model of project validation issues for the issues dock
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QAbstractListModel>
#include <QString>
#include <string>
#include <vector>

#include "videosynth/model.h"
#include "videosynth/results.h"

namespace videosynth::gui {

// The editor field group a validation issue concerns, so activating the
// issue can reveal that group. kNone leaves navigation at the section (or
// project) level.
enum class IssueField {
  kNone,
  kCaptureNoise,
  kSharedNoise,
  kRandomDropouts,
  kScratchDropouts,
  kLumaBandwidth,
  kChannel,
  kChromaError,
};

// One validation finding presented in the issues dock.
struct ValidationIssue {
  enum class Severity { kError, kWarning };

  Severity severity = Severity::kError;
  QString message;
  // Index of the section the issue refers to, or -1 for project-level
  // issues. Used by the issue-activation hook to navigate to the offending
  // editor (navigation lands with the section editors).
  int section_index = -1;
  // Field group the message names (ClassifyIssueField).
  IssueField field = IssueField::kNone;
};

// Classifies a validator message by the block it concerns, from the message
// prefix ("Noise validation error", "Channel validation error", ...) and the
// key named after the section reference ("shared.", "random.", ...).
//
// Thread-safety: thread-safe (pure function).
IssueField ClassifyIssueField(const std::string& message);

// Converts a ValidationResult into presentable issues, attributing each
// message to a section by matching the quoted section name the validator
// embeds in its messages ("... section '<name>' ..."). Messages without a
// recognisable section reference become project-level issues.
//
// Thread-safety: thread-safe (pure function); safe to call from validation
// worker threads.
std::vector<ValidationIssue> BuildValidationIssues(
    const Project& project, const ValidationResult& result);

// Read-only list model over the current validation issues.
//
// Thread-safety: NOT thread-safe. GUI (owning) thread only; worker results
// must be marshalled before calling SetIssues.
class ValidationIssuesModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Roles {
    kSeverityRole = Qt::UserRole + 1,  // int(ValidationIssue::Severity)
    kSectionIndexRole,                 // int, -1 = project level
    kFieldRole,                        // int(IssueField)
  };

  explicit ValidationIssuesModel(QObject* parent = nullptr);

  // Default argument mirrors the QAbstractItemModel base signature.
  // NOLINTNEXTLINE(google-default-arguments)
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;

  void SetIssues(std::vector<ValidationIssue> issues);
  const std::vector<ValidationIssue>& issues() const { return issues_; }

  // Selects the theme variant of the severity foreground colours
  // (theme_color_tokens.h).
  void SetDarkTheme(bool dark_theme);

  int error_count() const;
  int warning_count() const;

 private:
  std::vector<ValidationIssue> issues_;
  bool dark_theme_ = false;
};

}  // namespace videosynth::gui
