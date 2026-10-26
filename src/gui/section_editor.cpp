/*
 * File:        section_editor.cpp
 * Module:      gui
 * Purpose:     Per-section form editor: source and span, optional audio,
 *              noise, dropout, OSD blocks, and line injections
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "section_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "asset_roots.h"
#include "form_field_width.h"
#include "project_templates.h"
#include "section_block_presenters.h"
#include "section_list_model.h"
#include "source_path_model.h"
#include "videosynth/path_resolution.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/source_frame_range.h"
#include "videosynth/timing_constants.h"

namespace videosynth::gui {

namespace {

constexpr int kMaxDurationFrames = 10000000;
constexpr int kMaxOverlayOffset = 9999;

// OSD overlay table columns.
enum OsdColumn {
  kOsdTextColumn = 0,
  kOsdXColumn,
  kOsdYColumn,
  kOsdScaleColumn,
  kOsdFgColumn,
  kOsdBgColumn,
  kOsdColumnCount,
};

int64_t ParseSeed(const QString& text) {
  bool ok = false;
  const qlonglong value = text.trimmed().toLongLong(&ok);
  return ok ? static_cast<int64_t>(value) : 0;
}

}  // namespace

SectionEditor::SectionEditor(ProjectDocument* document,
                             SourceProbeController* probe_controller,
                             QWidget* parent)
    : QWidget(parent),
      document_(document),
      probe_controller_(probe_controller) {
  BuildUi();
  SetCurrentSection(-1);

  connect(document_, &ProjectDocument::SectionEdited, this, [this](int index) {
    if (index == section_index_ && !committing_) {
      LoadFromDocument();
    }
  });
  // Standard changes alter the injection catalogues and probe verdict.
  connect(document_, &ProjectDocument::ProjectSettingsChanged, this, [this] {
    if (section_index_ >= 0 && !committing_) {
      LoadFromDocument();
    }
  });
  connect(probe_controller_, &SourceProbeController::ProbeStarted, this,
          [this] {
            probe_status_label_->setText(tr("Probing source…"));
            probe_detail_label_->clear();
          });
  connect(probe_controller_, &SourceProbeController::ReportChanged, this,
          [this] { UpdateProbeDisplay(); });
}

void SectionEditor::BuildUi() {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);

  placeholder_ = new QLabel(tr("Select a section in the Sections list."), this);
  placeholder_->setAlignment(Qt::AlignCenter);
  outer->addWidget(placeholder_);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  content_ = new QWidget(scroll);
  auto* layout = new QVBoxLayout(content_);

  // Batch-editing banner: visible only while the sections list holds a
  // multi-row selection.
  multi_edit_hint_ = new QLabel(content_);
  multi_edit_hint_->setWordWrap(true);
  multi_edit_hint_->setVisible(false);
  layout->addWidget(multi_edit_hint_);

  layout->addWidget(BuildGeneralGroup());
  layout->addWidget(BuildProbeGroup());
  layout->addWidget(BuildAudioGroup());
  layout->addWidget(BuildNoiseGroup());
  layout->addWidget(BuildDropoutsGroup());
  layout->addWidget(BuildOsdGroup());
  layout->addWidget(BuildInjectionsGroup());
  layout->addStretch();

  scroll->setWidget(content_);
  outer->addWidget(scroll);
}

QWidget* SectionEditor::BuildGeneralGroup() {
  auto* group = new QGroupBox(tr("Section"), content_);
  auto* form = new QFormLayout(group);

  name_edit_ = new QLineEdit(group);
  name_edit_->setMinimumWidth(name_edit_->sizeHint().width() * 2);
  form->addRow(tr("Name:"), name_edit_);

  section_type_combo_ = new QComboBox(group);
  section_type_combo_->addItem(tr("(none)"));
  section_type_combo_->addItem(QStringLiteral("lead_in"));
  section_type_combo_->addItem(QStringLiteral("programme_area"));
  section_type_combo_->addItem(QStringLiteral("lead_out"));
  form->addRow(tr("Disc section type:"), section_type_combo_);

  // Source: a two-way choice between a built-in (shipped) asset and a local
  // file. Built-in composes {bundled}/<type>/<raster>/<file> from the
  // project raster so only the filename is a real choice; Local file is a
  // browsed path stored project-relative or absolute.
  source_mode_combo_ = new QComboBox(group);
  source_mode_combo_->setObjectName(QStringLiteral("sourceModeCombo"));
  source_mode_combo_->addItem(tr("Built-in asset"), QStringLiteral("builtin"));
  source_mode_combo_->addItem(tr("Local file"), QStringLiteral("own"));
  source_mode_combo_->setToolTip(
      tr("Built-in = an asset shipped with videosynth (only the file name is "
         "yours to pick; the folder follows the project's video standard). "
         "Local file = any file on disk, stored relative to the project or as "
         "a full path."));
  form->addRow(tr("Source:"), source_mode_combo_);

  source_stack_ = new QStackedWidget(group);

  // Page 0: built-in asset — asset-type toggle plus a filename dropdown.
  auto* builtin_page = new QWidget(source_stack_);
  auto* builtin_row = new QHBoxLayout(builtin_page);
  builtin_row->setContentsMargins(0, 0, 0, 0);
  builtin_type_combo_ = new QComboBox(builtin_page);
  builtin_type_combo_->setObjectName(QStringLiteral("builtinTypeCombo"));
  builtin_type_combo_->addItem(tr("Still image (EXR)"), QStringLiteral("exr"));
  builtin_type_combo_->addItem(tr("Video"), QStringLiteral("mkv"));
  builtin_file_combo_ = new QComboBox(builtin_page);
  builtin_file_combo_->setObjectName(QStringLiteral("builtinFileCombo"));
  builtin_row->addWidget(builtin_type_combo_);
  builtin_row->addWidget(builtin_file_combo_, 1);
  source_stack_->addWidget(builtin_page);

  // Page 1: my own file — path edit, browse, and a portability checkbox.
  auto* own_page = new QWidget(source_stack_);
  auto* own_col = new QVBoxLayout(own_page);
  own_col->setContentsMargins(0, 0, 0, 0);
  auto* own_row = new QHBoxLayout();
  source_edit_ = new QLineEdit(own_page);
  source_edit_->setObjectName(QStringLiteral("sourceEdit"));
  auto* browse = new QPushButton(tr("Browse…"), own_page);
  own_row->addWidget(source_edit_, 1);
  own_row->addWidget(browse);
  own_col->addLayout(own_row);
  source_relative_check_ = new QCheckBox(tr("Relative to project"), own_page);
  source_relative_check_->setObjectName(QStringLiteral("sourceRelativeCheck"));
  source_relative_check_->setToolTip(
      tr("Store the path relative to the project file so the project stays "
         "portable; unticked stores a full absolute path."));
  own_col->addWidget(source_relative_check_);
  source_stack_->addWidget(own_page);

  form->addRow(QString(), source_stack_);

  source_resolved_hint_ = new QLabel(group);
  source_resolved_hint_->setWordWrap(true);
  source_resolved_hint_->setEnabled(false);
  form->addRow(QString(), source_resolved_hint_);

  // Selected source range: which frames of a video the section plays. The
  // duration below then counts or loops within this range.
  start_frame_spin_ = new QSpinBox(group);
  start_frame_spin_->setObjectName(QStringLiteral("startFrameSpin"));
  start_frame_spin_->setRange(0, kMaxDurationFrames);
  start_frame_spin_->setToolTip(
      tr("First source frame the section plays (0-based)."));
  end_frame_spin_ = new QSpinBox(group);
  end_frame_spin_->setObjectName(QStringLiteral("endFrameSpin"));
  end_frame_spin_->setRange(0, kMaxDurationFrames);
  end_frame_spin_->setToolTip(
      tr("Last source frame the section plays (0-based, inclusive)."));
  end_frame_to_source_end_check_ = new QCheckBox(tr("To end of source"), group);
  end_frame_to_source_end_check_->setObjectName(
      QStringLiteral("endFrameToSourceEndCheck"));
  auto* range_row = new QHBoxLayout();
  range_row->addWidget(start_frame_spin_);
  range_row->addWidget(new QLabel(tr("to"), group));
  range_row->addWidget(end_frame_spin_);
  range_row->addWidget(end_frame_to_source_end_check_);
  range_row->addStretch();
  form->addRow(tr("Source frames:"), range_row);

  duration_spin_ = new QSpinBox(group);
  duration_spin_->setRange(1, kMaxDurationFrames);
  duration_spin_->setSuffix(tr(" frames"));
  // Read-along seconds mirror of the frame count: purely a convenience view,
  // the stored duration is always frames.
  duration_seconds_spin_ = new QDoubleSpinBox(group);
  duration_seconds_spin_->setDecimals(3);
  duration_seconds_spin_->setSuffix(tr(" s"));
  duration_seconds_spin_->setRange(0.0, kMaxDurationFrames);
  // Convert on Enter/focus-out/arrow steps rather than every keystroke so a
  // half-typed value never rounds to frames mid-edit.
  duration_seconds_spin_->setKeyboardTracking(false);
  duration_seconds_spin_->setToolTip(
      tr("The same duration in seconds at the project standard's frame rate. "
         "The stored duration is always frames; editing either field updates "
         "the other."));
  duration_all_check_ = new QCheckBox(tr("All source frames"), group);
  duration_repeat_label_ = new QLabel(tr("repeat"), group);
  duration_repeat_spin_ = new QSpinBox(group);
  duration_repeat_spin_->setRange(1, kMaxDurationFrames);
  duration_repeat_spin_->setSuffix(tr("x"));
  duration_repeat_spin_->setToolTip(
      tr("How many times the selected source frames are replayed in this "
         "section."));
  auto* duration_row = new QHBoxLayout();
  duration_row->addWidget(duration_spin_);
  duration_row->addWidget(duration_seconds_spin_);
  duration_row->addWidget(duration_all_check_);
  duration_row->addWidget(duration_repeat_label_);
  duration_row->addWidget(duration_repeat_spin_);
  duration_row->addStretch();
  form->addRow(tr("Duration:"), duration_row);

  // Live summary of the resolved source length and the repeat multiplier.
  duration_summary_label_ = new QLabel(group);
  duration_summary_label_->setEnabled(false);
  form->addRow(QString(), duration_summary_label_);

  frame_range_label_ = new QLabel(group);
  form->addRow(tr("Frame range:"), frame_range_label_);

  // Disc position range: only meaningful when the project settings select a
  // laserdisc format, so the row is hidden otherwise.
  disc_range_title_ = new QLabel(group);
  disc_range_label_ = new QLabel(group);
  form->addRow(disc_range_title_, disc_range_label_);
  general_form_ = form;

  connect(name_edit_, &QLineEdit::editingFinished, this,
          &SectionEditor::CommitSection);
  connect(section_type_combo_, &QComboBox::activated, this,
          &SectionEditor::CommitSection);
  // A different file resets the frame selection; storing the same file
  // relative or absolute does not.
  const auto commit_source = [this] {
    if (!updating_) {
      CommitSourceSelection();
      RequestProbe();
      UpdateSourceResolvedHint();
    }
  };
  const auto commit_source_path_form = [this] {
    if (!updating_) {
      CommitSection();
      RequestProbe();
      UpdateSourceResolvedHint();
    }
  };
  connect(source_mode_combo_, &QComboBox::activated, this,
          [this, commit_source] {
            source_stack_->setCurrentIndex(source_mode_combo_->currentIndex());
            commit_source();
          });
  connect(builtin_type_combo_, &QComboBox::activated, this,
          [this, commit_source] {
            if (!updating_) {
              PopulateBuiltinFiles(QString());
            }
            commit_source();
          });
  connect(builtin_file_combo_, &QComboBox::activated, this,
          [commit_source](int) { commit_source(); });
  connect(source_edit_, &QLineEdit::editingFinished, this, commit_source);
  connect(source_relative_check_, &QCheckBox::toggled, this,
          [commit_source_path_form](bool) { commit_source_path_form(); });
  connect(browse, &QPushButton::clicked, this, &SectionEditor::OnBrowseSource);
  connect(start_frame_spin_, &QSpinBox::valueChanged, this, [this](int start) {
    if (updating_) {
      return;
    }
    // Moving the start past a set end drags the end along, so the range
    // never inverts.
    if (!end_frame_to_source_end_check_->isChecked() &&
        end_frame_spin_->value() < start) {
      const QSignalBlocker blocker(end_frame_spin_);
      end_frame_spin_->setValue(start);
    }
    CommitSection();
  });
  connect(end_frame_spin_, &QSpinBox::valueChanged, this, [this](int) {
    if (!updating_) {
      CommitSection();
    }
  });
  connect(end_frame_to_source_end_check_, &QCheckBox::toggled, this,
          [this](bool to_source_end) {
            end_frame_spin_->setEnabled(!to_source_end &&
                                        start_frame_spin_->isEnabled());
            if (updating_) {
              return;
            }
            if (!to_source_end) {
              // Start from the whole remaining source when its length is
              // known, otherwise from a one-frame range.
              const int frame_count = ProbedSourceFrameCount();
              const QSignalBlocker blocker(end_frame_spin_);
              end_frame_spin_->setValue(
                  std::max(start_frame_spin_->value(), frame_count - 1));
            }
            CommitSection();
          });
  connect(duration_spin_, &QSpinBox::valueChanged, this, [this](int) {
    SyncDurationSecondsFromFrames();
    if (!updating_) {
      CommitSection();
    }
  });
  connect(duration_seconds_spin_, &QDoubleSpinBox::valueChanged, this,
          [this](double seconds) {
            const double rate = ProjectFrameRateHz();
            if (rate <= 0.0) {
              return;
            }
            // Blocked so the frames handler does not rewrite the seconds
            // field mid-interaction; the deferred reload after the commit
            // normalises the display to the stored frame count.
            const QSignalBlocker blocker(duration_spin_);
            duration_spin_->setValue(
                DurationSecondsToFrames(seconds, rate, kMaxDurationFrames));
            if (!updating_) {
              CommitSection();
            }
          });
  connect(duration_all_check_, &QCheckBox::toggled, this, [this](bool all) {
    duration_spin_->setEnabled(!all);
    duration_seconds_spin_->setEnabled(!all && ProjectFrameRateHz() > 0.0);
    duration_repeat_label_->setEnabled(all);
    duration_repeat_spin_->setEnabled(all);
    UpdateDurationSummary();
    if (!updating_) {
      CommitSection();
    }
  });
  connect(duration_repeat_spin_, &QSpinBox::valueChanged, this, [this](int) {
    UpdateDurationSummary();
    if (!updating_) {
      CommitSection();
    }
  });
  return group;
}

QWidget* SectionEditor::BuildProbeGroup() {
  auto* group = new QGroupBox(tr("Source Profile"), content_);
  auto* layout = new QVBoxLayout(group);
  probe_status_label_ = new QLabel(group);
  probe_detail_label_ = new QLabel(group);
  probe_detail_label_->setWordWrap(true);
  layout->addWidget(probe_status_label_);
  layout->addWidget(probe_detail_label_);
  return group;
}

QWidget* SectionEditor::BuildAudioGroup() {
  audio_group_ = new QGroupBox(tr("Audio channel pairs"), content_);
  audio_group_->setCheckable(true);
  auto* layout = new QVBoxLayout(audio_group_);
  audio_editor_ = new AudioChannelPairsEditor(audio_group_);
  audio_editor_->setMinimumHeight(260);
  layout->addWidget(audio_editor_);

  connect(audio_editor_, &AudioChannelPairsEditor::ChannelPairsEdited, this,
          [this] {
            if (!updating_) {
              CommitSection();
            }
          });
  // The block exists exactly when at least one channel pair exists; ticking the
  // box seeds the first one, unticking clears them (SectionFromWidgets).
  connect(audio_group_, &QGroupBox::toggled, this, [this](bool checked) {
    audio_editor_->setVisible(checked);
    if (updating_) {
      return;
    }
    if (checked && audio_editor_->channel_pairs().empty()) {
      Section section = SectionFromWidgets();
      section.audio_channel_pairs.push_back(MakeDefaultAudioChannelPair(0));
      CommitSectionToDocument(section);
      LoadFromDocument();
      return;
    }
    CommitSection();
  });
  audio_editor_->setVisible(audio_group_->isChecked());
  return audio_group_;
}

QWidget* SectionEditor::BuildNoiseGroup() {
  noise_group_ = new QGroupBox(tr("Noise injection"), content_);
  noise_group_->setCheckable(true);
  auto* outer = new QVBoxLayout(noise_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(noise_group_);
  auto* form = new QFormLayout(body);

  noise_db_spin_ = new QDoubleSpinBox(body);
  noise_db_spin_->setRange(editor_limits::kNoiseDbMin,
                           editor_limits::kNoiseDbMax);
  noise_db_spin_->setDecimals(1);
  noise_db_spin_->setSuffix(tr(" dB"));
  CapFieldWidthAtSizeHint(noise_db_spin_);
  form->addRow(tr("Noise floor (Black PSNR):"), noise_db_spin_);

  noise_spread_spin_ = new QDoubleSpinBox(body);
  // noise_db − spread must stay ≥ the 20 dB floor (validator rule).
  noise_spread_spin_->setRange(
      0.0, editor_limits::kNoiseDbMax - editor_limits::kNoiseDbMin);
  noise_spread_spin_->setDecimals(1);
  noise_spread_spin_->setSuffix(tr(" dB"));
  CapFieldWidthAtSizeHint(noise_spread_spin_);
  form->addRow(tr("White spread:"), noise_spread_spin_);

  noise_seed_check_ = new QCheckBox(tr("Fixed seed"), body);
  noise_seed_edit_ = new QLineEdit(body);
  CapFieldWidthToText(noise_seed_edit_, QStringLiteral("10,000,000"));
  auto* seed_row = new QHBoxLayout();
  seed_row->addWidget(noise_seed_check_);
  seed_row->addWidget(noise_seed_edit_);
  seed_row->addStretch();
  form->addRow(seed_row);
  outer->addWidget(body);

  const auto commit = [this] {
    if (!updating_) {
      CommitSection();
    }
  };
  connect(noise_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(noise_group_, &QGroupBox::toggled, this, commit);
  body->setVisible(noise_group_->isChecked());
  connect(noise_db_spin_, &QDoubleSpinBox::valueChanged, this, commit);
  connect(noise_spread_spin_, &QDoubleSpinBox::valueChanged, this, commit);
  connect(noise_seed_check_, &QCheckBox::toggled, this, [this](bool checked) {
    noise_seed_edit_->setEnabled(checked);
    if (!updating_) {
      CommitSection();
    }
  });
  connect(noise_seed_edit_, &QLineEdit::editingFinished, this, commit);
  return noise_group_;
}

QWidget* SectionEditor::BuildDropoutsGroup() {
  auto* group = new QGroupBox(tr("Dropouts"), content_);
  auto* layout = new QVBoxLayout(group);

  const auto commit = [this] {
    if (!updating_) {
      CommitSection();
    }
  };

  const auto build_block = [&](const QString& title,
                               const QString& scale_tool_tip,
                               QGroupBox** out_group, QSpinBox** out_scale,
                               QCheckBox** out_check, QLineEdit** out_edit) {
    auto* block = new QGroupBox(title, group);
    block->setCheckable(true);
    auto* outer = new QVBoxLayout(block);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* body = new QWidget(block);
    auto* form = new QFormLayout(body);
    auto* scale = new QSpinBox(body);
    scale->setRange(editor_limits::kDropoutScaleMin,
                    editor_limits::kDropoutScaleMax);
    scale->setToolTip(scale_tool_tip);
    CapFieldWidthAtSizeHint(scale);
    form->addRow(tr("Scale:"), scale);
    auto* check = new QCheckBox(tr("Fixed seed"), body);
    auto* edit = new QLineEdit(body);
    CapFieldWidthToText(edit, QStringLiteral("10,000,000"));
    auto* seed_row = new QHBoxLayout();
    seed_row->addWidget(check);
    seed_row->addWidget(edit);
    seed_row->addStretch();
    form->addRow(seed_row);
    outer->addWidget(body);

    connect(block, &QGroupBox::toggled, body, &QWidget::setVisible);
    connect(block, &QGroupBox::toggled, this, commit);
    body->setVisible(block->isChecked());
    connect(scale, &QSpinBox::valueChanged, this, commit);
    connect(check, &QCheckBox::toggled, this, [this, edit](bool checked) {
      edit->setEnabled(checked);
      if (!updating_) {
        CommitSection();
      }
    });
    connect(edit, &QLineEdit::editingFinished, this, commit);

    *out_group = block;
    *out_scale = scale;
    *out_check = check;
    *out_edit = edit;
    layout->addWidget(block);
  };

  build_block(
      tr("Random (Poisson)"),
      tr("Severity level, not a physical unit: 1 = rare, short dropouts; "
         "20 = frequent, longer ones. Each step raises the expected dropouts "
         "per frame and their length in samples exponentially."),
      &random_dropouts_group_, &random_scale_spin_, &random_seed_check_,
      &random_seed_edit_);
  build_block(
      tr("Scratch"),
      tr("Severity level, not a physical unit: 1 = a couple of brief, narrow "
         "scratches; 20 = many long-lived, wide ones. Each step raises the "
         "scratch count, lifetime in frames, and width in samples "
         "exponentially."),
      &scratch_dropouts_group_, &scratch_scale_spin_, &scratch_seed_check_,
      &scratch_seed_edit_);
  return group;
}

QWidget* SectionEditor::BuildOsdGroup() {
  osd_group_ = new QGroupBox(tr("On-screen display"), content_);
  osd_group_->setCheckable(true);
  auto* outer = new QVBoxLayout(osd_group_);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QWidget(osd_group_);
  auto* layout = new QVBoxLayout(body);

  osd_table_ = new QTableWidget(0, kOsdColumnCount, body);
  osd_table_->setHorizontalHeaderLabels({tr("Text"), tr("X"), tr("Y"),
                                         tr("Scale"), tr("Fg luma"),
                                         tr("Bg luma")});
  osd_table_->horizontalHeader()->setSectionResizeMode(kOsdTextColumn,
                                                       QHeaderView::Stretch);
  osd_table_->verticalHeader()->setVisible(false);
  osd_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  osd_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(osd_table_);

  auto* buttons = new QHBoxLayout();
  auto* add_button = new QPushButton(tr("Add overlay"), body);
  remove_overlay_button_ = new QPushButton(tr("Remove overlay"), body);
  buttons->addWidget(add_button);
  buttons->addWidget(remove_overlay_button_);
  buttons->addStretch();
  layout->addLayout(buttons);

  QString token_help = tr("Tokens:");
  for (const OsdTokenHelp& help : OsdTokenCatalogue()) {
    token_help += QStringLiteral("\n%1 — %2")
                      .arg(QString::fromStdString(help.token),
                           QString::fromStdString(help.description));
  }
  auto* token_label = new QLabel(token_help, body);
  token_label->setWordWrap(true);
  layout->addWidget(token_label);
  outer->addWidget(body);

  connect(osd_group_, &QGroupBox::toggled, body, &QWidget::setVisible);
  connect(osd_group_, &QGroupBox::toggled, this, [this](bool checked) {
    if (updating_) {
      return;
    }
    if (checked && osd_table_->rowCount() == 0) {
      // The block exists exactly when overlays exist; seed the first one.
      OnAddOverlay();
      return;
    }
    CommitSection();
  });
  connect(add_button, &QPushButton::clicked, this,
          &SectionEditor::OnAddOverlay);
  connect(remove_overlay_button_, &QPushButton::clicked, this,
          &SectionEditor::OnRemoveOverlay);
  body->setVisible(osd_group_->isChecked());
  return osd_group_;
}

QWidget* SectionEditor::BuildInjectionsGroup() {
  injections_group_ = new QGroupBox(tr("Line injections"), content_);
  injections_group_->setCheckable(true);
  auto* layout = new QVBoxLayout(injections_group_);
  injections_editor_ = new LineInjectionsEditor(injections_group_);
  injections_editor_->setMinimumHeight(220);
  layout->addWidget(injections_editor_);

  connect(injections_editor_, &LineInjectionsEditor::InjectionsEdited, this,
          [this] {
            if (!updating_) {
              CommitSection();
            }
          });
  // The block exists exactly when at least one injection exists; ticking the
  // box seeds a default one, unticking clears them (SectionFromWidgets).
  connect(injections_group_, &QGroupBox::toggled, this, [this](bool checked) {
    injections_editor_->setVisible(checked);
    if (updating_) {
      return;
    }
    if (checked) {
      if (injections_editor_->injections().empty()) {
        // Emits InjectionsEdited, which commits the now-non-empty block.
        injections_editor_->AddDefaultInjection();
      }
    } else {
      CommitSection();
    }
  });
  injections_editor_->setVisible(injections_group_->isChecked());
  return injections_group_;
}

void SectionEditor::SetCurrentSection(int index) {
  section_index_ =
      (index >= 0 && index < document_->section_count()) ? index : -1;
  const bool has_section = section_index_ >= 0;
  placeholder_->setVisible(!has_section);
  content_->setVisible(has_section);
  if (has_section) {
    LoadFromDocument();
    RequestProbe();
  }
}

void SectionEditor::SetSelectedSections(std::vector<int> indices) {
  selected_sections_ = std::move(indices);
  UpdateMultiEditHint();
  injections_editor_->SetBatchSectionTypes(SelectedSectionTypes());
}

std::vector<SectionType> SectionEditor::SelectedSectionTypes() const {
  if (selected_sections_.size() < 2) {
    return {};
  }
  std::vector<SectionType> types;
  for (const int index : selected_sections_) {
    if (index >= 0 && index < document_->section_count()) {
      types.push_back(document_->project()
                          .sections[static_cast<std::size_t>(index)]
                          .section_type);
    }
  }
  return types.size() > 1 ? types : std::vector<SectionType>{};
}

void SectionEditor::UpdateMultiEditHint() {
  const int count = static_cast<int>(selected_sections_.size());
  const bool batch_editing = count > 1;
  if (batch_editing) {
    multi_edit_hint_->setText(
        tr("Editing %1 selected sections: changes made here apply to all of "
           "them (names stay individual; lead_in and lead_out cannot be "
           "applied to more than one section).")
            .arg(count));
  }
  multi_edit_hint_->setVisible(batch_editing);

  // Batch editing mirrors the disc section type onto every selected section,
  // so types limited to a single section (lead_in, lead_out) must not be
  // offered while a multi-selection is active.
  if (auto* model =
          qobject_cast<QStandardItemModel*>(section_type_combo_->model())) {
    for (int i = 0; i < model->rowCount(); ++i) {
      const SectionType type =
          SectionTypeFromString(section_type_combo_->itemText(i).toStdString());
      model->item(i)->setEnabled(!batch_editing ||
                                 SectionTypeAllowsBatchAssignment(type));
    }
  }
}

void SectionEditor::LoadFromDocument() {
  if (section_index_ < 0 || section_index_ >= document_->section_count()) {
    return;
  }
  updating_ = true;
  const Project& project = document_->project();
  const Section& section =
      project.sections[static_cast<std::size_t>(section_index_)];

  name_edit_->setText(QString::fromStdString(section.name));
  section_type_combo_->setCurrentIndex(
      section.section_type == SectionType::kUnknown
          ? 0
          : section_type_combo_->findText(QString::fromStdString(
                SectionTypeToString(section.section_type))));
  LoadSourceWidgets(section.source);
  duration_all_check_->setChecked(section.duration_frames_all);
  duration_spin_->setEnabled(!section.duration_frames_all);
  duration_seconds_spin_->setEnabled(!section.duration_frames_all &&
                                     ProjectFrameRateHz() > 0.0);
  if (section.duration_frames > 0) {
    duration_spin_->setValue(section.duration_frames);
  }
  SyncDurationSecondsFromFrames();
  duration_repeat_spin_->setValue(std::max(1, section.duration_frames_repeat));
  duration_repeat_label_->setEnabled(section.duration_frames_all);
  duration_repeat_spin_->setEnabled(section.duration_frames_all);
  // A still has no length of its own; the box stays usable only to untick a
  // project that set it anyway.
  duration_all_check_->setEnabled(!IsProgressiveStillSource(section.source) ||
                                  section.duration_frames_all);
  end_frame_to_source_end_check_->setChecked(!section.end_frame_specified);
  UpdateSourceRangeWidgets();
  start_frame_spin_->setValue(section.start_frame);
  end_frame_spin_->setValue(section.end_frame_specified ? section.end_frame
                                                        : section.start_frame);
  UpdateDurationSummary();

  UpdateFrameRangeDisplay();

  // Audio (block present exactly when it carries channel pairs).
  audio_group_->setChecked(!section.audio_channel_pairs.empty());
  audio_editor_->SetProjectBaseDir(ProjectBaseDir());
  UpdateAudioSourceContext();
  audio_editor_->SetChannelPairs(section.audio_channel_pairs);

  // Noise.
  noise_group_->setChecked(section.noise.enabled);
  noise_db_spin_->setValue(section.noise.noise_db);
  noise_spread_spin_->setValue(section.noise.noise_spread_db);
  noise_seed_check_->setChecked(section.noise.noise_seed_specified);
  noise_seed_edit_->setEnabled(section.noise.noise_seed_specified);
  noise_seed_edit_->setText(
      section.noise.noise_seed_specified
          ? QString::number(static_cast<qlonglong>(section.noise.noise_seed))
          : QString());

  // Dropouts.
  const DropoutParameters& dropouts = section.dropouts;
  random_dropouts_group_->setChecked(dropouts.random.enabled);
  random_scale_spin_->setValue(
      qMax(editor_limits::kDropoutScaleMin, dropouts.random.scale));
  random_seed_check_->setChecked(dropouts.random.seed_specified);
  random_seed_edit_->setEnabled(dropouts.random.seed_specified);
  random_seed_edit_->setText(
      dropouts.random.seed_specified
          ? QString::number(static_cast<qlonglong>(dropouts.random.seed))
          : QString());
  scratch_dropouts_group_->setChecked(dropouts.scratch.enabled);
  scratch_scale_spin_->setValue(
      qMax(editor_limits::kDropoutScaleMin, dropouts.scratch.scale));
  scratch_seed_check_->setChecked(dropouts.scratch.seed_specified);
  scratch_seed_edit_->setEnabled(dropouts.scratch.seed_specified);
  scratch_seed_edit_->setText(
      dropouts.scratch.seed_specified
          ? QString::number(static_cast<qlonglong>(dropouts.scratch.seed))
          : QString());

  // OSD.
  osd_group_->setChecked(OsdBlockEnabled(section));
  LoadOsdTable(section);

  // Line injections. Section-level injections carry only laserdisc biphase
  // codes, which are meaningless without a project-wide disc format, so the
  // whole group is hidden until CAV/CLV is chosen in project settings.
  const DiscType project_disc_type =
      DiscTypeFromString(project.line_injections.disc_type);
  const bool laserdisc_project = project_disc_type != DiscType::kUnknown;
  injections_group_->setVisible(laserdisc_project);
  injections_group_->setChecked(!section.line_injections.empty());
  injections_editor_->SetContext(project.cvbs_presets.video_standard_preset,
                                 section.section_type, project_disc_type);
  injections_editor_->SetBatchSectionTypes(SelectedSectionTypes());
  injections_editor_->SetInjections(section.line_injections);

  updating_ = false;
}

void SectionEditor::LoadOsdTable(const Section& section) {
  osd_table_->setRowCount(static_cast<int>(section.osd.overlays.size()));

  const auto commit = [this] {
    if (!updating_) {
      CommitSection();
    }
  };

  for (int row = 0; row < static_cast<int>(section.osd.overlays.size());
       ++row) {
    const OsdOverlay& overlay =
        section.osd.overlays[static_cast<std::size_t>(row)];

    auto* text_edit = new QLineEdit(osd_table_);
    text_edit->setText(QString::fromStdString(overlay.text));
    connect(text_edit, &QLineEdit::editingFinished, this, commit);
    osd_table_->setCellWidget(row, kOsdTextColumn, text_edit);

    const auto make_int_spin = [&](int column, int min, int max, int value) {
      auto* spin = new QSpinBox(osd_table_);
      spin->setRange(min, max);
      spin->setValue(value);
      connect(spin, &QSpinBox::valueChanged, this, commit);
      osd_table_->setCellWidget(row, column, spin);
    };
    make_int_spin(kOsdXColumn, 0, kMaxOverlayOffset, overlay.x);
    make_int_spin(kOsdYColumn, 0, kMaxOverlayOffset, overlay.y);
    make_int_spin(kOsdScaleColumn, editor_limits::kOsdScaleMin,
                  editor_limits::kOsdScaleMax, overlay.scale);

    auto* fg_combo = new QComboBox(osd_table_);
    fg_combo->addItem(tr("White"), static_cast<int>(OsdFgLevel::kWhite));
    fg_combo->addItem(tr("Light grey"),
                      static_cast<int>(OsdFgLevel::kLightGrey));
    fg_combo->addItem(tr("Dark grey"), static_cast<int>(OsdFgLevel::kDarkGrey));
    fg_combo->addItem(tr("Black"), static_cast<int>(OsdFgLevel::kBlack));
    const int fg_index = fg_combo->findData(static_cast<int>(overlay.fg_level));
    fg_combo->setCurrentIndex(fg_index >= 0 ? fg_index : 0);
    connect(fg_combo, &QComboBox::currentIndexChanged, this, commit);
    osd_table_->setCellWidget(row, kOsdFgColumn, fg_combo);

    auto* bg_combo = new QComboBox(osd_table_);
    bg_combo->addItem(tr("Transparent"),
                      static_cast<int>(OsdBgLevel::kTransparent));
    bg_combo->addItem(tr("White"), static_cast<int>(OsdBgLevel::kWhite));
    bg_combo->addItem(tr("Light grey"),
                      static_cast<int>(OsdBgLevel::kLightGrey));
    bg_combo->addItem(tr("Dark grey"), static_cast<int>(OsdBgLevel::kDarkGrey));
    bg_combo->addItem(tr("Black"), static_cast<int>(OsdBgLevel::kBlack));
    const int bg_index = bg_combo->findData(static_cast<int>(overlay.bg_level));
    bg_combo->setCurrentIndex(bg_index >= 0 ? bg_index : 0);
    connect(bg_combo, &QComboBox::currentIndexChanged, this, commit);
    osd_table_->setCellWidget(row, kOsdBgColumn, bg_combo);
  }
  remove_overlay_button_->setEnabled(!section.osd.overlays.empty());
}

Section SectionEditor::SectionFromWidgets() const {
  Section section =
      document_->project().sections[static_cast<std::size_t>(section_index_)];

  section.name = name_edit_->text().toStdString();
  section.section_type =
      section_type_combo_->currentIndex() <= 0
          ? SectionType::kUnknown
          : SectionTypeFromString(
                section_type_combo_->currentText().toStdString());
  section.source = SourceFromWidgets();
  section.duration_frames_all = duration_all_check_->isChecked();
  section.duration_frames =
      section.duration_frames_all ? 0 : duration_spin_->value();
  section.duration_frames_repeat =
      section.duration_frames_all ? duration_repeat_spin_->value() : 1;
  // The range widgets are live for video sources only; otherwise the stored
  // values stand (the validator reports any a still should not carry).
  if (start_frame_spin_->isEnabled()) {
    section.start_frame = start_frame_spin_->value();
    section.end_frame_specified = !end_frame_to_source_end_check_->isChecked();
    section.end_frame =
        section.end_frame_specified ? end_frame_spin_->value() : 0;
  }

  // Audio block (unticking excludes it entirely).
  section.audio_channel_pairs = audio_group_->isChecked()
                                    ? audio_editor_->channel_pairs()
                                    : std::vector<AudioChannelPair>{};

  // Noise block.
  if (noise_group_->isChecked()) {
    section.noise.enabled = true;
    section.noise.noise_db = noise_db_spin_->value();
    section.noise.noise_spread_db = noise_spread_spin_->value();
    section.noise.noise_seed_specified = noise_seed_check_->isChecked();
    section.noise.noise_seed = noise_seed_check_->isChecked()
                                   ? ParseSeed(noise_seed_edit_->text())
                                   : 0;
  } else {
    SetNoiseBlockEnabled(&section, false);
  }

  // Dropout blocks.
  if (random_dropouts_group_->isChecked()) {
    section.dropouts.random.enabled = true;
    section.dropouts.random.scale = random_scale_spin_->value();
    section.dropouts.random.seed_specified = random_seed_check_->isChecked();
    section.dropouts.random.seed = random_seed_check_->isChecked()
                                       ? ParseSeed(random_seed_edit_->text())
                                       : 0;
  } else {
    SetRandomDropoutsEnabled(&section, false);
  }
  if (scratch_dropouts_group_->isChecked()) {
    section.dropouts.scratch.enabled = true;
    section.dropouts.scratch.scale = scratch_scale_spin_->value();
    section.dropouts.scratch.seed_specified = scratch_seed_check_->isChecked();
    section.dropouts.scratch.seed = scratch_seed_check_->isChecked()
                                        ? ParseSeed(scratch_seed_edit_->text())
                                        : 0;
  } else {
    SetScratchDropoutsEnabled(&section, false);
  }

  // OSD block.
  section.osd.overlays.clear();
  if (osd_group_->isChecked()) {
    for (int row = 0; row < osd_table_->rowCount(); ++row) {
      OsdOverlay overlay;
      if (auto* text_edit = qobject_cast<QLineEdit*>(
              osd_table_->cellWidget(row, kOsdTextColumn))) {
        overlay.text = text_edit->text().toStdString();
      }
      const auto int_value = [this, row](int column, int fallback) {
        auto* spin =
            qobject_cast<QSpinBox*>(osd_table_->cellWidget(row, column));
        return spin != nullptr ? spin->value() : fallback;
      };
      overlay.x = int_value(kOsdXColumn, 0);
      overlay.y = int_value(kOsdYColumn, 0);
      overlay.scale = int_value(kOsdScaleColumn, 1);
      if (auto* fg_combo = qobject_cast<QComboBox*>(
              osd_table_->cellWidget(row, kOsdFgColumn))) {
        overlay.fg_level =
            static_cast<OsdFgLevel>(fg_combo->currentData().toInt());
      }
      if (auto* bg_combo = qobject_cast<QComboBox*>(
              osd_table_->cellWidget(row, kOsdBgColumn))) {
        overlay.bg_level =
            static_cast<OsdBgLevel>(bg_combo->currentData().toInt());
      }
      section.osd.overlays.push_back(overlay);
    }
  }

  // Line injections (unticking excludes them entirely).
  section.line_injections = injections_group_->isChecked()
                                ? injections_editor_->injections()
                                : std::vector<Section::LineInjection>{};

  return section;
}

bool SectionEditor::CommitSectionToDocument(const Section& section) {
  // Snapshot the pre-edit state before the primary commit so the delta the
  // user just made can be mirrored onto the other selected sections.
  const Section before =
      document_->project().sections[static_cast<std::size_t>(section_index_)];
  committing_ = true;
  // A batch-mirrored edit undoes as one step across every selected section.
  std::optional<ScopedUndoBatch> batch;
  if (selected_sections_.size() > 1) {
    batch.emplace(document_, QStringLiteral("Edit sections"));
  }
  bool changed = document_->SetSection(section_index_, section);
  for (const int index : selected_sections_) {
    if (index == section_index_ || index < 0 ||
        index >= document_->section_count()) {
      continue;
    }
    Section target = ApplySectionEditDelta(
        before, section,
        document_->project().sections[static_cast<std::size_t>(index)]);
    changed = document_->SetSection(index, std::move(target)) || changed;
  }
  batch.reset();
  committing_ = false;
  return changed;
}

void SectionEditor::CommitSection() {
  if (updating_ || section_index_ < 0) {
    return;
  }
  CommitEditedSection(SectionFromWidgets());
}

void SectionEditor::CommitSourceSelection() {
  if (updating_ || section_index_ < 0) {
    return;
  }
  const std::string previous_source =
      document_->project()
          .sections[static_cast<std::size_t>(section_index_)]
          .source;
  Section section = SectionFromWidgets();
  ApplySourceSelectionDefaults(previous_source, duration_spin_->value(),
                               &section);
  CommitEditedSection(section);
}

void SectionEditor::CommitEditedSection(const Section& section) {
  const bool changed = CommitSectionToDocument(section);

  if (changed && !reload_pending_) {
    // Refresh derived displays (start frame, block defaults seeded by the
    // enable helpers, injection catalogue context). The reload rebuilds child
    // editors — deleting and recreating their widgets — so it must never run
    // synchronously here: CommitSection is frequently reached from a child
    // widget's own signal (e.g. a laserdisc-code checkbox toggle), and tearing
    // that widget down while Qt is still mid-dispatch of its click is a
    // use-after-free. Defer to the next event-loop pass, once the originating
    // event has fully unwound.
    reload_pending_ = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
          reload_pending_ = false;
          LoadFromDocument();
        },
        Qt::QueuedConnection);
  }
}

void SectionEditor::RequestProbe() {
  if (section_index_ < 0) {
    return;
  }
  const Project& project = document_->project();
  // Resolve the source path exactly as generation/preview will (asset_bases
  // and, for saved projects, the project file's directory) so the probe reads
  // the same file the pipeline would. anchor_unset mirrors the GUI runner.
  const Project resolved = videosynth::ResolveProjectPaths(
      project, GuiAssetRoots(), ProjectBaseDir().toStdString(),
      /*anchor_unset=*/true);
  probe_controller_->RequestProbe(
      resolved.sections[static_cast<std::size_t>(section_index_)],
      resolved.cvbs_presets.video_standard_preset);
}

