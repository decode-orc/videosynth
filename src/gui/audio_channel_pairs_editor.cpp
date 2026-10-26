/*
 * File:        audio_channel_pairs_editor.cpp
 * Module:      gui
 * Purpose:     Editor for a section's audio channel pairs (up to eight stereo
 *              pairs, each carrying test tones, an audio file or the section
 *              video's soundtrack)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "audio_channel_pairs_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "asset_roots.h"
#include "form_field_width.h"
#include "section_block_presenters.h"
#include "source_path_model.h"
#include "videosynth/path_resolution.h"
#include "videosynth/progressive_frame_source.h"

namespace videosynth::gui {

namespace {

// Indices of the channel-mode combo box entries.
constexpr int kChannelModeLinked = 0;       // One tone on both channels.
constexpr int kChannelModeIndependent = 1;  // Separate left/right tones.

// Content combo rows and content-stack pages, in AudioPairContent order.
constexpr int kContentTones = 0;
constexpr int kContentFile = 1;
constexpr int kContentVideo = 2;

int ContentRow(AudioPairContent content) {
  switch (content) {
    case AudioPairContent::kFile:
      return kContentFile;
    case AudioPairContent::kVideo:
      return kContentVideo;
    case AudioPairContent::kTones:
      break;
  }
  return kContentTones;
}

AudioPairContent ContentForRow(int row) {
  if (row == kContentFile) {
    return AudioPairContent::kFile;
  }
  if (row == kContentVideo) {
    return AudioPairContent::kVideo;
  }
  return AudioPairContent::kTones;
}

}  // namespace

AudioChannelPairsEditor::AudioChannelPairsEditor(QWidget* parent)
    : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  // Left: channel-pair list with add/remove.
  auto* list_panel = new QVBoxLayout();
  pair_list_ = new QListWidget(this);
  pair_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_panel->addWidget(pair_list_);

  auto* list_buttons = new QHBoxLayout();
  add_button_ = new QPushButton(tr("Add pair"), this);
  remove_button_ = new QPushButton(tr("Remove pair"), this);
  list_buttons->addWidget(add_button_);
  list_buttons->addWidget(remove_button_);
  list_buttons->addStretch();
  list_panel->addLayout(list_buttons);
  layout->addLayout(list_panel, 1);

  // Right: per-pair form.
  form_panel_ = new QWidget(this);
  auto* form_layout = new QVBoxLayout(form_panel_);
  form_layout->setContentsMargins(0, 0, 0, 0);

  auto* header_form = new QFormLayout();
  pair_spin_ = new QSpinBox(form_panel_);
  pair_spin_->setRange(0, kMaxAudioChannelPairs - 1);
  pair_spin_->setToolTip(
      tr("Channel-pair number 0–7 (names the _audio_<pair>.wav file)."));
  CapFieldWidthAtSizeHint(pair_spin_);
  header_form->addRow(tr("Channel pair:"), pair_spin_);
  description_edit_ = new QLineEdit(form_panel_);
  description_edit_->setPlaceholderText(tr("e.g. Analogue stereo"));
  // Twice the default hint so longer descriptions stay readable; the platform
  // form layout may otherwise keep the field at its narrow size hint.
  description_edit_->setMinimumWidth(description_edit_->sizeHint().width() * 2);
  header_form->addRow(tr("Description:"), description_edit_);
  content_combo_ = new QComboBox(form_panel_);
  content_combo_->addItem(tr("Test tones"));
  content_combo_->addItem(tr("Audio file"));
  content_combo_->addItem(tr("Video soundtrack"));
  content_combo_->setToolTip(
      tr("Test tones are synthesised; an audio file (WAV or FLAC) plays from "
         "its start in each section run; the video soundtrack follows the "
         "source frames the section shows."));
  CapFieldWidthAtSizeHint(content_combo_);
  header_form->addRow(tr("Content:"), content_combo_);
  form_layout->addLayout(header_form);

  content_stack_ = new QStackedWidget(form_panel_);

  // Page: test tones.
  auto* tones_page = new QWidget(content_stack_);
  auto* tones_layout = new QVBoxLayout(tones_page);
  tones_layout->setContentsMargins(0, 0, 0, 0);
  auto* mode_form = new QFormLayout();
  channel_mode_ = new QComboBox(tones_page);
  channel_mode_->addItem(tr("Same tone on both channels"));
  channel_mode_->addItem(tr("Independent left and right"));
  channel_mode_->setToolTip(
      tr("With one tone the left settings apply to both channels; independent "
         "mode edits the left and right tones separately."));
  mode_form->addRow(tr("Channels:"), channel_mode_);
  tones_layout->addLayout(mode_form);
  tones_layout->addWidget(
      BuildChannelEditor(tr("Tone (both channels)"), &left_));
  tones_layout->addWidget(BuildChannelEditor(tr("Right channel"), &right_));
  right_.group->setVisible(false);
  tones_layout->addStretch();
  content_stack_->addWidget(tones_page);

  // Page: audio file.
  auto* file_page = new QWidget(content_stack_);
  auto* file_form = new QFormLayout(file_page);
  file_form->setContentsMargins(0, 0, 0, 0);
  auto* path_row = new QHBoxLayout();
  file_path_edit_ = new QLineEdit(file_page);
  file_path_edit_->setPlaceholderText(tr("e.g. narration.flac"));
  auto* browse = new QPushButton(tr("Browse…"), file_page);
  path_row->addWidget(file_path_edit_, 1);
  path_row->addWidget(browse);
  file_form->addRow(tr("File:"), path_row);
  file_relative_check_ = new QCheckBox(tr("Relative to project"), file_page);
  file_relative_check_->setToolTip(
      tr("Store the path relative to the project file so the project stays "
         "portable."));
  file_form->addRow(QString(), file_relative_check_);
  file_repeat_check_ = new QCheckBox(tr("Repeat"), file_page);
  file_repeat_check_->setToolTip(
      tr("Restart the file when it ends; otherwise the rest of the section is "
         "silent."));
  file_form->addRow(QString(), file_repeat_check_);
  content_stack_->addWidget(file_page);

  // Page: video soundtrack.
  auto* video_page = new QWidget(content_stack_);
  auto* video_form = new QFormLayout(video_page);
  video_form->setContentsMargins(0, 0, 0, 0);
  video_stream_combo_ = new QComboBox(video_page);
  video_stream_combo_->setToolTip(
      tr("Audio stream of the section's video. Mono plays on both channels; "
         "more than two channels are downmixed to stereo."));
  video_form->addRow(tr("Stream:"), video_stream_combo_);
  video_stream_note_ = new QLabel(video_page);
  video_stream_note_->setWordWrap(true);
  video_form->addRow(video_stream_note_);
  content_stack_->addWidget(video_page);

  form_layout->addWidget(content_stack_);
  form_layout->addStretch();
  layout->addWidget(form_panel_, 2);

  const auto commit = [this] { CommitForm(); };
  connect(add_button_, &QPushButton::clicked, this,
          &AudioChannelPairsEditor::OnAdd);
  connect(remove_button_, &QPushButton::clicked, this,
          &AudioChannelPairsEditor::OnRemove);
  connect(pair_list_, &QListWidget::currentRowChanged, this,
          [this](int) { LoadForm(); });
  connect(pair_spin_, &QSpinBox::valueChanged, this,
          [commit](int) { commit(); });
  connect(description_edit_, &QLineEdit::editingFinished, this, commit);
  // activated (not currentIndexChanged) so programmatic LoadForm updates do
  // not re-enter the mode switch.
  connect(channel_mode_, &QComboBox::activated, this,
          &AudioChannelPairsEditor::OnChannelModeChanged);
  connect(content_combo_, &QComboBox::activated, this,
          &AudioChannelPairsEditor::OnContentChanged);
  connect(file_path_edit_, &QLineEdit::editingFinished, this, commit);
  connect(file_relative_check_, &QCheckBox::toggled, this,
          [commit](bool) { commit(); });
  connect(file_repeat_check_, &QCheckBox::toggled, this,
          [commit](bool) { commit(); });
  connect(browse, &QPushButton::clicked, this,
          &AudioChannelPairsEditor::OnBrowseAudioFile);
  connect(video_stream_combo_, &QComboBox::activated, this,
          [commit](int) { commit(); });

  LoadForm();
}

QWidget* AudioChannelPairsEditor::BuildChannelEditor(const QString& title,
                                                     ChannelWidgets* widgets) {
  const auto commit = [this] { CommitForm(); };

  auto* group = new QGroupBox(title, form_panel_);
  group->setCheckable(true);
  widgets->group = group;
  auto* form = new QFormLayout(group);

  widgets->waveform = new QComboBox(group);
  for (const std::string& waveform : AudioWaveformOptions()) {
    widgets->waveform->addItem(QString::fromStdString(waveform));
  }
  form->addRow(tr("Waveform:"), widgets->waveform);

  widgets->frequency = new QDoubleSpinBox(group);
  widgets->frequency->setRange(editor_limits::kAudioFrequencyMinHz,
                               editor_limits::kAudioFrequencyMaxHz);
  widgets->frequency->setSuffix(tr(" Hz"));
  widgets->frequency->setDecimals(1);
  widgets->frequency->setValue(1000.0);
  CapFieldWidthAtSizeHint(widgets->frequency);
  form->addRow(tr("Frequency:"), widgets->frequency);

  widgets->amplitude = new QDoubleSpinBox(group);
  widgets->amplitude->setRange(editor_limits::kAudioAmplitudeMin,
                               editor_limits::kAudioAmplitudeMax);
  widgets->amplitude->setSingleStep(0.05);
  widgets->amplitude->setDecimals(2);
  widgets->amplitude->setValue(0.5);
  CapFieldWidthAtSizeHint(widgets->amplitude);
  form->addRow(tr("Amplitude:"), widgets->amplitude);

  widgets->ramp_group = new QGroupBox(tr("Frequency ramp"), group);
  widgets->ramp_group->setCheckable(true);
  widgets->ramp_group->setChecked(false);
  auto* ramp_form = new QFormLayout(widgets->ramp_group);
  widgets->ramp_start = new QDoubleSpinBox(widgets->ramp_group);
  widgets->ramp_start->setRange(editor_limits::kAudioFrequencyMinHz,
                                editor_limits::kAudioFrequencyMaxHz);
  widgets->ramp_start->setSuffix(tr(" Hz"));
  widgets->ramp_start->setDecimals(1);
  widgets->ramp_end = new QDoubleSpinBox(widgets->ramp_group);
  widgets->ramp_end->setRange(editor_limits::kAudioFrequencyMinHz,
                              editor_limits::kAudioFrequencyMaxHz);
  widgets->ramp_end->setSuffix(tr(" Hz"));
  widgets->ramp_end->setDecimals(1);
  widgets->ramp_mode = new QComboBox(widgets->ramp_group);
  for (const std::string& mode : AudioRampModeOptions()) {
    widgets->ramp_mode->addItem(QString::fromStdString(mode));
  }
  widgets->ramp_period = new QDoubleSpinBox(widgets->ramp_group);
  widgets->ramp_period->setRange(0.0, 86400.0);
  widgets->ramp_period->setDecimals(3);
  widgets->ramp_period->setSuffix(tr(" s"));
  widgets->ramp_period->setSpecialValueText(tr("whole section"));
  CapFieldWidthAtSizeHint(widgets->ramp_start);
  CapFieldWidthAtSizeHint(widgets->ramp_end);
  CapFieldWidthAtSizeHint(widgets->ramp_period);
  ramp_form->addRow(tr("Start:"), widgets->ramp_start);
  ramp_form->addRow(tr("End:"), widgets->ramp_end);
  ramp_form->addRow(tr("Mode:"), widgets->ramp_mode);
  ramp_form->addRow(tr("Period:"), widgets->ramp_period);
  form->addRow(widgets->ramp_group);

  connect(group, &QGroupBox::toggled, this, [commit](bool) { commit(); });
  connect(widgets->ramp_group, &QGroupBox::toggled, this,
          [this, widgets](bool checked) {
            if (updating_) {
              return;
            }
            if (checked && widgets->ramp_start->value() == 0.0 &&
                widgets->ramp_end->value() == 0.0) {
              // Seed an audible sweep instead of a degenerate 0→0 Hz ramp.
              updating_ = true;
              widgets->ramp_start->setValue(200.0);
              widgets->ramp_end->setValue(4000.0);
              updating_ = false;
            }
            CommitForm();
          });
  connect(widgets->waveform, &QComboBox::activated, this,
          [commit](int) { commit(); });
  connect(widgets->frequency, &QDoubleSpinBox::valueChanged, this,
          [commit](double) { commit(); });
  connect(widgets->amplitude, &QDoubleSpinBox::valueChanged, this,
          [commit](double) { commit(); });
  connect(widgets->ramp_start, &QDoubleSpinBox::valueChanged, this,
          [commit](double) { commit(); });
  connect(widgets->ramp_end, &QDoubleSpinBox::valueChanged, this,
          [commit](double) { commit(); });
  connect(widgets->ramp_mode, &QComboBox::activated, this,
          [commit](int) { commit(); });
  connect(widgets->ramp_period, &QDoubleSpinBox::valueChanged, this,
          [commit](double) { commit(); });
  return group;
}

void AudioChannelPairsEditor::ApplyChannelMode(bool independent) {
  left_.group->setTitle(independent ? tr("Left channel")
                                    : tr("Tone (both channels)"));
  right_.group->setVisible(independent);
}

void AudioChannelPairsEditor::OnChannelModeChanged(int index) {
  const bool independent = index == kChannelModeIndependent;
  if (independent) {
    // Seed the right channel from the left so splitting starts from a copy
    // instead of forcing the user to re-enter matching settings.
    updating_ = true;
    LoadChannel(right_, ChannelFromWidgets(left_));
    updating_ = false;
  }
  ApplyChannelMode(independent);
  CommitForm();
}

void AudioChannelPairsEditor::SetChannelPairs(
    std::vector<AudioChannelPair> pairs) {
  // Preserve the selected row across a refresh. Editing a channel round-trips
  // through the owning section editor, which calls this to reload; resetting to
  // row 0 would yank the user off the pair they are editing. Pairs never
  // reorder on edit, so the row index stays valid (clamped for a shorter list).
  const int previous_row = pair_list_->currentRow();
  pairs_ = std::move(pairs);
  const int select_row =
      pairs_.empty()
          ? -1
          : qBound(0, previous_row, static_cast<int>(pairs_.size()) - 1);
  RebuildList(select_row);
}

void AudioChannelPairsEditor::RebuildList(int select_row) {
  updating_ = true;
  pair_list_->clear();
  for (const AudioChannelPair& channel_pair : pairs_) {
    pair_list_->addItem(PairSummary(channel_pair));
  }
  updating_ = false;
  pair_list_->setCurrentRow(qBound(-1, select_row, pair_list_->count() - 1));
  LoadForm();
}

AudioChannelPair* AudioChannelPairsEditor::Current() {
  const int row = pair_list_->currentRow();
  if (row < 0 || row >= static_cast<int>(pairs_.size())) {
    return nullptr;
  }
  return &pairs_[static_cast<std::size_t>(row)];
}

void AudioChannelPairsEditor::LoadChannel(const ChannelWidgets& widgets,
                                          const AudioParameters& audio) {
  widgets.group->setChecked(audio.enabled);
  widgets.waveform->setCurrentIndex(
      qMax(0, widgets.waveform->findText(
                  audio.waveform_text.empty()
                      ? QStringLiteral("sine")
                      : QString::fromStdString(audio.waveform_text))));
  widgets.frequency->setValue(audio.frequency_hz);
  widgets.amplitude->setValue(audio.amplitude);
  widgets.ramp_group->setChecked(audio.ramp_enabled);
  widgets.ramp_start->setValue(audio.ramp_start_hz);
  widgets.ramp_end->setValue(audio.ramp_end_hz);
  widgets.ramp_mode->setCurrentIndex(
      qMax(0, widgets.ramp_mode->findText(
                  audio.ramp_mode_text.empty()
                      ? QStringLiteral("up")
                      : QString::fromStdString(audio.ramp_mode_text))));
  widgets.ramp_period->setValue(audio.ramp_period_seconds);
}

void AudioChannelPairsEditor::LoadForm() {
  updating_ = true;
  AudioChannelPair* channel_pair = Current();
  form_panel_->setEnabled(channel_pair != nullptr);
  remove_button_->setEnabled(channel_pair != nullptr);
  add_button_->setEnabled(static_cast<int>(pairs_.size()) <
                          kMaxAudioChannelPairs);

  if (channel_pair == nullptr) {
    updating_ = false;
    return;
  }

  pair_spin_->setValue(channel_pair->pair);
  description_edit_->setText(QString::fromStdString(channel_pair->description));
  content_combo_->setCurrentIndex(ContentRow(channel_pair->content));

  // A {bundled}/… path is not a "my own file" spelling, so it shows verbatim.
  SourceSelection file_selection =
      ParseSourceSelection(channel_pair->file.path);
  if (file_selection.builtin) {
    file_selection.relative = false;
    file_selection.text = channel_pair->file.path;
  }
  loaded_file_path_ = channel_pair->file.path;
  loaded_file_text_ = QString::fromStdString(file_selection.text);
  loaded_file_relative_ = file_selection.relative;
  file_path_edit_->setText(loaded_file_text_);
  file_relative_check_->setChecked(loaded_file_relative_);
  file_repeat_check_->setChecked(channel_pair->file.repeat);
  UpdateContentWidgets(channel_pair);
  // Linked mode is inferred, not stored: identical left/right descriptors edit
  // as one tone; any difference (including one silent channel) edits as
  // independent channels.
  const bool independent = !(channel_pair->left == channel_pair->right);
  channel_mode_->setCurrentIndex(independent ? kChannelModeIndependent
                                             : kChannelModeLinked);
  ApplyChannelMode(independent);
  LoadChannel(left_, channel_pair->left);
  LoadChannel(right_, channel_pair->right);
  updating_ = false;
}

AudioParameters AudioChannelPairsEditor::ChannelFromWidgets(
    const ChannelWidgets& widgets) const {
  AudioParameters audio;
  if (!widgets.group->isChecked()) {
    return audio;  // Disabled (silent) channel.
  }
  audio.enabled = true;
  const std::string waveform = widgets.waveform->currentText().toStdString();
  audio.waveform = AudioWaveformFromString(waveform);
  audio.waveform_text = waveform;
  audio.frequency_hz = widgets.frequency->value();
  audio.amplitude = widgets.amplitude->value();
  if (widgets.ramp_group->isChecked()) {
    // The ramp replaces the fixed frequency, which is kept for the widgets
    // but not saved.
    audio.ramp_enabled = true;
    audio.ramp_start_hz = widgets.ramp_start->value();
    audio.ramp_end_hz = widgets.ramp_end->value();
    audio.ramp_start_specified = true;
    audio.ramp_end_specified = true;
    const std::string mode = widgets.ramp_mode->currentText().toStdString();
    audio.ramp_mode = AudioRampModeFromString(mode);
    audio.ramp_mode_text = mode;
    audio.ramp_period_seconds = widgets.ramp_period->value();
  } else {
    audio.frequency_specified = true;
  }
  return audio;
}

void AudioChannelPairsEditor::CommitForm() {
  if (updating_) {
    return;
  }
  AudioChannelPair* channel_pair = Current();
  if (channel_pair == nullptr) {
    return;
  }
  channel_pair->pair = pair_spin_->value();
  channel_pair->pair_specified = true;
  channel_pair->description = description_edit_->text().toStdString();
  // Only the shown kind's fields are read back; the others stay as
  // SetAudioPairContent left them.
  switch (channel_pair->content) {
    case AudioPairContent::kTones:
      channel_pair->left = ChannelFromWidgets(left_);
      channel_pair->right =
          channel_mode_->currentIndex() == kChannelModeIndependent
              ? ChannelFromWidgets(right_)
              : channel_pair->left;
      break;
    case AudioPairContent::kFile:
      channel_pair->file.path = FilePathFromWidgets();
      channel_pair->file.repeat = file_repeat_check_->isChecked();
      break;
    case AudioPairContent::kVideo:
      channel_pair->video_audio.stream =
          std::max(0, video_stream_combo_->currentIndex());
      break;
  }

  if (QListWidgetItem* item = pair_list_->currentItem()) {
    item->setText(PairSummary(*channel_pair));
  }
  emit ChannelPairsEdited();
}

void AudioChannelPairsEditor::OnAdd() {
  const int next = NextFreeAudioChannelPair(pairs_);
  if (next < 0) {
    return;  // All eight channel pairs already in use.
  }
  pairs_.push_back(MakeDefaultAudioChannelPair(next));
  RebuildList(static_cast<int>(pairs_.size()) - 1);
  emit ChannelPairsEdited();
}

void AudioChannelPairsEditor::OnRemove() {
  const int row = pair_list_->currentRow();
  if (row < 0 || row >= static_cast<int>(pairs_.size())) {
    return;
  }
  pairs_.erase(pairs_.begin() + row);
  RebuildList(qMin(row, static_cast<int>(pairs_.size()) - 1));
  emit ChannelPairsEdited();
}

void AudioChannelPairsEditor::SetProjectBaseDir(const QString& base_dir) {
  base_dir_ = base_dir;
}

void AudioChannelPairsEditor::SetSourceContext(
    const std::string& source, bool probe_ok,
    const ProgressiveFrameSourceProfile& profile) {
  source_ = source;
  probe_ok_ = probe_ok;
  profile_ = profile;
  const bool was_updating = updating_;
  updating_ = true;
  UpdateContentWidgets(Current());
  updating_ = was_updating;
}

void AudioChannelPairsEditor::UpdateContentWidgets(
    const AudioChannelPair* channel_pair) {
  const AudioPairContent content = channel_pair != nullptr
                                       ? channel_pair->content
                                       : AudioPairContent::kTones;
  content_stack_->setCurrentIndex(ContentRow(content));

  const int stored_stream =
      channel_pair != nullptr ? channel_pair->video_audio.stream : 0;
  const VideoSoundtrackEditorState state = VideoSoundtrackEditorStateFor(
      source_, probe_ok_, profile_, stored_stream);
  // A pair already on the soundtrack keeps showing it, so the validator can
  // explain what is wrong instead of the choice silently vanishing.
  if (auto* model =
          qobject_cast<QStandardItemModel*>(content_combo_->model())) {
    if (QStandardItem* item = model->item(kContentVideo)) {
      item->setEnabled(state.selectable || content == AudioPairContent::kVideo);
    }
  }

  video_stream_combo_->clear();
  for (const std::string& label : state.stream_labels) {
    video_stream_combo_->addItem(QString::fromStdString(label));
  }
  video_stream_combo_->setCurrentIndex(
      qMin(stored_stream, video_stream_combo_->count() - 1));
  video_stream_combo_->setEnabled(state.selectable);

  QString note;
  if (!IsProgressiveVideoSource(source_)) {
    note = tr("The section source is not a video, so it has no soundtrack.");
  } else if (!probe_ok_) {
    note =
        tr("The source's audio streams are listed once it has been "
           "probed.");
  } else if (!state.selectable) {
    note = tr("The section's video has no audio streams.");
  }
  video_stream_note_->setText(note);
  video_stream_note_->setVisible(!note.isEmpty());
}

std::string AudioChannelPairsEditor::FilePathFromWidgets() const {
  QString text = file_path_edit_->text().trimmed();
  bool relative = file_relative_check_->isChecked();
  if (text == loaded_file_text_ && relative == loaded_file_relative_) {
    return loaded_file_path_;
  }
  if (relative && QDir::isAbsolutePath(text)) {
    // A typed absolute path is stored relative only when it lies under the
    // project directory.
    const QString under_project = QDir(base_dir_).relativeFilePath(text);
    if (under_project.startsWith(QStringLiteral(".."))) {
      relative = false;
    } else {
      text = under_project;
    }
  }
  SourceSelection selection;
  selection.relative = relative;
  selection.text = text.toStdString();
  return ComposeSource(selection, std::string());
}

void AudioChannelPairsEditor::OnContentChanged(int index) {
  AudioChannelPair* channel_pair = Current();
  if (updating_ || channel_pair == nullptr) {
    return;
  }
  SetAudioPairContent(channel_pair, ContentForRow(index));
  LoadForm();
  if (QListWidgetItem* item = pair_list_->currentItem()) {
    item->setText(PairSummary(*channel_pair));
  }
  emit ChannelPairsEdited();
}

void AudioChannelPairsEditor::OnBrowseAudioFile() {
  AudioChannelPair* channel_pair = Current();
  if (channel_pair == nullptr) {
    return;
  }
  QString start;
  if (!channel_pair->file.path.empty()) {
    start = QFileInfo(QString::fromStdString(videosynth::ResolveAssetPath(
                          channel_pair->file.path, GuiAssetRoots(),
                          base_dir_.toStdString(), /*anchor_unset=*/true)))
                .absolutePath();
  }
  if (start.isEmpty()) {
    start = base_dir_;
  }
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Audio File"), start,
      tr("Audio files (*.wav *.flac *.WAV *.FLAC);;All files (*)"));
  if (path.isEmpty()) {
    return;
  }

  // Keep the path project-relative when requested and the file sits under
  // the project directory; otherwise store it absolute and untick the box so
  // the stored form matches what the user sees.
  updating_ = true;
  if (file_relative_check_->isChecked()) {
    const QString relative = QDir(base_dir_).relativeFilePath(path);
    if (!relative.startsWith(QStringLiteral(".."))) {
      file_path_edit_->setText(relative);
    } else {
      file_path_edit_->setText(path);
      file_relative_check_->setChecked(false);
    }
  } else {
    file_path_edit_->setText(path);
  }
  updating_ = false;
  CommitForm();
}

QString AudioChannelPairsEditor::PairSummary(
    const AudioChannelPair& channel_pair) const {
  QString summary = tr("Pair %1").arg(channel_pair.pair);
  if (!channel_pair.description.empty()) {
    summary += QStringLiteral(" — %1").arg(
        QString::fromStdString(channel_pair.description));
  }
  if (channel_pair.content == AudioPairContent::kFile) {
    const QString name =
        QFileInfo(QString::fromStdString(channel_pair.file.path)).fileName();
    return summary +
           QStringLiteral(" [%1]").arg(name.isEmpty() ? tr("no file") : name);
  }
  if (channel_pair.content == AudioPairContent::kVideo) {
    return summary +
           QStringLiteral(" [%1]").arg(
               tr("soundtrack %1").arg(channel_pair.video_audio.stream));
  }
  QString channels;
  if (channel_pair.left.enabled) {
    channels += QStringLiteral("L");
  }
  if (channel_pair.right.enabled) {
    channels += QStringLiteral("R");
  }
  summary +=
      QStringLiteral(" [%1]").arg(channels.isEmpty() ? tr("silent") : channels);
  return summary;
}

}  // namespace videosynth::gui
