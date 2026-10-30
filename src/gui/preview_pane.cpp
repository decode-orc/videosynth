/*
 * File:        preview_pane.cpp
 * Module:      gui
 * Purpose:     Dockable signal preview: frame navigator, source and encoded
 *              picture views, and a separately mountable line scope panel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_pane.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "asset_roots.h"
#include "generation_controller.h"
#include "preview_render.h"
#include "videosynth/path_resolution.h"
#include "waveform_mapping.h"

namespace videosynth::gui {

namespace {

constexpr int kDefaultRefreshDebounceMsec = 400;

// Wraps a picture view in a resizable scroll area so fixed zooms can pan.
QScrollArea* WrapInScrollArea(PictureViewWidget* view, QWidget* parent) {
  auto* scroll = new QScrollArea(parent);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(view);
  return scroll;
}

}  // namespace

PreviewPane::PreviewPane(ProjectDocument* document,
                         ThemeController* theme_controller, QWidget* parent)
    : QWidget(parent),
      document_(document),
      theme_controller_(theme_controller),
      service_(new PreviewFrameService(
          {}, PreviewFrameService::kDefaultCacheCapacity, this)) {
  BuildUi();

  refresh_timer_.setSingleShot(true);
  refresh_timer_.setInterval(kDefaultRefreshDebounceMsec);
  connect(&refresh_timer_, &QTimer::timeout, this,
          &PreviewPane::OnRefreshTimeout);

  connect(document_, &ProjectDocument::DocumentChanged, this,
          &PreviewPane::OnDocumentChanged);
  connect(document_, &ProjectDocument::DocumentReset, this,
          &PreviewPane::OnDocumentChanged);

  connect(service_, &PreviewFrameService::ScheduleInfoChanged, this,
          &PreviewPane::OnScheduleInfoChanged);
  connect(service_, &PreviewFrameService::FrameReady, this,
          &PreviewPane::OnFrameReady);
  connect(service_, &PreviewFrameService::PreviewFailed, this,
          &PreviewPane::OnPreviewFailed);

  scope_->SetDarkTheme(theme_controller_->is_dark());
  connect(theme_controller_, &ThemeController::ThemeChanged, this,
          [this](bool is_dark) { scope_->SetDarkTheme(is_dark); });
}

void PreviewPane::SetDebounceInterval(int msec) {
  refresh_timer_.setInterval(msec);
}

void PreviewPane::BuildUi() {
  auto* layout = new QVBoxLayout(this);

  // Navigator row.
  auto* navigator_layout = new QHBoxLayout();
  navigator_layout->addWidget(new QLabel(tr("Frame:"), this));
  frame_slider_ = new QSlider(Qt::Horizontal, this);
  frame_slider_->setRange(0, 0);
  navigator_layout->addWidget(frame_slider_, 1);
  frame_spinbox_ = new QSpinBox(this);
  frame_spinbox_->setRange(0, 0);
  navigator_layout->addWidget(frame_spinbox_);
  frame_total_label_ = new QLabel(tr("of 0"), this);
  navigator_layout->addWidget(frame_total_label_);
  layout->addLayout(navigator_layout);

  connect(frame_slider_, &QSlider::valueChanged, frame_spinbox_,
          &QSpinBox::setValue);
  connect(frame_spinbox_, &QSpinBox::valueChanged, frame_slider_,
          &QSlider::setValue);
  connect(frame_slider_, &QSlider::valueChanged, this,
          &PreviewPane::OnFrameIndexChanged);

  // Picture views.
  view_tabs_ = new QTabWidget(this);

  auto* source_page = new QWidget(view_tabs_);
  auto* source_layout = new QVBoxLayout(source_page);
  source_layout->setContentsMargins(0, 0, 0, 0);
  source_view_ = new PictureViewWidget(source_page);
  source_placeholder_ =
      new QLabel(tr("No decoded source frame available."), source_page);
  source_placeholder_->setAlignment(Qt::AlignCenter);
  auto* source_stack = new QStackedLayout();
  source_stack->addWidget(source_view_);
  source_stack->addWidget(source_placeholder_);
  source_layout->addLayout(source_stack);
  view_tabs_->addTab(source_page, tr("Source"));

  auto* encoded_page = new QWidget(view_tabs_);
  auto* encoded_layout = new QVBoxLayout(encoded_page);
  encoded_layout->setContentsMargins(0, 0, 0, 0);
  auto* encoded_controls = new QHBoxLayout();
  encoded_controls->addWidget(new QLabel(tr("Mode:"), encoded_page));
  encoded_mode_combo_ = new QComboBox(encoded_page);
  encoded_mode_combo_->addItem(tr("Composite"));
  encoded_mode_combo_->addItem(tr("Y"));
  encoded_mode_combo_->addItem(tr("C"));
  encoded_controls->addWidget(encoded_mode_combo_);
  encoded_controls->addWidget(new QLabel(tr("Zoom:"), encoded_page));
  zoom_combo_ = new QComboBox(encoded_page);
  zoom_combo_->addItem(tr("Fit"));
  zoom_combo_->addItem(tr("100%"));
  zoom_combo_->addItem(tr("200%"));
  encoded_controls->addWidget(zoom_combo_);
  encoded_controls->addStretch();
  encoded_layout->addLayout(encoded_controls);

  encoded_view_ = new PictureViewWidget(encoded_page);
  encoded_view_->SetCrosshairEnabled(true);
  encoded_layout->addWidget(WrapInScrollArea(encoded_view_, encoded_page), 1);
  view_tabs_->addTab(encoded_page, tr("Encoded"));

  // The live views and a "Loading…" placeholder share the picture area; the
  // placeholder is shown while a synthesis request is in flight so a slow
  // re-render never leaves a stale frame on screen.
  view_stack_ = new QStackedWidget(this);
  loading_placeholder_ = new QLabel(tr("Loading preview…"), view_stack_);
  loading_placeholder_->setAlignment(Qt::AlignCenter);
  view_stack_->addWidget(view_tabs_);
  view_stack_->addWidget(loading_placeholder_);
  layout->addWidget(view_stack_, 1);

  // The encoded signal is the primary inspection surface; open on it.
  view_tabs_->setCurrentWidget(encoded_page);

  connect(encoded_mode_combo_, &QComboBox::currentIndexChanged, this,
          &PreviewPane::OnEncodedModeChanged);
  connect(
      zoom_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto mode = index == 1 ? PictureViewWidget::ZoomMode::k100Percent
                          : index == 2
                              ? PictureViewWidget::ZoomMode::k200Percent
                              : PictureViewWidget::ZoomMode::kFit;
        encoded_view_->SetZoomMode(mode);
      });
  connect(encoded_view_, &PictureViewWidget::RowClicked, this,
          &PreviewPane::OnEncodedRowClicked);

  frame_info_label_ = new QLabel(this);
  layout->addWidget(frame_info_label_);

  BuildScopePanel();
}

// The scope lives in its own panel so the host can dock it separately from the
// picture views; it stays wired to this pane's frame data and line selection.
void PreviewPane::BuildScopePanel() {
  scope_panel_ = new QWidget(this);
  scope_panel_->installEventFilter(this);
  auto* scope_layout = new QVBoxLayout(scope_panel_);
  scope_layout->setContentsMargins(0, 0, 0, 0);

  auto* scope_controls = new QHBoxLayout();
  scope_controls->addWidget(new QLabel(tr("Line:"), scope_panel_));
  line_spinbox_ = new QSpinBox(scope_panel_);
  line_spinbox_->setRange(1, 1);
  scope_controls->addWidget(line_spinbox_);
  scope_controls->addWidget(new QLabel(tr("Trace:"), scope_panel_));
  trace_combo_ = new QComboBox(scope_panel_);
  trace_combo_->addItem(tr("Composite"));
  trace_combo_->addItem(tr("Y"));
  trace_combo_->addItem(tr("C"));
  trace_combo_->addItem(tr("Y+C overlay"));
  scope_controls->addWidget(trace_combo_);
  scope_controls->addWidget(new QLabel(tr("Range:"), scope_panel_));
  range_combo_ = new QComboBox(scope_panel_);
  range_combo_->addItem(tr("Standard"));
  range_combo_->addItem(tr("Sub-sync"));
  range_combo_->addItem(tr("Blanking detail"));
  range_combo_->addItem(tr("Fit to trace"));
  range_combo_->setToolTip(
      tr("Vertical range. Sub-sync lowers the floor so excursions below sync "
         "tip (such as the PAL pilot burst) stay on screen."));
  scope_controls->addWidget(range_combo_);
  scope_controls->addStretch();
  scope_layout->addLayout(scope_controls);

  scope_ = new WaveformScopeWidget(scope_panel_);
  scope_layout->addWidget(scope_, 1);
  readout_label_ = new QLabel(scope_panel_);
  scope_layout->addWidget(readout_label_);

  connect(line_spinbox_, &QSpinBox::valueChanged, this,
          &PreviewPane::OnScopeLineChanged);
  connect(
      trace_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        switch (index) {
          case 1:
            scope_->SetTraceMode(WaveformScopeWidget::TraceMode::kLuma);
            break;
          case 2:
            scope_->SetTraceMode(WaveformScopeWidget::TraceMode::kChroma);
            break;
          case 3:
            scope_->SetTraceMode(WaveformScopeWidget::TraceMode::kOverlay);
            break;
          default:
            scope_->SetTraceMode(WaveformScopeWidget::TraceMode::kComposite);
            break;
        }
      });
  connect(range_combo_, &QComboBox::currentIndexChanged, this,
          [this](int index) {
            switch (index) {
              case 1:
                scope_->SetRangeMode(PlotRangeMode::kSubSync);
                break;
              case 2:
                scope_->SetRangeMode(PlotRangeMode::kBlankingDetail);
                break;
              case 3:
                scope_->SetRangeMode(PlotRangeMode::kFit);
                break;
              default:
                scope_->SetRangeMode(PlotRangeMode::kStandard);
                break;
            }
          });
  connect(scope_, &WaveformScopeWidget::CursorMoved, this,
          &PreviewPane::OnCursorMoved);
}

void PreviewPane::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (needs_refresh_) {
    refresh_timer_.start();
  }
}

bool PreviewPane::eventFilter(QObject* watched, QEvent* event) {
  if (watched == scope_panel_ && event->type() == QEvent::Show &&
      needs_refresh_) {
    refresh_timer_.start();
  }
  return QWidget::eventFilter(watched, event);
}

bool PreviewPane::IsAnyPanelVisible() const {
  return isVisible() || (scope_panel_ != nullptr && scope_panel_->isVisible());
}

void PreviewPane::OnDocumentChanged() {
  needs_refresh_ = true;
  if (current_frame_ != nullptr) {
    SetStatusMessage(tr("Preview is out of date — updating…"));
  }
  if (IsAnyPanelVisible()) {
    refresh_timer_.start();
  }
}

void PreviewPane::OnRefreshTimeout() { PushProjectToService(); }

void PreviewPane::PushProjectToService() {
  needs_refresh_ = false;

  // Relative paths are anchored to the project file's directory because
  // synthesis executes on a worker thread (matches OnGenerate).
  const QString base_dir =
      document_->file_path().isEmpty()
          ? QDir::currentPath()
          : QFileInfo(document_->file_path()).absolutePath();
  service_->SetProject(videosynth::ResolveProjectPaths(
      document_->project(), GuiAssetRoots(), base_dir.toStdString(),
      /*anchor_unset=*/true));
  // A project edit clears the cache, so this always re-synthesises from
  // scratch: show the placeholder so a slow re-render (e.g. a new source
  // asset) never leaves a misleading stale frame up. Frame stepping goes
  // straight through RequestCurrentFrame and deliberately keeps the current
  // frame on screen so scrubbing the navigator never flickers.
  SetLoadingVisible(true);
  RequestCurrentFrame();
}

