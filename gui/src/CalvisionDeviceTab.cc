#include "CalvisionDeviceTab.hh"

#include "eudaq/RunControl.hh"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextStream>
#include <QThread>
#include <QVBoxLayout>

#include <map>
#include <set>

namespace {
const char *kFersRoModeToolTip =
    "Readout Mode\n"
    "0  Disable sorting\n"
    "1  Enable event sorting by Trigger Tstamp\n"
    "2  Enable event sorting by Trigger ID";

constexpr int kFersColProducer = 0;
constexpr int kFersColLinkType = 1;
constexpr int kFersColPid = 2;
constexpr int kFersColIpAddress = 3;
constexpr int kFersColRoMode = 4;
constexpr int kFersColReadPid = 5;
constexpr int kFersColReadModel = 6;
constexpr int kFersColReadFpga = 7;
constexpr int kFersColReadUc = 8;
constexpr int kFersColumnCount = 9;

QString configQuote(QString value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  return "\"" + value + "\"";
}

QTableWidgetItem *makeTableItem(const QString &text) {
  return new QTableWidgetItem(text);
}

void setTableText(QTableWidget *table, int row, int col, const QString &text) {
  if (!table->item(row, col)) {
    table->setItem(row, col, makeTableItem(text));
  } else {
    table->item(row, col)->setText(text);
  }
}

QString tableText(const QTableWidget *table, int row, int col,
                  const QString &fallback = QString()) {
  auto item = table->item(row, col);
  if (!item) {
    return fallback;
  }
  QString text = item->text().trimmed();
  return text.isEmpty() ? fallback : text;
}

int tableInt(const QTableWidget *table, int row, int col, int fallback) {
  bool ok = false;
  int value = tableText(table, row, col).toInt(&ok);
  return ok ? value : fallback;
}

void setReadOnlyTableText(QTableWidget *table,
                          int row,
                          int col,
                          const QString &text,
                          const QString &tooltip = QString()) {
  setTableText(table, row, col, text);
  if (auto item = table->item(row, col)) {
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setToolTip(tooltip);
  }
}

QString drsLinkType(const QTableWidget *table, int row) {
  auto combo = qobject_cast<QComboBox*>(table->cellWidget(row, 2));
  if (!combo) {
    return "USB";
  }
  QString type = combo->currentText().trimmed().toUpper();
  return type.isEmpty() ? "USB" : type;
}

QString fersLinkType(const QTableWidget *table, int row) {
  auto combo = qobject_cast<QComboBox*>(table->cellWidget(row, kFersColLinkType));
  if (!combo) {
    return "USB";
  }
  QString type = combo->currentText().trimmed().toUpper();
  return type.isEmpty() ? "USB" : type;
}

QString fersIpAddress(const QTableWidget *table, int row) {
  auto edit = qobject_cast<QLineEdit*>(table->cellWidget(row, kFersColIpAddress));
  if (!edit) {
    return QString();
  }
  return edit->text().trimmed();
}

int fersRoMode(const QTableWidget *table, int row) {
  auto combo = qobject_cast<QComboBox*>(table->cellWidget(row, kFersColRoMode));
  if (!combo) {
    return 0;
  }
  bool ok = false;
  int mode = combo->currentData().toInt(&ok);
  return ok ? mode : 0;
}

void updateFersLinkInputs(QTableWidget *table, int row) {
  const bool ethernet = fersLinkType(table, row) == "ETHERNET";
  auto ip_edit = qobject_cast<QLineEdit*>(table->cellWidget(row, kFersColIpAddress));
  if (ip_edit) {
    ip_edit->setEnabled(ethernet);
    ip_edit->setPlaceholderText(ethernet ? "192.168.50.x" : "unused for USB");
  }
}

int defaultFersPid(int row) {
  static const int defaults[] = {62689, 22687};
  return row < static_cast<int>(sizeof(defaults) / sizeof(defaults[0]))
             ? defaults[row]
             : 0;
}

int defaultDrsSerial(int row) {
  static const int defaults[] = {251, 29622, 53365};
  return row < static_cast<int>(sizeof(defaults) / sizeof(defaults[0]))
             ? defaults[row]
             : 0;
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

bool isRemovedDrsGroupSection(const QString &section) {
  return section == "2" || section == "3" || section == "TR1";
}

QString commentBody(const QString &line) {
  const QString trimmed = line.trimmed();
  if (!trimmed.startsWith("#") && !trimmed.startsWith(";")) {
    return QString();
  }
  return trimmed.mid(1).trimmed();
}

QStringList splitWaveDumpTokens(const QString &line) {
  QStringList tokens;
  QString token;
  bool in_quote = false;
  for (int i = 0; i < line.size(); ++i) {
    const QChar ch = line.at(i);
    if (ch == '"') {
      in_quote = !in_quote;
      continue;
    }
    if (ch.isSpace() && !in_quote) {
      if (!token.isEmpty()) {
        tokens << token;
        token.clear();
      }
      continue;
    }
    token += ch;
  }
  if (!token.isEmpty()) {
    tokens << token;
  }
  return tokens;
}

bool parseWaveDumpParamLine(const QString &line,
                            QString *key,
                            QString *value) {
  QString body = line.trimmed();
  if (body.isEmpty() || body.startsWith("#") || body.startsWith(";") ||
      body.startsWith("[")) {
    return false;
  }
  const int hash = body.indexOf('#');
  if (hash >= 0) {
    body = body.left(hash).trimmed();
  }

  int split = -1;
  for (int i = 0; i < body.size(); ++i) {
    if (body.at(i).isSpace()) {
      split = i;
      break;
    }
  }
  if (split <= 0) {
    return false;
  }
  const QString parsed_key = body.left(split).trimmed();
  const QString parsed_value = body.mid(split + 1).trimmed();
  if (parsed_key.isEmpty() || parsed_value.isEmpty()) {
    return false;
  }
  if (key) {
    *key = parsed_key;
  }
  if (value) {
    *value = parsed_value;
  }
  return true;
}

bool parseDrsBoardOverrideComment(const QString &line,
                                  int *board,
                                  QString *section,
                                  QString *key,
                                  QString *value) {
  const QString body = commentBody(line);
  if (!body.startsWith("EUDAQ_DRS_BOARD")) {
    return false;
  }
  const QStringList tokens = splitWaveDumpTokens(body);
  if (tokens.size() < 5 || tokens.at(0) != "EUDAQ_DRS_BOARD") {
    return false;
  }

  bool ok = false;
  const int parsed_board = tokens.at(1).toInt(&ok);
  if (!ok) {
    return false;
  }
  QString parsed_section = tokens.at(2);
  if (parsed_section.startsWith("[") && parsed_section.endsWith("]")) {
    parsed_section = parsed_section.mid(1, parsed_section.size() - 2).trimmed();
  }
  const QString parsed_key = tokens.at(3).trimmed();
  const int value_start = body.indexOf(parsed_key) + parsed_key.size();
  const QString parsed_value = body.mid(value_start).trimmed();
  if (parsed_section.isEmpty() || parsed_key.isEmpty() ||
      parsed_value.isEmpty()) {
    return false;
  }

  if (board) {
    *board = parsed_board;
  }
  if (section) {
    *section = parsed_section;
  }
  if (key) {
    *key = parsed_key;
  }
  if (value) {
    *value = parsed_value;
  }
  return true;
}

QString waveDumpStateKey(const QString &section, const QString &key) {
  return section + "\n" + key;
}

QString formatWaveDumpLine(const QString &key, const QString &value) {
  return key.leftJustified(24, ' ') + value.trimmed();
}

QString configLineKey(const QString &line) {
  const QString trimmed = line.trimmed();
  if (trimmed.isEmpty() || trimmed.startsWith("#") || trimmed.startsWith(";") ||
      trimmed.startsWith("[")) {
    return QString();
  }
  const int eq = trimmed.indexOf('=');
  if (eq < 0) {
    return QString();
  }
  return trimmed.left(eq).trimmed();
}

bool isGeneratedDeviceConfig(const QString &path) {
  const QFileInfo info(path);
  return info.fileName().startsWith("eudaq_calvision_devices_") &&
         info.fileName().endsWith(".conf");
}

QString defaultTemplateConfigPath() {
  return "user/calvision/misc/fers_w_drs.conf";
}

class ConfigTemplate {
public:
  bool load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      return false;
    }
    QTextStream in(&file);
    m_lines.clear();
    while (!in.atEnd()) {
      m_lines << in.readLine();
    }
    return true;
  }

