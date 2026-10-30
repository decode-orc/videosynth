/*
 * File:        section_list_model.cpp
 * Module:      gui
 * Purpose:     Table model of project sections for the sections dock
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "section_list_model.h"

#include "videosynth/clv_code_generator.h"
#include "videosynth/source_frame_range.h"

namespace videosynth::gui {

namespace {

// IEC 60856/60857: max picture number 99999, so a real value is always five
// digits; larger values are not truncated.
QString FormatCavPictureNumber(int picture_number) {
  return QStringLiteral("%1").arg(picture_number, 5, 10, QLatin1Char('0'));
}

QString FormatClvTimecodeText(const ClvTimecode& timecode) {
  return QStringLiteral("%1:%2:%3:%4")
      .arg(timecode.hours, 2, 10, QLatin1Char('0'))
      .arg(timecode.minutes, 2, 10, QLatin1Char('0'))
      .arg(timecode.seconds, 2, 10, QLatin1Char('0'))
      .arg(timecode.frames, 2, 10, QLatin1Char('0'));
}

}  // namespace

QString SectionListRow::DurationText() const {
  if (duration_all) {
    const QString all = range_frames > 0
                            ? QStringLiteral("all %1 frames").arg(range_frames)
                            : QStringLiteral("all frames");
    return duration_repeat > 1
               ? QStringLiteral("%1 x%2").arg(all).arg(duration_repeat)
               : all;
  }
  return QStringLiteral("%1 frames").arg(duration_frames);
}

std::vector<SectionListRow> BuildSectionListRows(const Project& project) {
  std::vector<SectionListRow> rows;
  rows.reserve(project.sections.size());

  // A 2:3 "all" section's output frames depend on where its cadence starts.
  const std::vector<int> cadence_start_slots = ResolveCadenceStartSlots(
      project.sections,
      std::vector<int>(project.sections.size(), kUnknownSourceFrameCount));
  int next_start_frame = 0;
  for (std::size_t i = 0; i < project.sections.size(); ++i) {
    const Section& section = project.sections[i];
    SectionListRow row;
    row.name = QString::fromStdString(section.name);
    row.type = QString::fromStdString(section.type);
    row.source = QString::fromStdString(section.source);
    row.start_frame = next_start_frame;
    row.duration_frames = section.duration_frames;
    row.duration_all = section.duration_frames_all;
    row.duration_repeat = section.duration_frames_repeat;
    row.output_frames = KnownSectionOutputFrameCount(
        section, kUnknownSourceFrameCount, cadence_start_slots[i]);
    SourceFrameRange range;
    if (section.duration_frames_all && section.end_frame_specified &&
        ResolveSourceFrameRange(section, kUnknownSourceFrameCount, &range,
                                nullptr)) {
      row.range_frames = range.count;
    }
    rows.push_back(row);

    // An open-ended 'all' resolves to the probed source length at generation
    // time; the recalculated display treats it as contributing nothing
    // (subsequent sections show the frames accumulated so far).
    if (row.output_frames > 0) {
      next_start_frame += static_cast<int>(row.output_frames);
    }
  }
  return rows;
}

QString FrameRangeText(int start_frame, int end_frame) {
  if (end_frame < start_frame) {
    return QStringLiteral("%1 – ?").arg(start_frame);
  }
  return QStringLiteral("%1 – %2").arg(start_frame).arg(end_frame);
}

QString DiscRangeTitle(DiscType disc_type) {
  switch (disc_type) {
    case DiscType::kCAV:
      return QStringLiteral("CAV picture numbers:");
    case DiscType::kCLV:
      return QStringLiteral("CLV timecode:");
    case DiscType::kUnknown:
      return {};
  }
  return {};
}

std::vector<int> BuildDiscFrameOffsets(const Project& project,
                                       DiscType disc_type) {
  std::vector<int> offsets(project.sections.size(), -1);
  if (disc_type == DiscType::kUnknown) {
    return offsets;
  }

  // Running 0-based disc frame offset; -1 until the first programme_area
  // section starts the count (IEC 60856/60857: lead-in carries no picture
  // numbers or timecodes, so its frames are never counted). CAV picture
  // numbers count pictures, so a 2:3 pulldown section advances the count once
  // per film frame rather than once per output frame.
  const std::vector<int> cadence_start_slots = ResolveCadenceStartSlots(
      project.sections,
      std::vector<int>(project.sections.size(), kUnknownSourceFrameCount));
  int counter = -1;
  for (std::size_t i = 0; i < project.sections.size(); ++i) {
    const Section& section = project.sections[i];
    if (section.section_type == SectionType::kProgrammeArea) {
      if (disc_type == DiscType::kCAV) {
        // An explicit picture_number start_value re-anchors the CAV count at
        // this section, overriding any continued counter (mirrors
        // BiphaseInjectionManager::InitializeSection).
        for (const Section::LineInjection& injection :
             section.line_injections) {
          for (const Section::LineInjectionCode& code : injection.codes) {
            if (code.code_type == "picture_number" &&
                code.start_value_specified) {
              counter = code.start_value - 1;
            }
          }
        }
      }
      if (counter < 0) {
        counter = 0;
      }
      offsets[i] = counter;
    }
    // Once started, the count runs across every later section (the engine's
    // timekeeping generators persist across boundaries); open-ended "all"
    // durations contribute nothing, as in BuildSectionListRows.
    const int64_t positions =
        (disc_type == DiscType::kCAV)
            ? KnownSectionPictureCount(section, kUnknownSourceFrameCount,
                                       cadence_start_slots[i])
            : KnownSectionOutputFrameCount(section, kUnknownSourceFrameCount);
    if (counter >= 0 && positions > 0) {
      counter += static_cast<int>(positions);
    }
  }
  return offsets;
}

QString DiscRangeText(DiscType disc_type, Standard standard, int start_offset,
                      int end_offset) {
  const bool end_known = end_offset >= start_offset;
  if (disc_type == DiscType::kCAV) {
    // Picture number == disc frame offset + 1 (numbering starts at 00001).
    const QString first = FormatCavPictureNumber(start_offset + 1);
    return end_known ? QStringLiteral("%1 – %2").arg(
                           first, FormatCavPictureNumber(end_offset + 1))
                     : QStringLiteral("%1 – ?").arg(first);
  }
  if (disc_type == DiscType::kCLV) {
    const QString first = FormatClvTimecodeText(
        ClvTimecodeForFrame(static_cast<std::size_t>(start_offset), standard));
    return end_known ? QStringLiteral("%1 – %2").arg(
                           first,
                           FormatClvTimecodeText(ClvTimecodeForFrame(
                               static_cast<std::size_t>(end_offset), standard)))
                     : QStringLiteral("%1 – ?").arg(first);
  }
  return {};
}

std::vector<SectionMoveStep> PlanMoveSectionsUp(const std::vector<int>& rows) {
  std::vector<SectionMoveStep> steps;
  // Rows packed against the top (or against already-pinned selected rows)
  // stay put; every other row shifts up one place. Ascending application
  // order keeps later rows' indices valid: a move only shuffles rows below
  // the ones still to come.
  int boundary = 0;
  for (const int row : rows) {
    if (row > boundary) {
      steps.push_back({row, row - 1});
    } else {
      boundary = row + 1;
    }
  }
  return steps;
}

std::vector<SectionMoveStep> PlanMoveSectionsDown(const std::vector<int>& rows,
                                                  int row_count) {
  std::vector<SectionMoveStep> steps;
  // Mirror of PlanMoveSectionsUp: walk from the bottom, pinning rows packed
  // against the end of the list, and apply in descending order.
  int boundary = row_count - 1;
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
    if (*it < boundary) {
      steps.push_back({*it, *it + 1});
    } else {
      boundary = *it - 1;
    }
  }
  return steps;
}

SectionListModel::SectionListModel(ProjectDocument* document, QObject* parent)
    : QAbstractTableModel(parent), document_(document) {
  rows_ = BuildSectionListRows(document_->project());

  const auto reload = [this] { Reload(); };
  connect(document_, &ProjectDocument::DocumentReset, this, reload);
  connect(document_, &ProjectDocument::SectionAdded, this, reload);
  connect(document_, &ProjectDocument::SectionRemoved, this, reload);
  connect(document_, &ProjectDocument::SectionMoved, this, reload);
  // An edit changes cells but not the row set, so refresh in place to keep the
  // dock's selection (a reset would deselect and close the section editor).
  connect(document_, &ProjectDocument::SectionEdited, this,
          [this] { RefreshRows(); });
}

// NOLINTNEXTLINE(google-default-arguments): base-signature default argument
int SectionListModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

// NOLINTNEXTLINE(google-default-arguments): base-signature default argument
int SectionListModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : kColumnCount;
}

QVariant SectionListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= static_cast<int>(rows_.size())) {
    return {};
  }
  if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
    return {};
  }

  const SectionListRow& row = rows_[static_cast<std::size_t>(index.row())];
  switch (index.column()) {
    case kNameColumn:
      return row.name;
    case kTypeColumn:
      return row.type;
    case kSourceColumn:
      return row.source;
    case kStartFrameColumn:
      return row.start_frame;
    case kDurationColumn:
      return row.DurationText();
    default:
      return {};
  }
}

QVariant SectionListModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
    return {};
  }
  switch (section) {
    case kNameColumn:
      return tr("Name");
    case kTypeColumn:
      return tr("Type");
    case kSourceColumn:
      return tr("Source");
    case kStartFrameColumn:
      return tr("Start");
    case kDurationColumn:
      return tr("Duration");
    default:
      return {};
  }
}

void SectionListModel::Reload() {
  beginResetModel();
  rows_ = BuildSectionListRows(document_->project());
  endResetModel();
}

void SectionListModel::RefreshRows() {
  std::vector<SectionListRow> rebuilt =
      BuildSectionListRows(document_->project());
  // A section edit cannot change the row count; if it somehow does, fall back
  // to a full reset to keep the model and view structurally consistent.
  if (rebuilt.size() != rows_.size()) {
    Reload();
    return;
  }
  rows_ = std::move(rebuilt);
  if (rows_.empty()) {
    return;
  }
  emit dataChanged(index(0, 0), index(rowCount() - 1, kColumnCount - 1));
}

}  // namespace videosynth::gui
