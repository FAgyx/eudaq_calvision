#include "CalvisionFersTab.hh"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegExp>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
constexpr const char *kDefaultConfigPath =
    "user/calvision/misc/conf/Janus_Config.txt";

constexpr const char *kDefaultJanusDefsPath =
    "/home/calvision/git/CAEN/"
    "Janus_5202_4.3.0_20260114_linux.tar.gz/"
    "Janus_5202_4.3.0_20260114_linux/bin/param_defs.txt";

QString trimCommentPrefix(QString line) {
  line = line.trimmed();
  while (line.startsWith("#") || line.startsWith(";")) {
    line = line.mid(1).trimmed();
  }
  return line;
}

bool splitCommentAware(const QString &line, QString *body, QString *comment) {
  bool in_quote = false;
  for (int i = 0; i < line.size(); ++i) {
    const QChar ch = line.at(i);
    if (ch == '"') {
      in_quote = !in_quote;
    } else if (ch == '#' && !in_quote) {
      if (body) {
        *body = line.left(i).trimmed();
      }
      if (comment) {
        *comment = line.mid(i + 1).trimmed();
      }
      return true;
    }
  }
  if (body) {
    *body = line.trimmed();
  }
  if (comment) {
    comment->clear();
  }
  return true;
}

QStringList splitJanusTokens(const QString &body) {
  QStringList tokens;
  QString token;
  bool in_quote = false;
  for (int i = 0; i < body.size(); ++i) {
    const QChar ch = body.at(i);
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
                    QString *value,
                    QString *comment) {
  QString body;
  splitCommentAware(line, &body, comment);
  if (body.isEmpty() || body.startsWith("#") || body.startsWith(";")) {
    return false;
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

struct ParsedKey {
  QString base;
  int board = -1;
  int channel = -1;
  bool has_board = false;
  bool has_channel = false;
};

bool parseIndexedKey(const QString &key, ParsedKey *parsed) {
  const int board_start = key.indexOf('[');
  if (board_start < 0) {
    if (parsed) {
      parsed->base = key;
    }
    return true;
  }

  const int board_end = key.indexOf(']', board_start + 1);
  if (board_end < 0) {
    return false;
  }

  bool ok = false;
  ParsedKey result;
  result.base = key.left(board_start);
  result.board = key.mid(board_start + 1,
                         board_end - board_start - 1).toInt(&ok);
  if (!ok) {
    return false;
  }
  result.has_board = true;

  int cursor = board_end + 1;
  if (cursor < key.size()) {
    if (key.at(cursor) != '[') {
      return false;
    }
    const int channel_end = key.indexOf(']', cursor + 1);
    if (channel_end < 0 || channel_end + 1 != key.size()) {
      return false;
    }
    result.channel = key.mid(cursor + 1,
                             channel_end - cursor - 1).toInt(&ok);
    if (!ok) {
      return false;
    }
    result.has_channel = true;
  }

  if (parsed) {
    *parsed = result;
  }
  return true;
}

QString indexedKey(const QString &base, int board, int channel) {
  QString key = base + "[" + QString::number(board) + "]";
  if (channel >= 0) {
    key += "[" + QString::number(channel) + "]";
  }
  return key;
}

bool isRuntimeOnlySection(const QString &section) {
  return section == "Connect" ||
         section == "Regs" ||
         section == "Statistics" ||
         section == "Log";
}

bool isRunCtrlOutputFileParamName(const QString &name) {
  return name == "OutputFiles" ||
         name == "DataAnalysis" ||
         name == "DataFilePath" ||
         name == "StopRunMode" ||
         name == "EventBuildingMode" ||
         name == "TstampCoincWindow" ||
         name == "PresetTime" ||
         name == "PresetCounts" ||
         name == "JobFirstRun" ||
         name == "JobLastRun" ||
         name == "RunSleep" ||
         name == "EnableJobs" ||
         name == "RunNumber_AutoIncr" ||
         name == "OutFileEnableMask" ||
         name == "EnableRawDataRead" ||
         name == "MaxSizeDataOutputFile" ||
         name.startsWith("OF_");
}

QString renamePathForDefs(const QString &defs_path) {
  return QFileInfo(defs_path).absoluteDir().filePath("param_rename.txt");
}

QString channelMonitorKey(const QString &name, int board, int channel) {
  return name + ":" + QString::number(board) + ":" + QString::number(channel);
}

QString hvMonitorKey(int board, const QString &name) {
  return QString::number(board) + ":" + name;
}

QString withUnit(const QString &value, const QString &unit) {
  const QString trimmed = value.trimmed();
  if (trimmed.isEmpty()) {
    return "--";
  }
  return unit.isEmpty() ? trimmed : trimmed + " " + unit;
}

bool splitValueAndUnit(const QString &text, double *value, QString *unit) {
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty()) {
    return false;
  }
  const QStringList parts = trimmed.split(QRegExp("\\s+"), Qt::SkipEmptyParts);
  if (parts.isEmpty()) {
    return false;
  }
  bool ok = false;
  const double parsed_value = parts.at(0).toDouble(&ok);
  if (!ok) {
    return false;
  }
  if (value) {
    *value = parsed_value;
  }
  if (unit) {
    *unit = parts.size() > 1 ? parts.at(1) : "V";
  }
  return true;
}

bool isHvStatusOn(const QString &status) {
  const QString normalized = status.trimmed().toUpper();
  return normalized == "ON" || normalized == "1";
}

bool isHvStatusKnown(const QString &status) {
  const QString normalized = status.trimmed().toUpper();
  return normalized == "ON" || normalized == "OFF" ||
         normalized == "1" || normalized == "0";
}

QString startRunModeTooltip() {
  return "ASYNC: Sends the start command to each FERS board individually. "
         "Best/default for the current one-producer-per-FERS setup.\n\n"
         "CHAIN_T0: Sends start to board 0, then propagates through a "
         "physical T0-out to T0-in daisy chain.\n\n"
         "CHAIN_T1: Sends start to board 0, then propagates through a "
         "physical T1-out to T1-in daisy chain.\n\n"
         "TDL: Starts boards through the CAEN concentrator/TDLink path in "
         "sync. Use only for FERS boards connected through TDLink.";
}

QString editorTooltipForValue(const QString &param_name,
                              const QString &base_tooltip,
                              const QString &value) {
  Q_UNUSED(value);
  if (param_name != "StartRunMode") {
    return base_tooltip;
  }
  const QString option_tooltip = startRunModeTooltip();
  if (option_tooltip.isEmpty()) {
    return base_tooltip;
  }
  return base_tooltip.isEmpty() ? option_tooltip
                                : base_tooltip + "\n\n" + option_tooltip;
}

QString paramEditorKey(const QString &name, int board, int channel) {
  return name + ":" + QString::number(board) + ":" + QString::number(channel);
}

bool isAcqModeCommonParam(const QString &name) {
  return name == "AcquisitionMode" ||
         name == "ChTrg_Width" ||
         name == "TriggerLogic" ||
         name == "Tlogic_Width" ||
         name == "MajorityLevel" ||
         name == "PtrgPeriod" ||
         name == "T0_Out" ||
         name == "T1_Out";
}

QStringList acqModeSpecificParamNames(const QString &mode) {
  const QString normalized = mode.trimmed().toUpper();
  if (normalized == "SPECTROSCOPY") {
    return {"BunchTrgSource", "VetoSource", "ValidationSource",
            "ValidationMode", "TrgIdMode"};
  }
  if (normalized == "SPECT_TIMING") {
    return {"EnableToT", "BunchTrgSource", "VetoSource",
            "ValidationSource", "ValidationMode", "TrgIdMode",
            "TrefSource", "TrefWindow", "TrefDelay"};
  }
  if (normalized == "TIMING_CSTART" || normalized == "TIMING_CSTOP") {
    return {"EnableToT", "EnableListZeroSuppr", "VetoSource",
            "TrefSource", "TrefWindow", "TrefDelay"};
  }
  if (normalized == "COUNTING") {
    return {"BunchTrgSource", "CountingMode", "EnableCntZeroSuppr"};
  }
  if (normalized == "WAVEFORM") {
    return {"BunchTrgSource"};
  }
  return {};
}

}

CalvisionFersTab::CalvisionFersTab(QWidget *parent)
    : QWidget(parent),
      m_path(nullptr),
      m_defs_path(nullptr),
      m_board_count(nullptr),
      m_update_hv_monitor(nullptr),
      m_tabs(nullptr),
      m_status(nullptr),
      m_loading(false),
      m_dirty(false),
      m_hv_controls_enabled(false),
      m_rebuild_pending(false) {
  setupUi();
  loadSettings();
}

void CalvisionFersTab::setupUi() {
  auto layout = new QVBoxLayout(this);

  auto intro = new QLabel(
      "FERS settings are generated from the same Janus param_defs.txt file. "
      "Global Settings are common defaults; B0/B1/... tabs are board "
      "overrides; channel group tabs write board/channel overrides.",
      this);
  intro->setWordWrap(true);
  layout->addWidget(intro);

  auto cfg_layout = new QHBoxLayout();
  cfg_layout->addWidget(new QLabel("Janus config:", this));
  m_path = new QLineEdit(this);
  cfg_layout->addWidget(m_path, 1);
  auto browse_cfg = new QPushButton("Browse", this);
  auto reload = new QPushButton("Reload", this);
  auto save = new QPushButton("Save", this);
  cfg_layout->addWidget(browse_cfg);
  cfg_layout->addWidget(reload);
  cfg_layout->addWidget(save);
  layout->addLayout(cfg_layout);

  auto defs_layout = new QHBoxLayout();
  defs_layout->addWidget(new QLabel("Janus defs:", this));
  m_defs_path = new QLineEdit(this);
  defs_layout->addWidget(m_defs_path, 1);
  auto browse_defs = new QPushButton("Browse", this);
  defs_layout->addWidget(browse_defs);
  defs_layout->addSpacing(12);
  defs_layout->addWidget(new QLabel("Boards:", this));
  m_board_count = new QSpinBox(this);
  m_board_count->setRange(1, kMaxBoards);
  m_board_count->setValue(2);
  m_board_count->setToolTip(
      "Number of board tabs to show. B0 maps to my_fers0, "
      "B1 maps to my_fers1, and so on.");
  defs_layout->addWidget(m_board_count);
  defs_layout->addSpacing(12);
  m_update_hv_monitor = new QPushButton("Update HV Monitor", this);
  m_update_hv_monitor->setEnabled(false);
  m_update_hv_monitor->setToolTip(
      "Poll FERS HV voltage/current/temperature readback now. Enabled only "
      "when the DAQ is not running.");
  defs_layout->addWidget(m_update_hv_monitor);
  layout->addLayout(defs_layout);

  m_tabs = new QTabWidget(this);
  layout->addWidget(m_tabs, 1);

  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  connect(browse_cfg, &QPushButton::clicked, this, [this]() {
    const QString start_dir =
        QFileInfo(configPath()).exists()
            ? QFileInfo(configPath()).absolutePath()
            : QDir::currentPath();
    const QString file = QFileDialog::getOpenFileName(
        this, "Select Janus FERS config", start_dir,
        "Janus config (*.txt);;All files (*)");
    if (!file.isEmpty()) {
      setConfigPath(file);
    }
  });

  connect(browse_defs, &QPushButton::clicked, this, [this]() {
    const QString start_dir =
        QFileInfo(normalizedDefsPath(m_defs_path->text())).exists()
            ? QFileInfo(normalizedDefsPath(m_defs_path->text())).absolutePath()
            : QDir::currentPath();
    const QString file = QFileDialog::getOpenFileName(
        this, "Select Janus param_defs.txt", start_dir,
        "Janus param defs (param_defs.txt);;Text files (*.txt);;All files (*)");
    if (!file.isEmpty()) {
      m_defs_path->setText(file);
      reloadAll();
      saveSettings();
    }
  });

  connect(reload, &QPushButton::clicked, this, [this]() {
    if (!m_dirty ||
        QMessageBox::question(this, "Reload FERS config",
                              "Discard unsaved FERS changes and reload?",
                              QMessageBox::Ok | QMessageBox::Cancel) ==
            QMessageBox::Ok) {
      reloadAll();
    }
  });

  connect(save, &QPushButton::clicked, this, [this]() {
    saveConfig();
  });

  connect(m_update_hv_monitor, &QPushButton::clicked, this, [this]() {
    if (m_hv_update_callback) {
      m_hv_update_callback();
      setStatus("Requested FERS HV monitor update.");
    }
  });

  connect(m_path, &QLineEdit::editingFinished, this, [this]() {
    const QString normalized = normalizedConfigPath(m_path->text());
    if (normalized != m_path->text()) {
      m_path->setText(normalized);
    }
    reloadAll();
    saveSettings();
  });

  connect(m_defs_path, &QLineEdit::editingFinished, this, [this]() {
    const QString normalized = normalizedDefsPath(m_defs_path->text());
    if (normalized != m_defs_path->text()) {
      m_defs_path->setText(normalized);
    }
    reloadAll();
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

void CalvisionFersTab::loadSettings() {
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/fers");
  const QString path =
      settings.value("janusConfigFile", defaultConfigPath()).toString();
  const QString defs =
      settings.value("janusParamDefsFile", defaultDefsPath()).toString();
  const int boards = settings.value("boardColumns", 2).toInt();
  settings.endGroup();

  m_loading = true;
  if (m_path) {
    m_path->setText(normalizedConfigPath(path));
  }
  if (m_defs_path) {
    m_defs_path->setText(normalizedDefsPath(defs));
  }
  if (m_board_count) {
    m_board_count->setValue(std::max(1, std::min(kMaxBoards, boards)));
  }
  m_loading = false;

  reloadAll();
}

QString CalvisionFersTab::defaultConfigPath() const {
  return kDefaultConfigPath;
}

QString CalvisionFersTab::defaultDefsPath() const {
  return kDefaultJanusDefsPath;
}

QString CalvisionFersTab::normalizedConfigPath(const QString &path) const {
  const QString trimmed = path.trimmed();
  return trimmed.isEmpty() ? defaultConfigPath() : trimmed;
}

QString CalvisionFersTab::normalizedDefsPath(const QString &path) const {
  const QString trimmed = path.trimmed();
  return trimmed.isEmpty() ? defaultDefsPath() : trimmed;
}

QString CalvisionFersTab::configPath() const {
  return normalizedConfigPath(m_path ? m_path->text() : QString());
}

void CalvisionFersTab::setConfigPath(const QString &path) {
  if (m_path) {
    m_path->setText(normalizedConfigPath(path));
  }
  reloadAll();
  saveSettings();
}

bool CalvisionFersTab::reloadAll() {
  m_loading = true;
  clearForm();
  m_loading = false;

  if (!loadParamDefinitions()) {
    m_dirty = false;
    return false;
  }
  const bool ok = loadConfig();
  buildForm();
  m_dirty = false;
  return ok;
}

bool CalvisionFersTab::loadParamDefinitions() {
  const QString defs_path = normalizedDefsPath(m_defs_path ? m_defs_path->text()
                                                           : QString());
  QFile file(defs_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    m_params.clear();
    m_sections.clear();
    m_param_index.clear();
    m_values.clear();
    setStatus("Could not read Janus parameter definitions: " + defs_path);
    return false;
  }

  m_sections.clear();
  m_params.clear();
  m_param_index.clear();
  m_renames.clear();
  loadRenameFile(renamePathForDefs(defs_path));

  QTextStream in(&file);
  QString current_section;
  QString last_param;
  int blank_count = 0;
  int order = 0;
  while (!in.atEnd()) {
    const QString line = in.readLine();
    QString body;
    QString comment;
    splitCommentAware(line, &body, &comment);
    if (body.isEmpty()) {
      continue;
    }
    if (body.startsWith("[") && body.endsWith("]")) {
      current_section = body.mid(1, body.size() - 2).trimmed();
      if (!current_section.isEmpty() && !m_sections.contains(current_section)) {
        m_sections << current_section;
      }
      continue;
    }
    if (current_section.isEmpty()) {
      continue;
    }

    const QStringList tokens = splitJanusTokens(body);
    if (tokens.isEmpty()) {
      continue;
    }
    if (tokens.at(0) == "-" && tokens.size() >= 2 && !last_param.isEmpty()) {
      const auto found = m_param_index.find(last_param);
      if (found != m_param_index.end()) {
        m_params[found->second].options << tokens.at(1);
      }
      continue;
    }
    if (tokens.size() < 4) {
      continue;
    }
    if (current_section == "RunCtrl" &&
        isRunCtrlOutputFileParamName(tokens.at(0))) {
      last_param.clear();
      continue;
    }

    ParamDef param;
    param.name = tokens.at(0);
    if (param.name == "_BLANK") {
      param.name += QString::number(++blank_count);
    }
    param.default_value = tokens.at(1);
    param.section = current_section;
    param.scope = tokens.at(2);
    param.type = tokens.at(3);
    param.description = comment;
    param.order = order++;
    const auto rename = m_renames.find(tokens.at(0));
    param.display_name = rename == m_renames.end() ? tokens.at(0)
                                                   : rename->second;

    m_param_index[param.name] = static_cast<int>(m_params.size());
    m_params.push_back(param);
    last_param = param.name;
  }

  resetValuesFromDefinitions();
  return true;
}

bool CalvisionFersTab::loadRenameFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }
  QTextStream in(&file);
  while (!in.atEnd()) {
    const QString line = in.readLine().trimmed();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }
    const int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }
    const QString key = line.left(eq).trimmed();
    const QString value = line.mid(eq + 1).trimmed();
    if (!key.isEmpty() && !value.isEmpty()) {
      m_renames[key] = value;
    }
  }
  return true;
}

