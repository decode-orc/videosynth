/*
 * File:        edit_project_dialog.cpp
 * Module:      gui
 * Purpose:     Modal dialog for editing dynamic project-level settings
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "edit_project_dialog.h"

#include <QDialogButtonBox>
#include <QScrollArea>
#include <QVBoxLayout>

namespace videosynth::gui {

EditProjectDialog::EditProjectDialog(const Project& project, QWidget* parent)
    : QDialog(parent), scratch_(new ProjectDocument(this)) {
  setWindowTitle(tr("Edit Project"));
  setModal(true);

  // Edit a private copy so Cancel discards cleanly; the standard is fixed at
  // creation time and shown read-only here.
  scratch_->ResetProject(project, QString());

  auto* layout = new QVBoxLayout(this);

  auto* scroll = new QScrollArea(this);
  scroll_ = scroll;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* content = new QWidget(scroll);
  auto* content_layout = new QVBoxLayout(content);

  settings_editor_ = new ProjectSettingsEditor(scratch_, content);
  settings_editor_->SetStandardEditable(false);
  content_layout->addWidget(settings_editor_);

  content_layout->addStretch();

  scroll->setWidget(content);
  layout->addWidget(scroll);

  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  resize(560, 680);
}

void EditProjectDialog::RevealField(IssueField field) {
  QWidget* group = settings_editor_->FieldGroup(field);
  if (group == nullptr) {
    return;
  }
  if (!isVisible()) {
    // Called before exec(): scroll once the dialog is shown and laid out.
    QMetaObject::invokeMethod(
        this, [this, field] { RevealField(field); }, Qt::QueuedConnection);
    return;
  }
  scroll_->ensureWidgetVisible(group);
  QWidget* focus = settings_editor_->FieldFocusWidget(field);
  (focus != nullptr && focus->isVisible() ? focus : group)
      ->setFocus(Qt::OtherFocusReason);
}

}  // namespace videosynth::gui