void PreviewPane::RequestCurrentFrame() {
  service_->RequestFrame(static_cast<std::size_t>(frame_slider_->value()),
                         CurrentOptions());
}

PreviewOptions PreviewPane::CurrentOptions() const {
  // Noise and dropouts are always applied so the preview matches what the
  // pipeline writes; the injection stages are per-section gated, so sections
  // without degradation stay clean.
  PreviewOptions options;
  options.apply_noise = true;
  options.apply_dropouts = true;
  return options;
}

void PreviewPane::OnScheduleInfoChanged() {
  const std::optional<PreviewScheduleInfo>& info = service_->schedule_info();
  if (!info.has_value()) {
    return;
  }

  // No signal blocking: when the new range clamps the current value, the
  // resulting valueChanged re-requests the (now valid) frame.
  const int last_frame =
      std::max(0, static_cast<int>(info->output_frame_count) - 1);
  frame_slider_->setRange(0, last_frame);
  frame_spinbox_->setRange(0, last_frame);
  frame_total_label_->setText(tr("of %1").arg(info->output_frame_count));
  line_spinbox_->setRange(1, std::max(1, info->lines_per_frame));

  if (pending_section_jump_ >= 0) {
    const int section_index = pending_section_jump_;
    pending_section_jump_ = -1;
    ShowSectionFirstFrame(section_index);
  }
}

