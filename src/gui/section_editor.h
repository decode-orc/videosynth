/*
 * File:        section_editor.h
 * Module:      gui
 * Purpose:     Per-section form editor: source and span, optional audio,
 *              noise, dropout, OSD blocks, and line injections
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QWidget>
#include <vector>

#include "audio_channel_pairs_editor.h"
#include "line_injections_editor.h"
#include "project_document.h"
#include "source_probe_controller.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace videosynth::gui {

// Editor for one section of the project. Widgets stage edits into a copy of
// the section and commit through ProjectDocument::SetSection; optional YAML
// blocks are toggled through the section_block_presenters helpers so
// disabled blocks never emit defaults. Every optional block (audio, noise,
// dropouts, OSD, line injections) sits in a checkable group whose contents
// are hidden until the box is ticked, so an unused block collapses to a
// single title row instead of cluttering the form. Source probing runs on the
// SourceProbeController worker and never blocks the UI.
//
// Multi-select batch editing: when the sections list holds a multi-row
// selection (SetSelectedSections), the editor still displays the current
// section, but every commit mirrors the changed fields onto the other
// selected sections via ApplySectionEditDelta (names stay individual).
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class SectionEditor : public QWidget {
  Q_OBJECT

 public:
  // Does not take ownership of `document` or `probe_controller`; both must
  // outlive the editor.
  SectionEditor(ProjectDocument* document,
                SourceProbeController* probe_controller,
                QWidget* parent = nullptr);

  // Selects the section to edit; -1 shows the disabled placeholder state.
  void SetCurrentSection(int index);
  int current_section() const { return section_index_; }

  // Full selection from the sections list (ascending, includes the current
  // section). Edits are mirrored onto every listed section; an empty or
  // single-entry list restores plain single-section editing.
  void SetSelectedSections(std::vector<int> indices);

 private:
  void BuildUi();
  QWidget* BuildGeneralGroup();
  QWidget* BuildProbeGroup();
  QWidget* BuildAudioGroup();
  QWidget* BuildNoiseGroup();
  QWidget* BuildDropoutsGroup();
  QWidget* BuildOsdGroup();
  QWidget* BuildInjectionsGroup();

  void LoadFromDocument();
  void LoadOsdTable(const Section& section);
  // Reads the staged widget state into a Section and commits it.
  void CommitSection();
  // Commits after the user picked a different source: a new video defaults
  // to all of its frames, a new still to a fixed duration (see
  // ApplySourceSelectionDefaults).
  void CommitSourceSelection();
  // Commits `section` and schedules the deferred reload of derived displays.
  void CommitEditedSection(const Section& section);
  // Commits `section` to the current section and mirrors the changed fields
  // onto the other selected sections. Returns true when the document changed.
  bool CommitSectionToDocument(const Section& section);
  // Shows/hides the batch-editing banner above the form.
  void UpdateMultiEditHint();
  // The section types of the current multi-row selection, or empty when the
  // selection holds fewer than two valid sections (single-section editing).
  std::vector<SectionType> SelectedSectionTypes() const;
  Section SectionFromWidgets() const;
  void RequestProbe();
  void UpdateProbeDisplay();

  void OnBrowseSource();
  void OnAddOverlay();
  void OnRemoveOverlay();

  // Composes the stored source string from the built-in / my-own picker,
  // classifies a stored source back into the picker, and refreshes the
  // "resolves to" hint. A built-in source is {bundled}/<type>/<raster>/<file>
  // recomposed from the current project raster; a my-own source is a
  // {project}-relative path, an absolute path, or a preserved logical token.
  std::string SourceFromWidgets() const;
  void LoadSourceWidgets(const std::string& source);
  void UpdateSourceResolvedHint();
  // Fills the built-in file dropdown by scanning the bundled asset folder for
  // the current project raster and selected asset type, keeping `keep_file`
  // selectable even when it is absent on disk so saving never drops it.
  void PopulateBuiltinFiles(const QString& keep_file);
  // The bundled raster subfolder ("720x576"/"720x486") for the project.
  QString ProjectBundledRaster() const;
  // Refreshes the "N frames x R = T total" duration hint from the selected
  // source range (resolved against the latest probe result) and the current
  // repeat multiplier, or the loop note for a fixed duration longer than the
  // range.
  void UpdateDurationSummary();
  // Hands the audio editor the section source and the latest probe report,
  // which decide the video soundtrack streams it offers.
  void UpdateAudioSourceContext();
  // Applies SourceRangeEditorStateFor to the source-range widgets: enabled for
  // video sources only, capped at the probed frame count.
  void UpdateSourceRangeWidgets();
  // The probed frame count of the current video source, or
  // kUnknownSourceFrameCount until a successful probe report arrives.
  int ProbedSourceFrameCount() const;
  // Refreshes the section's frame range and, for programme_area sections of
  // laserdisc projects, the CAV picture-number / CLV timecode range (anchored
  // at the programme area start; see BuildDiscFrameOffsets). The end of an
  // "all source frames" section resolves from the latest probe report; "?"
  // until then.
  void UpdateFrameRangeDisplay();
  // The project standard's frame rate, or 0.0 when no standard is set.
  double ProjectFrameRateHz() const;
  // Mirrors the frame count into the read-along seconds spinbox without
  // re-triggering the frames spinbox (seconds is display-only convenience;
  // the stored duration is always frames).
  void SyncDurationSecondsFromFrames();
  // Base directory relative source paths anchor to: the project file's folder,
  // or the working directory for a never-saved project.
  QString ProjectBaseDir() const;

  ProjectDocument* document_;
  SourceProbeController* probe_controller_;
  int section_index_ = -1;
  // Full sections-list selection (ascending). Size > 1 enables batch editing.
  std::vector<int> selected_sections_ = {};
  bool updating_ = false;
  bool committing_ = false;
  // Guards against queuing more than one deferred reload at a time (see
  // CommitSection): a reload rebuilds child editors, and doing so synchronously
  // from a child widget's own signal handler would free the widget mid-event.
  bool reload_pending_ = false;

  QWidget* content_ = nullptr;
  QLabel* placeholder_ = nullptr;
  QLabel* multi_edit_hint_ = nullptr;

  // General.
  QLineEdit* name_edit_ = nullptr;
  QComboBox* section_type_combo_ = nullptr;
  QComboBox* source_mode_combo_ = nullptr;
  QStackedWidget* source_stack_ = nullptr;
  QComboBox* builtin_type_combo_ = nullptr;
  QComboBox* builtin_file_combo_ = nullptr;
  QLineEdit* source_edit_ = nullptr;
  QCheckBox* source_relative_check_ = nullptr;
  QLabel* source_resolved_hint_ = nullptr;
  QSpinBox* duration_spin_ = nullptr;
  QDoubleSpinBox* duration_seconds_spin_ = nullptr;
  QCheckBox* duration_all_check_ = nullptr;
  QSpinBox* duration_repeat_spin_ = nullptr;
  QLabel* duration_repeat_label_ = nullptr;
  QLabel* duration_summary_label_ = nullptr;
  // Selected source range (video only): first frame, last frame, and
  // "to end of source" for an open-ended range.
  QSpinBox* start_frame_spin_ = nullptr;
  QSpinBox* end_frame_spin_ = nullptr;
  QCheckBox* end_frame_to_source_end_check_ = nullptr;
  // General-group form layout, kept for toggling the disc-range row.
  QFormLayout* general_form_ = nullptr;
  QLabel* frame_range_label_ = nullptr;
  // Disc position range row ("CAV picture numbers:" / "CLV timecode:");
  // hidden for non-laserdisc projects and for sections outside the
  // programme area (lead-in/lead-out carry no picture numbers or timecodes).
  QLabel* disc_range_title_ = nullptr;
  QLabel* disc_range_label_ = nullptr;

  // Probe.
  QLabel* probe_status_label_ = nullptr;
  QLabel* probe_detail_label_ = nullptr;

  // Audio.
  QGroupBox* audio_group_ = nullptr;
  AudioChannelPairsEditor* audio_editor_ = nullptr;

  // Noise.
  QGroupBox* noise_group_ = nullptr;
  QDoubleSpinBox* noise_db_spin_ = nullptr;
  QDoubleSpinBox* noise_spread_spin_ = nullptr;
  QCheckBox* noise_seed_check_ = nullptr;
  QLineEdit* noise_seed_edit_ = nullptr;

  // Dropouts.
  QGroupBox* random_dropouts_group_ = nullptr;
  QSpinBox* random_scale_spin_ = nullptr;
  QCheckBox* random_seed_check_ = nullptr;
  QLineEdit* random_seed_edit_ = nullptr;
  QGroupBox* scratch_dropouts_group_ = nullptr;
  QSpinBox* scratch_scale_spin_ = nullptr;
  QCheckBox* scratch_seed_check_ = nullptr;
  QLineEdit* scratch_seed_edit_ = nullptr;

  // OSD.
  QGroupBox* osd_group_ = nullptr;
  QTableWidget* osd_table_ = nullptr;
  QPushButton* remove_overlay_button_ = nullptr;

  // Line injections.
  QGroupBox* injections_group_ = nullptr;
  LineInjectionsEditor* injections_editor_ = nullptr;
};

}  // namespace videosynth::gui
