/*
 * File:        response_by_picture_editor.cpp
 * Module:      gui
 * Purpose:     Editor for the LaserVision replay response given at several
 *              disc positions (laserdisc.channel.response_by_picture)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "response_by_picture_editor.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "curve_points_editor.h"
#include "project_settings_presenter.h"

namespace videosynth::gui {

namespace {

// Visible entry rows before the table scrolls.
constexpr int kVisibleRows = 3;
// Abscissa spacing of a newly added response breakpoint.
constexpr double kResponsePointStepMhz = 1.0;

}  // namespace

ResponseByPictureEditor::ResponseByPictureEditor(QWidget* parent)
    : QWidget(parent) {
  namespace limits = project_editor_limits;
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  table_ = new QTableWidget(0, 1, this);
  table_->setHorizontalHeaderLabels({tr("Picture")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setMaximumHeight(
      table_->horizontalHeader()->sizeHint().height() +
      (kVisibleRows * table_->verticalHeader()->defaultSectionSize()) +
      (2 * table_->frameWidth()));
  table_->setToolTip(
      tr("Disc positions, by picture number in ascending order. The response "
         "between two positions is interpolated; beyond the first and last "
         "it is held."));
  layout->addWidget(table_);

  auto* buttons = new QHBoxLayout();
  auto* add_button = new QPushButton(tr("Add position"), this);
  remove_button_ = new QPushButton(tr("Remove position"), this);
  buttons->addWidget(add_button);
  buttons->addWidget(remove_button_);
  buttons->addStretch();
  layout->addLayout(buttons);

  layout->addWidget(new QLabel(tr("Response at the selected position:"), this));
  curve_editor_ = new CurvePointsEditor(
      CurveAxis{tr("Frequency"), 0.0, 0.0, 2, 0.1, tr(" MHz")},
      CurveAxis{tr("Response"), limits::kReplayResponseMinDb,
                limits::kReplayResponseMaxDb, 1, 0.5, tr(" dB")},
      kResponsePointStepMhz, CurvePoint{0.5, 0.0}, this);
  layout->addWidget(curve_editor_);

  connect(add_button, &QPushButton::clicked, this,
          &ResponseByPictureEditor::OnAddEntry);
  connect(remove_button_, &QPushButton::clicked, this,
          &ResponseByPictureEditor::OnRemoveEntry);
  connect(table_, &QTableWidget::itemSelectionChanged, this,
          &ResponseByPictureEditor::ShowSelectedCurve);
  connect(table_, &QTableWidget::currentCellChanged, this,
          &ResponseByPictureEditor::ShowSelectedCurve);
  connect(curve_editor_, &CurvePointsEditor::CurveEdited, this, [this] {
    const int row = SelectedRow();
    if (updating_ || row < 0) {
      return;
    }
    entries_[static_cast<std::size_t>(row)].response_db =
        ToResponsePoints(curve_editor_->points());
    emit EntriesEdited();
  });
  RebuildRows();
}

void ResponseByPictureEditor::SetEntries(
    const std::vector<ResponseByPictureEntry>& entries,
    double max_frequency_mhz) {
  curve_editor_->SetXRange(0.0, max_frequency_mhz);
  const bool rows_changed =
      static_cast<int>(entries.size()) != table_->rowCount();
  entries_ = entries;
  if (rows_changed) {
    RebuildRows();
  } else {
    SyncValues();
  }
}

int ResponseByPictureEditor::SelectedRow() const {
  const int row = table_->currentRow();
  return (row >= 0 && row < static_cast<int>(entries_.size())) ? row : -1;
}

void ResponseByPictureEditor::RebuildRows() {
  namespace limits = project_editor_limits;
  updating_ = true;
  const int previous = table_->currentRow();
  table_->setRowCount(static_cast<int>(entries_.size()));
  for (int row = 0; row < static_cast<int>(entries_.size()); ++row) {
    auto* picture = new QSpinBox(table_);
    picture->setRange(limits::kResponsePictureMin, limits::kResponsePictureMax);
    picture->setKeyboardTracking(false);
    table_->setCellWidget(row, 0, picture);
    connect(picture, &QSpinBox::valueChanged, this, [this, row](int value) {
      if (updating_) {
        return;
      }
      entries_[static_cast<std::size_t>(row)].picture = value;
      emit EntriesEdited();
    });
  }
  if (!entries_.empty()) {
    const int last = static_cast<int>(entries_.size()) - 1;
    table_->setCurrentCell(previous >= 0 && previous <= last ? previous : 0, 0);
  }
  updating_ = false;
  SyncValues();
}

void ResponseByPictureEditor::SyncValues() {
  updating_ = true;
  for (int row = 0; row < static_cast<int>(entries_.size()); ++row) {
    if (auto* picture = qobject_cast<QSpinBox*>(table_->cellWidget(row, 0))) {
      picture->setValue(entries_[static_cast<std::size_t>(row)].picture);
    }
  }
  remove_button_->setEnabled(!entries_.empty());
  updating_ = false;
  ShowSelectedCurve();
}

void ResponseByPictureEditor::ShowSelectedCurve() {
  const bool was_updating = updating_;
  updating_ = true;
  const int row = SelectedRow();
  curve_editor_->setEnabled(row >= 0);
  curve_editor_->SetPoints(
      row >= 0
          ? ToCurvePoints(entries_[static_cast<std::size_t>(row)].response_db)
          : std::vector<CurvePoint>{});
  updating_ = was_updating;
}

void ResponseByPictureEditor::OnAddEntry() {
  const std::size_t before = entries_.size();
  AddResponseByPictureEntry(&entries_);
  if (entries_.size() == before) {
    return;
  }
  RebuildRows();
  table_->setCurrentCell(table_->rowCount() - 1, 0);
  emit EntriesEdited();
}

void ResponseByPictureEditor::OnRemoveEntry() {
  const int row = SelectedRow() >= 0 ? SelectedRow() : table_->rowCount() - 1;
  const std::size_t before = entries_.size();
  RemoveResponseByPictureEntry(&entries_, row);
  if (entries_.size() == before) {
    return;
  }
  RebuildRows();
  emit EntriesEdited();
}

}  // namespace videosynth::gui
