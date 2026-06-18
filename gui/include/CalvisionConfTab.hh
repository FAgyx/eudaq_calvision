#ifndef INCLUDED_CalvisionConfTab_hh
#define INCLUDED_CalvisionConfTab_hh

#include <QWidget>

#include <QString>

#include <map>
#include <vector>

class QFormLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

class CalvisionConfTab : public QWidget {
public:
  explicit CalvisionConfTab(QWidget *parent = nullptr);

  QString templatePath() const;
  void setTemplatePath(const QString &path);
  bool saveTemplate();
  void saveSettings() const;

private:
  struct ConfigRecord {
    QString section;
    QString key;
    QString value;
    QString tooltip;
    int entry_index = -1;
    bool hidden = false;
  };

  struct ConfigEntry {
    QString display_section;
    QString key;
    QLineEdit *value;
    std::vector<int> records;
  };

  void setupUi();
  void loadSettings();
  bool loadTemplate();
  void clearForm();
  void buildForm();
  QFormLayout *ensureSection(const QString &section);
  int addConfigEntry(const QString &display_section,
                     const QString &key,
                     const QString &value,
                     const QString &tooltip,
                     const std::vector<int> &records);
  QString normalizedTemplatePath(const QString &path) const;
  bool isGeneratedDeviceConfig(const QString &path) const;
  QString defaultTemplatePath() const;
  void setStatus(const QString &text);

  QLineEdit *m_path;
  QScrollArea *m_scroll;
  QVBoxLayout *m_form_layout;
  QLabel *m_status;
  bool m_loading;
  bool m_dirty;
  std::map<QString, QFormLayout*> m_section_forms;
  std::vector<ConfigRecord> m_records;
  std::vector<ConfigEntry> m_entries;
};

#endif