void SectionEditor::UpdateProbeDisplay() {
  if (!probe_controller_->has_report()) {
    probe_status_label_->clear();
    probe_detail_label_->clear();
    return;
  }
  const SourceProbeReport& report = probe_controller_->report();
  if (!report.probe_ok) {
    probe_status_label_->setText(tr("Probe failed"));
    probe_detail_label_->setText(report.probe_error);
  } else {
    probe_status_label_->setText(report.profile_issues.isEmpty()
                                     ? tr("Source profile OK")
                                     : tr("Source profile incompatible"));
    probe_detail_label_->setText(FormatProbeReportDetail(report));
  }
  // The probed frame count resolves "all source frames" durations and caps
  // the range widgets, so the derived displays refresh on every report
  // regardless of the verdict.
  {
    const bool was_updating = updating_;
    updating_ = true;
    UpdateSourceRangeWidgets();
    updating_ = was_updating;
  }
  UpdateDurationSummary();
  UpdateFrameRangeDisplay();
  UpdateAudioSourceContext();
}

void SectionEditor::UpdateAudioSourceContext() {
  if (section_index_ < 0 || section_index_ >= document_->section_count()) {
    return;
  }
  const Section& section =
      document_->project().sections[static_cast<std::size_t>(section_index_)];
  const bool probe_ok =
      probe_controller_->has_report() && probe_controller_->report().probe_ok;
  audio_editor_->SetSourceContext(section.source, probe_ok,
                                  probe_ok ? probe_controller_->report().profile
                                           : ProgressiveFrameSourceProfile{});
}

