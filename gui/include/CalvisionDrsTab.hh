#ifndef INCLUDED_CalvisionDrsTab_hh
#define INCLUDED_CalvisionDrsTab_hh

#include <QWidget>

#include <QString>
#include <QStringList>

#include <map>
#include <vector>

class QLabel;
class QLineEdit;
class QSpinBox;
class QTabWidget;

class CalvisionDrsTab : public QWidget {
public:
  explicit CalvisionDrsTab(QWidget *parent = nullptr);

  QString configPath() const;
  void setConfigPath(const QString &path);
  bool saveConfig();
  void saveSettings() const;

  static constexpr int kMaxBoards = 16;

  struct ParamDef {
    QString section;
    QString key;
    QString display;
    QString tooltip;
    QStringList options;
    bool optional = false;
    int order = 0;
  };

private:
  struct ParamState {
    QString default_value;
    std::vector<QString> board_values;
  };

  void setupUi();
  void loadSettings();
  bool loadConfig();
  void clearForm();
  void buildDefinitions();
  void buildForm();
  QWidget *buildSectionTab(const QString &section);
  QWidget *makeEditor(QWidget *parent,
                      const ParamDef &param,
                      const QString &value,
                      bool board_override,
                      int board);
  QWidget *makeScrollPage(QWidget *content, QWidget *parent) const;
  QString normalizedConfigPath(const QString &path) const;
  QString defaultConfigPath() const;
  QString stateKey(const QString &section, const QString &key) const;
  QString editorTooltip(const ParamDef &param, bool board_override) const;
  QString formatConfigLine(const QString &key, const QString &value) const;
  void setParamValue(const QString &section,
                     const QString &key,
                     const QString &value,
                     int board);
  void markDirty();
  void setStatus(const QString &text);
  int boardCount() const;

  QLineEdit *m_path;
  QSpinBox *m_board_count;
  QTabWidget *m_tabs;
  QLabel *m_status;
  bool m_loading;
  bool m_dirty;
  QStringList m_sections;
  std::vector<ParamDef> m_params;
  std::map<QString, ParamState> m_values;
};

#endif