  bool save(const QString &path) const {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
      return false;
    }
    QTextStream out(&file);
    for (const QString &line : m_lines) {
      out << line << "\n";
    }
    return true;
  }

  bool hasSection(const QString &section) const {
    return findSectionStart(section) >= 0;
  }

  QString value(const QString &section,
                const QString &key,
                const QString &fallback = QString()) const {
    const int start = findSectionStart(section);
    if (start < 0) {
      return fallback;
    }
    const int end = findSectionEnd(start);
    for (int i = start + 1; i < end; ++i) {
      if (configLineKey(m_lines[i]) == key) {
        const int eq = m_lines[i].indexOf('=');
        return eq >= 0 ? m_lines[i].mid(eq + 1).trimmed() : fallback;
      }
    }
    return fallback;
  }

  bool copySection(const QString &source_section, const QString &target_section) {
    if (hasSection(target_section)) {
      return true;
    }
    const int source_start = findSectionStart(source_section);
    if (source_start < 0) {
      ensureSection(target_section);
      return false;
    }
    const int source_end = findSectionEnd(source_start);
    appendBlankIfNeeded();
    m_lines << "[" + target_section + "]";
    for (int i = source_start + 1; i < source_end; ++i) {
      m_lines << m_lines[i];
    }
    return true;
  }

  void setValue(const QString &section, const QString &key, const QString &value) {
    int start = findSectionStart(section);
    if (start < 0) {
      ensureSection(section);
      start = findSectionStart(section);
    }

    const int end = findSectionEnd(start);
    for (int i = start + 1; i < end; ++i) {
      if (configLineKey(m_lines[i]) == key) {
        m_lines[i] = key + " = " + value;
        return;
      }
    }
    m_lines.insert(end, key + " = " + value);
  }

private:
  int findSectionStart(const QString &section) const {
    QString current;
    for (int i = 0; i < m_lines.size(); ++i) {
      if (parseSectionHeader(m_lines[i], &current) && current == section) {
        return i;
      }
    }
    return -1;
  }

  int findSectionEnd(int section_start) const {
    for (int i = section_start + 1; i < m_lines.size(); ++i) {
      QString section;
      if (parseSectionHeader(m_lines[i], &section)) {
        return i;
      }
    }
    return m_lines.size();
  }

  void appendBlankIfNeeded() {
    if (!m_lines.isEmpty() && !m_lines.last().trimmed().isEmpty()) {
      m_lines << "";
    }
  }

  void ensureSection(const QString &section) {
    if (hasSection(section)) {
      return;
    }
    appendBlankIfNeeded();
    m_lines << "[" + section + "]";
  }

  QStringList m_lines;
};

bool toIntValue(const QString &text, int *value) {
  bool ok = false;
  const int parsed = text.trimmed().toInt(&ok);
  if (ok && value) {
    *value = parsed;
  }
  return ok;
}

struct JanusKeyToken {
  QString base;
  int board = -1;
  int channel = -1;
  bool has_board = false;
  bool has_channel = false;
};

bool parseJanusKeyToken(const QString &token, JanusKeyToken *parsed) {
  const int board_start = token.indexOf('[');
  if (board_start < 0) {
    if (parsed) {
      parsed->base = token;
    }
    return true;
  }

  const int board_end = token.indexOf(']', board_start + 1);
  if (board_end < 0) {
    return false;
  }

  bool ok = false;
  const int board =
      token.mid(board_start + 1, board_end - board_start - 1).toInt(&ok);
  if (!ok) {
    return false;
  }

  JanusKeyToken result;
  result.base = token.left(board_start);
  result.board = board;
  result.has_board = true;

  int cursor = board_end + 1;
  if (cursor < token.size()) {
    if (token.at(cursor) != '[') {
      return false;
    }
    const int channel_end = token.indexOf(']', cursor + 1);
    if (channel_end < 0) {
      return false;
    }
    const int channel =
        token.mid(cursor + 1, channel_end - cursor - 1).toInt(&ok);
    if (!ok || channel_end + 1 != token.size()) {
      return false;
    }
    result.channel = channel;
    result.has_channel = true;
  }

  if (parsed) {
    *parsed = result;
  }
  return true;
}