void SectionEditor::UpdateDurationSummary() {
  if (section_index_ < 0 || section_index_ >= document_->section_count()) {
    duration_summary_label_->clear();
    return;
  }
  const Section& section =
      document_->project().sections[static_cast<std::size_t>(section_index_)];
  SourceFrameRange range;
  std::string range_error;
  const bool range_known = ResolveSourceFrameRange(
      section, ProbedSourceFrameCount(), &range, &range_error);

  // A fixed duration already shows in the spinbox; only a video range it
  // overruns needs a note.
  if (!duration_all_check_->isChecked()) {
    if (IsProgressiveVideoSource(section.source) && range_known &&
        duration_spin_->value() > range.count) {
      duration_summary_label_->setText(
          tr("Loops the %1-frame source range").arg(range.count));
    } else {
      duration_summary_label_->clear();
    }
    return;
  }

  const int repeat = std::max(1, duration_repeat_spin_->value());
  if (!range_known) {
    if (ProbedSourceFrameCount() != kUnknownSourceFrameCount) {
      // The probe answered, so the range itself is invalid.
      duration_summary_label_->setText(QString::fromStdString(range_error));
      return;
    }
    // Probe pending or unavailable: still convey the multiplier.
    duration_summary_label_->setText(
        repeat > 1 ? tr("Source frame count x %1 (probing source…)").arg(repeat)
                   : tr("Using source frame count (probing source…)"));
    return;
  }

  const int64_t total = static_cast<int64_t>(range.count) * repeat;
  duration_summary_label_->setText(tr("%1 frames x %2 = %3 total")
                                       .arg(range.count)
                                       .arg(repeat)
                                       .arg(static_cast<qlonglong>(total)));
}

