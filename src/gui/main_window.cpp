/*
 * File:        main_window.cpp
 * Module:      gui
 * Purpose:     Application main window shell with menus and About dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "main_window.h"

#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <vector>

#include "asset_roots.h"
#include "edit_project_dialog.h"
#include "generation_preferences.h"
#include "new_project_dialog.h"
#include "preferences_dialog.h"
#include "videosynth/audio_wav_writer.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"
#include "videosynth_version.h"

namespace videosynth::gui {

namespace {

constexpr const char* kGeometrySettingsKey = "main_window/geometry";
constexpr const char* kRecentFilesSettingsKey = "main_window/recent_files";
constexpr int kMaxRecentFiles = 10;

QString ProjectFileFilter() {
  return MainWindow::tr("VideoSynth projects (*.yaml *.yml);;All files (*)");
}

// True when at least one section enables the given optional capability, so
// the completion summary only lists artefacts the run actually produced.
bool AnySectionEnablesDropouts(const Project& project) {
  for (const Section& section : project.sections) {
    if (section.dropouts.random.enabled || section.dropouts.scratch.enabled) {
      return true;
    }
  }
  return false;
}

}  // namespace

MainWindow::MainWindow(ThemeController* theme_controller,
                       const LoggingOptions& logging_overrides, QWidget* parent)
    : QMainWindow(parent),
      theme_controller_(theme_controller),
      logging_overrides_(logging_overrides),
      document_(new ProjectDocument(this)),
      validation_controller_(new ValidationController({}, this)),
      issues_model_(new ValidationIssuesModel(this)),
      probe_controller_(new SourceProbeController({}, this)),
      generation_controller_(new GenerationController({}, this)),
      log_model_(
          new LogMessageModel(LogMessageModel::kDefaultMaxEntries, this)) {
  setWindowIcon(QIcon(QStringLiteral(":/videosynth-gui/icon.png")));

  // The right-hand column (preview and line scope) runs the full window height;
  // the issues and log docks keep the bottom strip to themselves.
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

  // The status bar is built first: the preview docks report their status into
  // it as soon as they are constructed.
  statusBar()->showMessage(tr("Ready"));
  BuildGenerationStatusWidgets();
  preview_status_label_ = new QLabel(this);
  statusBar()->addPermanentWidget(preview_status_label_);
  validation_status_label_ = new QLabel(this);
  statusBar()->addPermanentWidget(validation_status_label_);

  BuildCentralArea();
  BuildSectionsDock();
  BuildPreviewDocks();
  BuildIssuesDock();
  BuildLogDock();
  BuildMenus();
  ConnectGenerationController();

  connect(document_, &ProjectDocument::DocumentChanged, this, [this] {
    validation_controller_->RequestValidation(document_->project());
  });
  connect(document_, &ProjectDocument::DocumentReset, this, [this] {
    // A pending explicit-validation request belongs to the previous document.
    explicit_validation_requested_ = false;
    UpdateProjectOpenState();
    if (document_->is_open()) {
      validation_controller_->RequestValidation(document_->project());
    } else {
      issues_model_->SetIssues({});
      UpdateValidationStatus();
    }
    section_editor_->SetCurrentSection(section_list_dock_->current_section());
    UpdateWindowTitle();
  });
  connect(document_, &ProjectDocument::ModifiedStateChanged, this,
          [this](bool modified) { setWindowModified(modified); });
  connect(document_, &ProjectDocument::FilePathChanged, this,
          [this](const QString&) { UpdateWindowTitle(); });
  connect(validation_controller_, &ValidationController::IssuesChanged, this,
          [this] {
            issues_model_->SetIssues(validation_controller_->issues());
            UpdateValidationStatus();
            if (explicit_validation_requested_) {
              // Defer to the event loop: latest-wins means the controller may
              // immediately restart a queued run after this signal, so only
              // treat the result as final once it has settled to idle.
              QTimer::singleShot(0, this, [this] {
                if (!explicit_validation_requested_ ||
                    validation_controller_->is_validating()) {
                  return;
                }
                explicit_validation_requested_ = false;
                ShowExplicitValidationResult();
              });
            }
          });
  issues_model_->SetDarkTheme(theme_controller_->is_dark());
  log_model_->SetDarkTheme(theme_controller_->is_dark());
  connect(theme_controller_, &ThemeController::ThemeChanged, this,
          [this](bool is_dark) {
            issues_model_->SetDarkTheme(is_dark);
            log_model_->SetDarkTheme(is_dark);
          });

  // Start with nothing loaded: the welcome page is shown and project-dependent
  // actions are disabled until the user creates or opens a project.
  UpdateProjectOpenState();
  UpdateWindowTitle();
  RestoreWindowGeometry();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (generation_controller_->is_running()) {
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, tr("Generation In Progress"),
        tr("A generation run is in progress.\nCancel it and quit?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
      event->ignore();
      return;
    }
    // The controller destructor joins the worker; the pipeline stops at its
    // next cancellation check and removes in-progress output files.
    generation_controller_->RequestCancellation();
  }

  if (!MaybeSave()) {
    event->ignore();
    return;
  }

  QSettings settings;
  settings.setValue(QLatin1String(kGeometrySettingsKey), saveGeometry());
  QMainWindow::closeEvent(event);
}

void MainWindow::BuildMenus() {
  QMenu* file_menu = menuBar()->addMenu(tr("&File"));
  file_menu->addAction(tr("&New Project…"), QKeySequence::New, this,
                       &MainWindow::OnNewProject);
  file_menu->addAction(tr("&Open Project…"), QKeySequence::Open, this,
                       &MainWindow::OnOpenProject);
  recent_files_menu_ = file_menu->addMenu(tr("Open &Recent"));
  UpdateRecentFilesMenu();
  file_menu->addSeparator();
  save_action_ = file_menu->addAction(tr("&Save"), QKeySequence::Save, this,
                                      &MainWindow::OnSave);
  save_as_action_ = file_menu->addAction(tr("Save &As…"), QKeySequence::SaveAs,
                                         this, &MainWindow::OnSaveAs);
  file_menu->addSeparator();
  file_menu->addAction(tr("E&xit"), QKeySequence::Quit, this,
                       &MainWindow::close);

  QMenu* edit_menu = menuBar()->addMenu(tr("&Edit"));
  undo_action_ = edit_menu->addAction(tr("&Undo"), QKeySequence::Undo, this,
                                      [this] { document_->Undo(); });
  redo_action_ = edit_menu->addAction(tr("&Redo"), QKeySequence::Redo, this,
                                      [this] { document_->Redo(); });
  connect(document_, &ProjectDocument::UndoStateChanged, this,
          &MainWindow::UpdateUndoActions);
  UpdateUndoActions();
  edit_menu->addSeparator();
  edit_menu->addAction(tr("&Preferences…"), QKeySequence::Preferences, this,
                       &MainWindow::OnPreferences);

  QMenu* project_menu = menuBar()->addMenu(tr("&Project"));
  edit_project_action_ = project_menu->addAction(tr("&Edit Project…"), this,
                                                 &MainWindow::OnEditProject);
  project_validate_action_ =
      project_menu->addAction(tr("&Validate"), this, [this] {
        explicit_validation_requested_ = true;
        validation_controller_->RequestValidation(document_->project());
      });

  QMenu* generate_menu = menuBar()->addMenu(tr("&Generate"));
  generate_action_ = generate_menu->addAction(
      tr("&Generate"), QKeySequence(Qt::CTRL | Qt::Key_G), this,
      &MainWindow::OnGenerate);
  cancel_generation_action_ =
      generate_menu->addAction(tr("&Cancel Generation"), this, [this] {
        generation_controller_->RequestCancellation();
        statusBar()->showMessage(tr("Cancelling generation…"));
      });
  cancel_generation_action_->setEnabled(false);

  QMenu* view_menu = menuBar()->addMenu(tr("&View"));

  // Toggle actions for the dockable panels. Each action is checkable, shows or
  // hides its dock, and auto-unchecks when the dock is closed via its title-bar
  // button, so the menu always reflects the current layout.
  QMenu* panels_menu = view_menu->addMenu(tr("&Panels"));
  panels_menu->addAction(sections_dock_->toggleViewAction());
  panels_menu->addAction(preview_dock_->toggleViewAction());
  panels_menu->addAction(scope_dock_->toggleViewAction());
  panels_menu->addAction(issues_dock_->toggleViewAction());
  panels_menu->addAction(log_dock_->toggleViewAction());
  view_menu->addSeparator();

  QMenu* theme_menu = view_menu->addMenu(tr("&Theme"));
  auto* theme_group = new QActionGroup(theme_menu);
  theme_group->setExclusive(true);

  const struct {
    ThemeManager::Mode mode;
    QString label;
  } theme_entries[] = {
      {ThemeManager::Mode::kAuto, tr("&Auto")},
      {ThemeManager::Mode::kLight, tr("&Light")},
      {ThemeManager::Mode::kDark, tr("&Dark")},
  };

  for (const auto& entry : theme_entries) {
    QAction* action = theme_menu->addAction(entry.label);
    action->setCheckable(true);
    action->setChecked(theme_controller_->mode() == entry.mode);
    theme_group->addAction(action);
    const ThemeManager::Mode mode = entry.mode;
    connect(action, &QAction::triggered, this,
            [this, mode] { theme_controller_->SetMode(mode); });
  }

  QMenu* help_menu = menuBar()->addMenu(tr("&Help"));
  help_menu->addAction(tr("&About"), this, &MainWindow::OnAbout);
}

void MainWindow::BuildCentralArea() {
  central_stack_ = new QStackedWidget(this);

  welcome_page_ = new WelcomePage(central_stack_);
  connect(welcome_page_, &WelcomePage::NewProjectRequested, this,
          &MainWindow::OnNewProject);
  connect(welcome_page_, &WelcomePage::OpenProjectRequested, this,
          &MainWindow::OnOpenProject);
  connect(welcome_page_, &WelcomePage::RecentFileRequested, this,
          [this](const QString& path) {
            if (MaybeSave()) {
              LoadProjectFromFile(path);
            }
          });
  central_stack_->addWidget(welcome_page_);

  section_editor_ =
      new SectionEditor(document_, probe_controller_, central_stack_);
  central_stack_->addWidget(section_editor_);

  setCentralWidget(central_stack_);
}

void MainWindow::BuildSectionsDock() {
  sections_dock_ = new QDockWidget(tr("Sections"), this);
  sections_dock_->setObjectName(QStringLiteral("sections_dock"));
  section_list_dock_ = new SectionListDock(document_, sections_dock_);
  sections_dock_->setWidget(section_list_dock_);
  addDockWidget(Qt::LeftDockWidgetArea, sections_dock_);

  connect(section_list_dock_, &SectionListDock::CurrentSectionChanged, this,
          [this](int index) {
            section_editor_->SetCurrentSection(index);
            // Selecting a section also moves the preview to that section's
            // first output frame. Unlike the explicit "preview this section"
            // action this does not surface the dock: a hidden preview stays
            // hidden and picks the frame up when it is next shown. A selection
            // the preview itself drove is skipped: the frame already belongs to
            // that section, so jumping would undo the user's scrub.
            if (index >= 0 && !selecting_section_from_preview_) {
              preview_pane_->ShowSectionFirstFrame(index);
            }
          });
  // Multi-row selections put the editor in batch mode: edits to the current
  // section are mirrored onto every selected section.
  connect(section_list_dock_, &SectionListDock::SelectedSectionsChanged, this,
          [this](const QList<int>& indices) {
            section_editor_->SetSelectedSections(
                std::vector<int>(indices.begin(), indices.end()));
          });
}

void MainWindow::BuildPreviewDocks() {
  // One pane drives both docks: the pane widget holds the navigator and picture
  // views, and hands over its line-scope panel for the second dock, so the two
  // always show the same frame and line selection.
  preview_pane_ = new PreviewPane(document_, theme_controller_, this);
  // The section editor's minimum width would otherwise squeeze the right-hand
  // column down to its controls; keep enough room for a legible picture and
  // waveform.
  preview_pane_->setMinimumWidth(380);
  preview_pane_->scope_panel()->setMinimumHeight(200);

  preview_dock_ = new QDockWidget(tr("Preview"), this);
  preview_dock_->setObjectName(QStringLiteral("preview_dock"));
  preview_dock_->setWidget(preview_pane_);
  addDockWidget(Qt::RightDockWidgetArea, preview_dock_);

  scope_dock_ = new QDockWidget(tr("Line Scope"), this);
  scope_dock_->setObjectName(QStringLiteral("scope_dock"));
  scope_dock_->setWidget(preview_pane_->scope_panel());
  addDockWidget(Qt::RightDockWidgetArea, scope_dock_);
  splitDockWidget(preview_dock_, scope_dock_, Qt::Vertical);

  // Stale/error status goes to the status bar rather than an inline banner, so
  // the preview content never reflows.
  connect(preview_pane_, &PreviewPane::StatusMessageChanged, this,
          [this](const QString& message) {
            preview_status_label_->setText(message);
          });

  // Scrubbing the navigator into another section selects it in the list, so the
  // editor always describes the frame on screen. The guard keeps the resulting
  // CurrentSectionChanged from snapping the navigator back to that section's
  // first frame, which would undo the user's scrub.
  connect(preview_pane_, &PreviewPane::CurrentSectionChanged, this,
          [this](int index) {
            if (index < 0 || index == section_list_dock_->current_section()) {
              return;
            }
            selecting_section_from_preview_ = true;
            section_list_dock_->SelectSection(index);
            selecting_section_from_preview_ = false;
          });
}

void MainWindow::BuildIssuesDock() {
  issues_dock_ = new QDockWidget(tr("Issues"), this);
  issues_dock_->setObjectName(QStringLiteral("issues_dock"));

  auto* view = new QListView(issues_dock_);
  view->setModel(issues_model_);
  view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view->setSelectionMode(QAbstractItemView::SingleSelection);
  view->setWordWrap(true);
  connect(view, &QListView::doubleClicked, this, &MainWindow::OnIssueActivated);

  issues_dock_->setWidget(view);
  addDockWidget(Qt::BottomDockWidgetArea, issues_dock_);
}

void MainWindow::BuildLogDock() {
  log_dock_ = new QDockWidget(tr("Log"), this);
  log_dock_->setObjectName(QStringLiteral("log_dock"));

  auto* view = new QListView(log_dock_);
  view->setModel(log_model_);
  view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  view->setUniformItemSizes(true);

  // Follow the newest message while the run is producing output.
  connect(log_model_, &QAbstractItemModel::rowsInserted, view,
          [view] { view->scrollToBottom(); });

  log_dock_->setWidget(view);
  addDockWidget(Qt::BottomDockWidgetArea, log_dock_);
  // Hidden by default to leave the editor and preview more room; the run
  // failure path and View > Panels both point the user back to it.
  log_dock_->hide();
}

void MainWindow::BuildGenerationStatusWidgets() {
  generation_progress_bar_ = new QProgressBar(this);
  generation_progress_bar_->setVisible(false);
  generation_progress_bar_->setMaximumWidth(300);
  generation_progress_bar_->setFormat(tr("%v / %m frames"));
  statusBar()->addPermanentWidget(generation_progress_bar_);
}

void MainWindow::ConnectGenerationController() {
  connect(generation_controller_, &GenerationController::RunStarted, this,
          [this] {
            generate_action_->setEnabled(false);
            cancel_generation_action_->setEnabled(true);
            // Busy indicator until the first frame progress arrives.
            generation_progress_bar_->setRange(0, 0);
            generation_progress_bar_->setVisible(true);
            statusBar()->showMessage(tr("Generating…"));
          });
  connect(
      generation_controller_, &GenerationController::StageStarted, this,
      [this](const QString& stage_name) {
        statusBar()->showMessage(tr("Generation stage: %1").arg(stage_name));
      });
  connect(
      generation_controller_, &GenerationController::FrameProgress, this,
      [this](qulonglong frames_completed, qulonglong frames_total) {
        generation_progress_bar_->setRange(0, static_cast<int>(frames_total));
        generation_progress_bar_->setValue(static_cast<int>(frames_completed));
      });
  connect(generation_controller_, &GenerationController::LogMessageReceived,
          this, [this](int severity, const QString& message) {
            log_model_->Append(static_cast<LogSeverity>(severity), message);
          });
  connect(generation_controller_, &GenerationController::RunFinished, this,
          &MainWindow::OnGenerationFinished);
}

void MainWindow::UpdateProjectOpenState() {
  const bool open = document_->is_open();

  central_stack_->setCurrentWidget(open ? static_cast<QWidget*>(section_editor_)
                                        : static_cast<QWidget*>(welcome_page_));
  if (!open) {
    welcome_page_->SetRecentFiles(RecentFilePaths());
  }

  save_action_->setEnabled(open);
  save_as_action_->setEnabled(open);
  edit_project_action_->setEnabled(open);
  project_validate_action_->setEnabled(open);
  // Keep Generate disabled while a run is already in progress.
  generate_action_->setEnabled(open && !generation_controller_->is_running());
  if (sections_dock_ != nullptr) {
    sections_dock_->setEnabled(open);
  }
  if (preview_dock_ != nullptr) {
    preview_dock_->setEnabled(open);
    scope_dock_->setEnabled(open);
  }
  if (!open) {
    preview_status_label_->clear();
  }
  UpdateUndoActions();
}

void MainWindow::UpdateUndoActions() {
  if (undo_action_ == nullptr || redo_action_ == nullptr) {
    return;  // Menus not built yet (UndoStateChanged during startup reset).
  }
  const bool can_undo = document_->is_open() && document_->CanUndo();
  const bool can_redo = document_->is_open() && document_->CanRedo();
  undo_action_->setEnabled(can_undo);
  undo_action_->setText(can_undo
                            ? tr("&Undo %1").arg(document_->UndoDescription())
                            : tr("&Undo"));
  redo_action_->setEnabled(can_redo);
  redo_action_->setText(can_redo
                            ? tr("&Redo %1").arg(document_->RedoDescription())
                            : tr("&Redo"));
}

void MainWindow::OnPreferences() {
  QSettings settings;
  PreferencesDialog dialog(LoadGenerationPreferences(settings), this);
  if (dialog.exec() == QDialog::Accepted) {
    const GenerationPreferences preferences = dialog.preferences();
    SaveGenerationPreferences(preferences, &settings);
    statusBar()->showMessage(tr("Preferences saved"), 3000);
  }
}

void MainWindow::OnGenerate() {
  if (generation_controller_->is_running()) {
    return;
  }

  if (document_->is_modified()) {
    QMessageBox prompt(this);
    prompt.setWindowTitle(tr("Unsaved Changes"));
    prompt.setText(tr("The project \"%1\" has unsaved changes.")
                       .arg(document_->display_name()));
    prompt.setInformativeText(
        tr("Save before generating, or generate from the current in-memory "
           "project?"));
    QPushButton* save_button =
        prompt.addButton(tr("Save and Generate"), QMessageBox::AcceptRole);
    prompt.addButton(tr("Generate Without Saving"), QMessageBox::ActionRole);
    QPushButton* cancel_button = prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(save_button);
    prompt.exec();
    if (prompt.clickedButton() == cancel_button) {
      return;
    }
    if (prompt.clickedButton() == save_button && !OnSave()) {
      return;
    }
  }

  // Relative paths in the project are anchored to the project file's
  // directory (or the working directory for never-saved projects) because
  // the run executes on a worker thread.
  const QString base_dir =
      document_->file_path().isEmpty()
          ? QDir::currentPath()
          : QFileInfo(document_->file_path()).absolutePath();
  const Project project = videosynth::ResolveProjectPaths(
      document_->project(), GuiAssetRoots(), base_dir.toStdString(),
      /*anchor_unset=*/true);

  last_run_artefacts_.clear();
  last_run_artefacts_.append(QString::fromStdString(project.output.video_path));
  last_run_artefacts_.append(
      QString::fromStdString(project.output.metadata_path));
  for (const int pair : ProjectAudioChannelPairs(project)) {
    last_run_artefacts_.append(QString::fromStdString(
        AudioWavWriter::DeriveAudioPath(project.output.video_path, pair)));
  }
  if (AnySectionEnablesDropouts(project)) {
    last_run_artefacts_.append(
        QString::fromStdString(DropoutInjectionStage::DeriveSidecarPath(
            project.output.metadata_path)));
  }

  const QSettings settings;
  const RunOptions options = ApplyLoggingOverrides(
      MakeRunOptions(LoadGenerationPreferences(settings)), logging_overrides_);
  log_model_->Clear();
  generation_controller_->StartGeneration(project, options);
}