QString firstJanusToken(const QString &line) {
  const QString trimmed = line.trimmed();
  if (trimmed.isEmpty() || trimmed.startsWith("#") || trimmed.startsWith(";")) {
    return QString();
  }
  const int hash = trimmed.indexOf('#');
  const QString body = hash >= 0 ? trimmed.left(hash).trimmed() : trimmed;
  int split = -1;
  for (int i = 0; i < body.size(); ++i) {
    if (body.at(i).isSpace()) {
      split = i;
      break;
    }
  }
  return split < 0 ? body : body.left(split);
}

bool writeMappedFersConfig(const QString &source_path,
                           const QString &target_path,
                           int fers_row,
                           const QString &producer_name) {
  QFile source(source_path);
  if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }
  QFile target(target_path);
  if (!target.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    return false;
  }

  QTextStream in(&source);
  QTextStream out(&target);
  out << "# Generated by euRun Devices tab from " << source_path << "\n";
  out << "# " << producer_name << " uses Janus board [" << fers_row
      << "] overrides, remapped to local FERSlib board [0].\n\n";

  while (!in.atEnd()) {
    QString line = in.readLine();
    const QString token = firstJanusToken(line);
    JanusKeyToken parsed;
    if (!token.isEmpty() && parseJanusKeyToken(token, &parsed) &&
        parsed.has_board) {
      if (parsed.board != fers_row) {
        continue;
      }
      QString mapped = parsed.base + "[0]";
      if (parsed.has_channel) {
        mapped += "[" + QString::number(parsed.channel) + "]";
      }
      const int token_pos = line.indexOf(token);
      if (token_pos >= 0) {
        line.replace(token_pos, token.size(), mapped);
      }
    }
    out << line << "\n";
  }
  return true;
}

bool writeMappedDrsConfig(const QString &source_path,
                          const QString &target_path,
                          int drs_row,
                          const QString &producer_name) {
  QFile source(source_path);
  if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }

  QStringList source_lines;
  QTextStream in(&source);
  while (!in.atEnd()) {
    source_lines << in.readLine();
  }

  std::map<QString, QString> overrides;
  for (const QString &line : source_lines) {
    int board = -1;
    QString section;
    QString key;
    QString value;
    if (parseDrsBoardOverrideComment(line, &board, &section, &key, &value) &&
        board == drs_row && !isRemovedDrsGroupSection(section)) {
      overrides[waveDumpStateKey(section, key)] = value;
    }
  }

  QStringList output;
  output << QString("# Generated by euRun Devices tab from ") + source_path;
  output << QString("# ") + producer_name +
                QString(" uses DRS board override B%1.").arg(drs_row);
  output << "";

  QString current_section = "COMMON";
  std::set<QString> applied;
  std::set<QString> seen_sections;

  auto flushSection = [&](const QString &section) {
    if (section.isEmpty()) {
      return;
    }
    for (const auto &entry : overrides) {
      const QStringList parts = entry.first.split('\n');
      if (parts.size() != 2 || parts.at(0) != section ||
          applied.find(entry.first) != applied.end()) {
        continue;
      }
      output << formatWaveDumpLine(parts.at(1), entry.second);
      applied.insert(entry.first);
    }
  };

  for (const QString &line : source_lines) {
    if (parseDrsBoardOverrideComment(line, nullptr, nullptr, nullptr, nullptr)) {
      continue;
    }

    QString section;
    if (parseSectionHeader(line, &section)) {
      flushSection(current_section);
      current_section = section;
      if (isRemovedDrsGroupSection(current_section)) {
        continue;
      }
      seen_sections.insert(section);
      output << line;
      continue;
    }

    if (isRemovedDrsGroupSection(current_section)) {
      continue;
    }

    QString key;
    QString value;
    if (parseWaveDumpParamLine(line, &key, &value)) {
      const QString id = waveDumpStateKey(current_section, key);
      const auto override_it = overrides.find(id);
      if (override_it != overrides.end()) {
        output << formatWaveDumpLine(key, override_it->second);
        applied.insert(id);
      } else {
        output << line;
      }
      continue;
    }

    output << line;
  }
  flushSection(current_section);

  for (const auto &entry : overrides) {
    if (applied.find(entry.first) != applied.end()) {
      continue;
    }
    const QStringList parts = entry.first.split('\n');
    if (parts.size() != 2) {
      continue;
    }
    const QString section = parts.at(0);
    if (seen_sections.find(section) == seen_sections.end()) {
      output << "";
      output << "[" + section + "]";
      seen_sections.insert(section);
    }
    output << formatWaveDumpLine(parts.at(1), entry.second);
    applied.insert(entry.first);
  }

  QFile target(target_path);
  if (!target.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    return false;
  }
  QTextStream out(&target);
  for (const QString &line : output) {
    out << line << "\n";
  }
  return true;
}

QString producerSection(const QString &prefix, int row) {
  return QString("Producer.%1%2").arg(prefix).arg(row);
}

struct EnsureResult {
  bool created = false;
  int source_row = -1;
};

EnsureResult ensureProducerSection(ConfigTemplate *config,
                                   const QString &prefix,
                                   int row) {
  EnsureResult result;
  const QString section = producerSection(prefix, row);
  if (config->hasSection(section)) {
    return result;
  }

  for (int src = row - 1; src >= 0; --src) {
    const QString source = producerSection(prefix, src);
    if (config->hasSection(source)) {
      config->copySection(source, section);
      result.created = true;
      result.source_row = src;
      return result;
    }
  }
  for (int src = row + 1; src < 32; ++src) {
    const QString source = producerSection(prefix, src);
    if (config->hasSection(source)) {
      config->copySection(source, section);
      result.created = true;
      result.source_row = src;
      return result;
    }
  }

  config->copySection(QString(), section);
  result.created = true;
  return result;
}

