/*
 * File:        edit_project_dialog.h
 * Module:      gui
 * Purpose:     Modal dialog for editing dynamic project-level settings
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QDialog>

class QScrollArea;

#include "project_document.h"
#include "project_settings_editor.h"
#include "validation_issues_model.h"
#include "videosynth/model.h"

namespace videosynth::gui {

// Edit Project dialog. Presents the project-settings form for an
// already-created project. The video standard is shown read-only (it is fixed
// at creation). Edits are staged into
// a private scratch ProjectDocument seeded with a copy of the current project;
// on OK the caller reads project() and applies the changed settings to the
// real document, and on Cancel they are discarded.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class EditProjectDialog : public QDialog {
  Q_OBJECT

 public:
  // Copies `project` into the dialog's scratch document.
  explicit EditProjectDialog(const Project& project, QWidget* parent = nullptr);

  // The edited project settings; only meaningful after accept.
  Project project() const { return scratch_->project(); }

  // Scrolls the group holding a project-level `field` (kChannel,
  // kChromaError) into view and focuses it; other fields change nothing.
  // Before the dialog is shown, the reveal waits until it is.
  void RevealField(IssueField field);

 private:
  ProjectDocument* scratch_;
  QScrollArea* scroll_ = nullptr;
  ProjectSettingsEditor* settings_editor_ = nullptr;
};

}  // namespace videosynth::gui
