/*
 * File:        project_document.h
 * Module:      gui
 * Purpose:     In-memory project document with change signals and dirty state
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QObject>
#include <QString>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/model.h"

namespace videosynth::gui {

// Describes the change a document command produced, so ProjectDocument can
// emit the matching granular signal after applying (or later reverting) it.
struct DocumentChange {
  enum class Kind {
    kProjectSettings,  // project info, presets, outputs, channel, chroma
    kSectionAdded,
    kSectionRemoved,
    kSectionMoved,
    kSectionEdited,
  };

  Kind kind = Kind::kProjectSettings;
  // Section index the change applies to (kSectionAdded/Removed/Edited and
  // the origin of kSectionMoved); -1 when not applicable.
  int index = -1;
  // Destination index of kSectionMoved; -1 otherwise.
  int to_index = -1;
};

// Command-pattern mutation of a videosynth::Project. Every document edit is
// routed through a command and retained on the document's undo stack: Apply
// performs the edit, Revert restores the prior state, and the returned
// DocumentChange drives signal emission.
//
// Thread-safety: NOT thread-safe. Commands run on the document's thread.
class IDocumentCommand {
 public:
  virtual ~IDocumentCommand() = default;

  // Performs the mutation and returns the change to announce.
  virtual DocumentChange Apply(Project* project) = 0;

  // Restores the state prior to Apply and returns the change to announce.
  virtual DocumentChange Revert(Project* project) = 0;

  // Short human-readable description for a future undo-stack UI.
  virtual QString Description() const = 0;
};

// Owns the in-memory videosynth::Project the GUI edits, together with its
// file path and dirty flag. All mutations go through command objects (see
// IDocumentCommand) and emit granular change signals; DocumentChanged fires
// after every granular signal so listeners (validation, previews) can attach
// once.
//
// Thread-safety: NOT thread-safe. Must be used from the thread that owns it
// (normally the GUI thread). Workers must copy project() and marshal results
// back via queued connections.
class ProjectDocument : public QObject {
  Q_OBJECT

 public:
  explicit ProjectDocument(QObject* parent = nullptr);

  const Project& project() const { return project_; }
  QString file_path() const { return file_path_; }
  bool is_modified() const { return modified_; }
  // True once a project has been created or opened; false at startup and after
  // CloseProject. The GUI shows its welcome surface while no project is open.
  bool is_open() const { return open_; }
  int section_count() const {
    return static_cast<int>(project_.sections.size());
  }

  // Display name for window titles: file base name, else the project name,
  // else "Untitled".
  QString display_name() const;

  // Replaces the whole document (File > New / Open). Marks the document open,
  // clears the dirty flag, and emits DocumentReset. `file_path` is empty for
  // unsaved templates.
  void ResetProject(Project project, const QString& file_path);

  // Returns to the "nothing open" state (File > Close): replaces the document
  // with a default-constructed Project, clears the path and dirty flag, marks
  // it not open, and emits DocumentReset. A no-op when already closed.
  void CloseProject();

  // Records a successful save to `file_path`: clears the dirty flag and
  // updates the path (Save As may change it).
  void MarkSaved(const QString& file_path);

  // --- Command-routed mutations -------------------------------------------
  // Each returns false (and leaves the document untouched) when the request
  // is a no-op or the index is out of range.

  bool SetProjectInfo(const std::string& name, const std::string& version,
                      const std::string& description);
  bool SetCvbsPresets(const CvbsPresets& presets);
  bool SetProjectLineInjections(ProjectLineInjections line_injections);
  bool SetOutputTargets(const OutputTargets& output);
  bool SetChannel(const ChannelParameters& channel);
  bool SetChromaError(const ChromaErrorParameters& chroma_error);

  // Inserts at `index`, or appends when index == -1 or index == count.
  bool InsertSection(int index, Section section);
  bool RemoveSection(int index);
  bool MoveSection(int from, int to);
  bool SetSection(int index, Section section);

  // Generic entry point used by the typed mutations above; exposed so future
  // editors can submit custom commands. The applied command is retained on
  // the undo stack (or in the open batch).
  bool ApplyCommand(std::unique_ptr<IDocumentCommand> command);

  // --- Undo / redo --------------------------------------------------------
  // Every applied command (or batch of commands) is one undo step. Undo and
  // redo re-announce the granular change signals of each reverted/reapplied
  // command, so views resync exactly as they do for a live edit. The modified
  // flag tracks the stack position of the last save: undoing back to the
  // saved state clears it. Both return false (and CanUndo/CanRedo are false)
  // while a batch is open or there is nothing to undo/redo.

  bool CanUndo() const;
  bool CanRedo() const;
  // Description of the next undo/redo step ("Move section", "Edit sections");
  // empty when the stack side is empty.
  QString UndoDescription() const;
  QString RedoDescription() const;
  bool Undo();
  bool Redo();

  // Groups every mutation applied between BeginBatch and EndBatch into a
  // single undo step described by `description` (multi-row moves, removes,
  // batch-mirrored section edits). Calls nest: only the outermost pair closes
  // the batch, and the outermost description wins. An empty batch adds no
  // undo step. Prefer ScopedUndoBatch over calling these directly.
  void BeginBatch(const QString& description);
  void EndBatch();

 signals:
  // Granular change signals; exactly one fires per applied command.
  void ProjectSettingsChanged();
  void SectionAdded(int index);
  void SectionRemoved(int index);
  void SectionMoved(int from, int to);
  void SectionEdited(int index);

  // Fires after every granular signal above (any document mutation).
  void DocumentChanged();

  // Whole-document replacement (New/Open).
  void DocumentReset();

  void ModifiedStateChanged(bool modified);
  void FilePathChanged(const QString& file_path);

  // Fires whenever the undo/redo stack or position changes (command applied,
  // undo, redo, batch closed, document reset); drives menu enablement.
  void UndoStateChanged();

 private:
  // One undo step: a described group of one or more applied commands.
  struct UndoEntry {
    QString description;
    std::vector<std::unique_ptr<IDocumentCommand>> commands;
  };

  void AnnounceChange(const DocumentChange& change);
  void SetModified(bool modified);
  void SetFilePath(const QString& file_path);
  bool IsValidSectionIndex(int index) const;
  // Truncates the redo tail and pushes `entry` as the newest undo step.
  void PushUndoEntry(UndoEntry entry);
  // Recomputes the modified flag from the undo position vs the saved position.
  void UpdateModifiedFromUndoPosition();
  void ClearUndoStack();

  Project project_;
  QString file_path_;
  bool modified_ = false;
  bool open_ = false;

  // Undo steps; [0, undo_index_) are applied, the rest are redoable.
  std::vector<UndoEntry> undo_stack_;
  std::size_t undo_index_ = 0;
  // Stack position of the last save; npos when the saved state was discarded
  // with a redo tail (the document can then never be position-clean again).
  std::size_t saved_undo_index_ = 0;
  static constexpr std::size_t kSavedStateUnreachable =
      static_cast<std::size_t>(-1);

  // Open batch state: nesting depth, outermost description, and the commands
  // applied since the outermost BeginBatch.
  int batch_depth_ = 0;
  QString batch_description_;
  std::vector<std::unique_ptr<IDocumentCommand>> batch_commands_;
};

// Groups every ProjectDocument mutation applied during its lifetime into one
// undo step (RAII wrapper over BeginBatch/EndBatch).
//
// Thread-safety: NOT thread-safe. Use on the document's (GUI) thread.
class ScopedUndoBatch {
 public:
  ScopedUndoBatch(ProjectDocument* document, const QString& description)
      : document_(document) {
    document_->BeginBatch(description);
  }
  ~ScopedUndoBatch() { document_->EndBatch(); }

  ScopedUndoBatch(const ScopedUndoBatch&) = delete;
  ScopedUndoBatch& operator=(const ScopedUndoBatch&) = delete;

 private:
  ProjectDocument* document_;
};

}  // namespace videosynth::gui