void SectionEditor::UpdateSourceRangeWidgets() {
  if (section_index_ < 0 || section_index_ >= document_->section_count()) {
    return;
  }
  const Section& section =
      document_->project().sections[static_cast<std::size_t>(section_index_)];
  const SourceRangeEditorState state = SourceRangeEditorStateFor(
      section, ProbedSourceFrameCount(), kMaxDurationFrames);
  start_frame_spin_->setEnabled(state.enabled);
  end_frame_to_source_end_check_->setEnabled(state.enabled);
  end_frame_spin_->setEnabled(state.enabled &&
                              !end_frame_to_source_end_check_->isChecked());
  start_frame_spin_->setMaximum(state.maximum);
  end_frame_spin_->setMaximum(state.maximum);
}

int SectionEditor::ProbedSourceFrameCount() const {
  if (!probe_controller_->has_report()) {
    return kUnknownSourceFrameCount;
  }
  const SourceProbeReport& report = probe_controller_->report();
  return report.probe_ok && report.profile.frame_count > 0
             ? report.profile.frame_count
             : kUnknownSourceFrameCount;
}

void SectionEditor::UpdateFrameRangeDisplay() {
  if (section_index_ < 0 || section_index_ >= document_->section_count()) {
    return;
  }
  const Project& project = document_->project();
  const Section& section =
      project.sections[static_cast<std::size_t>(section_index_)];

  // Cumulative start frame recalculated exactly as the sections list shows it
  // (preceding "all source frames" sections are open-ended there too).
  const std::vector<SectionListRow> rows = BuildSectionListRows(project);
  const int start_frame =
      rows[static_cast<std::size_t>(section_index_)].start_frame;

  // Duration: fixed durations and closed ranges are known immediately; an
  // open-ended "all source frames" duration resolves from the probed source
  // length, so it stays unknown until a successful probe report arrives.
  const int64_t duration =
      KnownSectionOutputFrameCount(section, ProbedSourceFrameCount());
  // An end below the start renders as "?" (unknown duration).
  const int end_frame = duration > 0
                            ? static_cast<int>(start_frame + duration - 1)
                            : start_frame - 1;

  frame_range_label_->setText(FrameRangeText(start_frame, end_frame));

  // Disc positions exist for programme_area sections only (IEC 60856/60857:
  // lead-in and lead-out carry no picture numbers or timecodes), so the row
  // hides for those sections and for non-laserdisc projects.
  const DiscType disc_type =
      DiscTypeFromString(project.line_injections.disc_type);
  const std::vector<int> disc_offsets =
      BuildDiscFrameOffsets(project, disc_type);
  const int disc_start = disc_offsets[static_cast<std::size_t>(section_index_)];
  const bool show_disc_range = disc_start >= 0;
  general_form_->setRowVisible(disc_range_label_, show_disc_range);
  if (show_disc_range) {
    const int disc_end = duration > 0
                             ? static_cast<int>(disc_start + duration - 1)
                             : disc_start - 1;
    disc_range_title_->setText(DiscRangeTitle(disc_type));
    disc_range_label_->setText(
        DiscRangeText(disc_type, project.cvbs_presets.video_standard_preset,
                      disc_start, disc_end));
  }
}