void MainWindow::OnGenerationFinished(GenerationController::RunStatus status) {
  generate_action_->setEnabled(document_->is_open());
  cancel_generation_action_->setEnabled(false);
  generation_progress_bar_->setVisible(false);

  switch (status) {
    case GenerationController::RunStatus::kSucceeded:
      statusBar()->showMessage(tr("Generation completed"), 5000);
      ShowGenerationSummary();
      break;
    case GenerationController::RunStatus::kCancelled:
      statusBar()->showMessage(tr("Generation cancelled"), 5000);
      break;
    case GenerationController::RunStatus::kFailed:
      statusBar()->showMessage(tr("Generation failed"), 5000);
      // The Log dock is hidden by default; surface it so the details the
      // message points at are actually on screen.
      log_dock_->show();
      log_dock_->raise();
      QMessageBox::critical(
          this, tr("Generation Failed"),
          tr("The generation run failed. See the Log panel for details."));
      break;
  }
}

void MainWindow::ShowGenerationSummary() {
  QMessageBox summary(this);
  summary.setWindowTitle(tr("Generation Complete"));
  summary.setIcon(QMessageBox::Information);

  summary.setText(tr("Generation completed successfully."));
  summary.setInformativeText(
      tr("Output files:\n%1").arg(last_run_artefacts_.join(QLatin1Char('\n'))));

  QPushButton* open_folder_button =
      summary.addButton(tr("Open Containing Folder"), QMessageBox::ActionRole);
  summary.addButton(QMessageBox::Ok);
  summary.setDefaultButton(QMessageBox::Ok);
  summary.exec();

  if (summary.clickedButton() == open_folder_button &&
      !last_run_artefacts_.isEmpty()) {
    const QString folder =
        QFileInfo(last_run_artefacts_.first()).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
  }
}

