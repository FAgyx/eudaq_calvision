#include "CalvisionConfTab.hh"

#include <algorithm>
#include <map>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLayoutItem>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

namespace {
QString defaultConfTemplatePath() {
  return "user/calvision/misc/fers_w_drs.conf";
}

bool parseSectionHeader(const QString &line, QString *section) {
  const QString trimmed = line.trimmed();
  if (!trimmed.startsWith("[") || !trimmed.endsWith("]")) {
    return false;
  }
  if (section) {
    *section = trimmed.mid(1, trimmed.size() - 2).trimmed();
  }
  return true;
}

bool splitConfigLine(const QString &line, QString *key, QString *value) {
  const QString trimmed = line.trimmed();
  if (trimmed.isEmpty() || trimmed.startsWith("#") || trimmed.startsWith(";") ||
      trimmed.startsWith("[")) {
    return false;
  }
  const int eq = trimmed.indexOf('=');
  if (eq < 0) {
    return false;
  }
  const QString parsed_key = trimmed.left(eq).trimmed();
  if (parsed_key.isEmpty()) {
    return false;
  }
  if (key) {
    *key = parsed_key;
  }
  if (value) {
    *value = trimmed.mid(eq + 1).trimmed();
  }
  return true;
}

QString cleanComment(QString line) {
  line = line.trimmed();
  while (line.startsWith("#") || line.startsWith(";")) {
    line = line.mid(1).trimmed();
  }
  return line;
}

bool looksLikeCommentedConfig(const QString &comment) {
  const int eq = comment.indexOf('=');
  if (eq < 0) {
    return false;
  }
  const QString maybe_key = comment.left(eq).trimmed();
  if (maybe_key.isEmpty()) {
    return false;
  }
  for (const QChar ch : maybe_key) {
    if (!ch.isLetterOrNumber() && ch != '_') {
      return false;
    }
  }
  return true;
}

bool allDigits(const QString &text) {
  if (text.isEmpty()) {
    return false;
  }
  for (const QChar ch : text) {
    if (!ch.isDigit()) {
      return false;
    }
  }
  return true;
}

QString deviceFamily(const QString &section) {
  const QString drs_prefix = "Producer.my_drs";
  const QString fers_prefix = "Producer.my_fers";
  if (section.startsWith(drs_prefix) &&
      allDigits(section.mid(drs_prefix.size()))) {
    return drs_prefix + "*";
  }
  if (section.startsWith(fers_prefix) &&
      allDigits(section.mid(fers_prefix.size()))) {
    return fers_prefix + "*";
  }
  return QString();
}

QString sharedKey(const QString &family, const QString &key) {
  return family + "\n" + key;
}

bool hideFromConfTab(const QString &section, const QString &key) {
  return section == "RunControl" &&
         (key == "EUDAQ_CTRL_PRODUCER_LAST_START" ||
          key == "EUDAQ_CTRL_PRODUCER_FIRST_STOP");
}

void appendUnique(QStringList *items, const QString &item) {
  const QString trimmed = item.trimmed();
  if (!trimmed.isEmpty() && !items->contains(trimmed)) {
    *items << trimmed;
  }
}

struct SharedCandidate {
  QString family;
  QString key;
  QString value;
  bool same_value = true;
  std::vector<int> records;
  QStringList tooltips;
  QStringList sections;
};
}

CalvisionConfTab::CalvisionConfTab(QWidget *parent)
    : QWidget(parent),
      m_path(nullptr),
      m_scroll(nullptr),
      m_form_layout(nullptr),
      m_status(nullptr),
      m_loading(false),
      m_dirty(false) {
  setupUi();
  loadSettings();
}

