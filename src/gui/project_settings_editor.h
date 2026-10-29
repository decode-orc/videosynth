/*
 * File:        project_settings_editor.h
 * Module:      gui
 * Purpose:     Form editor for project info, CVBS presets, line injections,
 *              channel and chroma error impairments, and output targets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QWidget>
#include <string>
#include <vector>

#include "project_document.h"
#include "project_settings_presenter.h"
#include "validation_issues_model.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace videosynth::gui {

// Editor for the project-level YAML blocks (project:, cvbs_presets:,
// line_injections:, channel:, chroma_error:, output:). Every control maps 1:1
// to a Project field and commits through ProjectDocument commands;
// standard-dependent enablement follows BuildProjectSettingsFormState so
// invalid combinations cannot be entered.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class ProjectSettingsEditor : public QWidget {
  Q_OBJECT

 public:
  // Does not take ownership of `document`; it must outlive the editor.
  explicit ProjectSettingsEditor(ProjectDocument* document,
                                 QWidget* parent = nullptr);

  // Locks or unlocks the video-standard control. The standard is chosen once
  // when a project is created (it drives raster/timing); the Edit Project
  // dialog calls this with `false` so it is shown read-only.
  void SetStandardEditable(bool editable);

  // The group holding `field` (kChannel, kChromaError) and the input to focus
  // in it, for validation issue navigation; null for other fields.
  QWidget* FieldGroup(IssueField field) const;
  QWidget* FieldFocusWidget(IssueField field) const;

 private:
  void BuildUi();
  void LoadFromDocument();
  void ApplyEnablement();

  void CommitProjectInfo();
  void CommitCvbsPresets();
  void CommitLineInjections();
  void CommitOutputTargets();
  void CommitChannel();
  void CommitChromaError();

  QWidget* BuildChannelGroup();
  QWidget* BuildChromaErrorGroup();
  void LoadChannelAndChromaError(const Project& project);

  // Rebuilds the VITS checklist from the standard's catalogue and the current
  // model. Called only on external/standard changes (never from a VITS widget's
  // own signal), so widgets are not destroyed mid-signal.
  void RebuildVitsChecklist();

  // Translated sentence describing the EFM selection's outcome.
  QString EfmStatusText(EfmOutputStatus status) const;

  void OnStandardChanged();
  void OnSignalTypeChanged();
  void OnBrowseVideoPath();
  void OnVitsPlacementChanged();
  void OnVitsRowToggled(int row);
  void OnVitsLineEdited(int row);

  ProjectDocument* document_;
  bool updating_ = false;
  bool committing_ = false;
  bool standard_editable_ = true;

  QLineEdit* name_edit_ = nullptr;
  QLineEdit* version_edit_ = nullptr;
  QPlainTextEdit* description_edit_ = nullptr;

  QComboBox* standard_combo_ = nullptr;
  QComboBox* sample_encoding_combo_ = nullptr;
  QComboBox* signal_state_combo_ = nullptr;
  QCheckBox* pilot_burst_check_ = nullptr;
  QCheckBox* vbi_burst_check_ = nullptr;
  QCheckBox* setup_ire_check_ = nullptr;
  QCheckBox* secam_field_id_check_ = nullptr;
  QComboBox* setup_ire_combo_ = nullptr;

  // One VITS checklist row: the type, its tick box, and the target-line editor.
  // The line editor is read-only only under standard placement for types with a
  // fixed broadcast line; laserdisc and custom placement make it editable.
  struct VitsRow {
    std::string vits_type;
    QCheckBox* check = nullptr;
    QLineEdit* lines = nullptr;
  };

  // Project-wide line injections (laserdisc disc format + VITS placement +
  // set).
  QComboBox* disc_type_combo_ = nullptr;
  QComboBox* vits_placement_combo_ = nullptr;
  QLabel* vits_label_ = nullptr;
  QGridLayout* vits_checklist_layout_ = nullptr;
  std::vector<VitsRow> vits_rows_;

  QComboBox* signal_type_combo_ = nullptr;
  QLineEdit* video_path_edit_ = nullptr;
  QLabel* video_path_hint_ = nullptr;
  QLabel* outputs_note_ = nullptr;

  // LaserDisc digital audio (EFM): enable plus the single channel pair the
  // stream is encoded from. The hint reports why no stream is written when the
  // selection is inert.
  // channel: (pressing impairments). The FM-beat group is disabled outside
  // 625-line PAL; fm_beat_hint_ says why.
  QGroupBox* channel_group_ = nullptr;
  QDoubleSpinBox* timing_offset_spin_ = nullptr;
  QGroupBox* edge_smear_group_ = nullptr;
  QDoubleSpinBox* edge_smear_fraction_spin_ = nullptr;
  QDoubleSpinBox* edge_smear_tau_spin_ = nullptr;
  QGroupBox* fm_beat_group_ = nullptr;
  QDoubleSpinBox* fm_beat_carrier_spin_ = nullptr;
  QDoubleSpinBox* fm_beat_level_spin_ = nullptr;
  QLabel* fm_beat_hint_ = nullptr;

  // chroma_error: (capture impairment), disabled for SECAM.
  QGroupBox* chroma_error_group_ = nullptr;
  QDoubleSpinBox* chroma_gain_spin_ = nullptr;
  QDoubleSpinBox* chroma_phase_spin_ = nullptr;
  QCheckBox* chroma_include_burst_check_ = nullptr;
  QGroupBox* chroma_jitter_group_ = nullptr;
  QDoubleSpinBox* jitter_phase_spin_ = nullptr;
  QDoubleSpinBox* jitter_gain_spin_ = nullptr;
  QCheckBox* jitter_seed_check_ = nullptr;
  QLineEdit* jitter_seed_edit_ = nullptr;
  QLabel* chroma_error_hint_ = nullptr;

  QCheckBox* efm_check_ = nullptr;
  QComboBox* efm_pair_combo_ = nullptr;
  QLabel* efm_hint_ = nullptr;
};

}  // namespace videosynth::gui