void MainWindow::RestoreWindowGeometry() {
  const QSettings settings;
  const QByteArray geometry =
      settings.value(QLatin1String(kGeometrySettingsKey)).toByteArray();
  if (!geometry.isEmpty()) {
    restoreGeometry(geometry);
  } else {
    resize(1440, 900);
  }

  // Dock sizes are not persisted, so give the right-hand column a usable
  // starting width and let the preview picture take twice the scope's height.
  resizeDocks({preview_dock_}, {width() * 2 / 5}, Qt::Horizontal);
  resizeDocks({preview_dock_, scope_dock_}, {2, 1}, Qt::Vertical);
}

void MainWindow::OnNewProject() {
  if (!MaybeSave()) {
    return;
  }
  NewProjectDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  // Saving is part of the create flow: a new project is written to disk
  // immediately so its relative paths always resolve against the project
  // file's directory (see asset_bases / ResolveProjectPaths).
  Project project = dialog.project();
  const QString suggested =
      QString::fromStdString(project.name.empty() ? "untitled" : project.name) +
      QStringLiteral(".yaml");
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save New Project"), suggested, ProjectFileFilter());
  if (path.isEmpty()) {
    return;
  }

  YamlProjectEmitter emitter;
  std::string error;
  if (!emitter.EmitFile(project, path.toStdString(), &error)) {
    QMessageBox::critical(this, tr("Save Failed"),
                          QString::fromStdString(error));
    return;
  }

  document_->ResetProject(std::move(project), path);
  AddToRecentFiles(path);
  section_list_dock_->SelectSection(0);
  statusBar()->showMessage(tr("New project created"), 3000);
}