double SectionEditor::ProjectFrameRateHz() const {
  const Standard standard =
      document_->project().cvbs_presets.video_standard_preset;
  if (standard == Standard::kUnknown) {
    return 0.0;
  }
  return GetTimingConstants(standard).frame_rate_hz;
}

void SectionEditor::SyncDurationSecondsFromFrames() {
  const QSignalBlocker blocker(duration_seconds_spin_);
  duration_seconds_spin_->setValue(
      DurationFramesToSeconds(duration_spin_->value(), ProjectFrameRateHz()));
}

void SectionEditor::OnBrowseSource() {
  // Only reachable in "Local file" mode (Built-in uses a dropdown). Open the
  // dialog at the current source's resolved location, falling back to the
  // project directory.
  QString start;
  if (!source_edit_->text().trimmed().isEmpty()) {
    start = QString::fromStdString(videosynth::ResolveAssetPath(
        SourceFromWidgets(), GuiAssetRoots(), ProjectBaseDir().toStdString(),
        /*anchor_unset=*/true));
  }
  if (start.isEmpty()) {
    start = ProjectBaseDir();
  }
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Section Source"), start,
      tr("Progressive sources (*.mkv *.mp4 *.exr *.MKV *.MP4 *.EXR);;All "
         "files (*)"));
  if (path.isEmpty()) {
    return;
  }

  // Keep the path project-relative when requested and the file actually sits
  // under the project directory; otherwise store an absolute path and untick
  // the checkbox so the stored form matches what the user sees.
  if (source_relative_check_->isChecked()) {
    const QString relative = QDir(ProjectBaseDir()).relativeFilePath(path);
    if (!relative.startsWith(QStringLiteral(".."))) {
      source_edit_->setText(relative);
    } else {
      source_edit_->setText(path);
      // Blocked so the checkbox's own handler does not commit the new file
      // ahead of the source-selection commit below.
      const QSignalBlocker blocker(source_relative_check_);
      source_relative_check_->setChecked(false);
    }
  } else {
    source_edit_->setText(path);
  }
  CommitSourceSelection();
  RequestProbe();
  UpdateSourceResolvedHint();
}

