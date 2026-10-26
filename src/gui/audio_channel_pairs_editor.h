/*
 * File:        audio_channel_pairs_editor.h
 * Module:      gui
 * Purpose:     Editor for a section's audio channel pairs (up to eight stereo
 *              pairs, each carrying test tones, an audio file or the section
 *              video's soundtrack)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QString>
#include <QWidget>
#include <string>
#include <vector>

#include "videosynth/interfaces.h"
#include "videosynth/model.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace videosynth::gui {

// Edits a working copy of a section's audio channel-pair list. Each pair names
// a channel-pair number 0–7 and a content kind: test tones, an audio file, or
// the section video's soundtrack.
//
// Tones carry left/right descriptors; an unchecked channel is stored silent
// (all zeros) per the CVBS File Format Specification (Audio Data). They are
// edited either as one tone applied to both channels (the default) or as
// independent left/right tones; the mode is purely a GUI convenience inferred
// from the model (left == right → linked), so the stored YAML format is
// unchanged. An audio file has a path (optionally project-relative) and a
// repeat flag; a video soundtrack names one of the source's audio streams,
// listed from the latest source probe (SetSourceContext).
//
// The owner reads back channel_pairs() and commits after every
// ChannelPairsEdited signal.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class AudioChannelPairsEditor : public QWidget {
  Q_OBJECT

 public:
  explicit AudioChannelPairsEditor(QWidget* parent = nullptr);

  // Replaces the working copy (no ChannelPairsEdited emission).
  void SetChannelPairs(std::vector<AudioChannelPair> pairs);

  const std::vector<AudioChannelPair>& channel_pairs() const { return pairs_; }

  // Directory that project-relative audio file paths are stored against, and
  // where the file browser opens by default.
  void SetProjectBaseDir(const QString& base_dir);

  // The section's source and its latest probe (probe_ok false while a probe
  // is pending or has failed), which decide the soundtrack streams offered.
  void SetSourceContext(const std::string& source, bool probe_ok,
                        const ProgressiveFrameSourceProfile& profile);

 signals:
  // The working copy changed through user interaction.
  void ChannelPairsEdited();

 private:
  // The widgets making up one channel (left or right) tone sub-editor.
  struct ChannelWidgets {
    QGroupBox* group = nullptr;  // checkable == channel active
    QComboBox* waveform = nullptr;
    QDoubleSpinBox* frequency = nullptr;
    QDoubleSpinBox* amplitude = nullptr;
    QGroupBox* ramp_group = nullptr;  // checkable == ramp enabled
    QDoubleSpinBox* ramp_start = nullptr;
    QDoubleSpinBox* ramp_end = nullptr;
    QComboBox* ramp_mode = nullptr;
    QDoubleSpinBox* ramp_period = nullptr;
  };

  QWidget* BuildChannelEditor(const QString& title, ChannelWidgets* widgets);
  // Retitles the left editor and shows/hides the right editor for the mode.
  void ApplyChannelMode(bool independent);
  void OnChannelModeChanged(int index);
  void RebuildList(int select_row);
  void LoadForm();
  void CommitForm();
  AudioChannelPair* Current();

  void LoadChannel(const ChannelWidgets& widgets, const AudioParameters& audio);
  AudioParameters ChannelFromWidgets(const ChannelWidgets& widgets) const;

  void OnAdd();
  void OnRemove();
  void OnContentChanged(int index);
  void OnBrowseAudioFile();
  // Shows the panel for the current content kind and refreshes the
  // soundtrack stream list and availability.
  void UpdateContentWidgets(const AudioChannelPair* channel_pair);
  // The stored file path for the path widgets, keeping the loaded path
  // verbatim while the widgets still show it.
  std::string FilePathFromWidgets() const;

  QString PairSummary(const AudioChannelPair& channel_pair) const;

  std::vector<AudioChannelPair> pairs_;
  bool updating_ = false;

  QListWidget* pair_list_ = nullptr;
  QPushButton* add_button_ = nullptr;
  QPushButton* remove_button_ = nullptr;
  QWidget* form_panel_ = nullptr;
  QSpinBox* pair_spin_ = nullptr;
  QLineEdit* description_edit_ = nullptr;
  QComboBox* content_combo_ = nullptr;
  QStackedWidget* content_stack_ = nullptr;
  QComboBox* channel_mode_ = nullptr;
  ChannelWidgets left_;
  ChannelWidgets right_;
  QLineEdit* file_path_edit_ = nullptr;
  QCheckBox* file_relative_check_ = nullptr;
  QCheckBox* file_repeat_check_ = nullptr;
  QComboBox* video_stream_combo_ = nullptr;
  QLabel* video_stream_note_ = nullptr;

  QString base_dir_;
  std::string source_;
  bool probe_ok_ = false;
  ProgressiveFrameSourceProfile profile_;
  // The file path as loaded into the widgets, so an edit elsewhere in the
  // form does not rewrite an untouched path into another spelling.
  std::string loaded_file_path_;
  QString loaded_file_text_;
  bool loaded_file_relative_ = false;
};

}  // namespace videosynth::gui