void MainWindow::OnOpenProject() {
  if (!MaybeSave()) {
    return;
  }
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Open Project"), QString(), ProjectFileFilter());
  if (path.isEmpty()) {
    return;
  }
  LoadProjectFromFile(path);
}

bool MainWindow::OnSave() {
  if (document_->file_path().isEmpty()) {
    return OnSaveAs();
  }
  return SaveToPath(document_->file_path());
}

bool MainWindow::OnSaveAs() {
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save Project As"), document_->file_path(), ProjectFileFilter());
  if (path.isEmpty()) {
    return false;
  }
  return SaveToPath(path);
}

void MainWindow::OnEditProject() { EditProject(IssueField::kNone); }

void MainWindow::EditProject(IssueField field) {
  if (!document_->is_open()) {
    return;
  }
  EditProjectDialog dialog(document_->project(), this);
  dialog.RevealField(field);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const Project edited = dialog.project();
  // Each setter is a no-op when its slice is unchanged, so only actual edits
  // mark the document modified.
  document_->SetProjectInfo(edited.name, edited.version, edited.description);
  document_->SetCvbsPresets(edited.cvbs_presets);
  document_->SetProjectLineInjections(edited.line_injections);
  document_->SetOutputTargets(edited.output);
  document_->SetChannel(edited.channel);
  document_->SetChromaError(edited.chroma_error);
  statusBar()->showMessage(tr("Project settings updated"), 3000);
}

