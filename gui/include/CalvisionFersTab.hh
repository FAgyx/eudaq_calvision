#ifndef INCLUDED_CalvisionFersTab_hh
#define INCLUDED_CalvisionFersTab_hh

#include <QWidget>

#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <vector>

class QLabel;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

class CalvisionFersTab : public QWidget {
public:
  struct HvReadback {
    QString hv_set;
    QString vmon;
    QString imon;
    QString det_temp;
    QString fpga_temp;
    QString board_temp;
    QString status;
  };

  explicit CalvisionFersTab(QWidget *parent = nullptr);

  QString configPath() const;
  void setConfigPath(const QString &path);
  bool saveConfig();
  void saveSettings() const;
  void updateHvReadbacks(const std::map<int, HvReadback> &readbacks);
  void setHvMonitorUpdateCallback(std::function<void()> callback);
  void setHvSwitchCallback(std::function<void(int, bool)> callback);
  void setHvMonitorUpdateEnabled(bool enabled);
  void setHvControlsEnabled(bool enabled);

private:
  static constexpr int kMaxBoards = 16;
  static constexpr int kMaxChannels = 64;
  static constexpr int kChannelsPerGroup = 8;

  struct ParamDef {
    QString name;
    QString default_value;
    QString section;
    QString scope;
    QString type;
    QString description;
    QString display_name;
    QStringList options;
    int order = 0;

    bool isSeparator() const { return type == "-"; }
    bool isMonitor() const { return type == "m"; }
    bool isGlobal() const { return scope == "g"; }
    bool isBoard() const { return scope == "b"; }
    bool isChannel() const { return scope == "c"; }
  };

  struct ParamState {
    QString default_value;
    std::vector<QString> board_values;
    std::vector<std::vector<QString>> channel_values;
  };

  void setupUi();
  void loadSettings();
  bool reloadAll();
  bool loadParamDefinitions();
  bool loadConfig();
  bool loadRenameFile(const QString &path);
  void resetValuesFromDefinitions();
  void clearForm();
  void buildForm();
  void buildSectionTab(const QString &section);
  QWidget *buildGlobalPanel(const QString &section, QWidget *parent);
  QWidget *buildAcqModeGlobalPanel(QWidget *parent);
  QWidget *buildTestProbeGlobalPanel(QWidget *parent);
  QWidget *buildBoardPanel(const QString &section, int board, QWidget *parent);
  QWidget *buildChannelGroupPanel(const QString &section,
                                  int board,
                                  int first_channel,
                                  QWidget *parent);
  QWidget *buildHvMonitorPanel(int board, QWidget *parent);
  QWidget *makeEditor(QWidget *parent,
                      const ParamDef &param,
                      const QString &value,
                      bool override_value,
                      int board,
                      int channel);
  QWidget *makeScrollPage(QWidget *content, QWidget *parent) const;
  QString normalizedConfigPath(const QString &path) const;
  QString normalizedDefsPath(const QString &path) const;
  QString defaultConfigPath() const;
  QString defaultDefsPath() const;
  QString editorValue(QWidget *editor, const ParamDef &param,
                      bool override_value) const;
  QString formatConfigLine(const QString &key,
                           const ParamDef &param,
                           const QString &value) const;
  QString normalizedValueForWrite(const ParamDef &param,
                                  const QString &value) const;
  QString displayNameFor(const ParamDef &param) const;
  QString tooltipFor(const ParamDef &param, bool override_value) const;
  QString effectiveValue(const QString &name, int board, int channel) const;
  QString monitorValueFor(const ParamDef &param, int board, int channel) const;
  QString vnomValue(int board, int channel) const;
  int boardCount() const;
  bool sectionHasEditableParams(const QString &section) const;
  bool sectionHasBoardParams(const QString &section) const;
  bool sectionHasChannelParams(const QString &section) const;
  void updateVnomLabels();
  void updateHvMonitorLabels();
  void updateHvSwitches();
  void syncParamEditors(const QString &name,
                        const QString &value,
                        int board,
                        int channel);
  void setParamValue(const QString &name,
                     const QString &value,
                     int board,
                     int channel);
  void markDirty();
  void setStatus(const QString &text);

  QLineEdit *m_path;
  QLineEdit *m_defs_path;
  QSpinBox *m_board_count;
  QPushButton *m_update_hv_monitor;
  QTabWidget *m_tabs;
  QLabel *m_status;
  bool m_loading;
  bool m_dirty;
  bool m_hv_controls_enabled;
  bool m_rebuild_pending;
  QStringList m_sections;
  QStringList m_open_lines;
  QStringList m_load_files;
  std::vector<ParamDef> m_params;
  std::map<QString, int> m_param_index;
  std::map<QString, ParamState> m_values;
  std::map<QString, QString> m_renames;
  std::map<QString, std::vector<QWidget*>> m_param_editors;
  std::map<QString, QLabel*> m_channel_monitor_labels;
  std::map<QString, QLabel*> m_hv_monitor_labels;
  std::map<int, QCheckBox*> m_hv_switches;
  std::map<int, HvReadback> m_hv_readbacks;
  std::function<void()> m_hv_update_callback;
  std::function<void(int, bool)> m_hv_switch_callback;
};

#endif
