/*
 * File:        response_by_picture_editor.h
 * Module:      gui
 * Purpose:     Editor for the LaserVision replay response given at several
 *              disc positions (laserdisc.channel.response_by_picture)
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

class CurvePointsEditor;

// A table of disc positions (picture numbers) with add/remove buttons, and
// below it the [MHz, dB] response curve of the selected position. Entries are
// added and removed through AddResponseByPictureEntry and
// RemoveResponseByPictureEntry (project_settings_presenter); every edit emits
// EntriesEdited and the owner reads entries() and commits them.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class ResponseByPictureEditor : public QWidget {
  Q_OBJECT

 public:
  explicit ResponseByPictureEditor(QWidget* parent = nullptr);

  // Shows `entries`, with curve frequencies limited to
  // [0, max_frequency_mhz]. Emits nothing. Rows are rebuilt only when the
  // entry count changes, so a reload after an edit keeps the edited widget.
  void SetEntries(const std::vector<ResponseByPictureEntry>& entries,
                  double max_frequency_mhz);
  const std::vector<ResponseByPictureEntry>& entries() const {
    return entries_;
  }

 signals:
  void EntriesEdited();

 private:
  void RebuildRows();
  void SyncValues();
  // Shows the selected entry's curve (or an empty, disabled curve editor).
  void ShowSelectedCurve();
  int SelectedRow() const;
  void OnAddEntry();
  void OnRemoveEntry();

  std::vector<ResponseByPictureEntry> entries_;
  bool updating_ = false;

  QTableWidget* table_ = nullptr;
  QPushButton* remove_button_ = nullptr;
  CurvePointsEditor* curve_editor_ = nullptr;
};

}  // namespace videosynth::gui
