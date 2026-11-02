/*
 * File:        chroma_line_sign_editor.h
 * Module:      gui
 * Purpose:     Table editor for a section's chroma_line_sign regions (source
 *              rectangle and the U/V V-switch modes)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QWidget>
#include <vector>

#include "videosynth/model.h"

class QPushButton;
class QTableWidget;

namespace videosynth::gui {

// Region table with add/remove buttons. Rows are added and removed through
// AddChromaLineSignRegion/RemoveChromaLineSignRegion
// (section_block_presenters), and every edit emits RegionsEdited; the owner
// reads regions() and commits them through its ProjectDocument command.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class ChromaLineSignEditor : public QWidget {
  Q_OBJECT

 public:
  explicit ChromaLineSignEditor(QWidget* parent = nullptr);

  // Shows `regions`, with rows limited to [0, raster_height). Emits nothing.
  // Rows are rebuilt only when the row count or the raster changes, so a
  // reload after an edit never destroys the cell widget being edited.
  void SetRegions(const std::vector<ChromaLineSignRegion>& regions,
                  int raster_height);
  const std::vector<ChromaLineSignRegion>& regions() const { return regions_; }

 signals:
  void RegionsEdited();

 private:
  void RebuildRows();
  void SyncValues();
  void OnAddRegion();
  void OnRemoveRegion();

  std::vector<ChromaLineSignRegion> regions_;
  int raster_height_ = 0;
  bool updating_ = false;

  QTableWidget* table_ = nullptr;
  QPushButton* add_button_ = nullptr;
  QPushButton* remove_button_ = nullptr;
};

}  // namespace videosynth::gui
