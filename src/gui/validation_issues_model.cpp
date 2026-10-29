/*
 * File:        validation_issues_model.cpp
 * Module:      gui
 * Purpose:     List model of project validation issues for the issues dock
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "validation_issues_model.h"

#include <QBrush>
#include <algorithm>
#include <string>

#include "theme_color_tokens.h"

namespace videosynth::gui {

namespace {

// The validator has no structured section reference; it embeds the section
// name in single quotes. Attribute the message to the first section whose
// quoted name appears in it.
int FindSectionIndex(const Project& project, const std::string& message) {
  for (std::size_t i = 0; i < project.sections.size(); ++i) {
    const std::string& name = project.sections[i].name;
    if (name.empty()) {
      continue;
    }
    if (message.find("'" + name + "'") != std::string::npos) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

IssueField ClassifyIssueField(const std::string& message) {
  if (StartsWith(message, "Channel validation error")) {
    return IssueField::kChannel;
  }
  if (StartsWith(message, "Chroma error validation error")) {
    return IssueField::kChromaError;
  }
  if (StartsWith(message, "Luma bandwidth validation error")) {
    return IssueField::kLumaBandwidth;
  }
  // Keys follow the quoted section name; looking only past it keeps a section
  // name such as 'shared.intro' from steering the classification.
  const std::size_t name_end = message.find("': ");
  const std::string detail =
      name_end == std::string::npos ? message : message.substr(name_end + 3);
  if (StartsWith(message, "Noise ")) {
    return (StartsWith(detail, "shared.") ||
            detail.find("noise.shared") != std::string::npos)
               ? IssueField::kSharedNoise
               : IssueField::kCaptureNoise;
  }
  if (StartsWith(message, "Dropout ")) {
    return StartsWith(detail, "scratch.") ? IssueField::kScratchDropouts
                                          : IssueField::kRandomDropouts;
  }
  return IssueField::kNone;
}

std::vector<ValidationIssue> BuildValidationIssues(
    const Project& project, const ValidationResult& result) {
  std::vector<ValidationIssue> issues;
  issues.reserve(result.errors.size() + result.warnings.size());

  for (const std::string& message : result.errors) {
    ValidationIssue issue;
    issue.severity = ValidationIssue::Severity::kError;
    issue.message = QString::fromStdString(message);
    issue.section_index = FindSectionIndex(project, message);
    issue.field = ClassifyIssueField(message);
    issues.push_back(issue);
  }
  for (const std::string& message : result.warnings) {
    ValidationIssue issue;
    issue.severity = ValidationIssue::Severity::kWarning;
    issue.message = QString::fromStdString(message);
    issue.section_index = FindSectionIndex(project, message);
    issue.field = ClassifyIssueField(message);
    issues.push_back(issue);
  }

  return issues;
}

ValidationIssuesModel::ValidationIssuesModel(QObject* parent)
    : QAbstractListModel(parent) {}

// NOLINTNEXTLINE(google-default-arguments): base-signature default argument
int ValidationIssuesModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(issues_.size());
}

QVariant ValidationIssuesModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= static_cast<int>(issues_.size())) {
    return {};
  }

  const ValidationIssue& issue = issues_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case Qt::DisplayRole:
      return issue.message;
    case kSeverityRole:
      return static_cast<int>(issue.severity);
    case kSectionIndexRole:
      return issue.section_index;
    case kFieldRole:
      return static_cast<int>(issue.field);
    case Qt::ForegroundRole:
      return QBrush(theme_tokens::IssueSeverityColor(
          issue.severity == ValidationIssue::Severity::kError, dark_theme_));
    default:
      return {};
  }
}

void ValidationIssuesModel::SetDarkTheme(bool dark_theme) {
  if (dark_theme_ == dark_theme) {
    return;
  }
  dark_theme_ = dark_theme;
  if (!issues_.empty()) {
    emit dataChanged(index(0), index(static_cast<int>(issues_.size()) - 1),
                     {Qt::ForegroundRole});
  }
}

void ValidationIssuesModel::SetIssues(std::vector<ValidationIssue> issues) {
  beginResetModel();
  issues_ = std::move(issues);
  endResetModel();
}

int ValidationIssuesModel::error_count() const {
  return static_cast<int>(
      std::count_if(issues_.begin(), issues_.end(), [](const auto& issue) {
        return issue.severity == ValidationIssue::Severity::kError;
      }));
}

int ValidationIssuesModel::warning_count() const {
  return static_cast<int>(issues_.size()) - error_count();
}

}  // namespace videosynth::gui
