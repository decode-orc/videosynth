/*
 * File:        noise_level_curve_editor.h
 * Module:      gui
 * Purpose:     Table editor for the shared noise level term's curve
 *              breakpoints (luma level in mV, gain in dB)
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

// Breakpoint table with add/remove buttons for the shared noise layer's
// level-term curve. Rows are added and removed through
// AddNoiseLevelPoint/RemoveNoiseLevelPoint (section_block_presenters), and
// every edit emits CurveEdited; the owner reads curve() and commits it
// through its ProjectDocument command.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class NoiseLevelCurveEditor : public QWidget {
  Q_OBJECT

 public:
  explicit NoiseLevelCurveEditor(QWidget* parent = nullptr);

  // Shows `curve`. Emits nothing. Rows are rebuilt only when the row count
  // changes, so a reload after an edit never destroys the cell widget being
  // edited.
  void SetCurve(const std::vector<NoiseLevelPoint>& curve);
  const std::vector<NoiseLevelPoint>& curve() const { return curve_; }

 signals:
  void CurveEdited();

 private:
  void RebuildRows();
  void SyncValues();
  void OnAddPoint();
  void OnRemovePoint();

  std::vector<NoiseLevelPoint> curve_;
  bool updating_ = false;

  QTableWidget* table_ = nullptr;
  QPushButton* remove_button_ = nullptr;
};

}  // namespace videosynth::gui
