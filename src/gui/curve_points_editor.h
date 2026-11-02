/*
 * File:        curve_points_editor.h
 * Module:      gui
 * Purpose:     Generic breakpoint table editor for two-column curves such as
 *              replay responses, group delay and differential gain/phase
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QString>
#include <QWidget>
#include <vector>

#include "project_settings_presenter.h"

class QPushButton;
class QTableWidget;

namespace videosynth::gui {

// One column of a curve table: its header, the accepted range, and how the
// spin box presents values.
struct CurveAxis {
  QString title;
  double min = 0.0;
  double max = 0.0;
  int decimals = 2;
  double step = 0.1;
  QString suffix;
};

// Breakpoint table with add/remove buttons for a curve of (x, y) points.
// Rows are added and removed through AddCurvePoint/RemoveCurvePoint
// (project_settings_presenter), and every edit emits CurveEdited; the owner
// reads points() and commits them through its ProjectDocument command.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class CurvePointsEditor : public QWidget {
  Q_OBJECT

 public:
  // `x_step` is the abscissa spacing of a newly added breakpoint and `first`
  // the breakpoint added to an empty table.
  CurvePointsEditor(CurveAxis x_axis, CurveAxis y_axis, double x_step,
                    CurvePoint first, QWidget* parent = nullptr);

  // Re-limits the abscissa (e.g. the Nyquist frequency of a new standard).
  void SetXRange(double min, double max);

  // Shows `points`. Emits nothing. Rows are rebuilt only when the row count
  // or the abscissa range changes, so a reload after an edit never destroys
  // the cell widget being edited.
  void SetPoints(const std::vector<CurvePoint>& points);
  const std::vector<CurvePoint>& points() const { return points_; }

 signals:
  void CurveEdited();

 private:
  void RebuildRows();
  void SyncValues();
  void OnAddPoint();
  void OnRemovePoint();

  CurveAxis x_axis_;
  CurveAxis y_axis_;
  double x_step_;
  CurvePoint first_;
  std::vector<CurvePoint> points_;
  bool updating_ = false;

  QTableWidget* table_ = nullptr;
  QPushButton* remove_button_ = nullptr;
};

}  // namespace videosynth::gui