void CalvisionFersTab::resetValuesFromDefinitions() {
  m_values.clear();
  for (const ParamDef &param : m_params) {
    if (param.isSeparator() || param.isMonitor()) {
      continue;
    }
    ParamState state;
    state.default_value = param.default_value;
    state.board_values.assign(kMaxBoards, QString());
    state.channel_values.assign(
        kMaxBoards, std::vector<QString>(kMaxChannels, QString()));
    m_values[param.name] = state;
  }
}

bool CalvisionFersTab::loadConfig() {
  resetValuesFromDefinitions();
  m_open_lines.clear();
  m_load_files.clear();

  QFile file(configPath());
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    setStatus("Could not read FERS config: " + configPath() +
              ". Showing Janus defaults from param_defs.txt.");
    return false;
  }

  QTextStream in(&file);
  while (!in.atEnd()) {
    const QString line = in.readLine();
    QString key;
    QString value;
    QString comment;
    if (!parseParamLine(line, &key, &value, &comment)) {
      continue;
    }

    ParsedKey parsed;
    if (!parseIndexedKey(key, &parsed)) {
      continue;
    }

    if (parsed.base == "Load") {
      m_load_files << value;
      continue;
    }

    const auto param_it = m_param_index.find(parsed.base);
    if (param_it == m_param_index.end()) {
      continue;
    }
    const ParamDef &param = m_params[param_it->second];
    if (param.name == "Open" && parsed.has_board) {
      if (parsed.board >= 0 && parsed.board < kMaxBoards) {
        m_values[param.name].board_values[parsed.board] = value;
      }
      continue;
    }
    if (param.isSeparator() || param.isMonitor()) {
      continue;
    }

    ParamState &state = m_values[param.name];
    if (!parsed.has_board) {
      state.default_value = value;
    } else if (!parsed.has_channel) {
      if (parsed.board < 0 || parsed.board >= kMaxBoards) {
        continue;
      }
      if (param.isBoard()) {
        state.board_values[parsed.board] = value;
      } else {
        state.default_value = value;
      }
    } else if (param.isChannel()) {
      if (parsed.board >= 0 && parsed.board < kMaxBoards &&
          parsed.channel >= 0 && parsed.channel < kMaxChannels) {
        state.channel_values[parsed.board][parsed.channel] = value;
      }
    }
  }

  setStatus("Loaded Janus FERS config using definitions: " +
            QFileInfo(normalizedDefsPath(m_defs_path->text())).absoluteFilePath());
  return true;
}

