/*
 * File:        noise_spectrum_editor.h
 * Module:      gui
 * Purpose:     Table editor for a noise layer's spectrum breakpoints
 *              (frequency in MHz, relative level in dB)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QWidget>
#include <vector>

#include "videosynth/model.h"

class QPushButton;
class QTableWidget;

namespace videosynth::gui {

// Breakpoint table with add/remove buttons for one noise layer's spectrum.
// An empty table means white noise. Rows are added and removed through
// AddNoiseSpectrumPoint/RemoveNoiseSpectrumPoint (section_block_presenters),
// and every edit emits SpectrumEdited; the owner reads spectrum() and commits
// it through its ProjectDocument command.
//
// Thread-safety: NOT thread-safe. GUI (main) thread only.
class NoiseSpectrumEditor : public QWidget {
  Q_OBJECT

 public:
  explicit NoiseSpectrumEditor(QWidget* parent = nullptr);

  // Shows `spectrum`, with frequencies limited to [0, max_frequency_mhz].
  // Emits nothing. Rows are rebuilt only when the row count changes, so a
  // reload after an edit never destroys the cell widget being edited.
  void SetSpectrum(const std::vector<NoiseSpectrumPoint>& spectrum,
                   double max_frequency_mhz);
  const std::vector<NoiseSpectrumPoint>& spectrum() const { return spectrum_; }

 signals:
  void SpectrumEdited();

 private:
  void RebuildRows();
  void SyncValues();
  void OnAddPoint();
  void OnRemovePoint();

  std::vector<NoiseSpectrumPoint> spectrum_;
  double max_frequency_mhz_ = 0.0;
  bool updating_ = false;

  QTableWidget* table_ = nullptr;
  QPushButton* remove_button_ = nullptr;
};

}  // namespace videosynth::gui
