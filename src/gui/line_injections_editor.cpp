/*
 * File:        line_injections_editor.cpp
 * Module:      gui
 * Purpose:     Editor for a section's laserdisc biphase code injections
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "line_injections_editor.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "form_field_width.h"
#include "line_injection_presenter.h"
#include "programme_status_dialog.h"

namespace videosynth::gui {

namespace {

// Returns the single optional parameter carried by the code, as display text.
QString CodeValueText(const Section::LineInjectionCode& code) {
  if (CodeTypeUsesStartValue(code.code_type) && code.start_value_specified) {
    return QString::number(code.start_value);
  }
  if (CodeTypeUsesChapter(code.code_type) && code.chapter_specified) {
    return QString::number(code.chapter);
  }
  if (CodeTypeUsesProgrammeStatus(code.code_type) &&
      code.programme_status_specified) {
    return QString::fromStdString(code.programme_status);
  }
  if (CodeTypeUsesUsersCode(code.code_type) && code.users_code_specified) {
    return QString::fromStdString(code.users_code);
  }
  return {};
}

// Placeholder describing the value a code carries, or empty for value-less
// codes (lead_in/lead_out/picture_stop/clv_code/…).
QString CodeValuePlaceholder(const std::string& code_type) {
  if (CodeTypeUsesStartValue(code_type)) {
    return QStringLiteral("start_value (blank = continue)");
  }
  if (CodeTypeUsesChapter(code_type)) {
    return QStringLiteral("chapter 0–79 (blank = continue)");
  }
  if (CodeTypeUsesProgrammeStatus(code_type)) {
    return QStringLiteral("hex status (e.g. 0x8DC000)");
  }
  if (CodeTypeUsesUsersCode(code_type)) {
    return QStringLiteral("hex code (e.g. 0x80D000)");
  }
  return {};
}

bool CodeTypeTakesValue(const std::string& code_type) {
  return !CodeValuePlaceholder(code_type).isEmpty();
}

// Writes `text` into the code's parameter field for its code type; an empty
// text clears the *_specified flag so the emitter drops the key.
void ApplyCodeValue(Section::LineInjectionCode* code, const QString& text) {
  const QString trimmed = text.trimmed();
  code->start_value = 0;
  code->start_value_specified = false;
  code->chapter = 0;
  code->chapter_specified = false;
  code->programme_status.clear();
  code->programme_status_specified = false;
  code->users_code.clear();
  code->users_code_specified = false;

  if (trimmed.isEmpty()) {
    return;
  }
  if (CodeTypeUsesStartValue(code->code_type)) {
    bool ok = false;
    const int value = trimmed.toInt(&ok);
    if (ok) {
      code->start_value = value;
      code->start_value_specified = true;
    }
  } else if (CodeTypeUsesChapter(code->code_type)) {
    bool ok = false;
    const int value = trimmed.toInt(&ok);
    if (ok) {
      code->chapter = value;
      code->chapter_specified = true;
    }
  } else if (CodeTypeUsesProgrammeStatus(code->code_type)) {
    code->programme_status = trimmed.toStdString();
    code->programme_status_specified = true;
  } else if (CodeTypeUsesUsersCode(code->code_type)) {
    code->users_code = trimmed.toStdString();
    code->users_code_specified = true;
  }
}

// Finds the code of `code_type` within an injection, or nullptr.
const Section::LineInjectionCode* FindCode(
    const Section::LineInjection& injection, const std::string& code_type) {
  for (const Section::LineInjectionCode& code : injection.codes) {
    if (code.code_type == code_type) {
      return &code;
    }
  }
  return nullptr;
}

}  // namespace

LineInjectionsEditor::LineInjectionsEditor(QWidget* parent) : QWidget(parent) {
  // A section carries a single laserdisc injection, so the editor is the code
  // checklist: the disc format (CAV/CLV) is a project-wide setting shown here
  // read-only (edit it in project settings) above the ticks.
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  disc_type_label_ = new QLabel(this);
  disc_type_label_->setWordWrap(true);
  layout->addWidget(disc_type_label_);

  empty_hint_label_ = new QLabel(this);
  empty_hint_label_->setWordWrap(true);
  empty_hint_label_->setEnabled(false);  // muted help text
  layout->addWidget(empty_hint_label_);

  auto* checklist_host = new QWidget(this);
  checklist_layout_ = new QGridLayout(checklist_host);
  checklist_layout_->setContentsMargins(0, 0, 0, 0);
  // Stretch an empty trailing column so the checkbox, value, and configure
  // button pack together on the left.
  checklist_layout_->setColumnStretch(3, 1);
  layout->addWidget(checklist_host);
  layout->addStretch();

  LoadInjectionForm();
}

void LineInjectionsEditor::SetContext(Standard standard,
                                      SectionType section_type,
                                      DiscType disc_type) {
  standard_ = standard;
  section_type_ = section_type;
  disc_type_ = disc_type;
  LoadInjectionForm();
}

void LineInjectionsEditor::SetBatchSectionTypes(
    std::vector<SectionType> section_types) {
  if (batch_section_types_ == section_types) {
    return;
  }
  batch_section_types_ = std::move(section_types);
  LoadInjectionForm();
}

void LineInjectionsEditor::SetInjections(
    std::vector<Section::LineInjection> injections) {
  // Only one laserdisc injection per section is meaningful (the runtime uses
  // the first and ignores the rest), so collapse any legacy list to a single
  // working injection rather than surfacing entries that would never generate.
  injections_.clear();
  if (!injections.empty()) {
    Section::LineInjection working = std::move(injections.front());
    working.type = "laserdisc";
    injections_.push_back(std::move(working));
  }
  LoadInjectionForm();
}

Section::LineInjection* LineInjectionsEditor::CurrentInjection() {
  return injections_.empty() ? nullptr : &injections_.front();
}

void LineInjectionsEditor::ClearChecklist() {
  code_rows_.clear();
  QLayoutItem* item = nullptr;
  while ((item = checklist_layout_->takeAt(0)) != nullptr) {
    delete item->widget();
    delete item;
  }
}

void LineInjectionsEditor::LoadInjectionForm() {
  updating_ = true;

  if (disc_type_ == DiscType::kUnknown) {
    disc_type_label_->setText(
        tr("Set the disc format (CAV/CLV) in the project's LaserDisc settings "
           "before adding laserdisc codes."));
  } else {
    disc_type_label_->setText(
        tr("Disc format: %1 (project-wide — change it in the project's "
           "LaserDisc settings).")
            .arg(QString::fromStdString(DiscTypeToString(disc_type_))));
  }

  RebuildCodeChecklist();
  updating_ = false;
}

void LineInjectionsEditor::RebuildCodeChecklist() {
  ClearChecklist();

  Section::LineInjection* injection = CurrentInjection();
  const std::vector<std::string> own_code_types =
      AvailableLaserdiscCodeTypes(disc_type_, section_type_, standard_);

  if (own_code_types.empty()) {
    empty_hint_label_->setText(
        disc_type_ == DiscType::kUnknown
            ? QString()
            : tr("Set this section's disc area (lead-in / programme area / "
                 "lead-out) to choose its laserdisc codes."));
    return;
  }

  // Keep the working copy in step with the section's own catalogue: drop any
  // code that is no longer valid for this disc format / section type (e.g.
  // after the section type changed) so the checklist and model never diverge.
  if (injection != nullptr) {
    auto& codes = injection->codes;
    codes.erase(std::remove_if(
                    codes.begin(), codes.end(),
                    [&own_code_types](const Section::LineInjectionCode& code) {
                      return std::find(own_code_types.begin(),
                                       own_code_types.end(),
                                       code.code_type) == own_code_types.end();
                    }),
                codes.end());
  }

  // A batch selection mirrors ticked codes onto every selected section, so
  // only the codes valid for all of the selected section types are offered
  // (the section's other codes keep their state but grow no rows).
  const bool batch_editing = !batch_section_types_.empty();
  const std::vector<std::string> code_types =
      batch_editing ? CommonLaserdiscCodeTypes(disc_type_, batch_section_types_,
                                               standard_)
                    : own_code_types;
  if (code_types.empty()) {
    empty_hint_label_->setText(
        tr("The selected sections' disc areas share no laserdisc "
           "codes, so none can be batch-edited; select the sections "
           "individually to edit their codes."));
    return;
  }
  empty_hint_label_->setText(
      batch_editing
          ? tr("Tick the laserdisc codes the selected sections carry. Only "
               "codes valid for every selected section's type are shown.")
          : tr("Tick the laserdisc codes this section carries. The codes "
               "normally expected for its section type are ticked by "
               "default."));

  int row = 0;
  for (const std::string& code_type : code_types) {
    const Section::LineInjectionCode* existing =
        injection != nullptr ? FindCode(*injection, code_type) : nullptr;
    const bool ticked = existing != nullptr;
    const QString help = QString::fromStdString(CodeTypeHelp(code_type));

    auto* check = new QCheckBox(QString::fromStdString(code_type));
    check->setChecked(ticked);
    check->setToolTip(help);
    connect(check, &QCheckBox::toggled, this, [this] { OnChecklistChanged(); });
    checklist_layout_->addWidget(check, row, 0);

    QLineEdit* value = nullptr;
    QToolButton* configure = nullptr;
    if (CodeTypeTakesValue(code_type)) {
      value = new QLineEdit();
      value->setText(existing != nullptr ? CodeValueText(*existing)
                                         : QString());
      value->setPlaceholderText(CodeValuePlaceholder(code_type));
      value->setToolTip(help);
      value->setEnabled(ticked);
      // Wide enough for the placeholder hint; the values themselves (chapter
      // numbers, hex words) are much shorter.
      CapFieldWidthToText(value, value->placeholderText());
      connect(value, &QLineEdit::editingFinished, this,
              [this] { OnChecklistChanged(); });
      // Left-aligned so rows with a narrower value edit do not centre it
      // within the column set by the widest one.
      checklist_layout_->addWidget(value, row, 1, Qt::AlignLeft);

      // The programme status word is composed of Amendment 2 flag fields, so
      // offer a picker dialog beside the raw hex edit.
      if (CodeTypeUsesProgrammeStatus(code_type)) {
        configure = new QToolButton();
        configure->setText(QStringLiteral("…"));
        configure->setToolTip(
            tr("Configure the programme status flags (IEC 60856/60857 "
               "Amendment 2)."));
        configure->setEnabled(ticked);
        connect(configure, &QToolButton::clicked, this,
                [this, value] { OnConfigureProgrammeStatus(value); });
        checklist_layout_->addWidget(configure, row, 2);
      }
    }

    code_rows_.push_back({code_type, check, value, configure});
    ++row;
  }
}

void LineInjectionsEditor::OnChecklistChanged() {
  if (updating_) {
    return;
  }

  // Ensure a working injection exists once anything is ticked.
  Section::LineInjection* injection = CurrentInjection();
  if (injection == nullptr) {
    Section::LineInjection created;
    created.type = "laserdisc";
    injections_.push_back(std::move(created));
    injection = &injections_.front();
  }

  // Rebuild in the section's own catalogue order. A batch selection narrows
  // the shown rows to the codes common to every selected section type, so a
  // code without a row keeps its current state rather than being dropped.
  std::vector<Section::LineInjectionCode> new_codes;
  for (const std::string& code_type :
       AvailableLaserdiscCodeTypes(disc_type_, section_type_, standard_)) {
    const auto row_it = std::find_if(code_rows_.begin(), code_rows_.end(),
                                     [&code_type](const CodeRow& code_row) {
                                       return code_row.code_type == code_type;
                                     });
    if (row_it == code_rows_.end()) {
      if (const Section::LineInjectionCode* existing =
              FindCode(*injection, code_type)) {
        new_codes.push_back(*existing);
      }
      continue;
    }
    const CodeRow& code_row = *row_it;
    if (code_row.value != nullptr) {
      code_row.value->setEnabled(code_row.check->isChecked());
    }
    if (code_row.configure != nullptr) {
      code_row.configure->setEnabled(code_row.check->isChecked());
    }
    if (!code_row.check->isChecked()) {
      continue;
    }
    Section::LineInjectionCode code;
    code.code_type = code_row.code_type;
    if (code_row.value != nullptr) {
      ApplyCodeValue(&code, code_row.value->text());
    }
    new_codes.push_back(std::move(code));
  }

  if (new_codes != injection->codes) {
    injection->codes = std::move(new_codes);
    AnnounceEdit();
  }
}

void LineInjectionsEditor::OnConfigureProgrammeStatus(QLineEdit* value_edit) {
  // Seed the flag picker from the current hex text; on accept write the
  // composed word back and commit through the normal checklist path.
  ProgrammeStatusDialog dialog(standard_, value_edit->text(), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  value_edit->setText(dialog.hex_text());
  OnChecklistChanged();
}

void LineInjectionsEditor::AnnounceEdit() {
  if (!updating_) {
    emit InjectionsEdited();
  }
}

void LineInjectionsEditor::AddDefaultInjection() {
  if (!injections_.empty()) {
    return;
  }
  Section::LineInjection injection;
  injection.type = "laserdisc";
  for (const std::string& code_type :
       RecommendedLaserdiscCodeTypes(disc_type_, section_type_, standard_)) {
    Section::LineInjectionCode code;
    code.code_type = code_type;
    injection.codes.push_back(std::move(code));
  }
  injections_.push_back(std::move(injection));
  LoadInjectionForm();
  AnnounceEdit();
}

}  // namespace videosynth::gui
