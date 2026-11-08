/*
 * File:        project_settings_editor.h
 * Module:      gui
 * Purpose:     Form editor for project info, CVBS presets, VITS, channel,
 *              chroma error and line timing impairments, output targets and
 *              the LaserDisc block
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
class QSpinBox;

namespace videosynth::gui {

class CurvePointsEditor;
class ResponseByPictureEditor;

// Editor for the project-level YAML blocks (project:, cvbs_presets:,
// line_injections:, channel:, chroma_error:, line_timing:, output:,
// laserdisc:). Every
// control maps 1:1 to a Project field and commits through ProjectDocument
// commands; standard-dependent enablement follows BuildProjectSettingsFormState
// so invalid combinations cannot be entered. Everything LaserDisc-specific
// (disc format, pilot/VBI burst, EFM digital audio or data and the
// LaserVision channel) sits in one checkable "LaserDisc" group whose body stays
// hidden until the project declares a laserdisc block.
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

  // The group holding `field` (kChannel, kChromaError, kLineTiming,
  // kLaserdiscChannel, kEfmData) and the input to focus in it, for validation
  // issue navigation; null for other fields.
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
  void CommitLineTiming();
  void CommitLaserdisc();
  // Clears every section's laserdisc data (disc area and codes), which the
  // validator rejects once the project has no laserdisc block. Returns true
  // when any section changed.
  bool ClearSectionLaserdiscBlocks();
  // Clears every section's EFM data flag, which the validator rejects once the
  // project has no laserdisc.efm_data block. Returns true when any section
  // changed.
  bool ClearSectionEfmDataFlags();

  QWidget* BuildVitsGroup();
  QWidget* BuildChannelGroup();
  QWidget* BuildChromaErrorGroup();
  QWidget* BuildLineTimingGroup();
  QWidget* BuildLaserdiscGroup();
  QWidget* BuildLaserdiscChannelGroup(QWidget* parent);
  QWidget* BuildEfmDataGroup(QWidget* parent);
  void LoadChannelAndChromaError(const Project& project);
  void LoadLineTiming(const Project& project);
  void LoadLaserdisc(const Project& project);
  void ApplyLaserdiscEnablement(const ProjectSettingsFormState& state);

  // Rebuilds the VITS checklist from the standard's catalogue and the current
  // model. Called only on external/standard changes (never from a VITS widget's
  // own signal), so widgets are not destroyed mid-signal.
  void RebuildVitsChecklist();

  // Translated sentence describing the EFM selection's outcome.
  QString EfmStatusText(EfmOutputStatus status) const;
  // Translated sentence describing the EFM data selection's outcome.
  QString EfmDataStatusText(EfmDataStatus status) const;

  void OnStandardChanged();
  void OnSignalTypeChanged();
  void OnBrowseVideoPath();
  void OnBrowseEfmDataImage();
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

  // Project-wide VITS placement and set.
  QComboBox* vits_placement_combo_ = nullptr;
  QLabel* vits_label_ = nullptr;
  QGridLayout* vits_checklist_layout_ = nullptr;
  std::vector<VitsRow> vits_rows_;

  QComboBox* signal_type_combo_ = nullptr;
  QLineEdit* video_path_edit_ = nullptr;
  QLabel* video_path_hint_ = nullptr;
  QLabel* outputs_note_ = nullptr;

  // channel: (pressing impairments of any standard).
  QGroupBox* channel_group_ = nullptr;
  QDoubleSpinBox* timing_offset_spin_ = nullptr;
  QGroupBox* edge_smear_group_ = nullptr;
  QDoubleSpinBox* edge_smear_fraction_spin_ = nullptr;
  QDoubleSpinBox* edge_smear_tau_spin_ = nullptr;

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
  QGroupBox* differential_group_ = nullptr;
  CurvePointsEditor* differential_gain_editor_ = nullptr;
  CurvePointsEditor* differential_phase_editor_ = nullptr;
  QGroupBox* crosstalk_group_ = nullptr;
  QDoubleSpinBox* crosstalk_v_to_u_spin_ = nullptr;
  QDoubleSpinBox* crosstalk_u_to_v_spin_ = nullptr;
  QCheckBox* crosstalk_alternate_check_ = nullptr;
  QLabel* chroma_error_hint_ = nullptr;

  // line_timing: (capture layer, with an optional pressing layer). One
  // widget set per layer.
  struct LineTimingLayerWidgets {
    QDoubleSpinBox* line_ns = nullptr;
    QDoubleSpinBox* correlation = nullptr;
    QDoubleSpinBox* field_ns = nullptr;
    QDoubleSpinBox* slope_ns = nullptr;
    QCheckBox* seed_check = nullptr;
    QLineEdit* seed_edit = nullptr;
  };
  QGroupBox* line_timing_group_ = nullptr;
  LineTimingLayerWidgets line_timing_capture_;
  QCheckBox* line_timing_burst_check_ = nullptr;
  QGroupBox* line_timing_shared_group_ = nullptr;
  LineTimingLayerWidgets line_timing_shared_;

  QCheckBox* truth_check_ = nullptr;

  // laserdisc: the whole LaserDisc block, unavailable for SECAM
  // (laserdisc_hint_ says why).
  QGroupBox* laserdisc_group_ = nullptr;
  QLabel* laserdisc_hint_ = nullptr;
  QComboBox* disc_type_combo_ = nullptr;
  QCheckBox* pilot_burst_check_ = nullptr;
  QCheckBox* vbi_burst_check_ = nullptr;
  // LaserDisc digital audio (EFM): enable plus the single channel pair the
  // stream is encoded from. The hint reports why no stream is written when the
  // selection is inert.
  QCheckBox* efm_check_ = nullptr;
  QComboBox* efm_pair_combo_ = nullptr;
  QLabel* efm_hint_ = nullptr;
  // LaserDisc EFM data (AIV / LV-ROM): the sector image, its placement and
  // the Q-channel CONTROL and ADR of the data track. Exclusive with digital
  // audio: a disc has one EFM channel.
  QGroupBox* efm_data_group_ = nullptr;
  QLineEdit* efm_data_image_edit_ = nullptr;
  QPushButton* efm_data_browse_button_ = nullptr;
  QSpinBox* efm_data_first_address_spin_ = nullptr;
  QSpinBox* efm_data_offset_spin_ = nullptr;
  QComboBox* efm_data_q_control_combo_ = nullptr;
  QComboBox* efm_data_q_mode_combo_ = nullptr;
  QLabel* efm_data_hint_ = nullptr;

  // laserdisc.channel: the LaserVision pressing and replay channel. The
  // FM-beat group is disabled outside 625-line PAL; fm_beat_hint_ says why.
  QGroupBox* ld_channel_group_ = nullptr;
  QGroupBox* fm_beat_group_ = nullptr;
  QDoubleSpinBox* fm_beat_carrier_spin_ = nullptr;
  QDoubleSpinBox* fm_beat_level_spin_ = nullptr;
  QLabel* fm_beat_hint_ = nullptr;
  QGroupBox* group_delay_group_ = nullptr;
  QComboBox* group_delay_preset_combo_ = nullptr;
  CurvePointsEditor* group_delay_curve_editor_ = nullptr;
  QDoubleSpinBox* group_delay_scale_spin_ = nullptr;
  QComboBox* response_mode_combo_ = nullptr;
  CurvePointsEditor* response_curve_editor_ = nullptr;
  ResponseByPictureEditor* response_by_picture_editor_ = nullptr;
  QGroupBox* field2_offset_group_ = nullptr;
  QDoubleSpinBox* field2_x_spin_ = nullptr;
  QDoubleSpinBox* field2_y_spin_ = nullptr;
};

}  // namespace videosynth::gui