void MainWindow::OnIssueActivated(const QModelIndex& index) {
  if (!index.isValid()) {
    return;
  }
  const int section_index =
      index.data(ValidationIssuesModel::kSectionIndexRole).toInt();
  const auto field = static_cast<IssueField>(
      index.data(ValidationIssuesModel::kFieldRole).toInt());
  emit IssueNavigationRequested(section_index);

  // Section issues focus that section's editor, revealing the field group the
  // issue names; project-level issues open the Edit Project dialog at it.
  if (section_index >= 0 && section_index < document_->section_count()) {
    section_list_dock_->SelectSection(section_index);
    section_editor_->SetCurrentSection(section_index);
    section_editor_->RevealField(field);
    statusBar()->showMessage(
        tr("Issue relates to section %1").arg(section_index + 1), 3000);
  } else {
    statusBar()->showMessage(tr("Project-level issue"), 3000);
    EditProject(field);
  }
}

bool MainWindow::MaybeSave() {
  if (!document_->is_open() || !document_->is_modified()) {
    return true;
  }

  const QMessageBox::StandardButton choice = QMessageBox::warning(
      this, tr("Unsaved Changes"),
      tr("The project \"%1\" has unsaved changes.\nDo you want to save them?")
          .arg(document_->display_name()),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);

  switch (choice) {
    case QMessageBox::Save:
      return OnSave();
    case QMessageBox::Discard:
      return true;
    default:
      return false;
  }
}