void CalvisionFersTab::clearForm() {
  if (!m_tabs) {
    return;
  }
  m_channel_monitor_labels.clear();
  m_hv_monitor_labels.clear();
  m_hv_switches.clear();
  m_param_editors.clear();
  while (m_tabs->count() > 0) {
    QWidget *widget = m_tabs->widget(0);
    m_tabs->removeTab(0);
    delete widget;
  }
}

void CalvisionFersTab::buildForm() {
  if (!m_tabs) {
    return;
  }
  for (const QString &section : m_sections) {
    if (isRuntimeOnlySection(section) || !sectionHasEditableParams(section)) {
      continue;
    }
    buildSectionTab(section);
  }
  if (m_tabs->count() == 0) {
    auto empty = new QLabel("No editable Janus FERS parameters found.", m_tabs);
    empty->setWordWrap(true);
    m_tabs->addTab(empty, "FERS");
  }
}

void CalvisionFersTab::buildSectionTab(const QString &section) {
  auto page = new QWidget(m_tabs);
  auto layout = new QHBoxLayout(page);

  QWidget *global_panel = nullptr;
  if (section == "AcqMode") {
    global_panel = buildAcqModeGlobalPanel(page);
  } else if (section == "Test-Probe") {
    global_panel = buildTestProbeGlobalPanel(page);
  } else {
    global_panel = buildGlobalPanel(section, page);
  }
  layout->addWidget(global_panel, sectionHasBoardParams(section) ||
                                  sectionHasChannelParams(section) ? 1 : 2);

  if (sectionHasBoardParams(section) || sectionHasChannelParams(section)) {
    auto boards = new QTabWidget(page);
    for (int board = 0; board < boardCount(); ++board) {
      boards->addTab(buildBoardPanel(section, board, boards),
                     "B" + QString::number(board));
    }
    layout->addWidget(boards, 2);
  }

  m_tabs->addTab(page, section);
}