std::string SectionEditor::SourceFromWidgets() const {
  SourceSelection selection;
  selection.builtin =
      source_mode_combo_->currentData().toString() == QLatin1String("builtin");
  selection.type = builtin_type_combo_->currentData().toString().toStdString();
  selection.file = builtin_file_combo_->currentText().trimmed().toStdString();
  selection.relative = source_relative_check_->isChecked();
  selection.text = source_edit_->text().trimmed().toStdString();
  return ComposeSource(selection, ProjectBundledRaster().toStdString());
}

void SectionEditor::LoadSourceWidgets(const std::string& source) {
  const SourceSelection selection = ParseSourceSelection(source);

  if (selection.builtin) {
    source_mode_combo_->setCurrentIndex(
        source_mode_combo_->findData(QStringLiteral("builtin")));
    source_stack_->setCurrentIndex(source_mode_combo_->currentIndex());
    const int type_index =
        builtin_type_combo_->findData(QString::fromStdString(selection.type));
    builtin_type_combo_->setCurrentIndex(type_index >= 0 ? type_index : 0);
    // The raster is re-derived from the current project, so a stored raster
    // that no longer matches the standard is corrected on the next save.
    PopulateBuiltinFiles(QString::fromStdString(selection.file));
    UpdateSourceResolvedHint();
    return;
  }

  source_mode_combo_->setCurrentIndex(
      source_mode_combo_->findData(QStringLiteral("own")));
  source_stack_->setCurrentIndex(source_mode_combo_->currentIndex());
  // Keep the built-in dropdown current for when the user switches to it.
  PopulateBuiltinFiles(QString());
  source_relative_check_->setChecked(selection.relative);
  source_edit_->setText(QString::fromStdString(selection.text));
  UpdateSourceResolvedHint();
}