bool MainWindow::SaveToPath(const QString& path) {
  YamlProjectEmitter emitter;
  std::string error;
  if (!emitter.EmitFile(document_->project(), path.toStdString(), &error)) {
    QMessageBox::critical(this, tr("Save Failed"),
                          QString::fromStdString(error));
    return false;
  }

  document_->MarkSaved(path);
  AddToRecentFiles(path);
  UpdateWindowTitle();
  statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
  return true;
}

void MainWindow::LoadProjectFromFile(const QString& path) {
  YamlProjectParser parser;
  ParseResult result = parser.ParseFile(path.toStdString());
  if (!result.ok) {
    QStringList details;
    for (const std::string& error : result.errors) {
      details.append(QString::fromStdString(error));
    }
    QMessageBox::critical(
        this, tr("Open Failed"),
        tr("Could not open \"%1\":\n%2").arg(path, details.join('\n')));
    return;
  }

  document_->ResetProject(std::move(result.project), path);
  section_list_dock_->SelectSection(0);
  AddToRecentFiles(path);
  statusBar()->showMessage(tr("Opened %1").arg(path), 3000);
}

void MainWindow::UpdateWindowTitle() {
  if (!document_->is_open()) {
    setWindowTitle(QStringLiteral("videosynth"));
    setWindowModified(false);
    return;
  }
  setWindowTitle(
      QStringLiteral("%1[*] — videosynth").arg(document_->display_name()));
  setWindowModified(document_->is_modified());
}