void adjustGeneratedPlaneId(ConfigTemplate *config,
                            const QString &section,
                            const QString &key,
                            int row,
                            int source_row,
                            int fallback_base) {
  if (!config || source_row < 0) {
    if (config && config->value(section, key).isEmpty()) {
      config->setValue(section, key, QString::number(fallback_base + row));
    }
    return;
  }

  const QString source_section =
      section.left(section.size() - QString::number(row).size()) +
      QString::number(source_row);
  int source_value = 0;
  if (toIntValue(config->value(source_section, key), &source_value)) {
    config->setValue(section, key,
                     QString::number(source_value + (row - source_row)));
  } else {
    config->setValue(section, key, QString::number(fallback_base + row));
  }
}
}

CalvisionDeviceTab::CalvisionDeviceTab(QWidget *parent)
    : QWidget(parent),
      m_enabled(nullptr),
      m_fers_count(nullptr),
      m_drs_count(nullptr),
      m_fers_table(nullptr),
      m_drs_table(nullptr),
      m_status(nullptr),
      m_loading_settings(false) {
  setupUi();
  loadSettings();
}

CalvisionDeviceTab::~CalvisionDeviceTab() {
  terminateOwnedProcesses();
}

void CalvisionDeviceTab::setupUi() {
  QVBoxLayout *layout = new QVBoxLayout(this);

  QLabel *intro = new QLabel(
      "Define the active Calvision devices here. When enabled, Init writes a "
      "generated init/config pair, starts the selected producer processes, "
      "waits for them to connect, then sends the normal EUDAQ INIT command.",
      this);
  intro->setWordWrap(true);
  layout->addWidget(intro);

  m_enabled = new QCheckBox("Use this Devices tab on Init", this);
  m_enabled->setChecked(true);
  layout->addWidget(m_enabled);

  QHBoxLayout *counts = new QHBoxLayout();
  counts->addWidget(new QLabel("FERS producers:", this));
  m_fers_count = new QSpinBox(this);
  m_fers_count->setRange(0, 16);
  counts->addWidget(m_fers_count);
  counts->addSpacing(20);
  counts->addWidget(new QLabel("DRS producers:", this));
  m_drs_count = new QSpinBox(this);
  m_drs_count->setRange(0, 16);
  counts->addWidget(m_drs_count);
  counts->addStretch();
  layout->addLayout(counts);

  layout->addWidget(new QLabel("FERS / Citiroc", this));
  m_fers_table = new QTableWidget(this);
  m_fers_table->setColumnCount(kFersColumnCount);
  m_fers_table->setHorizontalHeaderLabels(
      {"Producer", "Link Type", "PID", "IP Address", "Readout Mode",
       "Read PID", "Model", "FPGA FW", "uC FW"});
  if (auto item = m_fers_table->horizontalHeaderItem(kFersColRoMode)) {
    item->setToolTip(kFersRoModeToolTip);
  }
  m_fers_table->setToolTip(kFersRoModeToolTip);
  m_fers_table->horizontalHeader()->setStretchLastSection(true);
  m_fers_table->verticalHeader()->setVisible(false);
  layout->addWidget(m_fers_table);

  layout->addWidget(new QLabel("DRS", this));
  m_drs_table = new QTableWidget(this);
  m_drs_table->setColumnCount(7);
  m_drs_table->setHorizontalHeaderLabels(
      {"Producer", "Serial", "Link Type", "Link Num/PID",
       "Node", "Board Offset", "Label"});
  m_drs_table->horizontalHeader()->setStretchLastSection(true);
  m_drs_table->verticalHeader()->setVisible(false);
  layout->addWidget(m_drs_table);

  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  QPushButton *save_button = new QPushButton("Save Device Settings", this);
  layout->addWidget(save_button);

  connect(m_fers_count,
          static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this, [this](int rows) {
            resizeFersRows(rows);
            saveSettings();
          });
  connect(m_drs_count,
          static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this, [this](int rows) {
            resizeDrsRows(rows);
            saveSettings();
          });
  connect(m_enabled, &QCheckBox::toggled,
          this, [this]() { saveSettings(); });
  connect(m_fers_table, &QTableWidget::itemChanged,
          this, [this]() { saveSettings(); });
  connect(m_drs_table, &QTableWidget::itemChanged,
          this, [this]() { saveSettings(); });
  connect(save_button, &QPushButton::clicked,
          this, [this]() {
            saveSettings();
            if (m_status) {
              m_status->setText("Device settings saved.");
            }
          });
}

void CalvisionDeviceTab::resizeFersRows(int rows) {
  const bool old_loading = m_loading_settings;
  m_loading_settings = true;
  m_fers_table->setRowCount(rows);
  for (int row = 0; row < rows; ++row) {
    const QString producer = QString("my_fers%1").arg(row);
    setTableText(m_fers_table, row, kFersColProducer, producer);
    if (auto item = m_fers_table->item(row, kFersColProducer)) {
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    }
    if (!m_fers_table->cellWidget(row, kFersColLinkType)) {
      auto combo = new QComboBox(m_fers_table);
      combo->addItem("USB");
      combo->addItem("Ethernet");
      m_fers_table->setCellWidget(row, kFersColLinkType, combo);
      connect(combo,
              static_cast<void (QComboBox::*)(const QString&)>(
                  &QComboBox::currentTextChanged),
              this, [this, row]() {
                updateFersLinkInputs(m_fers_table, row);
                saveSettings();
              });
    }
    if (!m_fers_table->item(row, kFersColPid)) {
      setTableText(m_fers_table, row, kFersColPid,
                   QString::number(defaultFersPid(row)));
    }
    if (!m_fers_table->cellWidget(row, kFersColIpAddress)) {
      auto ip_edit = new QLineEdit(m_fers_table);
      ip_edit->setPlaceholderText("unused for USB");
      ip_edit->setEnabled(false);
      m_fers_table->setCellWidget(row, kFersColIpAddress, ip_edit);
      connect(ip_edit, &QLineEdit::textChanged,
              this, [this]() { saveSettings(); });
    }
    if (!m_fers_table->cellWidget(row, kFersColRoMode)) {
      auto combo = new QComboBox(m_fers_table);
      combo->addItem("0 - Disable sorting", 0);
      combo->addItem("1 - Sort by Trigger Tstamp", 1);
      combo->addItem("2 - Sort by Trigger ID", 2);
      combo->setToolTip(kFersRoModeToolTip);
      m_fers_table->setCellWidget(row, kFersColRoMode, combo);
      connect(combo,
              static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
              this, [this]() { saveSettings(); });
    }
    setReadOnlyTableText(m_fers_table, row, kFersColReadPid, "");
    setReadOnlyTableText(m_fers_table, row, kFersColReadModel, "");
    setReadOnlyTableText(m_fers_table, row, kFersColReadFpga, "");
    setReadOnlyTableText(m_fers_table, row, kFersColReadUc, "");
    updateFersLinkInputs(m_fers_table, row);
  }
  m_loading_settings = old_loading;
}