QWidget *CalvisionFersTab::buildGlobalPanel(const QString &section,
                                            QWidget *parent) {
  auto box = new QGroupBox("Global Settings", parent);
  auto layout = new QVBoxLayout(box);
  auto content = new QWidget(box);
  auto grid = new QGridLayout(content);
  grid->setColumnStretch(1, 1);
  grid->setHorizontalSpacing(12);
  grid->setVerticalSpacing(6);

  int row = 0;
  for (const ParamDef &param : m_params) {
    if (param.section != section || param.isMonitor()) {
      continue;
    }
    if (param.isSeparator()) {
      if (!param.name.startsWith("_BLANK")) {
        auto label = new QLabel(displayNameFor(param), content);
        label->setStyleSheet("font-weight: bold; text-decoration: underline;");
        grid->addWidget(label, row++, 0, 1, 2);
      } else {
        grid->setRowMinimumHeight(row++, 8);
      }
      continue;
    }

    const auto state_it = m_values.find(param.name);
    const QString value =
        state_it == m_values.end() ? param.default_value
                                   : state_it->second.default_value;
    auto label = new QLabel(displayNameFor(param), content);
    label->setToolTip(tooltipFor(param, false));
    grid->addWidget(label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(makeEditor(content, param, value, false, -1, -1), row, 1);
    ++row;
  }

  layout->addWidget(makeScrollPage(content, box));
  return box;
}

QWidget *CalvisionFersTab::buildAcqModeGlobalPanel(QWidget *parent) {
  auto box = new QGroupBox("AcqMode Settings", parent);
  auto layout = new QVBoxLayout(box);

  auto note = new QLabel(
      "Only settings used by the selected Acquisition Mode are shown. "
      "Switch Acquisition Mode to reveal that mode's settings; hidden values "
      "are still preserved in the saved config.",
      box);
  note->setWordWrap(true);
  layout->addWidget(note);

  auto content = new QWidget(box);
  auto grid = new QGridLayout(content);
  grid->setColumnStretch(1, 1);
  grid->setHorizontalSpacing(12);
  grid->setVerticalSpacing(6);

  int row = 0;
  auto add_separator = [&](const QString &text) {
    auto label = new QLabel(text, content);
    label->setStyleSheet("font-weight: bold; text-decoration: underline;");
    grid->addWidget(label, row++, 0, 1, 2);
  };

  auto add_param_row = [this](QGridLayout *grid,
                              QWidget *owner,
                              const QString &name,
                              int *row) {
    const auto param_it = m_param_index.find(name);
    if (param_it == m_param_index.end()) {
      return false;
    }
    const ParamDef &param = m_params[param_it->second];
    if (param.section != "AcqMode" || param.isSeparator() ||
        param.isMonitor() || !param.isGlobal()) {
      return false;
    }
    const auto state_it = m_values.find(param.name);
    const QString value =
        state_it == m_values.end() ? param.default_value
                                   : state_it->second.default_value;
    auto label = new QLabel(displayNameFor(param), owner);
    label->setToolTip(tooltipFor(param, false));
    grid->addWidget(label, *row, 0, Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(makeEditor(owner, param, value, false, -1, -1),
                    *row, 1);
    ++(*row);
    return true;
  };

  add_separator("Common Settings");
  add_param_row(grid, content, "AcquisitionMode", &row);
  for (const ParamDef &param : m_params) {
    if (param.section == "AcqMode" && param.isGlobal() &&
        param.name != "AcquisitionMode" &&
        isAcqModeCommonParam(param.name)) {
      add_param_row(grid, content, param.name, &row);
    }
  }

  QString mode = effectiveValue("AcquisitionMode", -1, -1).trimmed();
  if (mode.isEmpty()) {
    mode = "SPECTROSCOPY";
  }
  add_separator("Settings for " + mode);
  int mode_row_count = 0;
  for (const QString &name : acqModeSpecificParamNames(mode)) {
    if (add_param_row(grid, content, name, &row)) {
      ++mode_row_count;
    }
  }
  if (mode_row_count == 0) {
    grid->addWidget(new QLabel("No extra settings for this mode.", content),
                    row++, 0, 1, 2);
  }

  layout->addWidget(makeScrollPage(content, box));
  return box;
}

QWidget *CalvisionFersTab::buildTestProbeGlobalPanel(QWidget *parent) {
  auto box = new QGroupBox("Global Settings", parent);
  auto layout = new QVBoxLayout(box);
  auto content = new QWidget(box);
  auto grid = new QGridLayout(content);
  grid->setColumnStretch(1, 1);
  grid->setColumnStretch(3, 1);
  grid->setHorizontalSpacing(12);
  grid->setVerticalSpacing(6);

  constexpr int kFormColumns = 2;
  int row = 0;
  int column = 0;
  for (const ParamDef &param : m_params) {
    if (param.section != "Test-Probe" || param.isMonitor()) {
      continue;
    }
    if (param.isSeparator()) {
      if (column != 0) {
        ++row;
        column = 0;
      }
      if (!param.name.startsWith("_BLANK")) {
        auto label = new QLabel(displayNameFor(param), content);
        label->setStyleSheet("font-weight: bold; text-decoration: underline;");
        grid->addWidget(label, row++, 0, 1, kFormColumns * 2);
      } else {
        grid->setRowMinimumHeight(row++, 8);
      }
      continue;
    }

    const auto state_it = m_values.find(param.name);
    const QString value =
        state_it == m_values.end() ? param.default_value
                                   : state_it->second.default_value;
    const int base_column = column * 2;
    auto label = new QLabel(displayNameFor(param), content);
    label->setToolTip(tooltipFor(param, false));
    grid->addWidget(label, row, base_column,
                    Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(makeEditor(content, param, value, false, -1, -1),
                    row, base_column + 1);

    ++column;
    if (column == kFormColumns) {
      column = 0;
      ++row;
    }
  }

  layout->addWidget(makeScrollPage(content, box));
  return box;
}

QWidget *CalvisionFersTab::buildBoardPanel(const QString &section,
                                           int board,
                                           QWidget *parent) {
  auto page = new QWidget(parent);
  auto layout = new QVBoxLayout(page);

  if (section == "HV_bias") {
    layout->addWidget(buildHvMonitorPanel(board, page));
  }

  if (sectionHasBoardParams(section)) {
    auto box = new QGroupBox("Board Overrides", page);
    auto box_layout = new QVBoxLayout(box);
    auto content = new QWidget(box);
    auto grid = new QGridLayout(content);
    grid->setColumnStretch(1, 1);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);

    int row = 0;
    for (const ParamDef &param : m_params) {
      if (param.section != section || !param.isBoard() ||
          param.isSeparator() || param.isMonitor()) {
        continue;
      }
      const auto state_it = m_values.find(param.name);
      const QString value =
          state_it == m_values.end() ? QString()
                                     : state_it->second.board_values[board];
      auto label = new QLabel(displayNameFor(param), content);
      label->setToolTip(tooltipFor(param, true));
      grid->addWidget(label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
      grid->addWidget(makeEditor(content, param, value, true, board, -1),
                      row, 1);
      ++row;
    }
    if (row == 0) {
      grid->addWidget(new QLabel("No board-wise parameters in this section.",
                                 content), 0, 0, 1, 2);
    }
    box_layout->addWidget(makeScrollPage(content, box));
    layout->addWidget(box, sectionHasChannelParams(section) ? 1 : 2);
  }

  if (sectionHasChannelParams(section)) {
    auto groups = new QTabWidget(page);
    for (int first = 0; first < kMaxChannels; first += kChannelsPerGroup) {
      groups->addTab(buildChannelGroupPanel(section, board, first, groups),
                     QString("%1:%2").arg(first).arg(first + kChannelsPerGroup - 1));
    }
    layout->addWidget(groups, 3);
  }

  if (!sectionHasBoardParams(section) && !sectionHasChannelParams(section)) {
    layout->addWidget(new QLabel("No board or channel overrides.", page));
  }
  return page;
}

QWidget *CalvisionFersTab::buildChannelGroupPanel(const QString &section,
                                                  int board,
                                                  int first_channel,
                                                  QWidget *parent) {
  constexpr int kChannelsPerRow = 4;
  constexpr int kChannelRows =
      (kChannelsPerGroup + kChannelsPerRow - 1) / kChannelsPerRow;

  auto content = new QWidget(parent);
  auto grid = new QGridLayout(content);
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(6);
  for (int col = 1; col <= kChannelsPerRow; ++col) {
    grid->setColumnStretch(col, 1);
  }

  int row = 0;
  bool any_row = false;
  for (const ParamDef &param : m_params) {
    if (param.section != section || !param.isChannel() ||
        param.isSeparator() || param.isMonitor()) {
      continue;
    }
    for (int channel_row = 0; channel_row < kChannelRows; ++channel_row) {
      auto label = new QLabel(
          channel_row == 0 ? displayNameFor(param) : QString(), content);
      label->setToolTip(tooltipFor(param, true));
      grid->addWidget(label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
      for (int col = 0; col < kChannelsPerRow; ++col) {
        const int offset = channel_row * kChannelsPerRow + col;
        if (offset >= kChannelsPerGroup) {
          continue;
        }
        const int channel = first_channel + offset;
        const auto state_it = m_values.find(param.name);
        const QString value =
            state_it == m_values.end()
                ? QString()
                : state_it->second.channel_values[board][channel];
        auto cell = new QWidget(content);
        auto cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setSpacing(2);
        auto header = new QLabel(QString("CH %1").arg(channel), cell);
        header->setAlignment(Qt::AlignCenter);
        header->setStyleSheet("font-weight: bold;");
        cell_layout->addWidget(header);
        cell_layout->addWidget(
            makeEditor(cell, param, value, true, board, channel));
        grid->addWidget(cell, row, col + 1);
      }
      ++row;
    }
    any_row = true;
  }

  for (const ParamDef &param : m_params) {
    if (param.section != section || !param.isChannel() ||
        param.isSeparator() || !param.isMonitor()) {
      continue;
    }
    for (int channel_row = 0; channel_row < kChannelRows; ++channel_row) {
      auto label = new QLabel(
          channel_row == 0 ? displayNameFor(param) : QString(), content);
      label->setToolTip(tooltipFor(param, true));
      grid->addWidget(label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
      for (int col = 0; col < kChannelsPerRow; ++col) {
        const int offset = channel_row * kChannelsPerRow + col;
        if (offset >= kChannelsPerGroup) {
          continue;
        }
        const int channel = first_channel + offset;
        auto cell = new QWidget(content);
        auto cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setSpacing(2);
        auto header = new QLabel(QString("CH %1").arg(channel), cell);
        header->setAlignment(Qt::AlignCenter);
        header->setStyleSheet("font-weight: bold;");
        cell_layout->addWidget(header);
        auto value_label =
            new QLabel(monitorValueFor(param, board, channel), cell);
        value_label->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
        value_label->setAlignment(Qt::AlignCenter);
        value_label->setMinimumWidth(72);
        value_label->setToolTip(tooltipFor(param, true));
        cell_layout->addWidget(value_label);
        grid->addWidget(cell, row, col + 1);
        m_channel_monitor_labels[channelMonitorKey(param.name, board, channel)] =
            value_label;
      }
      ++row;
    }
    any_row = true;
  }

  if (!any_row) {
    grid->addWidget(new QLabel("No channel-wise parameters in this section.",
                               content), 0, 0, 1, kChannelsPerRow + 1);
  }
  return makeScrollPage(content, parent);
}

QWidget *CalvisionFersTab::buildHvMonitorPanel(int board, QWidget *parent) {
  auto box = new QGroupBox("HV Monitor", parent);
  auto grid = new QGridLayout(box);
  grid->setColumnStretch(1, 1);
  grid->setColumnStretch(3, 1);
  grid->setHorizontalSpacing(12);
  grid->setVerticalSpacing(6);

  struct Field {
    const char *key;
    const char *label;
    const char *unit;
    const char *tooltip;
  };
  const std::vector<Field> fields = {
      {"hv_set", "HV Set", "V", "Configured HV bias read back from FERS."},
      {"vmon", "Vmon", "V", "Measured board-level HV output voltage."},
      {"imon", "Imon Total", "mA",
       "Measured board-level HV output current. FERS exposes total HV "
       "current, not per-channel current."},
      {"det_temp", "Det Temp", "degC", "Detector temperature readback."},
      {"fpga_temp", "FPGA Temp", "degC", "FPGA temperature readback."},
      {"board_temp", "Brd Temp", "degC",
       "Board/PCB temperature readback near the PIC/FPGA."},
      {"status", "HV Status", "", "HV status bits from the FERS service event."},
  };

  const auto readback_it = m_hv_readbacks.find(board);
  auto value_for = [&](const Field &field) -> QString {
    if (readback_it == m_hv_readbacks.end()) {
      return "--";
    }
    const HvReadback &readback = readback_it->second;
    const QString key(field.key);
    if (key == "hv_set") {
      return withUnit(readback.hv_set, field.unit);
    }
    if (key == "vmon") {
      return withUnit(readback.vmon, field.unit);
    }
    if (key == "imon") {
      return withUnit(readback.imon, field.unit);
    }
    if (key == "det_temp") {
      return withUnit(readback.det_temp, field.unit);
    }
    if (key == "fpga_temp") {
      return withUnit(readback.fpga_temp, field.unit);
    }
    if (key == "board_temp") {
      return withUnit(readback.board_temp, field.unit);
    }
    if (key == "status") {
      return withUnit(readback.status, field.unit);
    }
    return "--";
  };

  int row = 0;
  auto hv_switch_label = new QLabel("HV Switch", box);
  hv_switch_label->setToolTip(
      "Turn this board's high voltage module ON or OFF. Disabled while the "
      "DAQ is running.");
  grid->addWidget(hv_switch_label, row, 0,
                  Qt::AlignRight | Qt::AlignVCenter);

  auto hv_switch = new QCheckBox("Unknown", box);
  hv_switch->setToolTip(hv_switch_label->toolTip());
  hv_switch->setEnabled(m_hv_controls_enabled);
  if (readback_it != m_hv_readbacks.end() &&
      isHvStatusKnown(readback_it->second.status)) {
    const bool on = isHvStatusOn(readback_it->second.status);
    hv_switch->setChecked(on);
    hv_switch->setText(on ? "ON" : "OFF");
  }
  connect(hv_switch, &QCheckBox::toggled, this, [this, board](bool checked) {
    auto switch_it = m_hv_switches.find(board);
    if (switch_it != m_hv_switches.end() && switch_it->second) {
      switch_it->second->setText(checked ? "ON" : "OFF");
    }
    if (m_hv_switch_callback) {
      m_hv_switch_callback(board, checked);
      setStatus(QString("Requested FERS B%1 HV %2.")
                    .arg(board)
                    .arg(checked ? "ON" : "OFF"));
    }
  });
  grid->addWidget(hv_switch, row, 1);
  m_hv_switches[board] = hv_switch;

  auto hv_status_label = new QLabel("HV Status", box);
  hv_status_label->setToolTip("HV status bits from the FERS service event.");
  grid->addWidget(hv_status_label, row, 2,
                  Qt::AlignRight | Qt::AlignVCenter);

  auto hv_status_value =
      new QLabel(value_for({"status", "HV Status", "",
                            "HV status bits from the FERS service event."}),
                 box);
  hv_status_value->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
  hv_status_value->setAlignment(Qt::AlignCenter);
  hv_status_value->setMinimumWidth(110);
  hv_status_value->setToolTip(hv_status_label->toolTip());
  grid->addWidget(hv_status_value, row, 3);
  m_hv_monitor_labels[hvMonitorKey(board, "status")] = hv_status_value;
  ++row;

  int field_index = 0;
  for (const Field &field : fields) {
    if (QString(field.key) == "status") {
      continue;
    }
    const int field_row = row + field_index / 2;
    const int base_column = (field_index % 2) * 2;
    auto name = new QLabel(field.label, box);
    name->setToolTip(field.tooltip);
    grid->addWidget(name, field_row, base_column,
                    Qt::AlignRight | Qt::AlignVCenter);

    auto value = new QLabel(value_for(field), box);
    value->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    value->setAlignment(Qt::AlignCenter);
    value->setMinimumWidth(110);
    value->setToolTip(field.tooltip);
    grid->addWidget(value, field_row, base_column + 1);
    m_hv_monitor_labels[hvMonitorKey(board, field.key)] = value;
    ++field_index;
  }

  return box;
}

QWidget *CalvisionFersTab::makeEditor(QWidget *parent,
                                      const ParamDef &param,
                                      const QString &value,
                                      bool override_value,
                                      int board,
                                      int channel) {
  QWidget *editor = nullptr;
  const QString tooltip = tooltipFor(param, override_value);

  if (param.type == "b") {
    if (override_value) {
      auto combo = new QComboBox(parent);
      combo->addItem("");
      combo->addItem("0");
      combo->addItem("1");
      combo->setCurrentText(value.trimmed());
      combo->setToolTip(tooltip);
      connect(combo,
              static_cast<void (QComboBox::*)(const QString&)>(
                  &QComboBox::currentTextChanged),
              this, [this, name = param.name, board, channel](const QString &text) {
                setParamValue(name, text.trimmed(), board, channel);
              });
      editor = combo;
    } else {
      auto check = new QCheckBox(parent);
      check->setChecked(value.trimmed() != "0" && !value.trimmed().isEmpty());
      check->setToolTip(tooltip);
      connect(check, &QCheckBox::stateChanged,
              this, [this, name = param.name](int state) {
                setParamValue(name, state == Qt::Checked ? "1" : "0", -1, -1);
              });
      editor = check;
    }
  } else if (!param.options.isEmpty()) {
    auto combo = new QComboBox(parent);
    combo->setEditable(true);
    if (override_value) {
      combo->addItem("");
    }
    combo->addItems(param.options);
    if (!value.trimmed().isEmpty() && combo->findText(value.trimmed()) < 0) {
      combo->insertItem(override_value ? 1 : 0, value.trimmed());
    }
    combo->setCurrentText(value.trimmed());
    for (int index = 0; index < combo->count(); ++index) {
      const QString item_tooltip =
          editorTooltipForValue(param.name, tooltip, combo->itemText(index));
      combo->setItemData(index, item_tooltip, Qt::ToolTipRole);
    }
    combo->setToolTip(editorTooltipForValue(param.name, tooltip,
                                            combo->currentText()));
    connect(combo,
            static_cast<void (QComboBox::*)(const QString&)>(
                &QComboBox::currentTextChanged),
            this, [this, name = param.name, board, channel](const QString &text) {
              setParamValue(name, text.trimmed(), board, channel);
            });
    connect(combo,
            static_cast<void (QComboBox::*)(const QString&)>(
                &QComboBox::currentTextChanged),
            this, [combo, name = param.name, tooltip](const QString &text) {
              combo->setToolTip(editorTooltipForValue(name, tooltip, text));
            });
    editor = combo;
  } else {
    auto edit = new QLineEdit(value.trimmed(), parent);
    edit->setMinimumWidth(120);
    if (override_value) {
      edit->setPlaceholderText("inherit");
    }
    edit->setToolTip(tooltip);
    connect(edit, &QLineEdit::textChanged,
            this, [this, name = param.name, board, channel](const QString &text) {
              setParamValue(name, text.trimmed(), board, channel);
            });
    editor = edit;
  }
  if (editor) {
    m_param_editors[paramEditorKey(param.name, board, channel)].push_back(editor);
  }
  return editor;
}

QWidget *CalvisionFersTab::makeScrollPage(QWidget *content,
                                          QWidget *parent) const {
  auto scroll = new QScrollArea(parent);
  scroll->setWidgetResizable(true);
  scroll->setWidget(content);
  return scroll;
}

QString CalvisionFersTab::editorValue(QWidget *editor,
                                      const ParamDef &param,
                                      bool override_value) const {
  if (!editor) {
    return QString();
  }
  if (auto combo = qobject_cast<QComboBox*>(editor)) {
    return combo->currentText().trimmed();
  }
  if (auto check = qobject_cast<QCheckBox*>(editor)) {
    if (override_value && check->checkState() == Qt::PartiallyChecked) {
      return QString();
    }
    return check->isChecked() ? "1" : "0";
  }
  if (auto edit = qobject_cast<QLineEdit*>(editor)) {
    return edit->text().trimmed();
  }
  Q_UNUSED(param);
  return QString();
}

QString CalvisionFersTab::normalizedValueForWrite(const ParamDef &param,
                                                  const QString &value) const {
  QString out = value.trimmed();
  if (param.type == "h" && !out.isEmpty() && !out.startsWith("0x", Qt::CaseInsensitive)) {
    out = "0x" + out;
  }
  return out;
}

QString CalvisionFersTab::formatConfigLine(const QString &key,
                                           const ParamDef &param,
                                           const QString &value) const {
  QString comment = param.description.trimmed();
  if (param.type == "c" && !param.options.isEmpty()) {
    if (!comment.isEmpty()) {
      comment += ". ";
    }
    comment += "Options: " + param.options.join(", ");
  }
  QString line = key.leftJustified(35, ' ') +
                 normalizedValueForWrite(param, value).leftJustified(20, ' ');
  if (!comment.isEmpty()) {
    line += " # " + comment;
  }
  return line;
}

bool CalvisionFersTab::saveConfig() {
  if (m_params.empty() || m_values.empty()) {
    QMessageBox::warning(this, "FERS",
                         "No Janus parameter definitions are loaded; "
                         "refusing to save.");
    return false;
  }

  for (const ParamDef &param : m_params) {
    if (param.isSeparator() || param.isMonitor() || param.name == "Open") {
      continue;
    }
    const auto state_it = m_values.find(param.name);
    if (state_it == m_values.end()) {
      continue;
    }
    if (state_it->second.default_value.split(" ").value(0).trimmed().isEmpty()) {
      QMessageBox::warning(this, "FERS",
                           "Default value for " + displayNameFor(param) +
                           " cannot be blank.");
      return false;
    }
  }

  QStringList lines;
  lines << "# ******************************************************************************************";
  lines << "# params File generated by EUDAQ FERS tab from Janus param_defs.txt";
  lines << "# ******************************************************************************************";
  lines << "# ------------------------------------------------------------------------------------------";
  lines << "# Connect";
  lines << "# ------------------------------------------------------------------------------------------";

  const auto open_it = m_values.find("Open");
  if (open_it != m_values.end()) {
    const ParamState &open = open_it->second;
    for (int board = 0; board < kMaxBoards; ++board) {
      const QString value = open.board_values[board].trimmed();
      if (!value.isEmpty()) {
        lines << indexedKey("Open", board, -1) + " " + value;
      }
    }
  }
  lines << "";
  lines << "";
  lines << "# ******************************************************************************************";
  lines << "# Common and Default settings";
  lines << "# ******************************************************************************************";
  lines << "";

  for (const QString &section : m_sections) {
    if (section == "Connect" || isRuntimeOnlySection(section)) {
      continue;
    }
    bool any = false;
    for (const ParamDef &param : m_params) {
      if (param.section == section && !param.isSeparator() &&
          !param.isMonitor()) {
        any = true;
        break;
      }
    }
    if (!any) {
      continue;
    }
    lines << "# ------------------------------------------------------------------------------------------";
    lines << "# " + section;
    lines << "# ------------------------------------------------------------------------------------------";
    for (const ParamDef &param : m_params) {
      if (param.section != section || param.isSeparator() ||
          param.isMonitor()) {
        continue;
      }
      const auto state_it = m_values.find(param.name);
      const QString value =
          state_it == m_values.end() ? param.default_value
                                     : state_it->second.default_value;
      lines << formatConfigLine(param.name, param, value);
    }
    lines << "";
  }

  lines << "";
  lines << "";
  lines << "# ******************************************************************************************";
  lines << "# Board and Channel settings (overwrite default settings)";
  lines << "# ******************************************************************************************";

  for (int board = 0; board < kMaxBoards; ++board) {
    for (const ParamDef &param : m_params) {
      if (!param.isBoard() || param.isSeparator() || param.isMonitor() ||
          param.name == "Open") {
        continue;
      }
      const auto state_it = m_values.find(param.name);
      if (state_it == m_values.end()) {
        continue;
      }
      const QString value = state_it->second.board_values[board].trimmed();
      if (!value.isEmpty()) {
        lines << formatConfigLine(indexedKey(param.name, board, -1),
                                  param, value);
      }
    }

    for (int channel = 0; channel < kMaxChannels; ++channel) {
      for (const ParamDef &param : m_params) {
        if (!param.isChannel() || param.isSeparator() || param.isMonitor()) {
          continue;
        }
        const auto state_it = m_values.find(param.name);
        if (state_it == m_values.end()) {
          continue;
        }
        const QString value =
            state_it->second.channel_values[board][channel].trimmed();
        if (!value.isEmpty()) {
          lines << formatConfigLine(indexedKey(param.name, board, channel),
                                    param, value);
        }
      }
    }
  }
  lines << "";

  if (!m_load_files.isEmpty()) {
    lines << "# ******************************************************************************************";
    lines << "# Additional Configuration Files (might overwrite GUI settings)";
    lines << "# ******************************************************************************************";
    for (const QString &load_file : m_load_files) {
      lines << QString("Load").leftJustified(35, ' ') + load_file;
    }
    lines << "";
  }

  QFile file(configPath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    QMessageBox::warning(this, "FERS",
                         "Could not write FERS config: " + configPath());
    return false;
  }

  QTextStream out(&file);
  for (const QString &line : lines) {
    out << line << "\n";
  }

  m_dirty = false;
  saveSettings();
  setStatus("Saved Janus-style FERS config: " +
            QFileInfo(configPath()).absoluteFilePath());
  return true;
}

void CalvisionFersTab::saveSettings() const {
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2/fers");
  settings.setValue("janusConfigFile", configPath());
  if (m_defs_path) {
    settings.setValue("janusParamDefsFile",
                      normalizedDefsPath(m_defs_path->text()));
  }
  if (m_board_count) {
    settings.setValue("boardColumns", m_board_count->value());
  }
  settings.endGroup();
}

QString CalvisionFersTab::tooltipFor(const ParamDef &param,
                                     bool override_value) const {
  QString text = param.description.trimmed();
  if (!param.options.isEmpty()) {
    if (!text.isEmpty()) {
      text += "\n";
    }
    text += "Options: " + param.options.join(", ");
  }
  if (override_value) {
    if (!text.isEmpty()) {
      text += "\n\n";
    }
    text += "Blank means inherit the Global Settings value.";
  }
  return text;
}

QString CalvisionFersTab::displayNameFor(const ParamDef &param) const {
  return param.display_name.isEmpty() ? param.name : param.display_name;
}

QString CalvisionFersTab::effectiveValue(const QString &name,
                                         int board,
                                         int channel) const {
  const auto param_it = m_param_index.find(name);
  if (param_it == m_param_index.end()) {
    return QString();
  }
  const ParamDef &param = m_params[param_it->second];
  const auto state_it = m_values.find(name);
  if (state_it == m_values.end()) {
    return param.default_value;
  }
  const ParamState &state = state_it->second;

  if (channel >= 0 && board >= 0 && board < kMaxBoards &&
      channel < kMaxChannels) {
    const QString channel_value =
        state.channel_values[board][channel].trimmed();
    if (!channel_value.isEmpty()) {
      return channel_value;
    }
  }
  if (board >= 0 && board < kMaxBoards) {
    const QString board_value = state.board_values[board].trimmed();
    if (!board_value.isEmpty()) {
      return board_value;
    }
  }
  return state.default_value;
}

QString CalvisionFersTab::monitorValueFor(const ParamDef &param,
                                          int board,
                                          int channel) const {
  if (param.name == "Vnom") {
    return vnomValue(board, channel);
  }
  return "--";
}

QString CalvisionFersTab::vnomValue(int board, int channel) const {
  double vbias = 0.0;
  QString unit = "V";
  if (!splitValueAndUnit(effectiveValue("HV_Vbias", board, -1),
                         &vbias, &unit)) {
    return "--";
  }

  const QString range =
      effectiveValue("HV_Adjust_Range", -1, -1).trimmed().toUpper();
  double dac_fs = 4.2;
  if (range == "2.5") {
    dac_fs = 2.5;
  } else if (range == "DISABLED") {
    dac_fs = 0.0;
  }

  bool ok = false;
  const int dac = effectiveValue("HV_IndivAdj", board, channel)
                      .trimmed()
                      .toInt(&ok);
  if (!ok) {
    return "--";
  }

  const double vnom = vbias - dac_fs * static_cast<double>(255 - dac) / 255.0;
  return QString::number(vnom, 'f', 2) + " " + unit;
}

int CalvisionFersTab::boardCount() const {
  return m_board_count ? m_board_count->value() : 2;
}

bool CalvisionFersTab::sectionHasEditableParams(const QString &section) const {
  for (const ParamDef &param : m_params) {
    if (param.section == section && !param.isSeparator() &&
        !param.isMonitor()) {
      return true;
    }
  }
  return false;
}

bool CalvisionFersTab::sectionHasBoardParams(const QString &section) const {
  for (const ParamDef &param : m_params) {
    if (param.section == section && param.isBoard() &&
        !param.isSeparator() && !param.isMonitor()) {
      return true;
    }
  }
  return false;
}

bool CalvisionFersTab::sectionHasChannelParams(const QString &section) const {
  for (const ParamDef &param : m_params) {
    if (param.section == section && param.isChannel() &&
        !param.isSeparator()) {
      return true;
    }
  }
  return false;
}

void CalvisionFersTab::updateHvReadbacks(
    const std::map<int, HvReadback> &readbacks) {
  m_hv_readbacks = readbacks;
  updateHvMonitorLabels();
  updateHvSwitches();
}

void CalvisionFersTab::setHvMonitorUpdateCallback(
    std::function<void()> callback) {
  m_hv_update_callback = std::move(callback);
}

void CalvisionFersTab::setHvSwitchCallback(
    std::function<void(int, bool)> callback) {
  m_hv_switch_callback = std::move(callback);
}

void CalvisionFersTab::setHvMonitorUpdateEnabled(bool enabled) {
  if (m_update_hv_monitor) {
    m_update_hv_monitor->setEnabled(enabled);
  }
}

void CalvisionFersTab::setHvControlsEnabled(bool enabled) {
  m_hv_controls_enabled = enabled;
  for (auto &entry : m_hv_switches) {
    if (entry.second) {
      entry.second->setEnabled(enabled);
    }
  }
}

void CalvisionFersTab::updateVnomLabels() {
  for (auto &entry : m_channel_monitor_labels) {
    const QStringList parts = entry.first.split(':');
    if (parts.size() != 3 || parts.at(0) != "Vnom" || !entry.second) {
      continue;
    }
    bool board_ok = false;
    bool channel_ok = false;
    const int board = parts.at(1).toInt(&board_ok);
    const int channel = parts.at(2).toInt(&channel_ok);
    if (!board_ok || !channel_ok) {
      continue;
    }
    entry.second->setText(vnomValue(board, channel));
  }
}

void CalvisionFersTab::updateHvMonitorLabels() {
  for (auto &entry : m_hv_monitor_labels) {
    const QStringList parts = entry.first.split(':');
    if (parts.size() != 2 || !entry.second) {
      continue;
    }
    bool board_ok = false;
    const int board = parts.at(0).toInt(&board_ok);
    if (!board_ok) {
      continue;
    }
    const QString field = parts.at(1);
    const auto readback_it = m_hv_readbacks.find(board);
    if (readback_it == m_hv_readbacks.end()) {
      entry.second->setText("--");
      continue;
    }
    const HvReadback &readback = readback_it->second;
    if (field == "hv_set") {
      entry.second->setText(withUnit(readback.hv_set, "V"));
    } else if (field == "vmon") {
      entry.second->setText(withUnit(readback.vmon, "V"));
    } else if (field == "imon") {
      entry.second->setText(withUnit(readback.imon, "mA"));
    } else if (field == "det_temp") {
      entry.second->setText(withUnit(readback.det_temp, "degC"));
    } else if (field == "fpga_temp") {
      entry.second->setText(withUnit(readback.fpga_temp, "degC"));
    } else if (field == "board_temp") {
      entry.second->setText(withUnit(readback.board_temp, "degC"));
    } else if (field == "status") {
      entry.second->setText(withUnit(readback.status, ""));
    }
  }
}

void CalvisionFersTab::updateHvSwitches() {
  for (auto &entry : m_hv_switches) {
    QCheckBox *hv_switch = entry.second;
    if (!hv_switch) {
      continue;
    }
    const auto readback_it = m_hv_readbacks.find(entry.first);
    if (readback_it == m_hv_readbacks.end() ||
        !isHvStatusKnown(readback_it->second.status)) {
      const bool was_blocked = hv_switch->blockSignals(true);
      hv_switch->setText("Unknown");
      hv_switch->blockSignals(was_blocked);
      continue;
    }
    const bool on = isHvStatusOn(readback_it->second.status);
    const bool was_blocked = hv_switch->blockSignals(true);
    hv_switch->setChecked(on);
    hv_switch->setText(on ? "ON" : "OFF");
    hv_switch->blockSignals(was_blocked);
  }
}

void CalvisionFersTab::syncParamEditors(const QString &name,
                                        const QString &value,
                                        int board,
                                        int channel) {
  const auto editors_it = m_param_editors.find(paramEditorKey(name, board, channel));
  if (editors_it == m_param_editors.end()) {
    return;
  }

  const QString trimmed = value.trimmed();
  for (QWidget *editor : editors_it->second) {
    if (!editor) {
      continue;
    }
    const bool was_blocked = editor->blockSignals(true);
    if (auto combo = qobject_cast<QComboBox*>(editor)) {
      combo->setCurrentText(trimmed);
    } else if (auto check = qobject_cast<QCheckBox*>(editor)) {
      check->setChecked(trimmed != "0" && !trimmed.isEmpty());
    } else if (auto edit = qobject_cast<QLineEdit*>(editor)) {
      edit->setText(trimmed);
    }
    editor->blockSignals(was_blocked);
  }
}

void CalvisionFersTab::setParamValue(const QString &name,
                                     const QString &value,
                                     int board,
                                     int channel) {
  if (m_loading) {
    return;
  }
  auto state_it = m_values.find(name);
  if (state_it == m_values.end()) {
    return;
  }
  ParamState &state = state_it->second;
  if (board < 0) {
    state.default_value = value;
  } else if (channel < 0) {
    if (board >= 0 && board < kMaxBoards) {
      state.board_values[board] = value;
    }
  } else if (board >= 0 && board < kMaxBoards &&
             channel >= 0 && channel < kMaxChannels) {
    state.channel_values[board][channel] = value;
  }
  syncParamEditors(name, value, board, channel);
  if (name == "HV_Vbias" || name == "HV_Adjust_Range" ||
      name == "HV_IndivAdj") {
    updateVnomLabels();
  }
  if (name == "AcquisitionMode" && !m_rebuild_pending) {
    m_rebuild_pending = true;
    QTimer::singleShot(0, this, [this]() {
      m_rebuild_pending = false;
      clearForm();
      buildForm();
    });
  }
  markDirty();
}

void CalvisionFersTab::markDirty() {
  if (m_loading) {
    return;
  }
  m_dirty = true;
  setStatus("Modified. Save before Init or click Save.");
}

void CalvisionFersTab::setStatus(const QString &text) {
  if (m_status) {
    m_status->setText(text);
  }
}
