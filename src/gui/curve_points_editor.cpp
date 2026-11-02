/*
 * File:        curve_points_editor.cpp
 * Module:      gui
 * Purpose:     Generic breakpoint table editor for two-column curves such as
 *              replay responses, group delay and differential gain/phase
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "curve_points_editor.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <utility>

namespace videosynth::gui {

namespace {

enum CurveColumn {
  kXColumn = 0,
  kYColumn,
  kCurveColumnCount,
};

// Visible rows before the table scrolls.
constexpr int kVisibleRows = 4;

QDoubleSpinBox* MakeAxisSpin(const CurveAxis& axis, QWidget* parent) {
  auto* spin = new QDoubleSpinBox(parent);
  spin->setRange(axis.min, axis.max);
  spin->setDecimals(axis.decimals);
  spin->setSingleStep(axis.step);
  spin->setSuffix(axis.suffix);
  // Commit on Enter or focus loss, not per keystroke: a half-typed value
  // would otherwise be committed (and validated) mid-entry.
  spin->setKeyboardTracking(false);
  return spin;
}

}  // namespace

CurvePointsEditor::CurvePointsEditor(CurveAxis x_axis, CurveAxis y_axis,
                                     double x_step, CurvePoint first,
                                     QWidget* parent)
    : QWidget(parent),
      x_axis_(std::move(x_axis)),
      y_axis_(std::move(y_axis)),
      x_step_(x_step),
      first_(first) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  table_ = new QTableWidget(0, kCurveColumnCount, this);
  table_->setHorizontalHeaderLabels({x_axis_.title, y_axis_.title});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setMaximumHeight(
      table_->horizontalHeader()->sizeHint().height() +
      (kVisibleRows * table_->verticalHeader()->defaultSectionSize()) +
      (2 * table_->frameWidth()));
  layout->addWidget(table_);

  auto* buttons = new QHBoxLayout();
  auto* add_button = new QPushButton(tr("Add point"), this);
  remove_button_ = new QPushButton(tr("Remove point"), this);
  buttons->addWidget(add_button);
  buttons->addWidget(remove_button_);
  buttons->addStretch();
  layout->addLayout(buttons);

  connect(add_button, &QPushButton::clicked, this,
          &CurvePointsEditor::OnAddPoint);
  connect(remove_button_, &QPushButton::clicked, this,
          &CurvePointsEditor::OnRemovePoint);
  RebuildRows();
}

void CurvePointsEditor::SetXRange(double min, double max) {
  if (min == x_axis_.min && max == x_axis_.max) {
    return;
  }
  x_axis_.min = min;
  x_axis_.max = max;
  RebuildRows();
}

void CurvePointsEditor::SetPoints(const std::vector<CurvePoint>& points) {
  const bool rows_changed =
      static_cast<int>(points.size()) != table_->rowCount();
  points_ = points;
  if (rows_changed) {
    RebuildRows();
  } else {
    SyncValues();
  }
}

void CurvePointsEditor::RebuildRows() {
  updating_ = true;
  table_->setRowCount(static_cast<int>(points_.size()));
  for (int row = 0; row < static_cast<int>(points_.size()); ++row) {
    auto* x_spin = MakeAxisSpin(x_axis_, table_);
    table_->setCellWidget(row, kXColumn, x_spin);
    auto* y_spin = MakeAxisSpin(y_axis_, table_);
    table_->setCellWidget(row, kYColumn, y_spin);

    connect(x_spin, &QDoubleSpinBox::valueChanged, this,
            [this, row](double value) {
              if (updating_) {
                return;
              }
              points_[static_cast<std::size_t>(row)].x = value;
              emit CurveEdited();
            });
    connect(y_spin, &QDoubleSpinBox::valueChanged, this,
            [this, row](double value) {
              if (updating_) {
                return;
              }
              points_[static_cast<std::size_t>(row)].y = value;
              emit CurveEdited();
            });
  }
  updating_ = false;
  SyncValues();
}

void CurvePointsEditor::SyncValues() {
  updating_ = true;
  for (int row = 0; row < static_cast<int>(points_.size()); ++row) {
    const CurvePoint& point = points_[static_cast<std::size_t>(row)];
    if (auto* x_spin =
            qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row, kXColumn))) {
      x_spin->setValue(point.x);
    }
    if (auto* y_spin =
            qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row, kYColumn))) {
      y_spin->setValue(point.y);
    }
  }
  remove_button_->setEnabled(!points_.empty());
  updating_ = false;
}

void CurvePointsEditor::OnAddPoint() {
  const std::size_t before = points_.size();
  AddCurvePoint(&points_, x_step_, x_axis_.max, first_);
  if (points_.size() == before) {
    return;
  }
  RebuildRows();
  table_->selectRow(table_->rowCount() - 1);
  emit CurveEdited();
}

void CurvePointsEditor::OnRemovePoint() {
  const int row =
      table_->currentRow() >= 0 ? table_->currentRow() : table_->rowCount() - 1;
  const std::size_t before = points_.size();
  RemoveCurvePoint(&points_, row);
  if (points_.size() == before) {
    return;
  }
  RebuildRows();
  emit CurveEdited();
}

}  // namespace videosynth::gui
