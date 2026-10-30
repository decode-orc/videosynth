/*
 * File:        section_list_dock.h
 * Module:      gui
 * Purpose:     Sections dock content: ordered section list with add, remove,
 *              duplicate, and reorder operations
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QList>
#include <QWidget>
#include <vector>

#include "project_document.h"
#include "section_list_model.h"

class QTableView;
class QToolButton;

namespace videosynth::gui {

// Dock widget content for the ordered section list. Operations (add, remove,
// duplicate, move up/down) are routed through ProjectDocument commands;
// display sync comes from SectionListModel. The list allows extended
// (Ctrl/Shift) selection: Remove, Duplicate, and Up/Down act on every
// selected row (moves follow the PlanMoveSectionsUp/Down block semantics,
// reselecting the rows at their new positions).
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class SectionListDock : public QWidget {
  Q_OBJECT

 public:
  // Does not take ownership of `document`; it must outlive the dock.
  explicit SectionListDock(ProjectDocument* document,
                           QWidget* parent = nullptr);

  // Current (primary) section index, or -1.
  int current_section() const;

  // All selected section indices in ascending order (empty when none).
  QList<int> selected_sections() const;

  // Programmatic selection (issue navigation); emits CurrentSectionChanged.
  void SelectSection(int index);

 signals:
  void CurrentSectionChanged(int index);

  // Fires whenever the selected row set changes (including collapsing back to
  // a single row); carries the ascending selected section indices.
  void SelectedSectionsChanged(const QList<int>& indices);

 private:
  void OnAddSection();
  // Adds a 2:3 pulldown film section (525-line projects only).
  void OnAddFilmSection();
  void OnRemove();
  void OnDuplicate();
  void OnMoveUp();
  void OnMoveDown();

  void AddSection(Section section);
  // Selects rows [first, last] and makes `first` current.
  void SelectSectionRange(int first, int last);
  // The rows an Up/Down move acts on: the selection, or the current row.
  std::vector<int> RowsForMove() const;
  // Applies a reorder plan to the document and reselects the moved rows.
  void ApplyMovePlan(const std::vector<SectionMoveStep>& steps);
  void UpdateButtonStates();

  ProjectDocument* document_;
  SectionListModel* model_;
  QTableView* view_ = nullptr;
  QToolButton* remove_button_ = nullptr;
  QToolButton* duplicate_button_ = nullptr;
  QToolButton* up_button_ = nullptr;
  QToolButton* down_button_ = nullptr;
};

}  // namespace videosynth::gui