void CalvisionConfTab::setupUi() {
  QVBoxLayout *layout = new QVBoxLayout(this);

  QLabel *intro = new QLabel(
      "Edit active configuration values used by the Devices tab. Commented "
      "lines are skipped and are not written back. Init saves this clean "
      "template first, then generates the temporary /tmp EUDAQ config from it.",
      this);
  intro->setWordWrap(true);
  layout->addWidget(intro);

  QHBoxLayout *path_layout = new QHBoxLayout();
  path_layout->addWidget(new QLabel("Template:", this));
  m_path = new QLineEdit(this);
  path_layout->addWidget(m_path);

  QPushButton *browse = new QPushButton("Browse", this);
  QPushButton *reload = new QPushButton("Reload", this);
  QPushButton *save = new QPushButton("Save", this);
  path_layout->addWidget(browse);
  path_layout->addWidget(reload);
  path_layout->addWidget(save);
  layout->addLayout(path_layout);

  m_scroll = new QScrollArea(this);
  m_scroll->setWidgetResizable(true);
  QWidget *form_widget = new QWidget(m_scroll);
  m_form_layout = new QVBoxLayout(form_widget);
  m_form_layout->setContentsMargins(4, 4, 4, 4);
  m_form_layout->setSpacing(8);
  m_scroll->setWidget(form_widget);
  layout->addWidget(m_scroll, 1);

  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  connect(browse, &QPushButton::clicked, this, [this]() {
    const QString start_dir =
        QFileInfo(templatePath()).exists()
            ? QFileInfo(templatePath()).absolutePath()
            : QDir::currentPath();
    QString file = QFileDialog::getOpenFileName(
        this, "Select config template", start_dir, "Config files (*.conf);;All files (*)");
    if (file.isEmpty()) {
      return;
    }
    setTemplatePath(file);
  });

  connect(reload, &QPushButton::clicked, this, [this]() {
    if (!m_dirty || QMessageBox::question(
            this, "Reload config",
            "Discard unsaved changes and reload the template file?",
            QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
      loadTemplate();
    }
  });

  connect(save, &QPushButton::clicked, this, [this]() {
    saveTemplate();
  });

  connect(m_path, &QLineEdit::editingFinished, this, [this]() {
    const QString normalized = normalizedTemplatePath(m_path->text());
    if (normalized != m_path->text()) {
      m_path->setText(normalized);
    }
    loadTemplate();
    saveSettings();
  });

}

void CalvisionConfTab::loadSettings() {
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/conf");
  QString path = settings.value("templateConfigFile",
                                defaultTemplatePath()).toString();
  settings.endGroup();
  setTemplatePath(path);
}

QString CalvisionConfTab::defaultTemplatePath() const {
  return defaultConfTemplatePath();
}

bool CalvisionConfTab::isGeneratedDeviceConfig(const QString &path) const {
  const QFileInfo info(path);
  return info.fileName().startsWith("eudaq_calvision_devices_") &&
         info.fileName().endsWith(".conf");
}

QString CalvisionConfTab::normalizedTemplatePath(const QString &path) const {
  QString trimmed = path.trimmed();
  if (trimmed.isEmpty() || isGeneratedDeviceConfig(trimmed)) {
    trimmed = defaultTemplatePath();
  }
  return trimmed;
}

QString CalvisionConfTab::templatePath() const {
  return normalizedTemplatePath(m_path ? m_path->text() : QString());
}

void CalvisionConfTab::setTemplatePath(const QString &path) {
  const QString normalized = normalizedTemplatePath(path);
  if (m_path) {
    m_path->setText(normalized);
  }
  loadTemplate();
  saveSettings();
}

bool CalvisionConfTab::loadTemplate() {
  const QString path = templatePath();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    m_loading = true;
    clearForm();
    m_loading = false;
    m_dirty = false;
    setStatus("Could not read template: " + path);
    return false;
  }

  m_loading = true;
  clearForm();
  m_records.clear();

  QTextStream in(&file);
  QString current_section;
  QStringList pending_comments;
  int entry_count = 0;
  while (!in.atEnd()) {
    const QString line = in.readLine();
    const QString trimmed = line.trimmed();
    QString section;
    if (parseSectionHeader(line, &section)) {
      current_section = section;
      pending_comments.clear();
      continue;
    }

    if (trimmed.isEmpty()) {
      pending_comments.clear();
      continue;
    }

    if (trimmed.startsWith("#") || trimmed.startsWith(";")) {
      const QString comment = cleanComment(trimmed);
      if (!comment.isEmpty() && !looksLikeCommentedConfig(comment)) {
        pending_comments << comment;
      }
      continue;
    }

    QString key;
    QString value;
    if (current_section.isEmpty() ||
        !splitConfigLine(line, &key, &value)) {
      pending_comments.clear();
      continue;
    }
    ConfigRecord record;
    record.section = current_section;
    record.key = key;
    record.value = value;
    record.tooltip = pending_comments.join("\n");
    record.hidden = hideFromConfTab(record.section, record.key);
    m_records.push_back(record);
    pending_comments.clear();
    ++entry_count;
  }

  buildForm();

  if (entry_count == 0) {
    auto label = new QLabel("No active KEY = VALUE entries found.", m_scroll);
    label->setWordWrap(true);
    m_form_layout->addWidget(label);
  }
  m_form_layout->addStretch(1);

  m_loading = false;
  m_dirty = false;
  setStatus("Loaded " + QString::number(entry_count) +
            " active config values as " + QString::number(m_entries.size()) +
            " editable fields from: " + QFileInfo(path).absoluteFilePath());
  return true;
}

bool CalvisionConfTab::saveTemplate() {
  if (m_records.empty()) {
    QMessageBox::warning(this, "Conf",
                         "No active config values are loaded; refusing to "
                         "overwrite the template.");
    return false;
  }

  const QString path = templatePath();
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    QMessageBox::warning(this, "Conf",
                         "Could not write config template: " + path);
    return false;
  }

  QTextStream out(&file);
  QString last_section;
  bool wrote_any = false;
  for (const ConfigRecord &record : m_records) {
    if (record.hidden) {
      if (record.section != last_section) {
        if (wrote_any) {
          out << "\n";
        }
        out << "[" << record.section << "]\n";
        last_section = record.section;
      }
      out << record.key << " = " << record.value << "\n";
      wrote_any = true;
      continue;
    }
    if (record.entry_index < 0 ||
        record.entry_index >= static_cast<int>(m_entries.size())) {
      continue;
    }
    const ConfigEntry &entry = m_entries[record.entry_index];
    if (!entry.value) {
      continue;
    }
    if (record.section != last_section) {
      if (wrote_any) {
        out << "\n";
      }
      out << "[" << record.section << "]\n";
      last_section = record.section;
    }
    out << record.key << " = " << entry.value->text().trimmed() << "\n";
    wrote_any = true;
  }
  m_dirty = false;
  setStatus("Saved clean template without comments: " +
            QFileInfo(path).absoluteFilePath());
  saveSettings();
  return true;
}