QString SectionEditor::ProjectBundledRaster() const {
  return QString::fromStdString(
      BundledRaster(document_->project().cvbs_presets.video_standard_preset));
}

void SectionEditor::PopulateBuiltinFiles(const QString& keep_file) {
  const QString type = builtin_type_combo_->currentData().toString();
  const QString root = "{bundled}/" + type + "/" + ProjectBundledRaster();
  const QString dir = QString::fromStdString(videosynth::ResolveAssetPath(
      root.toStdString(), GuiAssetRoots(), ProjectBaseDir().toStdString(),
      /*anchor_unset=*/true));

  const QSignalBlocker blocker(builtin_file_combo_);
  builtin_file_combo_->clear();
  builtin_file_combo_->addItems(
      QDir(dir).entryList({"*." + type}, QDir::Files, QDir::Name));

  // Preserve a stored filename even when it is missing on disk (or belongs to
  // a raster the project no longer targets) so saving never silently drops it.
  if (!keep_file.isEmpty()) {
    if (builtin_file_combo_->findText(keep_file) < 0) {
      builtin_file_combo_->insertItem(0, keep_file);
    }
    builtin_file_combo_->setCurrentIndex(
        builtin_file_combo_->findText(keep_file));
  }
}

void SectionEditor::UpdateSourceResolvedHint() {
  const std::string resolved = videosynth::ResolveAssetPath(
      SourceFromWidgets(), GuiAssetRoots(), ProjectBaseDir().toStdString(),
      /*anchor_unset=*/true);
  source_resolved_hint_->setText(
      resolved.empty() ? QString()
                       : tr("→ %1").arg(QString::fromStdString(resolved)));
}

QString SectionEditor::ProjectBaseDir() const {
  return document_->file_path().isEmpty()
             ? QDir::currentPath()
             : QFileInfo(document_->file_path()).absolutePath();
}

void SectionEditor::OnAddOverlay() {
  if (section_index_ < 0) {
    return;
  }
  Section section = SectionFromWidgets();
  section.osd.overlays.push_back(MakeDefaultOsdOverlay());
  CommitSectionToDocument(section);
  LoadFromDocument();
}

void SectionEditor::OnRemoveOverlay() {
  if (section_index_ < 0) {
    return;
  }
  const int row = osd_table_->currentRow();
  Section section = SectionFromWidgets();
  if (row < 0 || row >= static_cast<int>(section.osd.overlays.size())) {
    return;
  }
  section.osd.overlays.erase(section.osd.overlays.begin() + row);
  CommitSectionToDocument(section);
  LoadFromDocument();
}

}  // namespace videosynth::gui