void MainWindow::UpdateValidationStatus() {
  if (!document_->is_open()) {
    validation_status_label_->clear();
    return;
  }
  const int errors = issues_model_->error_count();
  const int warnings = issues_model_->warning_count();
  if (errors == 0 && warnings == 0) {
    validation_status_label_->setText(tr("Project valid"));
  } else {
    validation_status_label_->setText(
        tr("%1 error(s), %2 warning(s)").arg(errors).arg(warnings));
  }
}

void MainWindow::ShowExplicitValidationResult() {
  const int errors = issues_model_->error_count();
  const int warnings = issues_model_->warning_count();

  if (errors == 0 && warnings == 0) {
    QMessageBox::information(
        this, tr("Validation Passed"),
        tr("The project is valid.\n\nNo errors or warnings were found."));
    return;
  }

  QMessageBox box(this);
  box.setWindowTitle(errors > 0 ? tr("Validation Failed")
                                : tr("Validation Passed"));
  box.setIcon(errors > 0 ? QMessageBox::Critical : QMessageBox::Warning);
  box.setText(errors > 0 ? tr("The project has validation errors and cannot be "
                              "generated.")
                         : tr("The project is valid but has warnings."));
  box.setInformativeText(
      tr("%1 error(s) and %2 warning(s) were found. See the Issues panel for "
         "details.")
          .arg(errors)
          .arg(warnings));
  box.setStandardButtons(QMessageBox::Ok);
  box.exec();
}