void CalvisionDeviceTab::resizeDrsRows(int rows) {
  const bool old_loading = m_loading_settings;
  m_loading_settings = true;
  m_drs_table->setRowCount(rows);
  for (int row = 0; row < rows; ++row) {
    const QString producer = QString("my_drs%1").arg(row);
    setTableText(m_drs_table, row, 0, producer);
    if (auto item = m_drs_table->item(row, 0)) {
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    }
    if (!m_drs_table->item(row, 1)) {
      setTableText(m_drs_table, row, 1,
                   QString::number(defaultDrsSerial(row)));
    }
    if (!m_drs_table->cellWidget(row, 2)) {
      auto combo = new QComboBox(m_drs_table);
      combo->addItem("USB");
      combo->addItem("A4818");
      m_drs_table->setCellWidget(row, 2, combo);
      connect(combo,
              static_cast<void (QComboBox::*)(const QString&)>(
                  &QComboBox::currentTextChanged),
              this, [this]() { saveSettings(); });
    }
    if (!m_drs_table->item(row, 3)) {
      setTableText(m_drs_table, row, 3, QString::number(row));
    }
    if (!m_drs_table->item(row, 4)) {
      setTableText(m_drs_table, row, 4, QString::number(row));
    }
    if (!m_drs_table->item(row, 5)) {
      setTableText(m_drs_table, row, 5, QString::number(row));
    }
    if (!m_drs_table->item(row, 6)) {
      setTableText(m_drs_table, row, 6,
                   row == 0 ? "USB Digitizer" : QString("DRS %1").arg(row));
    }
  }
  m_loading_settings = old_loading;
}

void CalvisionDeviceTab::loadSettings() {
  m_loading_settings = true;
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/devices");
  m_enabled->setChecked(settings.value("enabled", true).toBool());
  m_fers_count->setValue(settings.value("fersCount", 2).toInt());
  m_drs_count->setValue(settings.value("drsCount", 3).toInt());

  int fers_saved = settings.beginReadArray("fers");
  for (int row = 0; row < m_fers_table->rowCount() && row < fers_saved; ++row) {
    settings.setArrayIndex(row);
    if (auto combo = qobject_cast<QComboBox*>(
            m_fers_table->cellWidget(row, kFersColLinkType))) {
      int index = combo->findText(settings.value("linkType", "USB").toString(),
                                  Qt::MatchFixedString);
      combo->setCurrentIndex(index >= 0 ? index : 0);
    }
    setTableText(m_fers_table, row, kFersColPid,
                 settings.value("pid", defaultFersPid(row)).toString());
    if (auto edit = qobject_cast<QLineEdit*>(
            m_fers_table->cellWidget(row, kFersColIpAddress))) {
      edit->setText(settings.value("ipAddress", "").toString());
    }
    if (auto combo = qobject_cast<QComboBox*>(
            m_fers_table->cellWidget(row, kFersColRoMode))) {
      int mode = settings.value("roMode", 0).toInt();
      int index = combo->findData(mode);
      combo->setCurrentIndex(index >= 0 ? index : 0);
    }
    updateFersLinkInputs(m_fers_table, row);
  }
  settings.endArray();

  int drs_saved = settings.beginReadArray("drs");
  for (int row = 0; row < m_drs_table->rowCount() && row < drs_saved; ++row) {
    settings.setArrayIndex(row);
    setTableText(m_drs_table, row, 1,
                 settings.value("serial", defaultDrsSerial(row)).toString());
    if (auto combo = qobject_cast<QComboBox*>(m_drs_table->cellWidget(row, 2))) {
      int index = combo->findText(settings.value("linkType", "USB").toString(),
                                  Qt::MatchFixedString);
      combo->setCurrentIndex(index >= 0 ? index : 0);
    }
    setTableText(m_drs_table, row, 3,
                 settings.value("linkNum", row).toString());
    setTableText(m_drs_table, row, 4,
                 settings.value("node", row).toString());
    setTableText(m_drs_table, row, 5,
                 settings.value("boardOffset", row).toString());
    setTableText(m_drs_table, row, 6,
                 settings.value("label",
                                tableText(m_drs_table, row, 6)).toString());
  }
  settings.endArray();
  settings.endGroup();
  m_loading_settings = false;
  m_status->setText("Device tab is ready. Init will generate files in /tmp and launch selected producers.");
}

void CalvisionDeviceTab::saveSettings() const {
  if (m_loading_settings) {
    return;
  }
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/devices");
  settings.setValue("enabled", m_enabled->isChecked());
  settings.setValue("fersCount", m_fers_count->value());
  settings.setValue("drsCount", m_drs_count->value());

  settings.beginWriteArray("fers");
  for (int row = 0; row < m_fers_table->rowCount(); ++row) {
    settings.setArrayIndex(row);
    settings.setValue("linkType",
                      fersLinkType(m_fers_table, row) == "ETHERNET"
                          ? "Ethernet"
                          : "USB");
    settings.setValue("pid", tableText(m_fers_table, row, kFersColPid));
    settings.setValue("ipAddress", fersIpAddress(m_fers_table, row));
    settings.setValue("roMode", fersRoMode(m_fers_table, row));
  }
  settings.endArray();

  settings.beginWriteArray("drs");
  for (int row = 0; row < m_drs_table->rowCount(); ++row) {
    settings.setArrayIndex(row);
    settings.setValue("serial", tableText(m_drs_table, row, 1));
    settings.setValue("linkType", drsLinkType(m_drs_table, row));
    settings.setValue("linkNum", tableText(m_drs_table, row, 3));
    settings.setValue("node", tableText(m_drs_table, row, 4));
    settings.setValue("boardOffset", tableText(m_drs_table, row, 5));
    settings.setValue("label", tableText(m_drs_table, row, 6));
  }
  settings.endArray();
  settings.endGroup();
}

