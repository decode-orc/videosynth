/*
 * File:        chroma_line_sign_editor.cpp
 * Module:      gui
 * Purpose:     Table editor for a section's chroma_line_sign regions (source
 *              rectangle and the U/V V-switch modes)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "chroma_line_sign_editor.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "section_block_presenters.h"

namespace videosynth::gui {

namespace {

enum RegionColumn {
  kXColumn = 0,
  kYColumn,
  kWidthColumn,
  kHeightColumn,
  kUColumn,
  kVColumn,
  kRegionColumnCount,
};

// Visible rows before the table scrolls.
constexpr int kVisibleRows = 4;
// Source raster width shared by every supported standard.
constexpr int kRasterWidth = 720;

}  // namespace

ChromaLineSignEditor::ChromaLineSignEditor(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  table_ = new QTableWidget(0, kRegionColumnCount, this);
  table_->setHorizontalHeaderLabels(
      {tr("X"), tr("Y"), tr("Width"), tr("Height"), tr("U"), tr("V")});
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setMaximumHeight(
      table_->horizontalHeader()->sizeHint().height() +
      (kVisibleRows * table_->verticalHeader()->defaultSectionSize()) +
      (2 * table_->frameWidth()));
  table_->setToolTip(
      tr("Rectangles of the source picture, in pixels, whose U or V is "
         "multiplied by each line's PAL V-switch sign. With V following the "
         "switch the transmitted V carrier is unswitched, so a PAL decoder "
         "sees V alternate line by line (a correct delay-line decoder shows "
         "grey); with U following it, U alternates (anti-PAL)."));
  layout->addWidget(table_);

  auto* buttons = new QHBoxLayout();
  add_button_ = new QPushButton(tr("Add region"), this);
  remove_button_ = new QPushButton(tr("Remove region"), this);
  buttons->addWidget(add_button_);
  buttons->addWidget(remove_button_);
  buttons->addStretch();
  layout->addLayout(buttons);

  connect(add_button_, &QPushButton::clicked, this,
          &ChromaLineSignEditor::OnAddRegion);
  connect(remove_button_, &QPushButton::clicked, this,
          &ChromaLineSignEditor::OnRemoveRegion);
  RebuildRows();
}

void ChromaLineSignEditor::SetRegions(
    const std::vector<ChromaLineSignRegion>& regions, int raster_height) {
  const bool raster_changed = raster_height != raster_height_;
  raster_height_ = raster_height;
  const bool rows_changed =
      static_cast<int>(regions.size()) != table_->rowCount();
  regions_ = regions;
  if (rows_changed || raster_changed) {
    RebuildRows();
  } else {
    SyncValues();
  }
}

void ChromaLineSignEditor::RebuildRows() {
  updating_ = true;
  table_->setRowCount(static_cast<int>(regions_.size()));
  const int max_rows = std::max(1, raster_height_);
  for (int row = 0; row < static_cast<int>(regions_.size()); ++row) {
    // Spin boxes commit on Enter or focus loss, not per keystroke, so a
    // half-typed coordinate is never committed (and validated) mid-entry.
    auto add_spin = [&](int column, int minimum, int maximum,
                        int ChromaLineSignRegion::* field) {
      auto* spin = new QSpinBox(table_);
      spin->setRange(minimum, maximum);
      spin->setKeyboardTracking(false);
      table_->setCellWidget(row, column, spin);
      connect(spin, &QSpinBox::valueChanged, this,
              [this, row, field](int value) {
                if (updating_) {
                  return;
                }
                regions_[static_cast<std::size_t>(row)].*field = value;
                emit RegionsEdited();
              });
    };
    add_spin(kXColumn, 0, kRasterWidth - 1, &ChromaLineSignRegion::x);
    add_spin(kYColumn, 0, max_rows - 1, &ChromaLineSignRegion::y);
    add_spin(kWidthColumn, 1, kRasterWidth, &ChromaLineSignRegion::width);
    add_spin(kHeightColumn, 1, max_rows, &ChromaLineSignRegion::height);

    auto add_mode = [&](int column,
                        ChromaLineSignMode ChromaLineSignRegion::* field) {
      auto* combo = new QComboBox(table_);
      combo->addItem(tr("Normal"),
                     static_cast<int>(ChromaLineSignMode::kNormal));
      combo->addItem(tr("Follow switch"),
                     static_cast<int>(ChromaLineSignMode::kFollowSwitch));
      table_->setCellWidget(row, column, combo);
      connect(
          combo, &QComboBox::activated, this, [this, row, field, combo](int) {
            if (updating_) {
              return;
            }
            regions_[static_cast<std::size_t>(row)].*field =
                static_cast<ChromaLineSignMode>(combo->currentData().toInt());
            emit RegionsEdited();
          });
    };
    add_mode(kUColumn, &ChromaLineSignRegion::u);
    add_mode(kVColumn, &ChromaLineSignRegion::v);
  }
  updating_ = false;
  SyncValues();
}

void ChromaLineSignEditor::SyncValues() {
  updating_ = true;
  for (int row = 0; row < static_cast<int>(regions_.size()); ++row) {
    const ChromaLineSignRegion& region =
        regions_[static_cast<std::size_t>(row)];
    const std::pair<int, int> spins[] = {{kXColumn, region.x},
                                         {kYColumn, region.y},
                                         {kWidthColumn, region.width},
                                         {kHeightColumn, region.height}};
    for (const auto& [column, value] : spins) {
      if (auto* spin =
              qobject_cast<QSpinBox*>(table_->cellWidget(row, column))) {
        spin->setValue(value);
      }
    }
    const std::pair<int, ChromaLineSignMode> modes[] = {{kUColumn, region.u},
                                                        {kVColumn, region.v}};
    for (const auto& [column, mode] : modes) {
      if (auto* combo =
              qobject_cast<QComboBox*>(table_->cellWidget(row, column))) {
        combo->setCurrentIndex(combo->findData(static_cast<int>(mode)));
      }
    }
  }
  remove_button_->setEnabled(!regions_.empty());
  updating_ = false;
}

void ChromaLineSignEditor::OnAddRegion() {
  const std::size_t before = regions_.size();
  AddChromaLineSignRegion(&regions_, raster_height_);
  if (regions_.size() == before) {
    return;
  }
  RebuildRows();
  table_->selectRow(table_->rowCount() - 1);
  emit RegionsEdited();
}

void ChromaLineSignEditor::OnRemoveRegion() {
  const int row =
      table_->currentRow() >= 0 ? table_->currentRow() : table_->rowCount() - 1;
  const std::size_t before = regions_.size();
  RemoveChromaLineSignRegion(&regions_, row);
  if (regions_.size() == before) {
    return;
  }
  RebuildRows();
  emit RegionsEdited();
}

}  // namespace videosynth::gui
