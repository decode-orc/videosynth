/*
 * File:        section_list_dock.cpp
 * Module:      gui
 * Purpose:     Sections dock content: ordered section list with add, remove,
 *              duplicate, and reorder operations
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "section_list_dock.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <utility>

#include "project_templates.h"
#include "section_block_presenters.h"

namespace videosynth::gui {

SectionListDock::SectionListDock(ProjectDocument* document, QWidget* parent)
    : QWidget(parent),
      document_(document),
      model_(new SectionListModel(document, this)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* buttons = new QHBoxLayout();
  auto* add_button = new QToolButton(this);
  add_button->setText(tr("Add"));
  // Clicking adds a plain section; the arrow offers the section templates.
  // The film template is offered only while the project is 525-line.
  auto* add_menu = new QMenu(add_button);
  QAction* add_section_action = add_menu->addAction(tr("Section"));
  QAction* add_film_action = add_menu->addAction(tr("Film (2:3 pulldown)"));
  connect(add_menu, &QMenu::aboutToShow, this, [this, add_film_action] {
    add_film_action->setVisible(SupportsFilmPulldown(
        document_->project().cvbs_presets.video_standard_preset));
  });
  connect(add_section_action, &QAction::triggered, this,
          &SectionListDock::OnAddSection);
  connect(add_film_action, &QAction::triggered, this,
          &SectionListDock::OnAddFilmSection);
  add_button->setMenu(add_menu);
  add_button->setPopupMode(QToolButton::MenuButtonPopup);

  remove_button_ = new QToolButton(this);
  remove_button_->setText(tr("Remove"));
  duplicate_button_ = new QToolButton(this);
  duplicate_button_->setText(tr("Duplicate"));
  up_button_ = new QToolButton(this);
  up_button_->setText(tr("Up"));
  down_button_ = new QToolButton(this);
  down_button_->setText(tr("Down"));

  buttons->addWidget(add_button);
  buttons->addWidget(remove_button_);
  buttons->addWidget(duplicate_button_);
  buttons->addStretch();
  buttons->addWidget(up_button_);
  buttons->addWidget(down_button_);
  layout->addLayout(buttons);

  view_ = new QTableView(this);
  view_->setModel(model_);
  view_->setSelectionBehavior(QAbstractItemView::SelectRows);
  // Ctrl/Shift multi-selection: the section editor mirrors edits made to the
  // current section onto every selected section.
  view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view_->horizontalHeader()->setStretchLastSection(true);
  view_->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  view_->verticalHeader()->setVisible(false);
  layout->addWidget(view_);

  connect(add_button, &QToolButton::clicked, this,
          &SectionListDock::OnAddSection);
  connect(remove_button_, &QToolButton::clicked, this,
          &SectionListDock::OnRemove);
  connect(duplicate_button_, &QToolButton::clicked, this,
          &SectionListDock::OnDuplicate);
  connect(up_button_, &QToolButton::clicked, this, &SectionListDock::OnMoveUp);
  connect(down_button_, &QToolButton::clicked, this,
          &SectionListDock::OnMoveDown);

  connect(view_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this](const QModelIndex& current, const QModelIndex&) {
            UpdateButtonStates();
            emit CurrentSectionChanged(current.isValid() ? current.row() : -1);
          });
  connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          [this](const QItemSelection&, const QItemSelection&) {
            UpdateButtonStates();
            emit SelectedSectionsChanged(selected_sections());
          });
  // Model resets clear the selection; keep button state consistent.
  connect(model_, &SectionListModel::modelReset, this, [this] {
    UpdateButtonStates();
    emit CurrentSectionChanged(current_section());
    emit SelectedSectionsChanged(selected_sections());
  });

  UpdateButtonStates();
}

int SectionListDock::current_section() const {
  const QModelIndex current = view_->currentIndex();
  return current.isValid() ? current.row() : -1;
}

QList<int> SectionListDock::selected_sections() const {
  QList<int> rows;
  const QModelIndexList selected = view_->selectionModel()->selectedRows();
  rows.reserve(selected.size());
  for (const QModelIndex& index : selected) {
    rows.append(index.row());
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

void SectionListDock::SelectSection(int index) {
  if (index < 0 || index >= model_->rowCount()) {
    view_->clearSelection();
    return;
  }
  view_->setCurrentIndex(model_->index(index, 0));
}

void SectionListDock::SelectSectionRange(int first, int last) {
  // setCurrentIndex clears any prior selection and makes `first` current;
  // the range select then extends the selection through `last`.
  view_->setCurrentIndex(model_->index(first, 0));
  if (last > first) {
    const QItemSelection selection(
        model_->index(first, 0),
        model_->index(last, model_->columnCount() - 1));
    view_->selectionModel()->select(
        selection,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  }
}

void SectionListDock::OnAddSection() {
  AddSection(MakeProgressiveSectionTemplate(
      document_->section_count() + 1,
      document_->project().cvbs_presets.video_standard_preset));
}

void SectionListDock::OnAddFilmSection() {
  AddSection(MakeFilmSectionTemplate(
      document_->section_count() + 1,
      document_->project().cvbs_presets.video_standard_preset));
}

void SectionListDock::OnRemove() {
  QList<int> rows = selected_sections();
  if (rows.isEmpty() && current_section() >= 0) {
    rows.append(current_section());
  }
  if (rows.isEmpty()) {
    return;
  }
  // A multi-row removal undoes as one step.
  std::optional<ScopedUndoBatch> batch;
  if (rows.size() > 1) {
    batch.emplace(document_, QStringLiteral("Remove sections"));
  }
  // Remove from the highest row down so earlier indices stay valid.
  for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
    document_->RemoveSection(*it);
  }
  batch.reset();
  SelectSection(qMin(rows.first(), model_->rowCount() - 1));
}

void SectionListDock::OnDuplicate() {
  QList<int> rows = selected_sections();
  if (rows.isEmpty() && current_section() >= 0) {
    rows.append(current_section());
  }
  if (rows.isEmpty()) {
    return;
  }
  // Copies are inserted as one block after the last selected row (keeping a
  // duplicated run of sections contiguous), then become the new selection.
  // A multi-row duplication undoes as one step.
  std::optional<ScopedUndoBatch> batch;
  if (rows.size() > 1) {
    batch.emplace(document_, QStringLiteral("Duplicate sections"));
  }
  const int insert_at = rows.last() + 1;
  int inserted = 0;
  for (int row : rows) {
    const Project& project = document_->project();
    Section duplicate = MakeDuplicateSection(
        project.sections[static_cast<std::size_t>(row)], project.sections);
    document_->InsertSection(insert_at + inserted, std::move(duplicate));
    ++inserted;
  }
  batch.reset();
  SelectSectionRange(insert_at, insert_at + inserted - 1);
}

std::vector<int> SectionListDock::RowsForMove() const {
  const QList<int> selected = selected_sections();
  if (!selected.isEmpty()) {
    return std::vector<int>(selected.cbegin(), selected.cend());
  }
  const int index = current_section();
  return index >= 0 ? std::vector<int>{index} : std::vector<int>{};
}

void SectionListDock::ApplyMovePlan(const std::vector<SectionMoveStep>& steps) {
  if (steps.empty()) {
    return;
  }
  const std::vector<int> rows = RowsForMove();
  const int current = current_section();

  // A multi-row move undoes as one step. Each document move resets the model
  // and clears the view's selection, so the selection is rebuilt at the rows'
  // new positions afterwards.
  std::optional<ScopedUndoBatch> batch;
  if (steps.size() > 1) {
    batch.emplace(document_, QStringLiteral("Move sections"));
  }
  for (const SectionMoveStep& step : steps) {
    document_->MoveSection(step.from, step.to);
  }
  batch.reset();

  const auto moved_to = [&steps](int row) {
    for (const SectionMoveStep& step : steps) {
      if (step.from == row) {
        return step.to;
      }
    }
    return row;  // Pinned against the boundary.
  };
  view_->setCurrentIndex(
      model_->index(moved_to(current >= 0 ? current : 0), 0));
  for (const int row : rows) {
    const int new_row = moved_to(row);
    view_->selectionModel()->select(
        QItemSelection(model_->index(new_row, 0),
                       model_->index(new_row, model_->columnCount() - 1)),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
  }
}

void SectionListDock::OnMoveUp() {
  ApplyMovePlan(PlanMoveSectionsUp(RowsForMove()));
}

void SectionListDock::OnMoveDown() {
  ApplyMovePlan(PlanMoveSectionsDown(RowsForMove(), model_->rowCount()));
}

void SectionListDock::AddSection(Section section) {
  document_->InsertSection(-1, std::move(section));
  SelectSection(model_->rowCount() - 1);
}

void SectionListDock::UpdateButtonStates() {
  const int index = current_section();
  const bool has_selection = view_->selectionModel()->hasSelection();
  // Every list operation acts on the whole selection; Up/Down disable when
  // the selected block is already packed against the corresponding edge.
  remove_button_->setEnabled(has_selection || index >= 0);
  duplicate_button_->setEnabled(has_selection || index >= 0);
  const std::vector<int> rows = RowsForMove();
  up_button_->setEnabled(!PlanMoveSectionsUp(rows).empty());
  down_button_->setEnabled(
      !PlanMoveSectionsDown(rows, model_->rowCount()).empty());
}

}  // namespace videosynth::gui