void PreviewPane::OnFrameReady(std::shared_ptr<const PreviewFrameData> frame) {
  if (frame == nullptr || frame->revision != service_->revision()) {
    return;
  }
  current_frame_ = std::move(frame);
  SetStatusMessage(QString());
  SetLoadingVisible(false);
  UpdatePictures();
  UpdateScope();
  UpdateFrameInfoLabel();
  // Scrubbing the navigator across a section boundary re-selects that section
  // in the host, so the list and editor always describe the frame on screen.
  if (current_frame_->section_index != reported_section_index_) {
    reported_section_index_ = current_frame_->section_index;
    emit CurrentSectionChanged(reported_section_index_);
  }
}

void PreviewPane::OnPreviewFailed(quint64 revision, const QString& message) {
  if (revision != service_->revision()) {
    return;
  }
  // Reveal whatever we last had (the last good frame, or the empty views) with
  // the reason in the status bar, rather than stranding the "Loading…" text.
  SetLoadingVisible(false);
  if (current_frame_ != nullptr) {
    SetStatusMessage(tr("%1 — showing the last good frame.").arg(message));
  } else {
    SetStatusMessage(tr("Preview unavailable: %1").arg(message));
  }
}

void PreviewPane::OnFrameIndexChanged(int output_frame_index) {
  Q_UNUSED(output_frame_index);
  RequestCurrentFrame();
}

