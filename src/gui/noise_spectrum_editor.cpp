/*
 * File:        noise_spectrum_editor.cpp
 * Module:      gui
 * Purpose:     Table editor for a noise layer's spectrum breakpoints
 *              (frequency in MHz, relative level in dB)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "noise_spectrum_editor.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "section_block_presenters.h"

namespace videosynth::gui {

namespace {

enum SpectrumColumn {
  kFrequencyColumn = 0,
  kLevelColumn,
  kSpectrumColumnCount,
};

// Visible rows before the table scrolls.
constexpr int kVisibleRows = 4;

}  // namespace

NoiseSpectrumEditor::NoiseSpectrumEditor(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  table_ = new QTableWidget(0, kSpectrumColumnCount, this);
  table_->setHorizontalHeaderLabels({tr("Frequency"), tr("Level")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setMaximumHeight(
      table_->horizontalHeader()->sizeHint().height() +
      (kVisibleRows * table_->verticalHeader()->defaultSectionSize()) +
      (2 * table_->frameWidth()));
  table_->setToolTip(
      tr("Breakpoints of the noise power spectrum, joined by straight lines in "
         "dB against MHz; the end levels hold out to 0 Hz and to the Nyquist "
         "frequency. Only the shape matters: the noise is scaled back to the "
         "layer's level. An empty table gives white noise."));
  layout->addWidget(table_);

  auto* buttons = new QHBoxLayout();
  auto* add_button = new QPushButton(tr("Add point"), this);
  remove_button_ = new QPushButton(tr("Remove point"), this);
  buttons->addWidget(add_button);
  buttons->addWidget(remove_button_);
  buttons->addStretch();
  layout->addLayout(buttons);

  connect(add_button, &QPushButton::clicked, this,
          &NoiseSpectrumEditor::OnAddPoint);
  connect(remove_button_, &QPushButton::clicked, this,
          &NoiseSpectrumEditor::OnRemovePoint);
  RebuildRows();
}

void NoiseSpectrumEditor::SetSpectrum(
    const std::vector<NoiseSpectrumPoint>& spectrum, double max_frequency_mhz) {
  const bool limit_changed = max_frequency_mhz != max_frequency_mhz_;
  max_frequency_mhz_ = max_frequency_mhz;
  const bool rows_changed =
      static_cast<int>(spectrum.size()) != table_->rowCount();
  spectrum_ = spectrum;
  if (rows_changed || limit_changed) {
    RebuildRows();
  } else {
    SyncValues();
  }
}

void NoiseSpectrumEditor::RebuildRows() {
  updating_ = true;
  table_->setRowCount(static_cast<int>(spectrum_.size()));
  for (int row = 0; row < static_cast<int>(spectrum_.size()); ++row) {
    auto* frequency = new QDoubleSpinBox(table_);
    frequency->setRange(0.0, max_frequency_mhz_);
    frequency->setDecimals(2);
    frequency->setSingleStep(0.1);
    frequency->setSuffix(tr(" MHz"));
    // Commit on Enter or focus loss, not per keystroke: a half-typed
    // frequency would otherwise be committed (and validated) mid-entry.
    frequency->setKeyboardTracking(false);
    table_->setCellWidget(row, kFrequencyColumn, frequency);

    auto* level = new QDoubleSpinBox(table_);
    level->setRange(editor_limits::kNoiseSpectrumRelativeDbMin,
                    editor_limits::kNoiseSpectrumRelativeDbMax);
    level->setDecimals(1);
    level->setSuffix(tr(" dB"));
    level->setKeyboardTracking(false);
    table_->setCellWidget(row, kLevelColumn, level);

    connect(frequency, &QDoubleSpinBox::valueChanged, this,
            [this, row](double value) {
              if (updating_) {
                return;
              }
              spectrum_[static_cast<std::size_t>(row)].frequency_mhz = value;
              emit SpectrumEdited();
            });
    connect(level, &QDoubleSpinBox::valueChanged, this,
            [this, row](double value) {
              if (updating_) {
                return;
              }
              spectrum_[static_cast<std::size_t>(row)].relative_db = value;
              emit SpectrumEdited();
            });
  }
  updating_ = false;
  SyncValues();
}

void NoiseSpectrumEditor::SyncValues() {
  updating_ = true;
  for (int row = 0; row < static_cast<int>(spectrum_.size()); ++row) {
    const NoiseSpectrumPoint& point = spectrum_[static_cast<std::size_t>(row)];
    if (auto* frequency = qobject_cast<QDoubleSpinBox*>(
            table_->cellWidget(row, kFrequencyColumn))) {
      frequency->setValue(point.frequency_mhz);
    }
    if (auto* level = qobject_cast<QDoubleSpinBox*>(
            table_->cellWidget(row, kLevelColumn))) {
      level->setValue(point.relative_db);
    }
  }
  remove_button_->setEnabled(!spectrum_.empty());
  updating_ = false;
}

void NoiseSpectrumEditor::OnAddPoint() {
  const std::size_t before = spectrum_.size();
  AddNoiseSpectrumPoint(&spectrum_, max_frequency_mhz_);
  if (spectrum_.size() == before) {
    return;
  }
  RebuildRows();
  table_->selectRow(table_->rowCount() - 1);
  emit SpectrumEdited();
}

void NoiseSpectrumEditor::OnRemovePoint() {
  const int row =
      table_->currentRow() >= 0 ? table_->currentRow() : table_->rowCount() - 1;
  const std::size_t before = spectrum_.size();
  RemoveNoiseSpectrumPoint(&spectrum_, row);
  if (spectrum_.size() == before) {
    return;
  }
  RebuildRows();
  emit SpectrumEdited();
}

}  // namespace videosynth::gui