QStringList CalvisionDeviceTab::configuredProducerNames() const {
  QStringList names;
  for (int row = 0; row < m_drs_table->rowCount(); ++row) {
    names << QString("my_drs%1").arg(row);
  }
  for (int row = 0; row < m_fers_table->rowCount(); ++row) {
    names << QString("my_fers%1").arg(row);
  }
  return names;
}

QString CalvisionDeviceTab::euCliProducerPath() const {
  const QString app_dir = QCoreApplication::applicationDirPath();
  const QString cwd = QDir::currentPath();
  const QStringList candidates = {
      QDir(app_dir).filePath("euCliProducer"),
      QDir(app_dir).filePath("../bin/euCliProducer"),
      QDir(app_dir).filePath("../main/exe/euCliProducer"),
      QDir(cwd).filePath("bin/euCliProducer"),
      QDir(cwd).filePath("build/main/exe/euCliProducer")
  };
  for (const QString &candidate : candidates) {
    QFileInfo info(candidate);
    if (info.exists() && info.isExecutable()) {
      return info.absoluteFilePath();
    }
  }
  return QString();
}

QString CalvisionDeviceTab::launchLogDir() const {
  QString env_dir = QString::fromLocal8Bit(qgetenv("LAUNCH_LOG_DIR"));
  if (!env_dir.trimmed().isEmpty()) {
    return QDir(env_dir).absolutePath();
  }
  const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
  return QDir(QDir::currentPath()).filePath("logs/device_launch_" + stamp);
}

