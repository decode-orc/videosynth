/*
 * File:        section_list_model.h
 * Module:      gui
 * Purpose:     Table model of project sections for the sections dock
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <cstdint>
#include <vector>

#include "project_document.h"
#include "videosynth/biphase_types.h"
#include "videosynth/model.h"

namespace videosynth::gui {

// Presentation row for one section: display strings plus the automatically
// recalculated start frame (cumulative sum of the preceding sections'
// durations, matching the pipeline's frame schedule).
struct SectionListRow {
  QString name;
  QString type;
  QString source;
  int start_frame = 0;
  int duration_frames = 0;
  bool duration_all = false;
  int duration_repeat = 1;
  // Output frames the section produces, or -1 while an "all" duration waits
  // for the source to be probed (see KnownSectionOutputFrameCount).
  int64_t output_frames = -1;
  // Length of an "all" section's selected source range when it is known
  // without probing (an explicit end_frame); 0 otherwise.
  int range_frames = 0;

  // "500 frames", "all frames", "all frames x3", or with a known range
  // "all 10 frames" / "all 10 frames x3".
  QString DurationText() const;
};

// Builds the presentation rows for a project, recalculating start frames
// from the ordered section durations. An "all" duration counts its selected
// range x repeat when the range has an explicit end_frame, and is
// open-ended otherwise (no probe data is consulted).
//
// Thread-safety: thread-safe (pure function).
std::vector<SectionListRow> BuildSectionListRows(const Project& project);

// Formats a section's 0-based output frame span as "start – end". An
// `end_frame` below `start_frame` means the end is not yet known (an "all
// source frames" duration awaiting a probe) and renders as "start – ?".
//
// Thread-safety: thread-safe (pure function).
QString FrameRangeText(int start_frame, int end_frame);

// Row label for the disc position range shown under the frame range:
// "CAV picture numbers:" or "CLV timecode:". Empty for a non-laserdisc
// project (DiscType::kUnknown).
//
// Thread-safety: thread-safe (pure function).
QString DiscRangeTitle(DiscType disc_type);

// Per-section 0-based disc-position frame offsets: how far each section's
// first frame sits into the CAV picture-number / CLV timecode sequence.
// IEC 60856/60857: picture numbers and programme timecodes exist in the
// programme area only, so numbering anchors at the first programme_area
// section (lead_in frames are not counted) and lead_in / lead_out / untyped
// sections yield -1 (no disc position). Once numbering has started the
// count keeps running across every later section — mirroring the generation
// engine, whose timekeeping generators persist across section boundaries —
// but only programme_area sections receive an offset. A CAV programme_area
// section whose picture_number injection code specifies an explicit
// start_value re-anchors the count there. Open-ended "all source frames"
// durations (no end_frame) contribute no frames, matching BuildSectionListRows'
// open-ended treatment. All -1 for a non-laserdisc project.
//
// Thread-safety: thread-safe (pure function).
std::vector<int> BuildDiscFrameOffsets(const Project& project,
                                       DiscType disc_type);

// Formats a section's disc-position span (0-based frame offsets from
// BuildDiscFrameOffsets). CAV: five-digit picture numbers, offset 0 =
// picture 00001 ("00001 – 00250"). CLV: programme timecodes "HH:MM:SS:FF"
// at the standard's nominal CLV rate (PAL: 25 fps, NTSC: 30 fps), offset
// 0 = 00:00:00:00. An `end_offset` below `start_offset` renders the end as
// "?". Empty for a non-laserdisc project.
//
// Thread-safety: thread-safe (pure function).
QString DiscRangeText(DiscType disc_type, Standard standard, int start_offset,
                      int end_offset);

// One step of a multi-row reorder: MoveSection(from, to) on the document.
struct SectionMoveStep {
  int from = 0;
  int to = 0;
};

inline bool operator==(const SectionMoveStep& a, const SectionMoveStep& b) {
  return a.from == b.from && a.to == b.to;
}

// Plans a one-row move up/down for a multi-row selection. `rows` holds the
// selected row indices in ascending order; `row_count` is the list length.
// Every selected row shifts one place unless it is pinned against the list
// edge or against a contiguous run of selected rows already at the edge, so
// a block keeps its internal order and stops cleanly at the boundary. The
// returned steps are ordered so that applying them sequentially via
// MoveSection never disturbs a later step's indices (ascending for up,
// descending for down). An empty plan means nothing can move (drives the
// Up/Down button enablement).
//
// Thread-safety: thread-safe (pure functions).
std::vector<SectionMoveStep> PlanMoveSectionsUp(const std::vector<int>& rows);
std::vector<SectionMoveStep> PlanMoveSectionsDown(const std::vector<int>& rows,
                                                  int row_count);

// Read-only table model over the document's ordered section list. Stays in
// sync with the document by listening to its granular change signals; edits
// flow the other way through ProjectDocument commands (see SectionListDock).
//
// Thread-safety: NOT thread-safe. GUI (owning) thread only.
class SectionListModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column {
    kNameColumn = 0,
    kTypeColumn,
    kSourceColumn,
    kStartFrameColumn,
    kDurationColumn,
    kColumnCount,
  };

  // Does not take ownership of `document`; it must outlive the model.
  explicit SectionListModel(ProjectDocument* document,
                            QObject* parent = nullptr);

  // Default arguments mirror the QAbstractItemModel base signatures.
  // NOLINTNEXTLINE(google-default-arguments)
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  // NOLINTNEXTLINE(google-default-arguments)
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;

 private:
  // Structural change (add/remove/move/reset): rebuild rows and reset the
  // model. Resets clear the view's selection, so this is reserved for changes
  // that alter the row set.
  void Reload();
  // In-place edit of an existing section: rebuild rows and emit dataChanged
  // without a model reset, so the view keeps its current selection. The row
  // count is unchanged by a section edit (only its cells, and the derived
  // start frames of the rows below it).
  void RefreshRows();

  ProjectDocument* document_;
  std::vector<SectionListRow> rows_;
};

}  // namespace videosynth::gui
