#include "CalvisionDrsTab.hh"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr const char *kDefaultDrsConfigPath =
    "user/calvision/misc/conf/WaveDumpConfig_X742.txt";

QStringList kDefaultSections() {
  return {"COMMON", "0", "1", "TR0"};
}

QString sectionTitle(const QString &section) {
  if (section == "COMMON") {
    return "COMMON";
  }
  if (section.startsWith("TR")) {
    return section;
  }
  return "Group " + section;
}

QString makeStateKey(const QString &section, const QString &key) {
  return section + "\n" + key;
}

bool isRemovedGroupSection(const QString &section) {
  return section == "2" || section == "3" || section == "TR1";
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

QString commentBody(const QString &line) {
  const QString trimmed = line.trimmed();
  if (!trimmed.startsWith("#") && !trimmed.startsWith(";")) {
    return QString();
  }
  return trimmed.mid(1).trimmed();
}

QStringList splitTokens(const QString &line) {
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

bool parseParamLine(const QString &line,
                    QString *key,
                    QString *value) {
  const QString trimmed = line.trimmed();
  if (trimmed.isEmpty() || trimmed.startsWith("#") ||
      trimmed.startsWith(";") || trimmed.startsWith("[")) {
    return false;
  }

  QString body = trimmed;
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

bool parseBoardOverrideComment(const QString &line,
                               int *board,
                               QString *section,
                               QString *key,
                               QString *value) {
  QString body = commentBody(line);
  if (!body.startsWith("EUDAQ_DRS_BOARD")) {
    return false;
  }
  const QStringList tokens = splitTokens(body);
  if (tokens.size() < 5 || tokens.at(0) != "EUDAQ_DRS_BOARD") {
    return false;
  }

  bool ok = false;
  const int parsed_board = tokens.at(1).toInt(&ok);
  if (!ok) {
    return false;
  }

  QString parsed_section = tokens.at(2).trimmed();
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

void addParam(std::vector<CalvisionDrsTab::ParamDef> *params,
              const QString &section,
              const QString &key,
              const QString &display,
              const QString &tooltip,
              const QStringList &options,
              bool optional,
              int *order) {
  CalvisionDrsTab::ParamDef param;
  param.section = section;
  param.key = key;
  param.display = display.isEmpty() ? key : display;
  param.tooltip = tooltip;
  param.options = options;
  param.optional = optional;
  param.order = (*order)++;
  params->push_back(param);
}
}

CalvisionDrsTab::CalvisionDrsTab(QWidget *parent)
    : QWidget(parent),
      m_path(nullptr),
      m_board_count(nullptr),
      m_tabs(nullptr),
      m_status(nullptr),
      m_loading(false),
      m_dirty(false) {
  buildDefinitions();
  setupUi();
  loadSettings();
}

void CalvisionDrsTab::setupUi() {
  auto layout = new QVBoxLayout(this);
  auto intro = new QLabel(
      "Edit the WaveDumpConfig_X742.txt settings used by DRS producers. "
      "Default values are written as normal WaveDump commands; B0/B1/... "
      "values are board overrides that EUDAQ expands into per-board runtime "
      "config files during Init.",
      this);
  intro->setWordWrap(true);
  layout->addWidget(intro);

  auto top = new QHBoxLayout();
  top->addWidget(new QLabel("WaveDump config:", this));
  m_path = new QLineEdit(this);
  top->addWidget(m_path, 1);
  auto browse = new QPushButton("Browse", this);
  auto reload = new QPushButton("Reload", this);
  auto save = new QPushButton("Save", this);
  top->addWidget(browse);
  top->addWidget(reload);
  top->addWidget(save);
  top->addSpacing(12);
  top->addWidget(new QLabel("Boards:", this));
  m_board_count = new QSpinBox(this);
  m_board_count->setRange(1, kMaxBoards);
  m_board_count->setValue(3);
  m_board_count->setToolTip(
      "Number of board override columns. B0 maps to my_drs0, "
      "B1 maps to my_drs1, and so on.");
  top->addWidget(m_board_count);
  layout->addLayout(top);

  m_tabs = new QTabWidget(this);
  layout->addWidget(m_tabs, 1);

  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  connect(browse, &QPushButton::clicked, this, [this]() {
    const QString start_dir =
        QFileInfo(configPath()).exists()
            ? QFileInfo(configPath()).absolutePath()
            : QDir::currentPath();
    const QString file = QFileDialog::getOpenFileName(
        this, "Select WaveDump DRS config", start_dir,
        "WaveDump config (*.txt);;All files (*)");
    if (!file.isEmpty()) {
      setConfigPath(file);
    }
  });
  connect(reload, &QPushButton::clicked, this, [this]() {
    if (!m_dirty ||
        QMessageBox::question(this, "Reload DRS config",
                              "Discard unsaved DRS changes and reload?",
                              QMessageBox::Ok | QMessageBox::Cancel) ==
            QMessageBox::Ok) {
      loadConfig();
      clearForm();
      buildForm();
    }
  });
  connect(save, &QPushButton::clicked, this, [this]() {
    saveConfig();
  });
  connect(m_path, &QLineEdit::editingFinished, this, [this]() {
    const QString normalized = normalizedConfigPath(m_path->text());
    if (normalized != m_path->text()) {
      m_path->setText(normalized);
    }
    loadConfig();
    clearForm();
    buildForm();
    saveSettings();
  });
  connect(m_board_count,
          static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this, [this]() {
            if (m_loading) {
              return;
            }
            clearForm();
            buildForm();
            saveSettings();
          });
}

void CalvisionDrsTab::loadSettings() {
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/drs");
  const QString path =
      settings.value("waveDumpConfigFile", defaultConfigPath()).toString();
  const int boards = settings.value("boardColumns", 3).toInt();
  settings.endGroup();

  m_loading = true;
  if (m_path) {
    m_path->setText(normalizedConfigPath(path));
  }
  if (m_board_count) {
    m_board_count->setValue(std::max(1, std::min(kMaxBoards, boards)));
  }
  m_loading = false;

  loadConfig();
  buildForm();
}

QString CalvisionDrsTab::defaultConfigPath() const {
  return kDefaultDrsConfigPath;
}

QString CalvisionDrsTab::normalizedConfigPath(const QString &path) const {
  const QString trimmed = path.trimmed();
  return trimmed.isEmpty() ? defaultConfigPath() : trimmed;
}

QString CalvisionDrsTab::configPath() const {
  return normalizedConfigPath(m_path ? m_path->text() : QString());
}

void CalvisionDrsTab::setConfigPath(const QString &path) {
  if (m_path) {
    m_path->setText(normalizedConfigPath(path));
  }
  loadConfig();
  clearForm();
  buildForm();
  saveSettings();
}

QString CalvisionDrsTab::stateKey(const QString &section,
                                  const QString &key) const {
  return makeStateKey(section, key);
}

void CalvisionDrsTab::buildDefinitions() {
  m_sections = kDefaultSections();
  m_params.clear();
  int order = 0;

  addParam(&m_params, "COMMON", "OPEN", "OPEN",
           "WaveDump physical connection line. EUDAQ Devices tab normally "
           "overrides link type, link number/PID, and node for each producer.",
           {}, false, &order);
  addParam(&m_params, "COMMON", "RECORD_LENGTH", "Record Length",
           "Number of samples in the acquisition window. X742 accepts 1024, "
           "520, 256, or 136.", {"1024", "520", "256", "136"}, false, &order);
  addParam(&m_params, "COMMON", "POST_TRIGGER", "Post Trigger",
           "Post-trigger size in percent of the acquisition window.",
           {}, false, &order);
  addParam(&m_params, "COMMON", "PULSE_POLARITY", "Pulse Polarity",
           "Input signal polarity.", {"POSITIVE", "NEGATIVE"}, false, &order);
  addParam(&m_params, "COMMON", "EXTERNAL_TRIGGER", "External Trigger",
           "External trigger input mode.",
           {"DISABLED", "ACQUISITION_ONLY", "ACQUISITION_AND_TRGOUT"},
           false, &order);
  addParam(&m_params, "COMMON", "FAST_TRIGGER", "Fast Trigger",
           "Fast trigger input mode for X742.",
           {"DISABLED", "ACQUISITION_ONLY"}, false, &order);
  addParam(&m_params, "COMMON", "ENABLED_FAST_TRIGGER_DIGITIZING",
           "Fast Trigger Digitizing",
           "Digitize the fast trigger signal as channel 8 for each group.",
           {"YES", "NO"}, false, &order);
  addParam(&m_params, "COMMON", "CORRECTION_LEVEL", "Correction Level",
           "X742 correction level. Examples: AUTO, or '7 AUTO', or "
           "'7 13 Table_gr0 Table_gr2 Table_gr3'.",
           {}, false, &order);
  addParam(&m_params, "COMMON", "DRS4_FREQUENCY", "DRS4 Frequency",
           "0=5 GHz, 1=2.5 GHz, 2=1 GHz, 3=750 MHz.",
           {"0", "1", "2", "3"}, false, &order);
  addParam(&m_params, "COMMON", "OUTPUT_FILE_FORMAT", "Output File Format",
           "WaveDump output file format.", {"BINARY", "ASCII"}, false, &order);
  addParam(&m_params, "COMMON", "OUTPUT_FILE_HEADER", "Output File Header",
           "Include WaveDump output headers.", {"YES", "NO"}, false, &order);
  addParam(&m_params, "COMMON", "TEST_PATTERN", "Test Pattern",
           "Replace ADC data with test pattern.", {"YES", "NO"}, false, &order);
  addParam(&m_params, "COMMON", "FPIO_LEVEL", "FPIO Level",
           "Front panel I/O LEMO level.", {"NIM", "TTL"}, false, &order);
  addParam(&m_params, "COMMON", "MAX_NUM_EVENTS_BLT", "Max Events BLT",
           "Maximum number of events per block transfer.", {}, true, &order);
  addParam(&m_params, "COMMON", "USE_INTERRUPT", "Use Interrupt",
           "Request interrupt when at least N events are ready; 0 disables.",
           {}, true, &order);
  addParam(&m_params, "COMMON", "GNUPLOT_PATH", "Gnuplot Path",
           "WaveDump plotting executable path.", {}, true, &order);
  addParam(&m_params, "COMMON", "DECIMATION_FACTOR", "Decimation Factor",
           "Digitizer decimation factor.", {}, true, &order);
  addParam(&m_params, "COMMON", "ENABLE_DES_MODE", "Enable DES Mode",
           "Double sampling mode for supported boards.", {"YES", "NO"},
           true, &order);
  addParam(&m_params, "COMMON", "SKIP_STARTUP_CALIBRATION",
           "Skip Startup Calibration",
           "YES skips the startup calibration.", {"YES", "NO"}, true, &order);
  addParam(&m_params, "COMMON", "WRITE_REGISTER", "Write Register",
           "Optional direct register write: ADDRESS DATA MASK.", {},
           true, &order);

  const QStringList groups = {"0", "1"};
  for (const QString &section : groups) {
    addParam(&m_params, section, "ENABLE_INPUT", "Enable Input",
             "Enable or disable this DRS group.", {"YES", "NO"}, true, &order);
    addParam(&m_params, section, "DC_OFFSET", "DC Offset",
             "Group DC offset percent of full scale.", {}, true, &order);
    addParam(&m_params, section, "GRP_CH_DC_OFFSET", "Group Channel DC Offset",
             "Eight comma-separated per-channel offsets for this group.",
             {}, true, &order);
    addParam(&m_params, section, "BASELINE_LEVEL", "Baseline Level",
             "Alternative baseline setting accepted by WaveDump.", {},
             true, &order);
    addParam(&m_params, section, "TRIGGER_THRESHOLD", "Trigger Threshold",
             "Group trigger threshold.", {}, true, &order);
    addParam(&m_params, section, "GROUP_TRG_ENABLE_MASK",
             "Group Trigger Enable Mask",
             "Hex 8-bit group trigger enable mask.", {}, true, &order);
    addParam(&m_params, section, "CHANNEL_TRIGGER", "Channel Trigger",
             "Channel/group trigger mode.",
             {"DISABLED", "ACQUISITION_ONLY", "ACQUISITION_AND_TRGOUT",
              "TRGOUT_ONLY"},
             true, &order);
  }

  for (const QString &section : QStringList{"TR0"}) {
    addParam(&m_params, section, "DC_OFFSET", "DC Offset",
             "Fast trigger DC offset DAC setting.", {}, false, &order);
    addParam(&m_params, section, "TRIGGER_THRESHOLD", "Trigger Threshold",
             "Fast trigger threshold DAC setting.", {}, false, &order);
  }
}

bool CalvisionDrsTab::loadConfig() {
  buildDefinitions();
  m_values.clear();
  for (const ParamDef &param : m_params) {
    ParamState state;
    state.board_values.assign(kMaxBoards, QString());
    m_values[stateKey(param.section, param.key)] = state;
  }

  QFile file(configPath());
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    setStatus("Could not read DRS config: " + configPath());
    m_dirty = false;
    return false;
  }

  QString current_section = "COMMON";
  QTextStream in(&file);
  while (!in.atEnd()) {
    const QString line = in.readLine();
    QString section;
    if (parseSectionHeader(line, &section)) {
      current_section = section;
      if (!isRemovedGroupSection(current_section) &&
          !m_sections.contains(current_section)) {
        m_sections << current_section;
      }
      continue;
    }
    if (isRemovedGroupSection(current_section)) {
      continue;
    }

    int board = -1;
    QString override_section;
    QString override_key;
    QString override_value;
    if (parseBoardOverrideComment(line, &board, &override_section,
                                  &override_key, &override_value)) {
      if (isRemovedGroupSection(override_section)) {
        continue;
      }
      const QString id = stateKey(override_section, override_key);
      if (board >= 0 && board < kMaxBoards &&
          m_values.find(id) != m_values.end()) {
        m_values[id].board_values[board] = override_value;
      }
      continue;
    }

    QString key;
    QString value;
    if (!parseParamLine(line, &key, &value)) {
      continue;
    }

    const QString id = stateKey(current_section, key);
    auto state_it = m_values.find(id);
    if (state_it == m_values.end()) {
      ParamDef param;
      param.section = current_section;
      param.key = key;
      param.display = key;
      param.optional = true;
      param.order = static_cast<int>(m_params.size());
      m_params.push_back(param);
      ParamState state;
      state.board_values.assign(kMaxBoards, QString());
      state_it = m_values.emplace(id, state).first;
    }
    state_it->second.default_value = value;
  }

  m_loading = true;
  m_dirty = false;
  m_loading = false;
  setStatus("Loaded DRS WaveDump config: " +
            QFileInfo(configPath()).absoluteFilePath());
  return true;
}

void CalvisionDrsTab::clearForm() {
  if (!m_tabs) {
    return;
  }
  while (m_tabs->count() > 0) {
    QWidget *widget = m_tabs->widget(0);
    m_tabs->removeTab(0);
    delete widget;
  }
}

void CalvisionDrsTab::buildForm() {
  if (!m_tabs) {
    return;
  }
  for (const QString &section : m_sections) {
    m_tabs->addTab(buildSectionTab(section), sectionTitle(section));
  }
}

QWidget *CalvisionDrsTab::buildSectionTab(const QString &section) {
  auto content = new QWidget(m_tabs);
  auto grid = new QGridLayout(content);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(6);
  grid->addWidget(new QLabel("<b>Parameter</b>", content), 0, 0,
                  Qt::AlignRight | Qt::AlignVCenter);
  grid->addWidget(new QLabel("<b>Default</b>", content), 0, 1);
  grid->setColumnStretch(1, 1);
  for (int board = 0; board < boardCount(); ++board) {
    auto header = new QLabel(QString("<b>B%1</b>").arg(board), content);
    header->setToolTip(QString("Override for my_drs%1. Blank means inherit "
                               "Default.").arg(board));
    grid->addWidget(header, 0, board + 2);
    grid->setColumnStretch(board + 2, 1);
  }

  int row = 1;
  for (const ParamDef &param : m_params) {
    if (param.section != section) {
      continue;
    }
    const auto state_it = m_values.find(stateKey(param.section, param.key));
    const QString default_value =
        state_it == m_values.end() ? QString()
                                   : state_it->second.default_value;
    auto label = new QLabel(param.display, content);
    label->setToolTip(editorTooltip(param, false));
    grid->addWidget(label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(makeEditor(content, param, default_value, false, -1),
                    row, 1);

    for (int board = 0; board < boardCount(); ++board) {
      const QString board_value =
          state_it == m_values.end() ? QString()
                                     : state_it->second.board_values[board];
      grid->addWidget(makeEditor(content, param, board_value, true, board),
                      row, board + 2);
    }
    ++row;
  }

  if (row == 1) {
    grid->addWidget(new QLabel("No configurable parameters found.", content),
                    1, 0, 1, boardCount() + 2);
  }
  return makeScrollPage(content, m_tabs);
}

QWidget *CalvisionDrsTab::makeEditor(QWidget *parent,
                                     const ParamDef &param,
                                     const QString &value,
                                     bool board_override,
                                     int board) {
  const QString tooltip = editorTooltip(param, board_override);
  if (!param.options.isEmpty()) {
    auto combo = new QComboBox(parent);
    combo->setEditable(true);
    if (board_override || param.optional) {
      combo->addItem("");
    }
    combo->addItems(param.options);
    if (!value.trimmed().isEmpty() && combo->findText(value.trimmed()) < 0) {
      combo->insertItem((board_override || param.optional) ? 1 : 0,
                        value.trimmed());
    }
    combo->setCurrentText(value.trimmed());
    combo->setToolTip(tooltip);
    connect(combo,
            static_cast<void (QComboBox::*)(const QString&)>(
                &QComboBox::currentTextChanged),
            this, [this, section = param.section, key = param.key,
                   board_override, board](const QString &text) {
              setParamValue(section, key, text.trimmed(),
                            board_override ? board : -1);
            });
    return combo;
  }

  auto edit = new QLineEdit(value.trimmed(), parent);
  edit->setMinimumWidth(120);
  if (board_override || param.optional) {
    edit->setPlaceholderText(board_override ? "inherit" : "optional");
  }
  edit->setToolTip(tooltip);
  connect(edit, &QLineEdit::textChanged,
          this, [this, section = param.section, key = param.key,
                 board_override, board](const QString &text) {
            setParamValue(section, key, text.trimmed(),
                          board_override ? board : -1);
          });
  return edit;
}

QWidget *CalvisionDrsTab::makeScrollPage(QWidget *content,
                                         QWidget *parent) const {
  auto scroll = new QScrollArea(parent);
  scroll->setWidgetResizable(true);
  scroll->setWidget(content);
  return scroll;
}

QString CalvisionDrsTab::editorTooltip(const ParamDef &param,
                                       bool board_override) const {
  QString text = param.tooltip.trimmed();
  if (!param.options.isEmpty()) {
    if (!text.isEmpty()) {
      text += "\n";
    }
    text += "Options: " + param.options.join(", ");
  }
  if (param.optional) {
    if (!text.isEmpty()) {
      text += "\n";
    }
    text += "Blank default means this command is not written.";
  }
  if (board_override) {
    if (!text.isEmpty()) {
      text += "\n\n";
    }
    text += "Blank board value means inherit Default.";
  }
  return text;
}

QString CalvisionDrsTab::formatConfigLine(const QString &key,
                                          const QString &value) const {
  return key.leftJustified(24, ' ') + value.trimmed();
}

bool CalvisionDrsTab::saveConfig() {
  if (m_values.empty()) {
    QMessageBox::warning(this, "DRS",
                         "No DRS parameters are loaded; refusing to save.");
    return false;
  }

  QStringList lines;
  lines << "# ****************************************************************";
  lines << "# WaveDump Configuration File";
  lines << "# Generated by EUDAQ DRS tab";
  lines << "# ****************************************************************";
  lines << "";
  lines << "# NOTE:";
  lines << "# The lines between the commands @OFF and @ON will be skipped.";
  lines << "# EUDAQ_DRS_BOARD comments are ignored by WaveDump and used by euRun";
  lines << "# to generate per-board runtime configs.";
  lines << "";

  for (const QString &section : m_sections) {
    lines << "# ----------------------------------------------------------------";
    lines << "# " + sectionTitle(section);
    lines << "# ----------------------------------------------------------------";
    lines << "[" + section + "]";
    for (const ParamDef &param : m_params) {
      if (param.section != section) {
        continue;
      }
      const auto state_it = m_values.find(stateKey(param.section, param.key));
      if (state_it == m_values.end()) {
        continue;
      }
      const QString value = state_it->second.default_value.trimmed();
      if (!value.isEmpty()) {
        lines << formatConfigLine(param.key, value);
      }
    }
    lines << "";
  }

  QStringList override_lines;
  for (int board = 0; board < kMaxBoards; ++board) {
    for (const ParamDef &param : m_params) {
      const auto state_it = m_values.find(stateKey(param.section, param.key));
      if (state_it == m_values.end()) {
        continue;
      }
      const QString value = state_it->second.board_values[board].trimmed();
      if (!value.isEmpty()) {
        override_lines
            << QString("# EUDAQ_DRS_BOARD %1 [%2] %3 %4")
                   .arg(board)
                   .arg(param.section)
                   .arg(param.key)
                   .arg(value);
      }
    }
  }

  if (!override_lines.isEmpty()) {
    lines << "# ****************************************************************";
    lines << "# EUDAQ board overrides, ignored by WaveDump";
    lines << "# Syntax: # EUDAQ_DRS_BOARD <board> [SECTION] KEY VALUE";
    lines << "# ****************************************************************";
    lines << override_lines;
    lines << "";
  }

  QFile file(configPath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    QMessageBox::warning(this, "DRS",
                         "Could not write DRS config: " + configPath());
    return false;
  }
  QTextStream out(&file);
  for (const QString &line : lines) {
    out << line << "\n";
  }

  m_dirty = false;
  saveSettings();
  setStatus("Saved DRS WaveDump config: " +
            QFileInfo(configPath()).absoluteFilePath());
  return true;
}

void CalvisionDrsTab::saveSettings() const {
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/drs");
  settings.setValue("waveDumpConfigFile", configPath());
  if (m_board_count) {
    settings.setValue("boardColumns", m_board_count->value());
  }
  settings.endGroup();
}

void CalvisionDrsTab::setParamValue(const QString &section,
                                    const QString &key,
                                    const QString &value,
                                    int board) {
  if (m_loading) {
    return;
  }
  const QString id = stateKey(section, key);
  auto state_it = m_values.find(id);
  if (state_it == m_values.end()) {
    return;
  }
  if (board < 0) {
    state_it->second.default_value = value;
  } else if (board >= 0 && board < kMaxBoards) {
    state_it->second.board_values[board] = value;
  }
  markDirty();
}

void CalvisionDrsTab::markDirty() {
  if (m_loading) {
    return;
  }
  m_dirty = true;
  setStatus("Modified. Save before Init or click Save.");
}

void CalvisionDrsTab::setStatus(const QString &text) {
  if (m_status) {
    m_status->setText(text);
  }
}

int CalvisionDrsTab::boardCount() const {
  return m_board_count ? m_board_count->value() : 3;
}