bool CalvisionDeviceTab::writeGeneratedFiles(const QString &conf_template_path,
                                             const QString &fers_config_path,
                                             const QString &drs_config_path,
                                             QString *init_path,
                                             QString *conf_path) const {
  const int fers_count = m_fers_table->rowCount();
  const int drs_count = m_drs_table->rowCount();
  const QString base =
      QDir::temp().filePath(QString("eudaq_calvision_devices_%1")
                                .arg(QCoreApplication::applicationPid()));
  const QString ini_path = base + ".ini";
  const QString cfg_path = base + ".conf";

  QFile ini(ini_path);
  if (!ini.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    QMessageBox::warning(nullptr, "Devices",
                         "Could not write generated init file: " + ini_path);
    return false;
  }
  QTextStream ini_out(&ini);
  ini_out << "# Generated by euRun Devices tab. Do not edit this temporary file.\n";
  ini_out << "[LogCollector.log]\n";
  ini_out << "EULOG_GUI_LOG_FILE_PATTERN = EULog_$4R_$12D.log\n\n";

  QStringList drs_runtime_configs;

  for (int row = 0; row < fers_count; ++row) {
    const QString name = QString("my_fers%1").arg(row);
    const QString link_type = fersLinkType(m_fers_table, row);
    const bool ethernet = link_type == "ETHERNET";
    const int pid = tableInt(m_fers_table, row, kFersColPid, defaultFersPid(row));
    const QString ip_address = fersIpAddress(m_fers_table, row);
    const int ro_mode = fersRoMode(m_fers_table, row);
    if (ethernet && ip_address.isEmpty()) {
      QMessageBox::warning(nullptr, "Devices",
                           name + " is set to Ethernet but has no IP address.");
      return false;
    }
    ini_out << "[Producer." << name << "]\n";
    ini_out << "FERS_IP_ADDRESS = "
            << (ethernet ? QString("eth:") + ip_address
                         : QString("usb:") + QString::number(pid))
            << "\n";
    ini_out << "FERS_EXPECTED_PID = " << (ethernet ? -1 : pid) << "\n";
    ini_out << "FERS_DEV_LOCK_PATH = /tmp/" << name << ".lock\n";
    ini_out << "FERS_ID = " << configQuote(QString("FERS %1").arg(row)) << "\n";
    ini_out << "FERS_RO_MODE = " << ro_mode << "\n";
    ini_out << "FERS_PRODID = " << configQuote(name) << "\n\n";
  }

  for (int row = 0; row < drs_count; ++row) {
    const QString name = QString("my_drs%1").arg(row);
    const int serial = tableInt(m_drs_table, row, 1, defaultDrsSerial(row));
    const QString link_type = drsLinkType(m_drs_table, row);
    const int link_num = tableInt(m_drs_table, row, 3, row);
    const int node = tableInt(m_drs_table, row, 4, row);
    const int board_offset = tableInt(m_drs_table, row, 5, row);
    const QString label = tableText(m_drs_table, row, 6,
                                    QString("DRS %1").arg(row));
    QString drs_conf_file = "user/calvision/misc/conf/WaveDumpConfig_X742.txt";
    if (!drs_config_path.trimmed().isEmpty()) {
      drs_conf_file = base + QString("_drs%1.txt").arg(row);
      if (!writeMappedDrsConfig(drs_config_path.trimmed(),
                                drs_conf_file,
                                row,
                                name)) {
        QMessageBox::warning(nullptr, "Devices",
                             "Could not write mapped DRS config for " +
                             name + ": " + drs_conf_file);
        return false;
      }
    }
    drs_runtime_configs << drs_conf_file;
    ini_out << "[Producer." << name << "]\n";
    ini_out << "DRS_BASE_ADDRESS = 0\n";
    ini_out << "DRS_CONF_FILE = " << drs_conf_file << "\n";
    ini_out << "DRS_ENABLED = 1\n";
    ini_out << "DRS_LINK_TYPE = " << link_type << "\n";
    ini_out << "DRS_LINK_NUM = " << link_num << "\n";
    ini_out << "DRS_CONET_NODE = " << node << "\n";
    ini_out << "DRS_BOARD_OFFSET = " << board_offset << "\n";
    ini_out << "DRS_EXPECTED_SERIAL = " << serial << "\n";
    ini_out << "DRS_DEV_LOCK_PATH = /tmp/" << name << ".lock\n";
    ini_out << "DRS_ID = " << configQuote(label) << "\n";
    ini_out << "DRS_PRODID = " << configQuote(name) << "\n\n";
  }
  ini.close();

  ConfigTemplate conf_template;
  if (!conf_template.load(conf_template_path)) {
    QMessageBox::warning(nullptr, "Devices",
                         "Could not read config template file: " +
                         conf_template_path);
    return false;
  }

  QStringList drs_names;
  QStringList fers_names;
  for (int row = 0; row < drs_count; ++row) {
    drs_names << QString("my_drs%1").arg(row);
  }
  for (int row = 0; row < fers_count; ++row) {
    fers_names << QString("my_fers%1").arg(row);
  }
  int fast_builder_full_mask = 0;
  for (int row = 0; row < drs_count && row < 3; ++row) {
    fast_builder_full_mask |= (1 << row);
  }
  for (int row = 0; row < fers_count && row < 2; ++row) {
    fast_builder_full_mask |= (1 << (3 + row));
  }
  QStringList additional_displays;
  additional_displays << "log" << "_SERVER";

  QStringList first_stop_names;
  for (int i = fers_names.size() - 1; i >= 0; --i) {
    first_stop_names << fers_names.at(i);
  }
  first_stop_names << drs_names;
  conf_template.setValue("RunControl", "EUDAQ_CTRL_PRODUCER_LAST_START",
                         fers_names.join(","));
  conf_template.setValue("RunControl", "EUDAQ_CTRL_PRODUCER_FIRST_STOP",
                         first_stop_names.join(","));
  conf_template.setValue("RunControl", "ADDITIONAL_DISPLAY_NUMBERS",
                         configQuote(additional_displays.join(",")));

  conf_template.setValue("DataCollector.my_dc0",
                         "FERS_SYNC_EXPECTED_CONNECTIONS",
                         QString::number(fers_count + drs_count));
  conf_template.setValue("DataCollector.my_dc0",
                         "FERS_EVENT_BUILDER_FULL_MASK",
                         QString::number(fast_builder_full_mask));

  for (int row = 0; row < drs_count; ++row) {
    const QString name = QString("my_drs%1").arg(row);
    const QString section = "Producer." + name;
    const EnsureResult ensured =
        ensureProducerSection(&conf_template, "my_drs", row);
    if (ensured.created) {
      if (conf_template.value(section, "EUDAQ_DC").isEmpty()) {
        conf_template.setValue(section, "EUDAQ_DC", "my_dc0");
      }
      adjustGeneratedPlaneId(&conf_template, section, "DRS_PLANE_ID",
                             row, ensured.source_row, 20);
    }
    if (row < drs_runtime_configs.size() &&
        !drs_runtime_configs.at(row).trimmed().isEmpty()) {
      conf_template.setValue(section, "DRS_CONF_FILE",
                             configQuote(drs_runtime_configs.at(row)));
    }
  }

  for (int row = 0; row < fers_count; ++row) {
    const QString name = QString("my_fers%1").arg(row);
    const int ro_mode = fersRoMode(m_fers_table, row);
    const QString section = "Producer." + name;
    const EnsureResult ensured =
        ensureProducerSection(&conf_template, "my_fers", row);
    if (ensured.created) {
      if (conf_template.value(section, "EUDAQ_DC").isEmpty()) {
        conf_template.setValue(section, "EUDAQ_DC", "my_dc0");
      }
      adjustGeneratedPlaneId(&conf_template, section, "EX0_PLANE_ID",
                             row, ensured.source_row, 0);
    }
    if (!fers_config_path.trimmed().isEmpty()) {
      const QString mapped_path =
          base + QString("_fers%1.txt").arg(row);
      if (!writeMappedFersConfig(fers_config_path.trimmed(),
                                 mapped_path,
                                 row,
                                 name)) {
        QMessageBox::warning(nullptr, "Devices",
                             "Could not write mapped FERS config for " +
                             name + ": " + mapped_path);
        return false;
      }
      conf_template.setValue(section, "FERS_CONF_FILE",
                             configQuote(mapped_path));
    }
    conf_template.setValue(section, "FERS_RO_MODE", QString::number(ro_mode));
  }

  conf_template.setValue("Monitor.my_mon0", "FERS_MONITOR_BOARD_COUNT",
                         QString::number(fers_count));
  conf_template.setValue("Monitor.my_mon0", "DRS_MONITOR_BOARD_COUNT",
                         QString::number(drs_count));

  if (!conf_template.save(cfg_path)) {
    QMessageBox::warning(nullptr, "Devices",
                         "Could not write generated config file: " + cfg_path);
    return false;
  }

  if (init_path) {
    *init_path = ini_path;
  }
  if (conf_path) {
    *conf_path = cfg_path;
  }
  return true;
}

void CalvisionDeviceTab::updateFersReadbacks(
    const QMap<QString, FersReadback> &readbacks) {
  const bool old_loading = m_loading_settings;
  m_loading_settings = true;
  for (int row = 0; row < m_fers_table->rowCount(); ++row) {
    const QString producer = tableText(m_fers_table, row, kFersColProducer);
    const FersReadback readback = readbacks.value(producer);
    const QString tooltip = readback.summary;
    setReadOnlyTableText(m_fers_table, row, kFersColReadPid,
                         readback.pid, tooltip);
    setReadOnlyTableText(m_fers_table, row, kFersColReadModel,
                         readback.model, tooltip);
    setReadOnlyTableText(m_fers_table, row, kFersColReadFpga,
                         readback.fpga_fw, tooltip);
    setReadOnlyTableText(m_fers_table, row, kFersColReadUc,
                         readback.uc_fw, tooltip);
  }
  m_loading_settings = old_loading;
}