void PreviewPane::OnEncodedModeChanged() {
  // Composite/Y/C switches re-render from the cached buffers; no synthesis is
  // re-run.
  UpdatePictures();
}

void PreviewPane::OnEncodedRowClicked(int row) {
  const std::optional<PreviewScheduleInfo>& info = service_->schedule_info();
  if (!info.has_value() || info->standard == Standard::kUnknown) {
    return;
  }
  line_spinbox_->setValue(FrameRowToLineNumber(info->standard, row));
}

void PreviewPane::OnScopeLineChanged(int line_number) {
  // Keep the picture crosshair in step with the selected line (both when the
  // spinbox is edited directly and when a crosshair drag drives it).
  const std::optional<PreviewScheduleInfo>& info = service_->schedule_info();
  if (info.has_value() && info->standard != Standard::kUnknown) {
    encoded_view_->SetCrosshairRow(
        LineNumberToFrameRow(info->standard, line_number));
  }
  UpdateScope();
}

void PreviewPane::OnCursorMoved(int sample_index, double microseconds,
                                double millivolts) {
  const std::optional<PreviewScheduleInfo>& info = service_->schedule_info();
  QString text = tr("Sample %1 · %2 µs · %3 mV")
                     .arg(sample_index)
                     .arg(microseconds, 0, 'f', 2)
                     .arg(millivolts, 0, 'f', 1);
  // IRE applies to the System M standards (NTSC, PAL-M).
  if (info.has_value() && !Is625LineStandard(info->standard)) {
    text += tr(" · %1 IRE").arg(MillivoltsToIre(millivolts), 0, 'f', 1);
  }
  readout_label_->setText(text);
}

