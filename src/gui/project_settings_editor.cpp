/*
 * File:        project_settings_editor.cpp
 * Module:      gui
 * Purpose:     Form editor for project info, CVBS presets, line injections,
 *              channel and chroma error impairments, and output targets
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
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <utility>

#include "asset_roots.h"
#include "form_field_width.h"
#include "line_injection_presenter.h"
#include "project_settings_presenter.h"
#include "project_templates.h"

namespace {
constexpr int kVitsTypeColumn = 0;
constexpr int kVitsLinesColumn = 1;
// Sentinel disc_type combo entry meaning "not a laserdisc project".
const char* const kDiscTypeNone = "(none)";

// VITS placement combo item order: 0 standard, 1 laserdisc, 2 custom.
constexpr int kVitsPlacementLaserdiscIndex = 1;

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
  pilot_burst_check_ = new QCheckBox(
      tr("PAL laserdisc pilot burst (IEC 60856 §9.1.2)"), presets_group);
  vbi_burst_check_ = new QCheckBox(
      tr("NTSC laserdisc VBI burst (IEC 60857 §9.1.2)"), presets_group);
  setup_ire_check_ =
      new QCheckBox(tr("Override NTSC black setup IRE"), presets_group);
  setup_ire_combo_ = new QComboBox(presets_group);
  secam_field_id_check_ = new QCheckBox(
      tr("SECAM field identification (lines 7–15 / 320–328)"), presets_group);

  presets_form->addRow(tr("Video standard:"), standard_combo_);
  presets_form->addRow(tr("Sample encoding:"), sample_encoding_combo_);
  presets_form->addRow(tr("Signal state:"), signal_state_combo_);
  presets_form->addRow(pilot_burst_check_);
  presets_form->addRow(vbi_burst_check_);
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
  connect(pilot_burst_check_, &QCheckBox::toggled, this, [this] {
    if (!updating_) {
      CommitCvbsPresets();
    }
  });
  connect(vbi_burst_check_, &QCheckBox::toggled, this, [this] {
    if (!updating_) {
      CommitCvbsPresets();
    }
  });
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

  // --- line_injections: (project-wide laserdisc format + VITS set) ---------
  auto* injections_group = new QGroupBox(tr("Laserdisc && VITS"), this);
  auto* injections_layout = new QVBoxLayout(injections_group);
  auto* disc_form = new QFormLayout();
  disc_type_combo_ = new QComboBox(injections_group);
  disc_type_combo_->addItem(QString::fromLatin1(kDiscTypeNone));
  for (const std::string& disc_type : AvailableDiscTypes()) {
    disc_type_combo_->addItem(QString::fromStdString(disc_type));
  }
  disc_form->addRow(tr("Disc format:"), disc_type_combo_);

  vits_placement_combo_ = new QComboBox(injections_group);
  vits_placement_combo_->addItem(tr("Standard (broadcast lines)"));
  vits_placement_combo_->addItem(tr("Laserdisc (IEC 60856/60857)"));
  vits_placement_combo_->addItem(tr("Custom (choose lines)"));
  disc_form->addRow(tr("VITS placement:"), vits_placement_combo_);
  injections_layout->addLayout(disc_form);

  connect(vits_placement_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::OnVitsPlacementChanged);

  vits_label_ = new QLabel(injections_group);
  vits_label_->setWordWrap(true);
  injections_layout->addWidget(vits_label_);

  auto* vits_host = new QWidget(injections_group);
  vits_checklist_layout_ = new QGridLayout(vits_host);
  vits_checklist_layout_->setContentsMargins(0, 0, 0, 0);
  // Stretch an empty trailing column so each checkbox and its lines edit
  // pack together on the left.
  vits_checklist_layout_->setColumnStretch(2, 1);
  injections_layout->addWidget(vits_host);
  layout->addWidget(injections_group);

  connect(disc_type_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::CommitLineInjections);

  layout->addWidget(BuildChannelGroup());
  layout->addWidget(BuildChromaErrorGroup());

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

  // LaserDisc digital audio: one channel pair is additionally encoded as an
  // EFM stream (IEC 60856:1986 Amd 2 / IEC 60857:1986 Amd 2, clause 13); its
  // WAV track is written as usual.
  efm_check_ =
      new QCheckBox(tr("Encode one channel pair as EFM"), output_group);
  efm_pair_combo_ = new QComboBox(output_group);
  auto* efm_row = new QHBoxLayout();
  efm_row->addWidget(efm_check_);
  efm_row->addWidget(new QLabel(tr("Pair:"), output_group));
  efm_row->addWidget(efm_pair_combo_);
  efm_row->addStretch();
  efm_hint_ = new QLabel(output_group);
  efm_hint_->setWordWrap(true);

  output_form->addRow(tr("Signal type:"), signal_type_combo_);
  output_form->addRow(tr("Video path:"), video_row);
  output_form->addRow(QString(), video_path_hint_);
  output_form->addRow(tr("Digital audio:"), efm_row);
  output_form->addRow(QString(), efm_hint_);
  output_form->addRow(QString(), outputs_note_);
  layout->addWidget(output_group);

  layout->addStretch();

  connect(signal_type_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::OnSignalTypeChanged);
  connect(video_path_edit_, &QLineEdit::editingFinished, this,
          &ProjectSettingsEditor::CommitOutputTargets);
  connect(browse_button, &QPushButton::clicked, this,
          &ProjectSettingsEditor::OnBrowseVideoPath);
  connect(efm_check_, &QCheckBox::toggled, this, [this] {
    if (!updating_) {
      CommitOutputTargets();
    }
  });
  connect(efm_pair_combo_, &QComboBox::activated, this,
          &ProjectSettingsEditor::CommitOutputTargets);
}

QWidget* ProjectSettingsEditor::BuildChannelGroup() {
  namespace limits = project_editor_limits;
  // --- channel: (pressing impairments) -------------------------------------
  channel_group_ = new QGroupBox(tr("Channel (pressing)"), this);
  channel_group_->setCheckable(true);
  channel_group_->setToolTip(
      tr("Impairments of the pressing and its readout. Every project of one "
         "pressing should use the same channel settings."));
  auto* outer = new QVBoxLayout(channel_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(channel_group_);
  auto* layout = new QVBoxLayout(body);
  auto* form = new QFormLayout();

  const auto make_spin = [](QWidget* parent, double min, double max,
                            int decimals, double step, const QString& suffix) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    // Commit on Enter or focus loss, not per keystroke.
    spin->setKeyboardTracking(false);
    CapFieldWidthAtSizeHint(spin);
    return spin;
  };

  timing_offset_spin_ =
      make_spin(body, -limits::kTimingOffsetMaxSamples,
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
      make_spin(edge_smear_group_, 0.0, limits::kEdgeSmearFractionMax, 3, 0.01,
                QString());
  smear_form->addRow(tr("Fraction:"), edge_smear_fraction_spin_);
  edge_smear_tau_spin_ =
      make_spin(edge_smear_group_, limits::kEdgeSmearTauMinSamples,
                limits::kEdgeSmearTauMaxSamples, 2, 0.5, tr(" samples"));
  smear_form->addRow(tr("Time constant:"), edge_smear_tau_spin_);
  layout->addWidget(edge_smear_group_);

  fm_beat_group_ = new QGroupBox(tr("FM beat (herringbone)"), body);
  fm_beat_group_->setCheckable(true);
  fm_beat_group_->setToolTip(
      tr("The beat between the FM video carrier and the colour signal, "
         "recorded on the master (625-line PAL only)."));
  auto* beat_form = new QFormLayout(fm_beat_group_);
  fm_beat_carrier_spin_ =
      make_spin(fm_beat_group_, limits::kFmBeatCarrierMinHz,
                limits::kFmBeatCarrierMaxHz, 0, 1000.0, tr(" Hz"));
  fm_beat_carrier_spin_->setToolTip(tr("FM video carrier frequency at black."));
  beat_form->addRow(tr("Carrier at black:"), fm_beat_carrier_spin_);
  fm_beat_level_spin_ = make_spin(fm_beat_group_, limits::kFmBeatLevelMinDb,
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
  connect(fm_beat_group_, &QGroupBox::toggled, this, commit);
  for (QDoubleSpinBox* spin :
       {timing_offset_spin_, edge_smear_fraction_spin_, edge_smear_tau_spin_,
        fm_beat_carrier_spin_, fm_beat_level_spin_}) {
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

  const auto make_spin = [](QWidget* parent, double min, double max,
                            int decimals, double step, const QString& suffix) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    CapFieldWidthAtSizeHint(spin);
    return spin;
  };

  chroma_gain_spin_ = make_spin(body, limits::kChromaGainMin,
                                limits::kChromaGainMax, 3, 0.01, QString());
  chroma_gain_spin_->setToolTip(
      tr("Chroma gain; 1.0 leaves saturation as is."));
  form->addRow(tr("Gain:"), chroma_gain_spin_);
  chroma_phase_spin_ = make_spin(body, -limits::kChromaPhaseMaxDeg,
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
      make_spin(chroma_jitter_group_, 0.0, limits::kChromaJitterPhaseMaxDeg, 2,
                0.1, tr("°"));
  jitter_form->addRow(tr("Phase deviation:"), jitter_phase_spin_);
  jitter_gain_spin_ =
      make_spin(chroma_jitter_group_, 0.0, limits::kChromaJitterGainMax, 3,
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
  connect(chroma_include_burst_check_, &QCheckBox::toggled, this, commit);
  for (QDoubleSpinBox* spin : {chroma_gain_spin_, chroma_phase_spin_,
                               jitter_phase_spin_, jitter_gain_spin_}) {
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

void ProjectSettingsEditor::LoadChannelAndChromaError(const Project& project) {
  const ChannelParameters& channel = project.channel;
  channel_group_->setChecked(channel.enabled);
  timing_offset_spin_->setValue(channel.timing_offset_samples);
  edge_smear_group_->setChecked(channel.edge_smear.enabled);
  edge_smear_fraction_spin_->setValue(channel.edge_smear.fraction);
  edge_smear_tau_spin_->setValue(channel.edge_smear.tau_samples);
  fm_beat_group_->setChecked(channel.fm_beat.enabled);
  fm_beat_carrier_spin_->setValue(channel.fm_beat.carrier_black_hz);
  fm_beat_level_spin_->setValue(channel.fm_beat.level_db);

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

  pilot_burst_check_->setChecked(
      project.cvbs_presets.pal_laserdisc_pilot_burst);
  vbi_burst_check_->setChecked(project.cvbs_presets.ntsc_laserdisc_vbi_burst);
  setup_ire_check_->setChecked(
      project.cvbs_presets.ntsc_black_setup_ire_specified);
  secam_field_id_check_->setChecked(
      project.cvbs_presets.secam_field_identification);

  const QString disc_type =
      project.line_injections.disc_type.empty()
          ? QString::fromLatin1(kDiscTypeNone)
          : QString::fromStdString(project.line_injections.disc_type);
  int disc_index = disc_type_combo_->findText(disc_type);
  if (disc_index < 0) {
    disc_type_combo_->addItem(disc_type);
    disc_index = disc_type_combo_->count() - 1;
  }
  disc_type_combo_->setCurrentIndex(disc_index);
  vits_placement_combo_->setCurrentIndex(
      ComboIndexFromVitsPlacement(project.line_injections.placement));
  RebuildVitsChecklist();

  video_path_edit_->setText(QString::fromStdString(project.output.video_path));

  efm_pair_combo_->clear();
  for (const int pair : state.efm_pair_options) {
    efm_pair_combo_->addItem(QString::number(pair), pair);
  }
  const int efm_pair_index =
      efm_pair_combo_->findData(project.output.efm_audio.pair);
  efm_pair_combo_->setCurrentIndex(efm_pair_index >= 0 ? efm_pair_index : 0);
  efm_check_->setChecked(project.output.efm_audio.enabled);

  LoadChannelAndChromaError(project);

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
  pilot_burst_check_->setEnabled(state.pilot_burst_editable);
  vbi_burst_check_->setEnabled(state.vbi_burst_editable);
  setup_ire_check_->setEnabled(state.setup_ire_editable);
  secam_field_id_check_->setEnabled(state.secam_field_identification_editable);
  disc_type_combo_->setEnabled(state.laserdisc_editable);
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
  fm_beat_group_->setEnabled(state.fm_beat_editable ||
                             fm_beat_group_->isChecked());
  fm_beat_hint_->setVisible(!state.fm_beat_editable);
  chroma_error_group_->setEnabled(state.chroma_error_editable ||
                                  chroma_error_group_->isChecked());
  chroma_error_hint_->setVisible(!state.chroma_error_editable);

  efm_check_->setEnabled(state.efm_output_editable);
  efm_pair_combo_->setEnabled(state.efm_pair_editable);
  efm_hint_->setText(EfmStatusText(state.efm_status));
}

// Renders the presenter's EFM outcome as the sentence shown beneath the
// controls. The wording mirrors the validator messages for the same
// conditions so the form and the issues dock agree.
QString ProjectSettingsEditor::EfmStatusText(EfmOutputStatus status) const {
  const int pair = document_->project().output.efm_audio.pair;
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
  presets.pal_laserdisc_pilot_burst = pilot_burst_check_->isChecked();
  presets.ntsc_laserdisc_vbi_burst = vbi_burst_check_->isChecked();
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
    // LaserDisc digital audio only exists for PAL and NTSC, so switching to a
    // standard without it clears the selection rather than leaving a setting
    // the validator would reject.
    OutputTargets output = NormalizeOutputTargetsForStandard(
        document_->project().output, new_standard);
    if (!(output == document_->project().output)) {
      document_->SetOutputTargets(output);
    }
    // Likewise the FM beat (PAL only) and the chroma error (not SECAM).
    document_->SetChannel(NormalizeChannelForStandard(
        document_->project().channel, new_standard));
    document_->SetChromaError(NormalizeChromaErrorForStandard(
        document_->project().chroma_error, new_standard));

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
  const std::string disc = disc_type_combo_->currentText().toStdString();
  li.disc_type = (disc == kDiscTypeNone) ? std::string() : disc;
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

  // Start from the current model so disc_type and any manual VITS set are
  // preserved where the new mode allows it.
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
  output.efm_audio.enabled = efm_check_->isChecked();
  output.efm_audio.pair = efm_pair_combo_->currentData().toInt();
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
    SetFmBeatEnabled(&channel, fm_beat_group_->isChecked());
    if (fm_beat_group_->isChecked() && stored.fm_beat.enabled) {
      channel.fm_beat.carrier_black_hz = fm_beat_carrier_spin_->value();
      channel.fm_beat.level_db = fm_beat_level_spin_->value();
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
  }
  committing_ = true;
  const bool changed = document_->SetChromaError(chroma_error);
  committing_ = false;
  if (changed) {
    LoadFromDocument();
  }
}

QWidget* ProjectSettingsEditor::FieldGroup(IssueField field) const {
  switch (field) {
    case IssueField::kChannel:
      return channel_group_;
    case IssueField::kChromaError:
      return chroma_error_group_;
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

}  // namespace videosynth::gui