bool CalvisionDeviceTab::startProducer(eudaq::RunControl *rc,
                                       const QString &producer_type,
                                       const QString &producer_name) {
  if (!rc) {
    return false;
  }
  for (const auto &conn : rc->GetActiveConnections()) {
    if (conn && conn->GetType() == "Producer" &&
        QString::fromStdString(conn->GetName()) == producer_name) {
      return true;
    }
  }

  const QString producer_bin = euCliProducerPath();
  if (producer_bin.isEmpty()) {
    QMessageBox::warning(this, "Devices",
                         "Could not find euCliProducer near this euRun executable.");
    return false;
  }

  const QString log_dir = launchLogDir();
  QDir().mkpath(log_dir);
  auto proc = new QProcess(this);
  proc->setProgram(producer_bin);
  proc->setArguments({"-n", producer_type, "-t", producer_name});
  proc->setProcessChannelMode(QProcess::MergedChannels);
  proc->setStandardOutputFile(QDir(log_dir).filePath(producer_name + ".log"),
                              QIODevice::Append);
  proc->start();
  if (!proc->waitForStarted(3000)) {
    QMessageBox::warning(this, "Devices",
                         "Failed to start " + producer_name + ": "
                             + proc->errorString());
    proc->deleteLater();
    return false;
  }
  m_processes.push_back(proc);
  return true;
}

bool CalvisionDeviceTab::terminateUnwantedProducers(
    eudaq::RunControl *rc, const QStringList &wanted_names) {
  if (!rc) {
    return false;
  }
  QStringList unwanted_names;
  for (const auto &conn : rc->GetActiveConnections()) {
    if (!conn || conn->GetType() != "Producer") {
      continue;
    }
    const QString name = QString::fromStdString(conn->GetName());
    const bool is_calvision_device =
        name.startsWith("my_fers") || name.startsWith("my_drs");
    if (is_calvision_device && !wanted_names.contains(name)) {
      unwanted_names << name;
      rc->TerminateSingleConnection(conn);
    }
  }
  if (unwanted_names.isEmpty()) {
    return true;
  }

  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 5000) {
    QStringList still_connected;
    for (const auto &conn : rc->GetActiveConnections()) {
      if (conn && conn->GetType() == "Producer") {
        const QString name = QString::fromStdString(conn->GetName());
        if (unwanted_names.contains(name)) {
          still_connected << name;
        }
      }
    }
    if (still_connected.isEmpty()) {
      return true;
    }
    QApplication::processEvents();
    QThread::msleep(100);
  }
  QMessageBox::warning(this, "Devices",
                       "Some disabled producers did not disconnect within 5 seconds: "
                           + unwanted_names.join(", "));
  return false;
}

bool CalvisionDeviceTab::waitForConnections(eudaq::RunControl *rc,
                                            const QStringList &producer_names,
                                            int timeout_ms) {
  if (!rc) {
    return false;
  }
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout_ms) {
    QStringList connected;
    for (const auto &conn : rc->GetActiveConnections()) {
      if (conn && conn->GetType() == "Producer") {
        connected << QString::fromStdString(conn->GetName());
      }
    }
    bool all_connected = true;
    for (const QString &name : producer_names) {
      if (!connected.contains(name)) {
        all_connected = false;
        break;
      }
    }
    if (all_connected) {
      return true;
    }
    QApplication::processEvents();
    QThread::msleep(100);
  }
  return producer_names.isEmpty();
}

bool CalvisionDeviceTab::launchConfiguredProducers(eudaq::RunControl *rc) {
  const QStringList wanted = configuredProducerNames();
  if (!terminateUnwantedProducers(rc, wanted)) {
    return false;
  }

  for (int row = 0; row < m_drs_table->rowCount(); ++row) {
    if (!startProducer(rc, "DRSProducer", QString("my_drs%1").arg(row))) {
      return false;
    }
  }
  for (int row = 0; row < m_fers_table->rowCount(); ++row) {
    if (!startProducer(rc, "FERSProducer", QString("my_fers%1").arg(row))) {
      return false;
    }
  }

  if (!waitForConnections(rc, wanted, 10000)) {
    QMessageBox::warning(this, "Devices",
                         "Not all configured producers connected within 10 seconds. "
                         "Check the device launch logs.");
    return false;
  }
  return true;
}

void CalvisionDeviceTab::terminateOwnedProcesses() {
  for (auto proc : m_processes) {
    if (!proc) {
      continue;
    }
    if (proc->state() != QProcess::NotRunning) {
      proc->terminate();
      if (!proc->waitForFinished(2000)) {
        proc->kill();
        proc->waitForFinished(1000);
      }
    }
    proc->deleteLater();
  }
  m_processes.clear();
}

bool CalvisionDeviceTab::prepareForInit(eudaq::RunControl *rc,
                                        QLineEdit *init_file,
                                        QLineEdit *conf_file,
                                        const QString &conf_template_override,
                                        const QString &fers_config_override,
                                        const QString &drs_config_override) {
  if (!m_enabled || !m_enabled->isChecked()) {
    return true;
  }
  saveSettings();

  QString conf_template =
      conf_template_override.trimmed().isEmpty()
          ? (conf_file ? conf_file->text().trimmed() : QString())
          : conf_template_override.trimmed();
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/devices");
  if (!conf_template.isEmpty() && !isGeneratedDeviceConfig(conf_template)) {
    settings.setValue("templateConfigFile", conf_template);
  } else {
    conf_template =
        settings.value("templateConfigFile",
                       defaultTemplateConfigPath()).toString();
  }
  settings.endGroup();
  if (conf_template.isEmpty()) {
    conf_template = defaultTemplateConfigPath();
  }

  QString generated_ini;
  QString generated_conf;
  if (!writeGeneratedFiles(conf_template, fers_config_override,
                           drs_config_override,
                           &generated_ini, &generated_conf)) {
    return false;
  }
  if (init_file) {
    init_file->setText(generated_ini);
  }
  if (conf_file) {
    conf_file->setText(generated_conf);
  }
  if (m_status) {
    m_status->setText("Generated " + generated_ini + " and " + generated_conf +
                      " from " + conf_template);
  }

  return launchConfiguredProducers(rc);
}