void PreviewPane::UpdatePictures() {
  if (current_frame_ == nullptr) {
    return;
  }
  const std::optional<PreviewScheduleInfo>& info = service_->schedule_info();
  if (!info.has_value() || info->standard == Standard::kUnknown) {
    return;
  }

  // Source view.
  if (current_frame_->has_source_image) {
    const QImage source = RenderSourceImage(current_frame_->source_image);
    source_view_->SetImage(source, false);
    source_view_->setVisible(!source.isNull());
    source_placeholder_->setVisible(source.isNull());
  } else {
    source_view_->SetImage(QImage(), false);
    source_view_->setVisible(false);
    source_placeholder_->setVisible(true);
  }

  // Encoded view: the full woven interlaced frame in the selected channel.
  EncodedImageMode mode = EncodedImageMode::kComposite;
  switch (encoded_mode_combo_->currentIndex()) {
    case 1:
      mode = EncodedImageMode::kLuma;
      break;
    case 2:
      mode = EncodedImageMode::kChroma;
      break;
    default:
      mode = EncodedImageMode::kComposite;
      break;
  }
  encoded_view_->SetImage(
      RenderEncodedFrameImage(current_frame_->y_mv, current_frame_->c_mv,
                              info->standard, mode),
      false);
  encoded_view_->SetCrosshairRow(
      LineNumberToFrameRow(info->standard, line_spinbox_->value()));
}

void PreviewPane::UpdateScope() {
  if (current_frame_ == nullptr) {
    return;
  }
  const std::optional<PreviewScheduleInfo>& info = service_->schedule_info();
  if (!info.has_value() || info->standard == Standard::kUnknown) {
    return;
  }

  const int line = line_spinbox_->value();
  scope_->SetLineData(
      ExtractLineMillivolts(current_frame_->y_mv, info->standard, line),
      ExtractLineMillivolts(current_frame_->c_mv, info->standard, line),
      info->sample_rate_hz, info->levels);
}

void PreviewPane::UpdateFrameInfoLabel() {
  if (current_frame_ == nullptr) {
    frame_info_label_->clear();
    return;
  }
  QString text = tr("Output frame %1").arg(current_frame_->output_frame_index);
  if (!current_frame_->section_name.isEmpty()) {
    text += tr(" · section \"%1\"").arg(current_frame_->section_name);
  }
  const QString cadence = FormatFieldCadence(current_frame_->fields);
  if (!cadence.isEmpty()) {
    text += QStringLiteral(" · ") + cadence;
  }
  frame_info_label_->setText(text);
}

void PreviewPane::ShowSectionFirstFrame(int section_index) {
  const std::optional<PreviewScheduleInfo>& info = service_->schedule_info();
  if (!info.has_value()) {
    pending_section_jump_ = section_index;
    if (needs_refresh_) {
      refresh_timer_.start();
    } else {
      RequestCurrentFrame();
    }
    return;
  }

  if (section_index < 0 || static_cast<std::size_t>(section_index) >=
                               info->section_first_output_frame.size()) {
    return;
  }
  const qint64 first_frame =
      info->section_first_output_frame[static_cast<std::size_t>(section_index)];
  if (first_frame < 0) {
    SetStatusMessage(tr("The selected section contributes no output frames."));
    return;
  }
  if (frame_slider_->value() == static_cast<int>(first_frame)) {
    RequestCurrentFrame();
  } else {
    frame_slider_->setValue(static_cast<int>(first_frame));
  }
}

void PreviewPane::SetStatusMessage(const QString& message) {
  if (message == status_message_) {
    return;
  }
  status_message_ = message;
  emit StatusMessageChanged(message);
}

void PreviewPane::SetLoadingVisible(bool visible) {
  view_stack_->setCurrentWidget(
      visible ? static_cast<QWidget*>(loading_placeholder_)
              : static_cast<QWidget*>(view_tabs_));
}

}  // namespace videosynth::gui
