/*
 * File:        noise_level_curve_editor.cpp
 * Module:      gui
 * Purpose:     Table editor for the shared noise level term's curve
 *              breakpoints (luma level in mV, gain in dB)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "noise_level_curve_editor.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "section_block_presenters.h"

namespace videosynth::gui {

namespace {

enum CurveColumn {
  kLevelColumn = 0,
  kGainColumn,
  kCurveColumnCount,
};

// Visible rows before the table scrolls.
constexpr int kVisibleRows = 4;

}  // namespace

NoiseLevelCurveEditor::NoiseLevelCurveEditor(QWidget* parent)
    : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  table_ = new QTableWidget(0, kCurveColumnCount, this);
  table_->setHorizontalHeaderLabels({tr("Level"), tr("Gain")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setMaximumHeight(
      table_->horizontalHeader()->sizeHint().height() +
      (kVisibleRows * table_->verticalHeader()->defaultSectionSize()) +
      (2 * table_->frameWidth()));
  table_->setToolTip(
      tr("Gain of the level term against the pressing picture's luma, in mV "
         "above blanking. Breakpoints are joined by straight lines in dB; the "
         "end gains hold beyond the first and last points. 0 dB is the "
         "term's own noise level."));
  layout->addWidget(table_);

  auto* buttons = new QHBoxLayout();
  auto* add_button = new QPushButton(tr("Add point"), this);
  remove_button_ = new QPushButton(tr("Remove point"), this);
  buttons->addWidget(add_button);
  buttons->addWidget(remove_button_);
  buttons->addStretch();
  layout->addLayout(buttons);

  connect(add_button, &QPushButton::clicked, this,
          &NoiseLevelCurveEditor::OnAddPoint);
  connect(remove_button_, &QPushButton::clicked, this,
          &NoiseLevelCurveEditor::OnRemovePoint);
  RebuildRows();
}

void NoiseLevelCurveEditor::SetCurve(
    const std::vector<NoiseLevelPoint>& curve) {
  const bool rows_changed =
      static_cast<int>(curve.size()) != table_->rowCount();
  curve_ = curve;
  if (rows_changed) {
    RebuildRows();
  } else {
    SyncValues();
  }
}

void NoiseLevelCurveEditor::RebuildRows() {
  updating_ = true;
  table_->setRowCount(static_cast<int>(curve_.size()));
  for (int row = 0; row < static_cast<int>(curve_.size()); ++row) {
    auto* level = new QDoubleSpinBox(table_);
    level->setRange(editor_limits::kNoiseLevelMvMin,
                    editor_limits::kNoiseLevelMvMax);
    level->setDecimals(1);
    level->setSuffix(tr(" mV"));
    // Commit on Enter or focus loss, not per keystroke: a half-typed level
    // would otherwise be committed (and validated) mid-entry.
    level->setKeyboardTracking(false);
    table_->setCellWidget(row, kLevelColumn, level);

    auto* gain = new QDoubleSpinBox(table_);
    gain->setRange(editor_limits::kNoiseLevelRelativeDbMin,
                   editor_limits::kNoiseLevelRelativeDbMax);
    gain->setDecimals(1);
    gain->setSuffix(tr(" dB"));
    gain->setKeyboardTracking(false);
    table_->setCellWidget(row, kGainColumn, gain);

    connect(level, &QDoubleSpinBox::valueChanged, this,
            [this, row](double value) {
              if (updating_) {
                return;
              }
              curve_[static_cast<std::size_t>(row)].level_mv = value;
              emit CurveEdited();
            });
    connect(gain, &QDoubleSpinBox::valueChanged, this,
            [this, row](double value) {
              if (updating_) {
                return;
              }
              curve_[static_cast<std::size_t>(row)].relative_db = value;
              emit CurveEdited();
            });
  }
  updating_ = false;
  SyncValues();
}

void NoiseLevelCurveEditor::SyncValues() {
  updating_ = true;
  for (int row = 0; row < static_cast<int>(curve_.size()); ++row) {
    const NoiseLevelPoint& point = curve_[static_cast<std::size_t>(row)];
    if (auto* level = qobject_cast<QDoubleSpinBox*>(
            table_->cellWidget(row, kLevelColumn))) {
      level->setValue(point.level_mv);
    }
    if (auto* gain = qobject_cast<QDoubleSpinBox*>(
            table_->cellWidget(row, kGainColumn))) {
      gain->setValue(point.relative_db);
    }
  }
  remove_button_->setEnabled(!curve_.empty());
  updating_ = false;
}

void NoiseLevelCurveEditor::OnAddPoint() {
  const std::size_t before = curve_.size();
  AddNoiseLevelPoint(&curve_);
  if (curve_.size() == before) {
    return;
  }
  RebuildRows();
  table_->selectRow(table_->rowCount() - 1);
  emit CurveEdited();
}

void NoiseLevelCurveEditor::OnRemovePoint() {
  const int row =
      table_->currentRow() >= 0 ? table_->currentRow() : table_->rowCount() - 1;
  const std::size_t before = curve_.size();
  RemoveNoiseLevelPoint(&curve_, row);
  if (curve_.size() == before) {
    return;
  }
  RebuildRows();
  emit CurveEdited();
}

}  // namespace videosynth::gui