QStringList MainWindow::RecentFilePaths() const {
  const QSettings settings;
  return settings.value(QLatin1String(kRecentFilesSettingsKey)).toStringList();
}

void MainWindow::UpdateRecentFilesMenu() {
  recent_files_menu_->clear();

  const QStringList recent = RecentFilePaths();
  for (const QString& path : recent) {
    QAction* action = recent_files_menu_->addAction(QFileInfo(path).fileName());
    action->setToolTip(path);
    connect(action, &QAction::triggered, this, [this, path] {
      if (!MaybeSave()) {
        return;
      }
      LoadProjectFromFile(path);
    });
  }
  recent_files_menu_->setEnabled(!recent.isEmpty());

  if (welcome_page_ != nullptr) {
    welcome_page_->SetRecentFiles(recent);
  }
}

void MainWindow::AddToRecentFiles(const QString& path) {
  QSettings settings;
  QStringList recent =
      settings.value(QLatin1String(kRecentFilesSettingsKey)).toStringList();
  recent.removeAll(path);
  recent.prepend(path);
  while (recent.size() > kMaxRecentFiles) {
    recent.removeLast();
  }
  settings.setValue(QLatin1String(kRecentFilesSettingsKey), recent);
  UpdateRecentFilesMenu();
}

void MainWindow::OnAbout() {
  QMessageBox about_box(this);
  about_box.setWindowTitle(tr("About VideoSynth"));
  about_box.setIconPixmap(
      QPixmap(QStringLiteral(":/videosynth-gui/icon.png"))
          .scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));

  const QString about_text =
      QStringLiteral(
          "<h2>VideoSynth</h2>"
          "<p><b>Version:</b> %1</p>"
          "<p>Analogue video signal generator</p>"
          "<p><b>Copyright:</b> © 2026 Simon Inns</p>"
          "<p><b>License:</b> GNU General Public License v3.0 or later</p>"
          "<p>This program is free software: you can redistribute it and/or "
          "modify it under the terms of the GNU General Public License as "
          "published by the Free Software Foundation, either version 3 of "
          "the License, or (at your option) any later version.</p>"
          "<p>This program is distributed in the hope that it will be "
          "useful, but WITHOUT ANY WARRANTY; without even the implied "
          "warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. "
          "See the GNU General Public License for more details.</p>"
          "<p>You should have received a copy of the GNU General Public "
          "License along with this program. If not, see "
          "<a href='https://www.gnu.org/licenses/'>"
          "https://www.gnu.org/licenses/</a>.</p>")
          .arg(QStringLiteral(VIDEOSYNTH_VERSION));

  about_box.setText(about_text);
  about_box.setTextFormat(Qt::RichText);
  about_box.setTextInteractionFlags(Qt::TextBrowserInteraction);
  about_box.exec();
}

}  // namespace videosynth::gui