void CalvisionConfTab::clearForm() {
  m_entries.clear();
  m_section_forms.clear();
  if (!m_form_layout) {
    return;
  }
  QLayoutItem *item = nullptr;
  while ((item = m_form_layout->takeAt(0)) != nullptr) {
    if (QWidget *widget = item->widget()) {
      delete widget;
    }
    delete item;
  }
}

void CalvisionConfTab::buildForm() {
  std::map<QString, SharedCandidate> candidates;
  for (int i = 0; i < static_cast<int>(m_records.size()); ++i) {
    const ConfigRecord &record = m_records[i];
    if (record.hidden) {
      continue;
    }
    const QString family = deviceFamily(record.section);
    if (family.isEmpty()) {
      continue;
    }
    const QString key = sharedKey(family, record.key);
    auto &candidate = candidates[key];
    if (candidate.records.empty()) {
      candidate.family = family;
      candidate.key = record.key;
      candidate.value = record.value;
    } else if (candidate.value != record.value) {
      candidate.same_value = false;
    }
    candidate.records.push_back(i);
    appendUnique(&candidate.tooltips, record.tooltip);
    appendUnique(&candidate.sections, record.section);
  }

  std::map<QString, int> shared_entries;
  for (int i = 0; i < static_cast<int>(m_records.size()); ++i) {
    ConfigRecord &record = m_records[i];
    if (record.hidden) {
      continue;
    }
    const QString family = deviceFamily(record.section);
    const QString key = sharedKey(family, record.key);
    auto candidate = candidates.find(key);
    const bool use_shared =
        candidate != candidates.end() &&
        candidate->second.same_value &&
        candidate->second.records.size() > 1;
    if (use_shared) {
      auto entry = shared_entries.find(key);
      if (entry == shared_entries.end()) {
        QString tooltip = candidate->second.tooltips.join("\n\n");
        const QString applies =
            "Applies to: " + candidate->second.sections.join(", ");
        tooltip = tooltip.isEmpty() ? applies : tooltip + "\n\n" + applies;
        const int entry_index = addConfigEntry(
            candidate->second.family + " (shared)",
            candidate->second.key,
            candidate->second.value,
            tooltip,
            candidate->second.records);
        for (const int record_index : candidate->second.records) {
          m_records[record_index].entry_index = entry_index;
        }
        shared_entries[key] = entry_index;
      }
      continue;
    }

    const int entry_index = addConfigEntry(record.section,
                                          record.key,
                                          record.value,
                                          record.tooltip,
                                          {i});
    record.entry_index = entry_index;
  }
}

QFormLayout *CalvisionConfTab::ensureSection(const QString &section) {
  auto existing = m_section_forms.find(section);
  if (existing != m_section_forms.end()) {
    return existing->second;
  }

  auto group = new QGroupBox(section, m_scroll);
  auto form = new QFormLayout(group);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  group->setLayout(form);
  m_form_layout->addWidget(group);
  m_section_forms[section] = form;
  return form;
}

int CalvisionConfTab::addConfigEntry(const QString &display_section,
                                     const QString &key,
                                     const QString &value,
                                     const QString &tooltip,
                                     const std::vector<int> &records) {
  QFormLayout *form = ensureSection(display_section);
  if (!form) {
    return -1;
  }
  QWidget *parent = form->parentWidget() ? form->parentWidget() : m_scroll;

  auto label = new QLabel(key + " =", parent);
  auto edit = new QLineEdit(value, parent);
  edit->setMinimumWidth(360);
  if (!tooltip.trimmed().isEmpty()) {
    label->setToolTip(tooltip);
    edit->setToolTip(tooltip);
  }
  form->addRow(label, edit);
  m_entries.push_back({display_section, key, edit, records});
  const int entry_index = static_cast<int>(m_entries.size()) - 1;
  connect(edit, &QLineEdit::textChanged, this, [this]() {
    if (m_loading) {
      return;
    }
    m_dirty = true;
    setStatus("Modified. Save before Init or click Save.");
  });
  return entry_index;
}

void CalvisionConfTab::saveSettings() const {
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/conf");
  settings.setValue("templateConfigFile", templatePath());
  settings.endGroup();
}

void CalvisionConfTab::setStatus(const QString &text) {
  if (m_status) {
    m_status->setText(text);
  }
}
