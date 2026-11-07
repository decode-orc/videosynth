/*
 * File:        project_settings_editor.cpp
 * Module:      gui
 * Purpose:     Form editor for project info, CVBS presets, VITS, channel and
 *              chroma error impairments, output targets and the LaserDisc
 *              block
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "project_settings_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <algorithm>
#include <initializer_list>
#include <utility>

#include "asset_roots.h"
#include "curve_points_editor.h"
#include "form_field_width.h"
#include "line_injection_presenter.h"
#include "project_settings_presenter.h"
#include "project_templates.h"
#include "response_by_picture_editor.h"
#include "section_block_presenters.h"

namespace {
constexpr int kVitsTypeColumn = 0;
constexpr int kVitsLinesColumn = 1;
// Sentinel disc_type combo entry meaning "not a laserdisc project".
const char* const kDiscTypeNone = "(none)";

// VITS placement combo item order: 0 standard, 1 laserdisc, 2 custom.
constexpr int kVitsPlacementLaserdiscIndex = 1;

// Group-delay curve combo item order.
constexpr int kGroupDelayNominalIndex = 0;
constexpr int kGroupDelayCustomIndex = 1;

// Abscissa spacing of a newly added curve breakpoint.
constexpr double kCurvePointStepMhz = 1.0;
constexpr double kCurvePointStepIre = 25.0;
// Group delay is relative to its lowest breakpoint, conventionally 0.5 MHz.
constexpr double kGroupDelayReferenceMhz = 0.5;

// Replay response mode combo item order, matching ReplayResponseMode.
int ComboIndexFromResponseMode(videosynth::gui::ReplayResponseMode mode) {
  return static_cast<int>(mode);
}

videosynth::gui::ReplayResponseMode ResponseModeFromComboIndex(int index) {
  switch (index) {
    case 1:
      return videosynth::gui::ReplayResponseMode::kFixed;
    case 2:
      return videosynth::gui::ReplayResponseMode::kByPicture;
    default:
      return videosynth::gui::ReplayResponseMode::kNone;
  }
}

// A double spin box that commits on Enter or focus loss (not per keystroke),
// capped at its size hint so forms stay compact.
QDoubleSpinBox* MakeSpin(QWidget* parent, double min, double max, int decimals,
                         double step, const QString& suffix) {
  auto* spin = new QDoubleSpinBox(parent);
  spin->setRange(min, max);
  spin->setDecimals(decimals);
  spin->setSingleStep(step);
  spin->setSuffix(suffix);
  spin->setKeyboardTracking(false);
  videosynth::gui::CapFieldWidthAtSizeHint(spin);
  return spin;
}

videosynth::VitsPlacement VitsPlacementFromComboIndex(int index) {
  switch (index) {
    case 1:
      return videosynth::VitsPlacement::kLaserdisc;
    case 2:
      return videosynth::VitsPlacement::kCustom;
    default:
      return videosynth::VitsPlacement::kStandard;
  }
}

int64_t ParseSeed(const QString& text) {
  bool ok = false;
  const qlonglong value = text.trimmed().toLongLong(&ok);
  return ok ? static_cast<int64_t>(value) : 0;
}

int ComboIndexFromVitsPlacement(videosynth::VitsPlacement placement) {
  switch (placement) {
    case videosynth::VitsPlacement::kLaserdisc:
      return 1;
    case videosynth::VitsPlacement::kCustom:
      return 2;
    case videosynth::VitsPlacement::kStandard:
    default:
      return 0;
  }
}
}  // namespace

namespace videosynth::gui {

ProjectSettingsEditor::ProjectSettingsEditor(ProjectDocument* document,
                                             QWidget* parent)
    : QWidget(parent), document_(document) {
  BuildUi();
  LoadFromDocument();

  connect(document_, &ProjectDocument::DocumentReset, this,
          [this] { LoadFromDocument(); });
  connect(document_, &ProjectDocument::ProjectSettingsChanged, this, [this] {
    if (!committing_) {
      LoadFromDocument();
    }
  });
}

void ProjectSettingsEditor::BuildUi() {
  auto* layout = new QVBoxLayout(this);

  // --- project: ------------------------------------------------------------
  auto* project_group = new QGroupBox(tr("Project"), this);
  auto* project_form = new QFormLayout(project_group);
  name_edit_ = new QLineEdit(project_group);
  name_edit_->setMinimumWidth(name_edit_->sizeHint().width() * 2);
  version_edit_ = new QLineEdit(project_group);
  CapFieldWidthToText(version_edit_, QStringLiteral("10,000,000"));
  description_edit_ = new QPlainTextEdit(project_group);
  description_edit_->setFixedHeight(60);
  description_edit_->setTabChangesFocus(true);
  project_form->addRow(tr("Name:"), name_edit_);
  project_form->addRow(tr("Version:"), version_edit_);
  project_form->addRow(tr("Description:"), description_edit_);
  layout->addWidget(project_group);

  connect(name_edit_, &QLineEdit::editingFinished, this,
          &ProjectSettingsEditor::CommitProjectInfo);
  connect(version_edit_, &QLineEdit::editingFinished, this,
          &ProjectSettingsEditor::CommitProjectInfo);
  // QPlainTextEdit has no editingFinished; commit on every change (the
  // document command layer collapses no-op commits).
  connect(description_edit_, &QPlainTextEdit::textChanged, this, [this] {
    if (!updating_) {
      CommitProjectInfo();
    }
  });

  // --- cvbs_presets: -------------------------------------------------------
  auto* presets_group = new QGroupBox(tr("CVBS Presets"), this);
  auto* presets_form = new QFormLayout(presets_group);
  standard_combo_ = new QComboBox(presets_group);
  sample_encoding_combo_ = new QComboBox(presets_group);
  signal_state_combo_ = new QComboBox(presets_group);
  setup_ire_check_ =
      new QCheckBox(tr("Override NTSC black setup IRE"), presets_group);
  setup_ire_combo_ = new QComboBox(presets_group);
  secam_field_id_check_ = new QCheckBox(
      tr("SECAM field identification (lines 7–15 / 320–328)"), presets_group);

  presets_form->addRow(tr("Video standard:"), standard_combo_);
  presets_form->addRow(tr("Sample encoding:"), sample_encoding_combo_);
  presets_form->addRow(tr("Signal state:"), signal_state_combo_);
  auto* setup_row = new QHBoxLayout();
  setup_row->addWidget(setup_ire_check_);
  setup_row->addWidget(setup_ire_combo_);
  setup_row->addStretch();
  presets_form->addRow(setup_row);
  presets_form->addRow(secam_field_id_check_);
  layout->addWidget(presets_group);

  connect(standard_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::OnStandardChanged);
  connect(sample_encoding_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::CommitCvbsPresets);
  connect(signal_state_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::CommitCvbsPresets);
  connect(secam_field_id_check_, &QCheckBox::toggled, this, [this] {
    if (!updating_) {
      CommitCvbsPresets();
    }
  });
  connect(setup_ire_check_, &QCheckBox::toggled, this, [this](bool checked) {
    setup_ire_combo_->setEnabled(checked && setup_ire_check_->isEnabled());
    if (!updating_) {
      CommitCvbsPresets();
    }
  });
  connect(setup_ire_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::CommitCvbsPresets);

  // --- output: -------------------------------------------------------------
  auto* output_group = new QGroupBox(tr("Output"), this);
  auto* output_form = new QFormLayout(output_group);
  signal_type_combo_ = new QComboBox(output_group);

  video_path_edit_ = new QLineEdit(output_group);
  auto* browse_button = new QPushButton(tr("Browse…"), output_group);
  auto* video_row = new QHBoxLayout();
  video_row->addWidget(video_path_edit_);
  video_row->addWidget(browse_button);

  // The metadata path is not independently settable: metadata, audio, and any
  // sidecar files are always written to the same folder as the video output,
  // sharing its base name. It is therefore not presented as a separate field;
  // the note below documents where those files are written.
  video_path_hint_ = new QLabel(output_group);
  video_path_hint_->setWordWrap(true);

  outputs_note_ = new QLabel(
      tr("Metadata (.meta), audio (.wav), and sidecar files are written to the "
         "same folder as the video output, sharing its base name."),
      output_group);
  outputs_note_->setWordWrap(true);

  // The component picture that went into the modulator, for scoring
  // decoders against the source rather than against their own decode.
  truth_check_ =
      new QCheckBox(tr("Write the component picture (Y, U, V)"), output_group);
  truth_check_->setToolTip(
      tr("Writes “.truth” and its “.truth.meta” sidecar beside the video "
         "output: the Y, U and V that went into the composite at 4fsc over "
         "the active window, after the horizontal mapping, luma bandwidth "
         "limit, chroma low-pass and chroma line sign, and before any "
         "channel, chroma error, line timing, noise or dropout "
         "impairment."));

  output_form->addRow(tr("Signal type:"), signal_type_combo_);
  output_form->addRow(tr("Video path:"), video_row);
  output_form->addRow(QString(), video_path_hint_);
  output_form->addRow(tr("Picture truth:"), truth_check_);
  output_form->addRow(QString(), outputs_note_);
  layout->addWidget(output_group);

  // The remaining groups follow the project file's block order: the signal
  // content (VITS, then everything LaserDisc-specific in one group that stays
  // a single title row for projects without a laserdisc block), then the
  // pressing and capture impairments.
  layout->addWidget(BuildVitsGroup());
  layout->addWidget(BuildLaserdiscGroup());
  layout->addWidget(BuildChannelGroup());
  layout->addWidget(BuildChromaErrorGroup());
  layout->addWidget(BuildLineTimingGroup());

  layout->addStretch();

  connect(signal_type_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::OnSignalTypeChanged);
  connect(video_path_edit_, &QLineEdit::editingFinished, this,
          &ProjectSettingsEditor::CommitOutputTargets);
  connect(browse_button, &QPushButton::clicked, this,
          &ProjectSettingsEditor::OnBrowseVideoPath);
  connect(truth_check_, &QCheckBox::toggled, this, [this] {
    if (!updating_) {
      CommitOutputTargets();
    }
  });
}

QWidget* ProjectSettingsEditor::BuildChannelGroup() {
  namespace limits = project_editor_limits;
  // --- channel: (pressing impairments) -------------------------------------
  channel_group_ = new QGroupBox(tr("Channel (pressing)"), this);
  channel_group_->setCheckable(true);
  channel_group_->setToolTip(
      tr("Impairments of the pressing and its readout. Every project of one "
         "pressing should use the same channel settings. The LaserVision "
         "replay channel is in the LaserDisc group."));
  auto* outer = new QVBoxLayout(channel_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(channel_group_);
  auto* layout = new QVBoxLayout(body);
  auto* form = new QFormLayout();

  timing_offset_spin_ =
      MakeSpin(body, -limits::kTimingOffsetMaxSamples,
               limits::kTimingOffsetMaxSamples, 3, 0.05, tr(" samples"));
  timing_offset_spin_->setToolTip(
      tr("Sub-sample timing offset in 4fsc samples; positive values make the "
         "signal arrive later."));
  form->addRow(tr("Timing offset:"), timing_offset_spin_);
  layout->addLayout(form);

  edge_smear_group_ = new QGroupBox(tr("Edge smear"), body);
  edge_smear_group_->setCheckable(true);
  edge_smear_group_->setToolTip(
      tr("Readout smear: a fraction of the signal passes through a one-pole "
         "low-pass, trailing every edge."));
  auto* smear_form = new QFormLayout(edge_smear_group_);
  edge_smear_fraction_spin_ =
      MakeSpin(edge_smear_group_, 0.0, limits::kEdgeSmearFractionMax, 3, 0.01,
               QString());
  smear_form->addRow(tr("Fraction:"), edge_smear_fraction_spin_);
  edge_smear_tau_spin_ =
      MakeSpin(edge_smear_group_, limits::kEdgeSmearTauMinSamples,
               limits::kEdgeSmearTauMaxSamples, 2, 0.5, tr(" samples"));
  smear_form->addRow(tr("Time constant:"), edge_smear_tau_spin_);
  layout->addWidget(edge_smear_group_);

  outer->addWidget(body);

  const auto commit = [this] {
    if (!updating_) {
      CommitChannel();
    }
  };
  connect(channel_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(channel_group_, &QGroupBox::toggled, this, commit);
  body->setVisible(channel_group_->isChecked());
  connect(edge_smear_group_, &QGroupBox::toggled, this, commit);
  for (QDoubleSpinBox* spin :
       {timing_offset_spin_, edge_smear_fraction_spin_, edge_smear_tau_spin_}) {
    connect(spin, &QDoubleSpinBox::valueChanged, this, commit);
  }
  return channel_group_;
}

QWidget* ProjectSettingsEditor::BuildChromaErrorGroup() {
  namespace limits = project_editor_limits;
  // --- chroma_error: (capture impairment) ----------------------------------
  chroma_error_group_ = new QGroupBox(tr("Chroma error (capture)"), this);
  chroma_error_group_->setCheckable(true);
  chroma_error_group_->setToolTip(
      tr("Gain and phase error of the player's chroma path, applied to picture "
         "chroma only: burst, VITS and VBI lines are untouched."));
  auto* outer = new QVBoxLayout(chroma_error_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(chroma_error_group_);
  auto* layout = new QVBoxLayout(body);
  auto* form = new QFormLayout();

  chroma_gain_spin_ = MakeSpin(body, limits::kChromaGainMin,
                               limits::kChromaGainMax, 3, 0.01, QString());
  chroma_gain_spin_->setToolTip(
      tr("Chroma gain; 1.0 leaves saturation as is."));
  form->addRow(tr("Gain:"), chroma_gain_spin_);
  chroma_phase_spin_ = MakeSpin(body, -limits::kChromaPhaseMaxDeg,
                                limits::kChromaPhaseMaxDeg, 2, 0.5, tr("°"));
  chroma_phase_spin_->setToolTip(tr("Chroma phase advance (hue error)."));
  form->addRow(tr("Phase:"), chroma_phase_spin_);
  chroma_include_burst_check_ = new QCheckBox(tr("Include burst"), body);
  chroma_include_burst_check_->setToolTip(
      tr("Apply the error to the colour burst too (a whole chroma path), so a "
         "burst-referenced decoder can correct it. Off: picture chroma only "
         "(a mis-adjusted picture chroma path)."));
  form->addRow(QString(), chroma_include_burst_check_);
  layout->addLayout(form);

  chroma_jitter_group_ = new QGroupBox(tr("Per-line jitter"), body);
  chroma_jitter_group_->setCheckable(true);
  chroma_jitter_group_->setToolTip(
      tr("Gaussian gain and phase jitter drawn per line; the values are "
         "standard deviations."));
  auto* jitter_form = new QFormLayout(chroma_jitter_group_);
  jitter_phase_spin_ =
      MakeSpin(chroma_jitter_group_, 0.0, limits::kChromaJitterPhaseMaxDeg, 2,
               0.1, tr("°"));
  jitter_form->addRow(tr("Phase deviation:"), jitter_phase_spin_);
  jitter_gain_spin_ =
      MakeSpin(chroma_jitter_group_, 0.0, limits::kChromaJitterGainMax, 3,
               0.005, QString());
  jitter_form->addRow(tr("Gain deviation:"), jitter_gain_spin_);
  jitter_seed_check_ = new QCheckBox(tr("Fixed seed"), chroma_jitter_group_);
  jitter_seed_edit_ = new QLineEdit(chroma_jitter_group_);
  CapFieldWidthToText(jitter_seed_edit_, QStringLiteral("10,000,000"));
  auto* seed_row = new QHBoxLayout();
  seed_row->addWidget(jitter_seed_check_);
  seed_row->addWidget(jitter_seed_edit_);
  seed_row->addStretch();
  jitter_form->addRow(seed_row);
  layout->addWidget(chroma_jitter_group_);

  differential_group_ = new QGroupBox(tr("Differential gain and phase"), body);
  differential_group_->setCheckable(true);
  differential_group_->setToolTip(
      tr("Chroma gain and phase that depend on the luma level the chroma sits "
         "on (0 IRE at blanking, 100 IRE at white). Each curve's breakpoints "
         "are joined by straight lines and held beyond its ends; an empty "
         "curve leaves the chroma as is."));
  auto* differential_layout = new QVBoxLayout(differential_group_);
  differential_layout->addWidget(new QLabel(tr("Gain:"), differential_group_));
  differential_gain_editor_ = new CurvePointsEditor(
      CurveAxis{tr("Level"), limits::kDifferentialLevelMinIre,
                limits::kDifferentialLevelMaxIre, 1, 5.0, tr(" IRE")},
      CurveAxis{tr("Gain"), limits::kDifferentialGainMin,
                limits::kDifferentialGainMax, 3, 0.01, QString()},
      kCurvePointStepIre, CurvePoint{0.0, 1.0}, differential_group_);
  differential_layout->addWidget(differential_gain_editor_);
  differential_layout->addWidget(new QLabel(tr("Phase:"), differential_group_));
  differential_phase_editor_ = new CurvePointsEditor(
      CurveAxis{tr("Level"), limits::kDifferentialLevelMinIre,
                limits::kDifferentialLevelMaxIre, 1, 5.0, tr(" IRE")},
      CurveAxis{tr("Phase"), -limits::kDifferentialPhaseMaxDeg,
                limits::kDifferentialPhaseMaxDeg, 2, 0.5, tr("°")},
      kCurvePointStepIre, CurvePoint{0.0, 0.0}, differential_group_);
  differential_layout->addWidget(differential_phase_editor_);
  layout->addWidget(differential_group_);

  crosstalk_group_ = new QGroupBox(tr("Crosstalk"), body);
  crosstalk_group_->setCheckable(true);
  crosstalk_group_->setToolTip(
      tr("Mixing of the decoded colour-difference signals: U gains a share "
         "of V and V a share of U."));
  auto* crosstalk_form = new QFormLayout(crosstalk_group_);
  crosstalk_v_to_u_spin_ = MakeSpin(crosstalk_group_, -limits::kCrosstalkMax,
                                    limits::kCrosstalkMax, 3, 0.005, QString());
  crosstalk_v_to_u_spin_->setToolTip(tr("Share of V added to U."));
  crosstalk_form->addRow(tr("V into U:"), crosstalk_v_to_u_spin_);
  crosstalk_u_to_v_spin_ = MakeSpin(crosstalk_group_, -limits::kCrosstalkMax,
                                    limits::kCrosstalkMax, 3, 0.005, QString());
  crosstalk_u_to_v_spin_->setToolTip(tr("Share of U added to V."));
  crosstalk_form->addRow(tr("U into V:"), crosstalk_u_to_v_spin_);
  crosstalk_alternate_check_ =
      new QCheckBox(tr("Alternate with PAL V-switch"), crosstalk_group_);
  crosstalk_alternate_check_->setToolTip(
      tr("Flip the sign of the mixing on alternate lines with the PAL "
         "V-switch (PAL and PAL-M only)."));
  crosstalk_form->addRow(QString(), crosstalk_alternate_check_);
  layout->addWidget(crosstalk_group_);

  chroma_error_hint_ = new QLabel(
      tr("Chroma error models a quadrature (PAL/NTSC) chroma path, so it is "
         "not available for SECAM."),
      body);
  chroma_error_hint_->setWordWrap(true);
  outer->addWidget(body);
  outer->addWidget(chroma_error_hint_);

  const auto commit = [this] {
    if (!updating_) {
      CommitChromaError();
    }
  };
  connect(chroma_error_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(chroma_error_group_, &QGroupBox::toggled, this, commit);
  body->setVisible(chroma_error_group_->isChecked());
  connect(chroma_jitter_group_, &QGroupBox::toggled, this, commit);
  connect(differential_group_, &QGroupBox::toggled, this, commit);
  connect(crosstalk_group_, &QGroupBox::toggled, this, commit);
  connect(chroma_include_burst_check_, &QCheckBox::toggled, this, commit);
  connect(crosstalk_alternate_check_, &QCheckBox::toggled, this, commit);
  connect(differential_gain_editor_, &CurvePointsEditor::CurveEdited, this,
          commit);
  connect(differential_phase_editor_, &CurvePointsEditor::CurveEdited, this,
          commit);
  for (QDoubleSpinBox* spin :
       {chroma_gain_spin_, chroma_phase_spin_, jitter_phase_spin_,
        jitter_gain_spin_, crosstalk_v_to_u_spin_, crosstalk_u_to_v_spin_}) {
    connect(spin, &QDoubleSpinBox::valueChanged, this, commit);
  }
  connect(jitter_seed_check_, &QCheckBox::toggled, this, [this](bool checked) {
    jitter_seed_edit_->setEnabled(checked);
    if (!updating_) {
      CommitChromaError();
    }
  });
  connect(jitter_seed_edit_, &QLineEdit::editingFinished, this, commit);
  return chroma_error_group_;
}

QWidget* ProjectSettingsEditor::BuildLineTimingGroup() {
  namespace limits = project_editor_limits;
  // --- line_timing: (capture impairment, optional pressing part) -----------
  line_timing_group_ = new QGroupBox(tr("Line timing (capture)"), this);
  line_timing_group_->setCheckable(true);
  line_timing_group_->setToolTip(
      tr("Per-line timing error of the active picture left after time-base "
         "correction: each line is shifted by a few ns, differently on every "
         "line, field and frame. Sync, VBI and VITS lines stay where they "
         "are. The figures are r.m.s. values."));
  auto* outer = new QVBoxLayout(line_timing_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(line_timing_group_);
  auto* layout = new QVBoxLayout(body);

  // One layer's figures and seed, in `form`.
  const auto build_layer = [this](QWidget* parent, QFormLayout* form,
                                  LineTimingLayerWidgets* widgets) {
    widgets->line_ns =
        MakeSpin(parent, 0.0, limits::kLineTimingMaxNs, 2, 0.1, tr(" ns"));
    widgets->line_ns->setToolTip(
        tr("R.m.s. shift of each line within its field (field mean "
           "removed)."));
    form->addRow(tr("Per line:"), widgets->line_ns);
    widgets->correlation = MakeSpin(
        parent, 0.0, limits::kLineTimingCorrelationMax, 2, 0.05, QString());
    widgets->correlation->setToolTip(
        tr("Correlation of a line's error with the next line of the same "
           "field; 0 draws every line independently."));
    form->addRow(tr("Line correlation:"), widgets->correlation);
    widgets->field_ns =
        MakeSpin(parent, 0.0, limits::kLineTimingMaxNs, 2, 0.1, tr(" ns"));
    widgets->field_ns->setToolTip(tr("R.m.s. offset common to a whole field."));
    form->addRow(tr("Per field:"), widgets->field_ns);
    widgets->slope_ns =
        MakeSpin(parent, 0.0, limits::kLineTimingMaxNs, 2, 0.1, tr(" ns"));
    widgets->slope_ns->setToolTip(
        tr("R.m.s. difference between the shifts of the two halves of a line "
           "(a velocity error growing along the line)."));
    form->addRow(tr("Slope:"), widgets->slope_ns);
    widgets->seed_check = new QCheckBox(tr("Fixed seed"), parent);
    widgets->seed_edit = new QLineEdit(parent);
    CapFieldWidthToText(widgets->seed_edit, QStringLiteral("10,000,000"));
    auto* seed_row = new QHBoxLayout();
    seed_row->addWidget(widgets->seed_check);
    seed_row->addWidget(widgets->seed_edit);
    seed_row->addStretch();
    form->addRow(seed_row);
  };

  auto* form = new QFormLayout();
  build_layer(body, form, &line_timing_capture_);
  line_timing_capture_.seed_check->setToolTip(
      tr("Off: a different draw on every run."));
  line_timing_burst_check_ = new QCheckBox(tr("Include burst"), body);
  line_timing_burst_check_->setToolTip(
      tr("Move each picture line's colour burst with the line, for decoders "
         "that reference every line to its own burst. Off: the picture moves "
         "against a burst-locked line, as measured on real captures."));
  form->addRow(QString(), line_timing_burst_check_);
  layout->addLayout(form);

  line_timing_shared_group_ =
      new QGroupBox(tr("Recorded on the disc (pressing)"), body);
  line_timing_shared_group_->setCheckable(true);
  line_timing_shared_group_->setToolTip(
      tr("A second part shared by every capture of the pressing: drawn by "
         "disc position from its own fixed seed, and added to the capture "
         "part."));
  auto* shared_form = new QFormLayout(line_timing_shared_group_);
  build_layer(line_timing_shared_group_, shared_form, &line_timing_shared_);
  // Validation requires the pressing seed, so it is always fixed.
  line_timing_shared_.seed_check->setChecked(true);
  line_timing_shared_.seed_check->setEnabled(false);
  layout->addWidget(line_timing_shared_group_);
  outer->addWidget(body);

  const auto commit = [this] {
    if (!updating_) {
      CommitLineTiming();
    }
  };
  connect(line_timing_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(line_timing_group_, &QGroupBox::toggled, this, commit);
  body->setVisible(line_timing_group_->isChecked());
  connect(line_timing_shared_group_, &QGroupBox::toggled, this, commit);
  connect(line_timing_burst_check_, &QCheckBox::toggled, this, commit);
  for (LineTimingLayerWidgets* widgets :
       {&line_timing_capture_, &line_timing_shared_}) {
    for (QDoubleSpinBox* spin : {widgets->line_ns, widgets->correlation,
                                 widgets->field_ns, widgets->slope_ns}) {
      connect(spin, &QDoubleSpinBox::valueChanged, this, commit);
    }
    connect(widgets->seed_edit, &QLineEdit::editingFinished, this, commit);
  }
  connect(line_timing_capture_.seed_check, &QCheckBox::toggled, this,
          [this](bool checked) {
            line_timing_capture_.seed_edit->setEnabled(checked);
            if (!updating_) {
              CommitLineTiming();
            }
          });
  return line_timing_group_;
}

QWidget* ProjectSettingsEditor::BuildVitsGroup() {
  // --- line_injections: (project-wide VITS set) ----------------------------
  auto* vits_group = new QGroupBox(tr("VITS"), this);
  auto* vits_layout = new QVBoxLayout(vits_group);
  auto* placement_form = new QFormLayout();
  vits_placement_combo_ = new QComboBox(vits_group);
  vits_placement_combo_->addItem(tr("Standard (broadcast lines)"));
  vits_placement_combo_->addItem(tr("Laserdisc (IEC 60856/60857)"));
  vits_placement_combo_->addItem(tr("Custom (choose lines)"));
  placement_form->addRow(tr("Placement:"), vits_placement_combo_);
  vits_layout->addLayout(placement_form);

  connect(vits_placement_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::OnVitsPlacementChanged);

  vits_label_ = new QLabel(vits_group);
  vits_label_->setWordWrap(true);
  vits_layout->addWidget(vits_label_);

  auto* vits_host = new QWidget(vits_group);
  vits_checklist_layout_ = new QGridLayout(vits_host);
  vits_checklist_layout_->setContentsMargins(0, 0, 0, 0);
  // Stretch an empty trailing column so each checkbox and its lines edit
  // pack together on the left.
  vits_checklist_layout_->setColumnStretch(2, 1);
  vits_layout->addWidget(vits_host);
  return vits_group;
}

QWidget* ProjectSettingsEditor::BuildLaserdiscGroup() {
  // --- laserdisc: (LaserVision disc, its audio and its channel) ------------
  laserdisc_group_ = new QGroupBox(tr("LaserDisc"), this);
  laserdisc_group_->setCheckable(true);
  laserdisc_group_->setToolTip(
      tr("LaserDisc (LaserVision) properties of the project: the disc "
         "format, the LaserDisc-only signal features, digital audio and the "
         "pressing and replay channel. Untick for a project that is not a "
         "LaserDisc; sections then carry no disc area or codes."));
  auto* outer = new QVBoxLayout(laserdisc_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(laserdisc_group_);
  auto* layout = new QVBoxLayout(body);
  auto* form = new QFormLayout();

  disc_type_combo_ = new QComboBox(body);
  disc_type_combo_->addItem(QString::fromLatin1(kDiscTypeNone));
  for (const std::string& disc_type : AvailableDiscTypes()) {
    disc_type_combo_->addItem(QString::fromStdString(disc_type));
  }
  disc_type_combo_->setToolTip(
      tr("CAV (picture numbers) or CLV (time codes). Sections need a disc "
         "format before they can carry laserdisc codes."));
  form->addRow(tr("Disc format:"), disc_type_combo_);

  pilot_burst_check_ =
      new QCheckBox(tr("PAL pilot burst (IEC 60856 §9.1.2)"), body);
  pilot_burst_check_->setToolTip(
      tr("The 3.75 MHz pilot burst on every sync pulse (PAL only)."));
  form->addRow(pilot_burst_check_);
  vbi_burst_check_ =
      new QCheckBox(tr("NTSC VBI burst (IEC 60857 §9.1.2)"), body);
  vbi_burst_check_->setToolTip(
      tr("The colour burst kept on every vertical-interval line (NTSC "
         "only)."));
  form->addRow(vbi_burst_check_);

  // LaserDisc digital audio: one channel pair is additionally encoded as an
  // EFM stream (IEC 60856:1986 Amd 2 / IEC 60857:1986 Amd 2, clause 13); its
  // WAV track is written as usual.
  efm_check_ = new QCheckBox(tr("Encode one channel pair as EFM"), body);
  efm_pair_combo_ = new QComboBox(body);
  auto* efm_row = new QHBoxLayout();
  efm_row->addWidget(efm_check_);
  efm_row->addWidget(new QLabel(tr("Pair:"), body));
  efm_row->addWidget(efm_pair_combo_);
  efm_row->addStretch();
  efm_hint_ = new QLabel(body);
  efm_hint_->setWordWrap(true);
  form->addRow(tr("Digital audio:"), efm_row);
  form->addRow(QString(), efm_hint_);
  layout->addLayout(form);

  layout->addWidget(BuildEfmDataGroup(body));
  layout->addWidget(BuildLaserdiscChannelGroup(body));
  outer->addWidget(body);

  laserdisc_hint_ = new QLabel(
      tr("There is no SECAM LaserVision format, so the LaserDisc settings are "
         "not available for SECAM projects."),
      laserdisc_group_);
  laserdisc_hint_->setWordWrap(true);
  outer->addWidget(laserdisc_hint_);

  const auto commit = [this] {
    if (!updating_) {
      CommitLaserdisc();
    }
  };
  connect(laserdisc_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(laserdisc_group_, &QGroupBox::toggled, this, commit);
  body->setVisible(laserdisc_group_->isChecked());
  connect(disc_type_combo_, &QComboBox::activated, this, commit);
  connect(pilot_burst_check_, &QCheckBox::toggled, this, commit);
  connect(vbi_burst_check_, &QCheckBox::toggled, this, commit);
  connect(efm_check_, &QCheckBox::toggled, this, commit);
  connect(efm_pair_combo_, &QComboBox::activated, this, commit);
  return laserdisc_group_;
}

QWidget* ProjectSettingsEditor::BuildEfmDataGroup(QWidget* parent) {
  namespace limits = project_editor_limits;
  // LaserDisc EFM data (AIV / LV-ROM): a sector image encoded as ECMA-130
  // Mode 1 sectors in the EFM channel, locked to the pictures of the sections
  // that carry it.
  efm_data_group_ = new QGroupBox(tr("EFM data (AIV / LV-ROM)"), parent);
  efm_data_group_->setCheckable(true);
  efm_data_group_->setToolTip(
      tr("Encode a sector image (2048 bytes per sector) as CD-ROM Mode 1 "
         "sectors in the EFM channel, as AIV discs such as Domesday carry. "
         "A disc has one EFM channel, so this and digital audio exclude each "
         "other. Each section chooses whether it carries the data."));
  auto* outer = new QVBoxLayout(efm_data_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(efm_data_group_);
  auto* form = new QFormLayout(body);

  efm_data_image_edit_ = new QLineEdit(body);
  efm_data_image_edit_->setToolTip(
      tr("The sector image: the user data of consecutive sectors, 2048 bytes "
         "each. A relative path is relative to the project file."));
  efm_data_browse_button_ = new QPushButton(tr("Browse…"), body);
  auto* image_row = new QHBoxLayout();
  image_row->addWidget(efm_data_image_edit_, 1);
  image_row->addWidget(efm_data_browse_button_);
  form->addRow(tr("Sector image:"), image_row);

  efm_data_first_address_spin_ = new QSpinBox(body);
  efm_data_first_address_spin_->setRange(0, limits::kEfmDataMaxSectorAddress);
  efm_data_first_address_spin_->setSuffix(tr(" sectors"));
  efm_data_first_address_spin_->setToolTip(
      tr("The sector address of the image's first sector (75 sectors per "
         "second)."));
  form->addRow(tr("First address:"), efm_data_first_address_spin_);

  efm_data_offset_spin_ = new QSpinBox(body);
  efm_data_offset_spin_->setRange(0, limits::kEfmDataMaxSectorAddress);
  efm_data_offset_spin_->setSuffix(tr(" sectors"));
  efm_data_offset_spin_->setToolTip(
      tr("How far a picture's sector addresses run ahead of its time. The "
         "AIV rule is 150 (2 s): on PAL, picture p carries sectors 3p + 150 "
         "to 3p + 152."));
  form->addRow(tr("Picture offset:"), efm_data_offset_spin_);

  efm_data_q_control_combo_ = new QComboBox(body);
  efm_data_q_control_combo_->setToolTip(
      tr("The CONTROL field of the Q channel: a data track, with digital "
         "copying prohibited or permitted."));
  form->addRow(tr("Q control:"), efm_data_q_control_combo_);

  efm_data_q_mode_combo_ = new QComboBox(body);
  efm_data_q_mode_combo_->setToolTip(
      tr("The Q-channel mode. IEC 60856/60857 Amd 2 specify mode 4 for "
         "LaserDisc; AIV discs such as Domesday predate those amendments and "
         "record mode 1."));
  form->addRow(tr("Q mode:"), efm_data_q_mode_combo_);
  outer->addWidget(body);

  efm_data_hint_ = new QLabel(efm_data_group_);
  efm_data_hint_->setWordWrap(true);
  outer->addWidget(efm_data_hint_);

  const auto commit = [this] {
    if (!updating_) {
      CommitLaserdisc();
    }
  };
  connect(efm_data_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(efm_data_group_, &QGroupBox::toggled, this, commit);
  body->setVisible(efm_data_group_->isChecked());
  connect(efm_data_image_edit_, &QLineEdit::editingFinished, this, commit);
  connect(efm_data_browse_button_, &QPushButton::clicked, this,
          &ProjectSettingsEditor::OnBrowseEfmDataImage);
  connect(efm_data_first_address_spin_, &QSpinBox::valueChanged, this, commit);
  connect(efm_data_offset_spin_, &QSpinBox::valueChanged, this, commit);
  connect(efm_data_q_control_combo_, &QComboBox::activated, this, commit);
  connect(efm_data_q_mode_combo_, &QComboBox::activated, this, commit);
  return efm_data_group_;
}

QWidget* ProjectSettingsEditor::BuildLaserdiscChannelGroup(QWidget* parent) {
  namespace limits = project_editor_limits;
  ld_channel_group_ =
      new QGroupBox(tr("LaserVision channel (pressing)"), parent);
  ld_channel_group_->setCheckable(true);
  ld_channel_group_->setToolTip(
      tr("What the master, the disc and a player's replay path do to the "
         "video signal. It belongs to the pressing, so every project of one "
         "pressing should use the same settings."));
  auto* outer = new QVBoxLayout(ld_channel_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(ld_channel_group_);
  auto* layout = new QVBoxLayout(body);
  fm_beat_group_ = new QGroupBox(tr("FM beat (herringbone)"), body);
  fm_beat_group_->setCheckable(true);
  fm_beat_group_->setToolTip(
      tr("The beat between the FM video carrier and the colour signal, "
         "recorded on the master (625-line PAL only)."));
  auto* beat_form = new QFormLayout(fm_beat_group_);
  fm_beat_carrier_spin_ =
      MakeSpin(fm_beat_group_, limits::kFmBeatCarrierMinHz,
               limits::kFmBeatCarrierMaxHz, 0, 1000.0, tr(" Hz"));
  fm_beat_carrier_spin_->setToolTip(tr("FM video carrier frequency at black."));
  beat_form->addRow(tr("Carrier at black:"), fm_beat_carrier_spin_);
  fm_beat_level_spin_ = MakeSpin(fm_beat_group_, limits::kFmBeatLevelMinDb,
                                 limits::kFmBeatLevelMaxDb, 1, 1.0, tr(" dB"));
  fm_beat_level_spin_->setToolTip(
      tr("Beat level relative to the local chroma amplitude, at the carrier "
         "for 50 IRE."));
  beat_form->addRow(tr("Level:"), fm_beat_level_spin_);
  layout->addWidget(fm_beat_group_);
  fm_beat_hint_ = new QLabel(
      tr("The FM beat model is fitted to PAL LaserDisc pressings, so it is "
         "available for PAL projects only."),
      body);
  fm_beat_hint_->setWordWrap(true);
  layout->addWidget(fm_beat_hint_);

  group_delay_group_ = new QGroupBox(tr("Group delay"), body);
  group_delay_group_->setCheckable(true);
  group_delay_group_->setToolTip(
      tr("The LaserVision group-delay pre-distortion: an all-pass element "
         "whose delay follows a curve relative to its lowest breakpoint, "
         "scaled by the fraction of the recorded pre-distortion left in the "
         "signal."));
  auto* delay_form = new QFormLayout(group_delay_group_);
  group_delay_preset_combo_ = new QComboBox(group_delay_group_);
  group_delay_preset_combo_->addItem(tr("IEC 60856 nominal"));
  group_delay_preset_combo_->addItem(tr("Custom curve"));
  group_delay_preset_combo_->setToolTip(
      tr("IEC 60856 nominal: the PAL LaserVision pre-distortion of IEC 60856 "
         "§9.1.6 (PAL only). Custom curve: your own [MHz, ns] breakpoints."));
  delay_form->addRow(tr("Curve:"), group_delay_preset_combo_);
  group_delay_curve_editor_ = new CurvePointsEditor(
      CurveAxis{tr("Frequency"), 0.0, 0.0, 2, 0.1, tr(" MHz")},
      CurveAxis{tr("Delay"), -limits::kGroupDelayMaxNs,
                limits::kGroupDelayMaxNs, 1, 5.0, tr(" ns")},
      kCurvePointStepMhz, CurvePoint{kGroupDelayReferenceMhz, 0.0},
      group_delay_group_);
  delay_form->addRow(group_delay_curve_editor_);
  group_delay_scale_spin_ =
      MakeSpin(group_delay_group_, -limits::kGroupDelayScaleMax,
               limits::kGroupDelayScaleMax, 2, 0.05, QString());
  group_delay_scale_spin_->setToolTip(
      tr("Fraction of the curve applied: 1 leaves the full pre-distortion in "
         "the signal, 0 none, negative values correct it."));
  delay_form->addRow(tr("Scale:"), group_delay_scale_spin_);
  layout->addWidget(group_delay_group_);

  auto* response_group = new QGroupBox(tr("Replay response"), body);
  response_group->setToolTip(
      tr("Linear-phase amplitude response of the replay path, as [MHz, dB] "
         "breakpoints: one fixed response, or responses at several disc "
         "positions interpolated by picture number."));
  auto* response_layout = new QVBoxLayout(response_group);
  auto* response_form = new QFormLayout();
  response_mode_combo_ = new QComboBox(response_group);
  response_mode_combo_->addItem(tr("None"));
  response_mode_combo_->addItem(tr("Fixed response"));
  response_mode_combo_->addItem(tr("By disc position"));
  response_form->addRow(tr("Response:"), response_mode_combo_);
  response_layout->addLayout(response_form);
  response_curve_editor_ = new CurvePointsEditor(
      CurveAxis{tr("Frequency"), 0.0, 0.0, 2, 0.1, tr(" MHz")},
      CurveAxis{tr("Response"), limits::kReplayResponseMinDb,
                limits::kReplayResponseMaxDb, 1, 0.5, tr(" dB")},
      kCurvePointStepMhz, CurvePoint{kGroupDelayReferenceMhz, 0.0},
      response_group);
  response_layout->addWidget(response_curve_editor_);
  response_by_picture_editor_ = new ResponseByPictureEditor(response_group);
  response_layout->addWidget(response_by_picture_editor_);
  layout->addWidget(response_group);

  field2_offset_group_ = new QGroupBox(tr("Field-2 offset"), body);
  field2_offset_group_->setCheckable(true);
  field2_offset_group_->setToolTip(
      tr("Misregistration of field 2 against field 1, applied to the picture "
         "rows field 2 carries before encoding."));
  auto* offset_form = new QFormLayout(field2_offset_group_);
  field2_x_spin_ =
      MakeSpin(field2_offset_group_, -limits::kField2OffsetMaxSamples,
               limits::kField2OffsetMaxSamples, 3, 0.05, tr(" samples"));
  field2_x_spin_->setToolTip(
      tr("Horizontal offset in 4fsc samples; positive moves field 2 right."));
  offset_form->addRow(tr("Horizontal:"), field2_x_spin_);
  field2_y_spin_ =
      MakeSpin(field2_offset_group_, -limits::kField2OffsetMaxLines,
               limits::kField2OffsetMaxLines, 3, 0.05, tr(" lines"));
  field2_y_spin_->setToolTip(
      tr("Vertical offset in frame lines; positive moves field 2 down."));
  offset_form->addRow(tr("Vertical:"), field2_y_spin_);
  layout->addWidget(field2_offset_group_);
  outer->addWidget(body);

  const auto commit = [this] {
    if (!updating_) {
      CommitLaserdisc();
    }
  };
  connect(ld_channel_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(ld_channel_group_, &QGroupBox::toggled, this, commit);
  body->setVisible(ld_channel_group_->isChecked());
  for (QGroupBox* group :
       {fm_beat_group_, group_delay_group_, field2_offset_group_}) {
    connect(group, &QGroupBox::toggled, this, commit);
  }
  for (QDoubleSpinBox* spin :
       {fm_beat_carrier_spin_, fm_beat_level_spin_, group_delay_scale_spin_,
        field2_x_spin_, field2_y_spin_}) {
    connect(spin, &QDoubleSpinBox::valueChanged, this, commit);
  }
  connect(group_delay_preset_combo_, &QComboBox::activated, this, commit);
  connect(response_mode_combo_, &QComboBox::activated, this, commit);
  connect(group_delay_curve_editor_, &CurvePointsEditor::CurveEdited, this,
          commit);
  connect(response_curve_editor_, &CurvePointsEditor::CurveEdited, this,
          commit);
  connect(response_by_picture_editor_, &ResponseByPictureEditor::EntriesEdited,
          this, commit);
  return ld_channel_group_;
}

void ProjectSettingsEditor::LoadChannelAndChromaError(const Project& project) {
  const ChannelParameters& channel = project.channel;
  channel_group_->setChecked(channel.enabled);
  timing_offset_spin_->setValue(channel.timing_offset_samples);
  edge_smear_group_->setChecked(channel.edge_smear.enabled);
  edge_smear_fraction_spin_->setValue(channel.edge_smear.fraction);
  edge_smear_tau_spin_->setValue(channel.edge_smear.tau_samples);

  const ChromaErrorParameters& chroma_error = project.chroma_error;
  chroma_error_group_->setChecked(chroma_error.enabled);
  chroma_gain_spin_->setValue(chroma_error.gain);
  chroma_phase_spin_->setValue(chroma_error.phase_deg);
  chroma_include_burst_check_->setChecked(chroma_error.include_burst);
  const ChromaJitterParameters& jitter = chroma_error.line_jitter;
  chroma_jitter_group_->setChecked(jitter.enabled);
  jitter_phase_spin_->setValue(jitter.phase_deg);
  jitter_gain_spin_->setValue(jitter.gain);
  jitter_seed_check_->setChecked(jitter.seed_specified);
  jitter_seed_edit_->setEnabled(jitter.seed_specified);
  jitter_seed_edit_->setText(
      jitter.seed_specified
          ? QString::number(static_cast<qlonglong>(jitter.seed))
          : QString());
  const DifferentialParameters& differential = chroma_error.differential;
  differential_group_->setChecked(differential.enabled);
  differential_gain_editor_->SetPoints(ToCurvePoints(differential.gain));
  differential_phase_editor_->SetPoints(ToCurvePoints(differential.phase_deg));
  const CrosstalkParameters& crosstalk = chroma_error.crosstalk;
  crosstalk_group_->setChecked(crosstalk.enabled);
  crosstalk_v_to_u_spin_->setValue(crosstalk.v_to_u);
  crosstalk_u_to_v_spin_->setValue(crosstalk.u_to_v);
  crosstalk_alternate_check_->setChecked(crosstalk.alternate_with_switch);
}

void ProjectSettingsEditor::LoadLineTiming(const Project& project) {
  const LineTimingParameters& line_timing = project.line_timing;
  line_timing_group_->setChecked(line_timing.enabled);
  line_timing_burst_check_->setChecked(line_timing.include_burst);
  line_timing_shared_group_->setChecked(line_timing.shared.enabled);
  const auto load_layer = [](const LineTimingLayer& layer,
                             const LineTimingLayerWidgets& widgets) {
    widgets.line_ns->setValue(layer.line_ns);
    widgets.correlation->setValue(layer.line_correlation);
    widgets.field_ns->setValue(layer.field_ns);
    widgets.slope_ns->setValue(layer.slope_ns);
    widgets.seed_edit->setText(
        layer.seed_specified
            ? QString::number(static_cast<qlonglong>(layer.seed))
            : QString());
  };
  load_layer(line_timing.capture, line_timing_capture_);
  line_timing_capture_.seed_check->setChecked(
      line_timing.capture.seed_specified);
  line_timing_capture_.seed_edit->setEnabled(
      line_timing.capture.seed_specified);
  load_layer(line_timing.shared, line_timing_shared_);
}

void ProjectSettingsEditor::LoadLaserdisc(const Project& project) {
  const LaserdiscParameters& laserdisc = project.laserdisc;
  laserdisc_group_->setChecked(laserdisc.enabled);

  const QString disc_type = laserdisc.disc_type.empty()
                                ? QString::fromLatin1(kDiscTypeNone)
                                : QString::fromStdString(laserdisc.disc_type);
  int disc_index = disc_type_combo_->findText(disc_type);
  if (disc_index < 0) {
    // Preserve values the catalogue does not know (kept for round-trips).
    disc_type_combo_->addItem(disc_type);
    disc_index = disc_type_combo_->count() - 1;
  }
  disc_type_combo_->setCurrentIndex(disc_index);
  pilot_burst_check_->setChecked(laserdisc.pilot_burst);
  vbi_burst_check_->setChecked(laserdisc.vbi_burst);

  const ProjectSettingsFormState state = BuildProjectSettingsFormState(project);
  efm_pair_combo_->clear();
  for (const int pair : state.efm_pair_options) {
    efm_pair_combo_->addItem(QString::number(pair), pair);
  }
  const int efm_pair_index =
      efm_pair_combo_->findData(laserdisc.efm_audio.pair);
  efm_pair_combo_->setCurrentIndex(efm_pair_index >= 0 ? efm_pair_index : 0);
  efm_check_->setChecked(laserdisc.efm_audio.enabled);

  const EfmDataOutput& efm_data = laserdisc.efm_data;
  efm_data_group_->setChecked(efm_data.enabled);
  efm_data_image_edit_->setText(QString::fromStdString(efm_data.image));
  efm_data_first_address_spin_->setValue(efm_data.first_address);
  efm_data_offset_spin_->setValue(efm_data.picture_offset_sectors);
  efm_data_q_control_combo_->clear();
  for (const EfmDataQControl control : state.efm_data_q_control_options) {
    efm_data_q_control_combo_->addItem(control == EfmDataQControl::kData
                                           ? tr("Data, copy prohibited")
                                           : tr("Data, copy permitted"),
                                       static_cast<int>(control));
  }
  efm_data_q_control_combo_->setCurrentIndex(
      std::max(0, efm_data_q_control_combo_->findData(
                      static_cast<int>(efm_data.q_control))));
  efm_data_q_mode_combo_->clear();
  for (const int mode : state.efm_data_q_mode_options) {
    efm_data_q_mode_combo_->addItem(
        mode == kEfmDataQModeLaserdisc
            ? tr("4 (LaserDisc, IEC 60856/60857 Amd 2)")
            : tr("%1 (compact disc, pre-standard AIV)").arg(mode),
        mode);
  }
  int q_mode_index = efm_data_q_mode_combo_->findData(efm_data.q_mode);
  if (q_mode_index < 0) {
    // Preserve a value the validator rejects so the issues dock can say why.
    efm_data_q_mode_combo_->addItem(QString::number(efm_data.q_mode),
                                    efm_data.q_mode);
    q_mode_index = efm_data_q_mode_combo_->count() - 1;
  }
  efm_data_q_mode_combo_->setCurrentIndex(q_mode_index);

  const LaserdiscChannelParameters& channel = laserdisc.channel;
  ld_channel_group_->setChecked(channel.enabled);
  fm_beat_group_->setChecked(channel.fm_beat.enabled);
  fm_beat_carrier_spin_->setValue(channel.fm_beat.carrier_black_hz);
  fm_beat_level_spin_->setValue(channel.fm_beat.level_db);

  const double max_frequency_mhz =
      NoiseSpectrumMaxFrequencyMhz(project.cvbs_presets.video_standard_preset);
  const GroupDelayParameters& group_delay = channel.group_delay;
  group_delay_group_->setChecked(group_delay.enabled);
  const bool custom_delay = group_delay.preset == GroupDelayPreset::kCustom;
  group_delay_preset_combo_->setCurrentIndex(
      custom_delay ? kGroupDelayCustomIndex : kGroupDelayNominalIndex);
  group_delay_curve_editor_->SetXRange(0.0, max_frequency_mhz);
  group_delay_curve_editor_->SetPoints(ToCurvePoints(group_delay.points_ns));
  group_delay_curve_editor_->setVisible(custom_delay);
  group_delay_scale_spin_->setValue(group_delay.scale);

  const ReplayResponseMode response_mode = ReplayResponseModeOf(channel);
  response_mode_combo_->setCurrentIndex(
      ComboIndexFromResponseMode(response_mode));
  response_curve_editor_->SetXRange(0.0, max_frequency_mhz);
  response_curve_editor_->SetPoints(ToCurvePoints(channel.response_db));
  response_curve_editor_->setVisible(response_mode ==
                                     ReplayResponseMode::kFixed);
  response_by_picture_editor_->SetEntries(channel.response_by_picture,
                                          max_frequency_mhz);
  response_by_picture_editor_->setVisible(response_mode ==
                                          ReplayResponseMode::kByPicture);

  const Field2OffsetParameters& offset = channel.field2_offset;
  field2_offset_group_->setChecked(offset.enabled);
  field2_x_spin_->setValue(offset.x_samples);
  field2_y_spin_->setValue(offset.y_lines);
}

void ProjectSettingsEditor::LoadFromDocument() {
  updating_ = true;
  const Project& project = document_->project();
  const ProjectSettingsFormState state = BuildProjectSettingsFormState(project);

  name_edit_->setText(QString::fromStdString(project.name));
  version_edit_->setText(QString::fromStdString(project.version));
  if (description_edit_->toPlainText().toStdString() != project.description) {
    description_edit_->setPlainText(
        QString::fromStdString(project.description));
  }

  const auto fill_combo = [](QComboBox* combo,
                             const std::vector<std::string>& options,
                             const QString& current) {
    combo->clear();
    for (const std::string& option : options) {
      combo->addItem(QString::fromStdString(option));
    }
    const int index = combo->findText(current);
    if (index >= 0) {
      combo->setCurrentIndex(index);
    } else if (!current.isEmpty()) {
      // Preserve values the catalogue does not know (kept for round-trips).
      combo->addItem(current);
      combo->setCurrentIndex(combo->count() - 1);
    }
  };

  fill_combo(standard_combo_, state.standard_options,
             QString::fromStdString(
                 StandardToString(project.cvbs_presets.video_standard_preset)));
  fill_combo(
      sample_encoding_combo_, state.sample_encoding_options,
      QString::fromStdString(project.cvbs_presets.sample_encoding_preset));
  fill_combo(signal_state_combo_, state.signal_state_options,
             QString::fromStdString(project.cvbs_presets.signal_state_preset));
  fill_combo(signal_type_combo_, state.signal_type_options,
             QString::fromStdString(project.output.signal_type));

  setup_ire_combo_->clear();
  for (const double option : state.ntsc_black_setup_ire_options) {
    setup_ire_combo_->addItem(QString::number(option), option);
  }
  const int ire_index =
      setup_ire_combo_->findData(project.cvbs_presets.ntsc_black_setup_ire);
  setup_ire_combo_->setCurrentIndex(ire_index >= 0 ? ire_index : 0);

  setup_ire_check_->setChecked(
      project.cvbs_presets.ntsc_black_setup_ire_specified);
  secam_field_id_check_->setChecked(
      project.cvbs_presets.secam_field_identification);

  vits_placement_combo_->setCurrentIndex(
      ComboIndexFromVitsPlacement(project.line_injections.placement));
  RebuildVitsChecklist();

  video_path_edit_->setText(QString::fromStdString(project.output.video_path));

  truth_check_->setChecked(project.output.truth);

  LoadChannelAndChromaError(project);
  LoadLineTiming(project);
  LoadLaserdisc(project);

  ApplyEnablement();
  updating_ = false;
}

void ProjectSettingsEditor::RebuildVitsChecklist() {
  vits_rows_.clear();
  QLayoutItem* item = nullptr;
  while ((item = vits_checklist_layout_->takeAt(0)) != nullptr) {
    delete item->widget();
    delete item;
  }

  const Project& project = document_->project();
  const Standard standard = project.cvbs_presets.video_standard_preset;
  const VitsPlacement placement = project.line_injections.placement;
  const std::vector<std::string> vits_types = AvailableVitsTypes(standard);

  switch (placement) {
    case VitsPlacement::kStandard:
      vits_label_->setText(
          tr("VITS test signals (applied project-wide) — tick the signals to "
             "include; each sits on its standard broadcast line."));
      break;
    case VitsPlacement::kLaserdisc:
      vits_label_->setText(
          tr("VITS test signals placed on the laserdisc VBI lines "
             "(IEC 60856/60857); the spec-required set is pre-selected. "
             "Lines stay clear of the address-code ranges."));
      break;
    case VitsPlacement::kCustom:
      vits_label_->setText(
          tr("VITS test signals (applied project-wide) — tick the signals to "
             "include and set each target VBI line."));
      break;
  }

  const auto find_existing =
      [&project](const std::string& vits_type) -> const VitsInjection* {
    for (const VitsInjection& vits : project.line_injections.vits) {
      if (vits.vits_type == vits_type) {
        return &vits;
      }
    }
    return nullptr;
  };

  int row = 0;
  for (const std::string& vits_type : vits_types) {
    const VitsInjection* existing = find_existing(vits_type);
    const bool ticked = existing != nullptr;
    // Fixed placement lines only bind under standard placement; laserdisc and
    // custom placement let the user move each signal.
    const bool line_locked = placement == VitsPlacement::kStandard &&
                             VitsHasFixedLine(standard, vits_type);

    auto* check = new QCheckBox(QString::fromStdString(vits_type));
    check->setChecked(ticked);
    connect(check, &QCheckBox::toggled, this,
            [this, row] { OnVitsRowToggled(row); });
    vits_checklist_layout_->addWidget(check, row, kVitsTypeColumn);

    const std::vector<int> lines =
        ticked ? existing->target_lines : DefaultVitsLines(standard, vits_type);
    auto* lines_edit = new QLineEdit();
    lines_edit->setText(QString::fromStdString(FormatTargetLines(lines)));
    lines_edit->setPlaceholderText(tr("e.g. 19, 282"));
    lines_edit->setReadOnly(line_locked);
    lines_edit->setEnabled(ticked);
    lines_edit->setToolTip(
        line_locked ? tr("Fixed placement line for this signal (set by the "
                         "standard).")
                    : tr("Target VBI line(s), comma-separated."));
    // Wide enough for a comma-separated handful of VBI line numbers.
    CapFieldWidthToText(lines_edit, QStringLiteral("999, 999, 999, 999"));
    connect(lines_edit, &QLineEdit::editingFinished, this,
            [this, row] { OnVitsLineEdited(row); });
    vits_checklist_layout_->addWidget(lines_edit, row, kVitsLinesColumn);

    vits_rows_.push_back({vits_type, check, lines_edit});
    ++row;
  }
}

void ProjectSettingsEditor::SetStandardEditable(bool editable) {
  standard_editable_ = editable;
  standard_combo_->setEnabled(editable);
}

void ProjectSettingsEditor::ApplyEnablement() {
  const ProjectSettingsFormState state =
      BuildProjectSettingsFormState(document_->project());
  standard_combo_->setEnabled(standard_editable_);
  setup_ire_check_->setEnabled(state.setup_ire_editable);
  secam_field_id_check_->setEnabled(state.secam_field_identification_editable);
  if (auto* placement_model =
          qobject_cast<QStandardItemModel*>(vits_placement_combo_->model())) {
    if (QStandardItem* laserdisc_item =
            placement_model->item(kVitsPlacementLaserdiscIndex)) {
      laserdisc_item->setEnabled(state.laserdisc_editable);
    }
  }
  setup_ire_combo_->setEnabled(state.setup_ire_editable &&
                               setup_ire_check_->isChecked());
  video_path_hint_->setText(
      state.video_path_requires_luma_suffix
          ? tr("Y/C output: the video path must end in “.cvbsy” (a matching "
               "“.cvbsc” chroma file is written alongside it).")
          : QString());

  // A block the standard cannot carry stays enabled while it is set, so the
  // user can still untick it; the hint explains the validation error.
  chroma_error_group_->setEnabled(state.chroma_error_editable ||
                                  chroma_error_group_->isChecked());
  chroma_error_hint_->setVisible(!state.chroma_error_editable);
  crosstalk_alternate_check_->setEnabled(
      state.crosstalk_alternate_editable ||
      crosstalk_alternate_check_->isChecked());
  line_timing_burst_check_->setEnabled(state.line_timing_burst_editable ||
                                       line_timing_burst_check_->isChecked());

  ApplyLaserdiscEnablement(state);
  truth_check_->setEnabled(state.truth_editable || truth_check_->isChecked());
}

void ProjectSettingsEditor::ApplyLaserdiscEnablement(
    const ProjectSettingsFormState& state) {
  laserdisc_group_->setEnabled(state.laserdisc_editable ||
                               laserdisc_group_->isChecked());
  laserdisc_hint_->setVisible(!state.laserdisc_editable);
  // A flag the standard cannot carry stays enabled while it is set, so the
  // user can still untick it; the issues dock explains the error.
  pilot_burst_check_->setEnabled(state.pilot_burst_editable ||
                                 pilot_burst_check_->isChecked());
  vbi_burst_check_->setEnabled(state.vbi_burst_editable ||
                               vbi_burst_check_->isChecked());
  efm_check_->setEnabled(state.efm_output_editable || efm_check_->isChecked());
  efm_pair_combo_->setEnabled(state.efm_pair_editable);
  efm_hint_->setText(EfmStatusText(state.efm_status));
  efm_data_group_->setEnabled(state.efm_data_editable ||
                              efm_data_group_->isChecked());
  for (QWidget* widget : std::initializer_list<QWidget*>{
           efm_data_image_edit_, efm_data_browse_button_,
           efm_data_first_address_spin_, efm_data_offset_spin_,
           efm_data_q_control_combo_, efm_data_q_mode_combo_}) {
    widget->setEnabled(state.efm_data_fields_editable);
  }
  const QString efm_data_text = EfmDataStatusText(state.efm_data_status);
  efm_data_hint_->setText(efm_data_text);
  efm_data_hint_->setVisible(!efm_data_text.isEmpty());

  fm_beat_group_->setEnabled(state.fm_beat_editable ||
                             fm_beat_group_->isChecked());
  fm_beat_hint_->setVisible(!state.fm_beat_editable);
  if (auto* preset_model = qobject_cast<QStandardItemModel*>(
          group_delay_preset_combo_->model())) {
    if (QStandardItem* nominal_item =
            preset_model->item(kGroupDelayNominalIndex)) {
      nominal_item->setEnabled(state.group_delay_nominal_editable ||
                               group_delay_preset_combo_->currentIndex() ==
                                   kGroupDelayNominalIndex);
    }
  }
}

// Renders the presenter's EFM outcome as the sentence shown beneath the
// controls. The wording mirrors the validator messages for the same
// conditions so the form and the issues dock agree.
QString ProjectSettingsEditor::EfmStatusText(EfmOutputStatus status) const {
  const int pair = document_->project().laserdisc.efm_audio.pair;
  switch (status) {
    case EfmOutputStatus::kUnsupportedStandard:
      return tr(
          "LaserDisc digital audio is specified for PAL, NTSC and PAL-M "
          "only; no “.efm” file is written for this standard.");
    case EfmOutputStatus::kPairOutOfRange:
      return tr("Channel pair %1 is outside the supported range 0–7.")
          .arg(pair);
    case EfmOutputStatus::kPairNotDeclared:
      return tr("No section declares audio channel pair %1, so no “.efm” "
                "file is written.")
          .arg(pair);
    case EfmOutputStatus::kActive:
      return tr("Channel pair %1 is also written as an EFM stream "
                "(“.efm” plus its “.efm.meta” sidecar) beside its WAV "
                "track.")
          .arg(pair);
    case EfmOutputStatus::kDisabled:
    default:
      return {};
  }
}

// Renders the presenter's EFM data outcome; the wording follows the
// validator messages for the same conditions.
QString ProjectSettingsEditor::EfmDataStatusText(EfmDataStatus status) const {
  switch (status) {
    case EfmDataStatus::kUnsupportedStandard:
      return tr(
          "LaserDisc EFM data is specified for PAL, NTSC and PAL-M only; no "
          "“.efm” file is written for this standard.");
    case EfmDataStatus::kConflictsWithAudio:
      return tr(
          "EFM data and digital audio cannot be used together; a disc has "
          "one EFM channel.");
    case EfmDataStatus::kNoImage:
      return tr("Choose the sector image to encode.");
    case EfmDataStatus::kNoSection:
      return tr(
          "No section carries EFM data yet, so the stream carries none. Tick "
          "“Carries EFM data” in a section's LaserDisc group.");
    case EfmDataStatus::kActive:
      return tr(
          "The sector image is written as an EFM stream (“.efm” plus its "
          "“.efm.meta” sidecar) during the sections that carry EFM data.");
    case EfmDataStatus::kDisabled:
    default:
      return {};
  }
}

void ProjectSettingsEditor::CommitProjectInfo() {
  if (updating_) {
    return;
  }
  committing_ = true;
  document_->SetProjectInfo(name_edit_->text().toStdString(),
                            version_edit_->text().toStdString(),
                            description_edit_->toPlainText().toStdString());
  committing_ = false;
}

void ProjectSettingsEditor::CommitCvbsPresets() {
  if (updating_) {
    return;
  }

  const Standard old_standard =
      document_->project().cvbs_presets.video_standard_preset;

  CvbsPresets presets = document_->project().cvbs_presets;
  presets.video_standard_preset =
      StandardFromString(standard_combo_->currentText().toStdString());
  presets.sample_encoding_preset =
      sample_encoding_combo_->currentText().toStdString();
  presets.signal_state_preset =
      signal_state_combo_->currentText().toStdString();
  presets.ntsc_black_setup_ire_specified = setup_ire_check_->isChecked();
  presets.secam_field_identification = secam_field_id_check_->isChecked();
  presets.ntsc_black_setup_ire =
      setup_ire_check_->isChecked() ? setup_ire_combo_->currentData().toDouble()
                                    : 7.5;
  presets = NormalizeCvbsPresetsForStandard(presets);

  committing_ = true;
  document_->SetCvbsPresets(presets);
  // Switching the standard changes the required source raster (720x576 PAL vs
  // 720x486 System-M). Re-point any section still on a bundled default
  // colour-bar EXR to the new standard's raster so a freshly seeded project
  // stays previewable; user-chosen sources are left untouched.
  const Standard new_standard = presets.video_standard_preset;
  if (new_standard != old_standard) {
    // Clear the settings the new standard cannot carry rather than leave
    // ones the validator would reject: the picture truth (not SECAM), the
    // LaserDisc block (none for SECAM; pilot burst, FM beat and the nominal
    // group delay PAL-only, VBI burst NTSC-only, EFM per
    // StandardHasLaserdiscDigitalAudio) and the chroma error (not SECAM).
    document_->SetOutputTargets(NormalizeOutputTargetsForStandard(
        document_->project().output, new_standard));
    document_->SetLaserdisc(NormalizeLaserdiscForStandard(
        document_->project().laserdisc, new_standard));
    if (!document_->project().laserdisc.enabled) {
      ClearSectionLaserdiscBlocks();
    }
    document_->SetChromaError(NormalizeChromaErrorForStandard(
        document_->project().chroma_error, new_standard));
    document_->SetLineTiming(NormalizeLineTimingForStandard(
        document_->project().line_timing, new_standard));

    // The VITS catalogue is standard-specific, so a standard change must
    // reconcile the project VITS set (re-seed under laserdisc placement, drop
    // cross-standard entries otherwise) rather than leave it stale and invalid.
    document_->SetProjectLineInjections(ReconcileVitsForStandard(
        document_->project().line_injections, new_standard));

    Project remapped = document_->project();
    if (RemapBundledDefaultSources(&remapped, new_standard) > 0) {
      // Push only the sections the remap actually changed through the document
      // so each becomes an undoable edit and observers (preview, list) refresh.
      const std::vector<Section>& live = document_->project().sections;
      for (int index = 0; index < static_cast<int>(remapped.sections.size());
           ++index) {
        const std::size_t i = static_cast<std::size_t>(index);
        if (remapped.sections[i].source != live[i].source) {
          document_->SetSection(index, remapped.sections[i]);
        }
      }
    }
  }
  committing_ = false;
  LoadFromDocument();
}

void ProjectSettingsEditor::CommitLineInjections() {
  if (updating_) {
    return;
  }

  ProjectLineInjections li;
  li.placement =
      VitsPlacementFromComboIndex(vits_placement_combo_->currentIndex());

  for (const VitsRow& vits_row : vits_rows_) {
    if (!vits_row.check->isChecked()) {
      continue;
    }
    VitsInjection vits;
    vits.vits_type = vits_row.vits_type;
    ParseTargetLines(vits_row.lines->text().toStdString(), &vits.target_lines);
    li.vits.push_back(std::move(vits));
  }

  // Commit without a full reload: the checklist widgets already reflect the
  // user's intent, and rebuilding here would destroy the widget whose signal
  // triggered this commit. External changes still reload via the document
  // observers (guarded by committing_).
  committing_ = true;
  document_->SetProjectLineInjections(std::move(li));
  committing_ = false;
}

void ProjectSettingsEditor::OnVitsPlacementChanged() {
  if (updating_) {
    return;
  }
  const Standard standard =
      document_->project().cvbs_presets.video_standard_preset;
  const VitsPlacement placement =
      VitsPlacementFromComboIndex(vits_placement_combo_->currentIndex());

  // Start from the current model so any manual VITS set is preserved where
  // the new mode allows it.
  ProjectLineInjections li = document_->project().line_injections;
  li.placement = placement;
  switch (placement) {
    case VitsPlacement::kLaserdisc:
      // One-click preset: place the spec-required VITS set on the laserdisc
      // VBI lines, replacing the current selection.
      li.vits = LaserdiscVitsSet(standard);
      break;
    case VitsPlacement::kStandard:
      // Reset each ticked type to its recommended broadcast line so standard
      // placement is self-consistent (laserdisc lines would be rejected).
      for (VitsInjection& vits : li.vits) {
        vits.target_lines = DefaultVitsLines(standard, vits.vits_type);
      }
      break;
    case VitsPlacement::kCustom:
      // Keep the current lines; the user takes over placement.
      break;
  }

  committing_ = true;
  document_->SetProjectLineInjections(li);
  committing_ = false;

  // Safe to rebuild here: this handler is driven by the placement combo, not a
  // VITS row widget, so no VITS widget is being destroyed mid-signal.
  updating_ = true;
  RebuildVitsChecklist();
  updating_ = false;
}

void ProjectSettingsEditor::OnVitsRowToggled(int row) {
  if (updating_ || row < 0 || row >= static_cast<int>(vits_rows_.size())) {
    return;
  }
  const VitsRow& vits_row = vits_rows_[static_cast<std::size_t>(row)];
  vits_row.lines->setEnabled(vits_row.check->isChecked());
  CommitLineInjections();
}

void ProjectSettingsEditor::OnVitsLineEdited(int row) {
  if (updating_ || row < 0 || row >= static_cast<int>(vits_rows_.size())) {
    return;
  }
  const VitsRow& vits_row = vits_rows_[static_cast<std::size_t>(row)];
  std::vector<int> lines;
  if (!ParseTargetLines(vits_row.lines->text().toStdString(), &lines)) {
    // Restore the last-known-good value from the model; range feedback comes
    // from the issues panel.
    const auto& vits_set = document_->project().line_injections.vits;
    std::vector<int> restored;
    for (const VitsInjection& vits : vits_set) {
      if (vits.vits_type == vits_row.vits_type) {
        restored = vits.target_lines;
        break;
      }
    }
    vits_row.lines->setText(
        QString::fromStdString(FormatTargetLines(restored)));
    return;
  }
  CommitLineInjections();
}

void ProjectSettingsEditor::CommitOutputTargets() {
  if (updating_) {
    return;
  }

  OutputTargets output = document_->project().output;
  output.signal_type = signal_type_combo_->currentText().toStdString();
  output.video_path = EnforceSignalTypeVideoPath(
      video_path_edit_->text().toStdString(), output.signal_type);
  // All outputs are co-located with the video; the metadata path is always
  // derived from it rather than set independently.
  output.metadata_path = DeriveMetadataPath(output.video_path);
  output.truth = truth_check_->isChecked();
  output = NormalizeOutputTargetsForStandard(
      std::move(output),
      document_->project().cvbs_presets.video_standard_preset);

  committing_ = true;
  document_->SetOutputTargets(output);
  committing_ = false;
  LoadFromDocument();
}

void ProjectSettingsEditor::CommitChannel() {
  if (updating_) {
    return;
  }
  const ChannelParameters& stored = document_->project().channel;
  ChannelParameters channel = stored;
  if (!channel_group_->isChecked()) {
    SetChannelEnabled(&channel, false);
  } else {
    SetChannelEnabled(&channel, true);
    channel.timing_offset_samples = timing_offset_spin_->value();
    // Freshly ticked sub-blocks take the presenter defaults; the widgets
    // still hold the disabled block's values.
    SetEdgeSmearEnabled(&channel, edge_smear_group_->isChecked());
    if (edge_smear_group_->isChecked() && stored.edge_smear.enabled) {
      channel.edge_smear.fraction = edge_smear_fraction_spin_->value();
      channel.edge_smear.tau_samples = edge_smear_tau_spin_->value();
    }
  }
  committing_ = true;
  const bool changed = document_->SetChannel(channel);
  committing_ = false;
  if (changed) {
    LoadFromDocument();
  }
}

void ProjectSettingsEditor::CommitChromaError() {
  if (updating_) {
    return;
  }
  const ChromaErrorParameters& stored = document_->project().chroma_error;
  ChromaErrorParameters chroma_error = stored;
  if (!chroma_error_group_->isChecked()) {
    SetChromaErrorEnabled(&chroma_error, false);
  } else {
    SetChromaErrorEnabled(&chroma_error, true);
    chroma_error.gain = chroma_gain_spin_->value();
    chroma_error.phase_deg = chroma_phase_spin_->value();
    chroma_error.include_burst = chroma_include_burst_check_->isChecked();
    SetChromaJitterEnabled(&chroma_error, chroma_jitter_group_->isChecked());
    if (chroma_jitter_group_->isChecked() && stored.line_jitter.enabled) {
      ChromaJitterParameters& jitter = chroma_error.line_jitter;
      jitter.phase_deg = jitter_phase_spin_->value();
      jitter.gain = jitter_gain_spin_->value();
      jitter.seed_specified = jitter_seed_check_->isChecked();
      jitter.seed =
          jitter.seed_specified ? ParseSeed(jitter_seed_edit_->text()) : 0;
    }
    SetDifferentialEnabled(&chroma_error, differential_group_->isChecked());
    if (differential_group_->isChecked() && stored.differential.enabled) {
      chroma_error.differential.gain =
          ToLevelCurvePoints(differential_gain_editor_->points());
      chroma_error.differential.phase_deg =
          ToLevelCurvePoints(differential_phase_editor_->points());
    }
    SetCrosstalkEnabled(&chroma_error, crosstalk_group_->isChecked());
    if (crosstalk_group_->isChecked() && stored.crosstalk.enabled) {
      CrosstalkParameters& crosstalk = chroma_error.crosstalk;
      crosstalk.v_to_u = crosstalk_v_to_u_spin_->value();
      crosstalk.u_to_v = crosstalk_u_to_v_spin_->value();
      crosstalk.alternate_with_switch = crosstalk_alternate_check_->isChecked();
    }
  }
  committing_ = true;
  const bool changed = document_->SetChromaError(chroma_error);
  committing_ = false;
  if (changed) {
    LoadFromDocument();
  }
}

void ProjectSettingsEditor::CommitLineTiming() {
  if (updating_) {
    return;
  }
  const LineTimingParameters& stored = document_->project().line_timing;
  LineTimingParameters line_timing = stored;
  // Reads one layer's widgets into `layer`.
  const auto read_layer = [](const LineTimingLayerWidgets& widgets,
                             LineTimingLayer* layer) {
    layer->line_ns = widgets.line_ns->value();
    layer->line_correlation = widgets.correlation->value();
    layer->field_ns = widgets.field_ns->value();
    layer->slope_ns = widgets.slope_ns->value();
    layer->seed_specified = widgets.seed_check->isChecked();
    layer->seed =
        layer->seed_specified ? ParseSeed(widgets.seed_edit->text()) : 0;
  };
  if (!line_timing_group_->isChecked()) {
    SetLineTimingEnabled(&line_timing, false);
  } else {
    // A freshly ticked block takes the presenter defaults; the widgets still
    // hold the disabled block's values.
    SetLineTimingEnabled(&line_timing, true);
    if (stored.enabled) {
      line_timing.capture.enabled = true;
      read_layer(line_timing_capture_, &line_timing.capture);
      line_timing.include_burst = line_timing_burst_check_->isChecked();
    }
    SetSharedLineTimingEnabled(&line_timing,
                               line_timing_shared_group_->isChecked());
    if (line_timing_shared_group_->isChecked() && stored.shared.enabled) {
      read_layer(line_timing_shared_, &line_timing.shared);
      line_timing.shared.seed_specified = true;
    }
  }
  committing_ = true;
  const bool changed = document_->SetLineTiming(line_timing);
  committing_ = false;
  if (changed) {
    LoadFromDocument();
  }
}

void ProjectSettingsEditor::CommitLaserdisc() {
  if (updating_) {
    return;
  }
  const Project& project = document_->project();
  const LaserdiscParameters& stored = project.laserdisc;
  const Standard standard = project.cvbs_presets.video_standard_preset;
  LaserdiscParameters laserdisc = stored;
  if (!laserdisc_group_->isChecked()) {
    SetLaserdiscEnabled(&laserdisc, false);
  } else {
    SetLaserdiscEnabled(&laserdisc, true);
    const std::string disc = disc_type_combo_->currentText().toStdString();
    laserdisc.disc_type = (disc == kDiscTypeNone) ? std::string() : disc;
    laserdisc.pilot_burst = pilot_burst_check_->isChecked();
    laserdisc.vbi_burst = vbi_burst_check_->isChecked();
    laserdisc.efm_audio.enabled = efm_check_->isChecked();
    laserdisc.efm_audio.pair = efm_pair_combo_->currentData().toInt();

    // A freshly ticked group keeps the defaults; the widgets still hold the
    // disabled block's values.
    SetEfmDataEnabled(&laserdisc, efm_data_group_->isChecked());
    if (efm_data_group_->isChecked() && stored.efm_data.enabled) {
      EfmDataOutput& efm_data = laserdisc.efm_data;
      efm_data.image = efm_data_image_edit_->text().trimmed().toStdString();
      efm_data.first_address = efm_data_first_address_spin_->value();
      efm_data.picture_offset_sectors = efm_data_offset_spin_->value();
      efm_data.q_control = static_cast<EfmDataQControl>(
          efm_data_q_control_combo_->currentData().toInt());
      efm_data.q_mode = efm_data_q_mode_combo_->currentData().toInt();
    }

    // Freshly ticked sub-blocks take the presenter defaults; the widgets
    // still hold the disabled block's values.
    const LaserdiscChannelParameters& stored_channel = stored.channel;
    SetLaserdiscChannelEnabled(&laserdisc, ld_channel_group_->isChecked());
    if (ld_channel_group_->isChecked()) {
      LaserdiscChannelParameters& channel = laserdisc.channel;
      SetFmBeatEnabled(&laserdisc, fm_beat_group_->isChecked());
      if (fm_beat_group_->isChecked() && stored_channel.fm_beat.enabled) {
        channel.fm_beat.carrier_black_hz = fm_beat_carrier_spin_->value();
        channel.fm_beat.level_db = fm_beat_level_spin_->value();
      }

      SetGroupDelayEnabled(&laserdisc, group_delay_group_->isChecked(),
                           standard);
      if (group_delay_group_->isChecked() &&
          stored_channel.group_delay.enabled) {
        const GroupDelayPreset preset =
            group_delay_preset_combo_->currentIndex() == kGroupDelayCustomIndex
                ? GroupDelayPreset::kCustom
                : GroupDelayPreset::kIec60856Nominal;
        if (preset != stored_channel.group_delay.preset) {
          SetGroupDelayPreset(&laserdisc, preset);
        } else if (preset == GroupDelayPreset::kCustom) {
          channel.group_delay.points_ns =
              ToGroupDelayPoints(group_delay_curve_editor_->points());
        }
        channel.group_delay.scale = group_delay_scale_spin_->value();
      }

      const ReplayResponseMode mode =
          ResponseModeFromComboIndex(response_mode_combo_->currentIndex());
      if (mode != ReplayResponseModeOf(stored_channel)) {
        SetReplayResponseMode(&laserdisc, mode);
      } else if (mode == ReplayResponseMode::kFixed) {
        channel.response_db =
            ToResponsePoints(response_curve_editor_->points());
      } else if (mode == ReplayResponseMode::kByPicture) {
        channel.response_by_picture = response_by_picture_editor_->entries();
      }

      SetField2OffsetEnabled(&laserdisc, field2_offset_group_->isChecked());
      if (field2_offset_group_->isChecked() &&
          stored_channel.field2_offset.enabled) {
        channel.field2_offset.x_samples = field2_x_spin_->value();
        channel.field2_offset.y_lines = field2_y_spin_->value();
      }
    }
  }

  committing_ = true;
  bool changed = false;
  {
    // Sections may carry laserdisc data only while the project has a
    // laserdisc block, so unticking the block clears it from every section in
    // the same undo step.
    const ScopedUndoBatch batch(document_, tr("Edit laserdisc"));
    changed = document_->SetLaserdisc(laserdisc);
    if (!laserdisc.enabled) {
      changed = ClearSectionLaserdiscBlocks() || changed;
    } else if (!laserdisc.efm_data.enabled) {
      // Sections may carry EFM data only while the project declares it.
      changed = ClearSectionEfmDataFlags() || changed;
    }
  }
  committing_ = false;
  if (changed) {
    LoadFromDocument();
  }
}

bool ProjectSettingsEditor::ClearSectionEfmDataFlags() {
  bool changed = false;
  const int count = document_->section_count();
  for (int index = 0; index < count; ++index) {
    Section section =
        document_->project().sections[static_cast<std::size_t>(index)];
    if (ClearSectionEfmData(&section)) {
      changed = document_->SetSection(index, std::move(section)) || changed;
    }
  }
  return changed;
}

bool ProjectSettingsEditor::ClearSectionLaserdiscBlocks() {
  bool changed = false;
  const int count = document_->section_count();
  for (int index = 0; index < count; ++index) {
    Section section =
        document_->project().sections[static_cast<std::size_t>(index)];
    if (ClearSectionLaserdiscBlock(&section)) {
      changed = document_->SetSection(index, std::move(section)) || changed;
    }
  }
  return changed;
}

QWidget* ProjectSettingsEditor::FieldGroup(IssueField field) const {
  switch (field) {
    case IssueField::kChannel:
      return channel_group_;
    case IssueField::kChromaError:
      return chroma_error_group_;
    case IssueField::kLineTiming:
      return line_timing_group_;
    case IssueField::kLaserdiscChannel:
      return ld_channel_group_;
    case IssueField::kEfmData:
      return efm_data_group_;
    default:
      return nullptr;
  }
}

QWidget* ProjectSettingsEditor::FieldFocusWidget(IssueField field) const {
  switch (field) {
    case IssueField::kChannel:
      return timing_offset_spin_;
    case IssueField::kChromaError:
      return chroma_gain_spin_;
    case IssueField::kLineTiming:
      return line_timing_capture_.line_ns;
    case IssueField::kLaserdiscChannel:
      return ld_channel_group_;
    case IssueField::kEfmData:
      return efm_data_image_edit_;
    default:
      return nullptr;
  }
}

void ProjectSettingsEditor::OnStandardChanged() { CommitCvbsPresets(); }

void ProjectSettingsEditor::OnSignalTypeChanged() { CommitOutputTargets(); }

void ProjectSettingsEditor::OnBrowseVideoPath() {
  const bool yc = signal_type_combo_->currentText() == QStringLiteral("yc");
  const QString filter = yc ? tr("Y/C luma files (*.cvbsy);;All files (*)")
                            : tr("Composite files (*.cvbs);;All files (*)");
  // Open the dialog at the output path's *resolved* location. A relative
  // output path is project-relative, so anchor it to the project directory
  // rather than the process working directory.
  const QString project_dir =
      document_->file_path().isEmpty()
          ? QDir::currentPath()
          : QFileInfo(document_->file_path()).absolutePath();
  QString start = QString::fromStdString(videosynth::ResolveAssetPath(
      video_path_edit_->text().toStdString(), GuiAssetRoots(),
      project_dir.toStdString(), /*anchor_unset=*/true));
  if (start.isEmpty()) {
    start = project_dir;
  }
  const QString path =
      QFileDialog::getSaveFileName(this, tr("Output Video"), start, filter);
  if (path.isEmpty()) {
    return;
  }
  video_path_edit_->setText(path);
  CommitOutputTargets();
}

void ProjectSettingsEditor::OnBrowseEfmDataImage() {
  const QString project_dir =
      document_->file_path().isEmpty()
          ? QDir::currentPath()
          : QFileInfo(document_->file_path()).absolutePath();
  QString start = project_dir;
  const std::string current = efm_data_image_edit_->text().toStdString();
  if (!current.empty()) {
    start = QFileInfo(QString::fromStdString(videosynth::ResolveAssetPath(
                          current, GuiAssetRoots(), project_dir.toStdString(),
                          /*anchor_unset=*/true)))
                .absolutePath();
  }
  const QString path = QFileDialog::getOpenFileName(
      this, tr("EFM Data Sector Image"), start,
      tr("Sector images (*.dat *.bin *.img *.iso);;All files (*)"));
  if (path.isEmpty()) {
    return;
  }
  // Store a file under the project directory project-relative, so the
  // project stays portable; anything else is stored absolute.
  const QString relative = QDir(project_dir).relativeFilePath(path);
  efm_data_image_edit_->setText(
      document_->file_path().isEmpty() ||
              relative.startsWith(QStringLiteral(".."))
          ? path
          : relative);
  CommitLaserdisc();
}

}  // namespace videosynth::gui
