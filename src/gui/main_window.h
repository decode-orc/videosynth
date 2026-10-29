/*
 * File:        main_window.h
 * Module:      gui
 * Purpose:     Application main window shell with menus and About dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QCloseEvent>
#include <QMainWindow>
#include <QModelIndex>
#include <QStringList>

#include "generation_controller.h"
#include "log_message_model.h"
#include "logging_options.h"
#include "preview_pane.h"
#include "project_document.h"
#include "section_editor.h"
#include "section_list_dock.h"
#include "source_probe_controller.h"
#include "theme_controller.h"
#include "validation_controller.h"
#include "validation_issues_model.h"
#include "welcome_page.h"

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QProgressBar;
class QStackedWidget;

namespace videosynth::gui {

// Main window shell: menu bar (File / Edit / Project / Generate / View /
// Help), status bar, and docks (sections, preview, line scope, issues, log).
// The central area is a stack that shows a welcome surface when no project is
// open and the per-section editor once one is. Project creation runs through
// the New Project dialog and project-level settings through the Edit Project
// dialog. The signal preview and its line scope are two docks on the right,
// sharing one PreviewPane so they track the same frame; the Log dock starts
// hidden to leave the editor more room. Generation runs in-process on a worker
// thread through GenerationController with progress, logging, and
// cancellation.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  // Does not take ownership of `theme_controller`; it must outlive the
  // window. `logging_overrides` carries the command-line --log-level/
  // --log-file values, applied over the persisted preferences for every
  // generation run this session.
  explicit MainWindow(ThemeController* theme_controller,
                      const LoggingOptions& logging_overrides = {},
                      QWidget* parent = nullptr);

  ProjectDocument* document() { return document_; }

 signals:
  // Issue-activation hook: emitted when the user double-clicks an issue in
  // the dock. -1 means a project-level issue.
  void IssueNavigationRequested(int section_index);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void OnAbout();
  void OnNewProject();
  void OnOpenProject();
  bool OnSave();
  bool OnSaveAs();
  void OnEditProject();
  void OnIssueActivated(const QModelIndex& index);
  void OnPreferences();
  void OnGenerate();
  void OnGenerationFinished(GenerationController::RunStatus status);

 private:
  // Opens the Edit Project dialog revealing `field` (validation issue
  // navigation); IssueField::kNone opens it at the top.
  void EditProject(IssueField field);
  void BuildMenus();
  void BuildCentralArea();
  void BuildSectionsDock();
  void BuildPreviewDocks();
  void BuildIssuesDock();
  void BuildLogDock();
  void BuildGenerationStatusWidgets();
  void ConnectGenerationController();
  void ShowGenerationSummary();
  void RestoreWindowGeometry();

  // Enables/disables the project-dependent actions and docks and selects the
  // welcome or section-editor page to match the document's open state.
  void UpdateProjectOpenState();
  // Refreshes the Edit menu's Undo/Redo enablement and step descriptions.
  void UpdateUndoActions();

  // Returns false when the user cancels an unsaved-changes prompt.
  bool MaybeSave();
  bool SaveToPath(const QString& path);
  void LoadProjectFromFile(const QString& path);
  void UpdateWindowTitle();
  void UpdateValidationStatus();
  // Shows a modal summarising the outcome of a user-initiated validation
  // (pass or fail). Only invoked for explicit Validate actions, never for the
  // debounced background validation that keeps the Issues dock current.
  void ShowExplicitValidationResult();
  QStringList RecentFilePaths() const;
  void UpdateRecentFilesMenu();
  void AddToRecentFiles(const QString& path);

  ThemeController* theme_controller_;
  // Command-line logging overrides applied to every generation run.
  LoggingOptions logging_overrides_;
  ProjectDocument* document_;
  ValidationController* validation_controller_;
  ValidationIssuesModel* issues_model_;
  SourceProbeController* probe_controller_;
  GenerationController* generation_controller_;
  LogMessageModel* log_model_;
  QMenu* recent_files_menu_ = nullptr;
  QLabel* validation_status_label_ = nullptr;
  // Set when the user triggers Validate from a menu; consumed by the next
  // completed validation run to show the pass/fail result modal.
  bool explicit_validation_requested_ = false;
  QProgressBar* generation_progress_bar_ = nullptr;
  QLabel* preview_status_label_ = nullptr;
  // Set while the preview's own frame-to-section report drives the list
  // selection, so the selection handler does not move the navigator back.
  bool selecting_section_from_preview_ = false;

  // Project-dependent actions, disabled while no project is open.
  QAction* save_action_ = nullptr;
  QAction* save_as_action_ = nullptr;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  QAction* edit_project_action_ = nullptr;
  QAction* project_validate_action_ = nullptr;
  QAction* generate_action_ = nullptr;
  QAction* cancel_generation_action_ = nullptr;

  QStackedWidget* central_stack_ = nullptr;
  WelcomePage* welcome_page_ = nullptr;
  SectionEditor* section_editor_ = nullptr;
  PreviewPane* preview_pane_ = nullptr;
  SectionListDock* section_list_dock_ = nullptr;
  QDockWidget* sections_dock_ = nullptr;
  QDockWidget* preview_dock_ = nullptr;
  QDockWidget* scope_dock_ = nullptr;
  QDockWidget* issues_dock_ = nullptr;
  QDockWidget* log_dock_ = nullptr;

  // Output artefacts of the most recently started run, for the completion
  // summary dialog.
  QStringList last_run_artefacts_;
};

}  // namespace videosynth::gui
