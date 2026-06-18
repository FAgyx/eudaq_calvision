#ifndef INCLUDED_CalvisionDeviceTab_hh
#define INCLUDED_CalvisionDeviceTab_hh

#include <QWidget>

#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

class QLabel;
class QCheckBox;
class QLineEdit;
class QProcess;
class QSpinBox;
class QTableWidget;

namespace eudaq {
class RunControl;
}

class CalvisionDeviceTab : public QWidget {
public:
  struct FersReadback {
    QString pid;
    QString model;
    QString fpga_fw;
    QString uc_fw;
    QString summary;
  };

  explicit CalvisionDeviceTab(QWidget *parent = nullptr);
  ~CalvisionDeviceTab() override;

  bool prepareForInit(eudaq::RunControl *rc,
                      QLineEdit *init_file,
                      QLineEdit *conf_file,
                      const QString &conf_template_override = QString(),
                      const QString &fers_config_override = QString(),
                      const QString &drs_config_override = QString());
  void saveSettings() const;
  void terminateOwnedProcesses();
  void updateFersReadbacks(const QMap<QString, FersReadback> &readbacks);

private:
  void setupUi();
  void loadSettings();
  void resizeFersRows(int rows);
  void resizeDrsRows(int rows);
  bool writeGeneratedFiles(const QString &conf_template_path,
                           const QString &fers_config_path,
                           const QString &drs_config_path,
                           QString *init_path,
                           QString *conf_path) const;
  bool launchConfiguredProducers(eudaq::RunControl *rc);
  bool startProducer(eudaq::RunControl *rc,
                     const QString &producer_type,
                     const QString &producer_name);
  bool waitForConnections(eudaq::RunControl *rc,
                          const QStringList &producer_names,
                          int timeout_ms);
  bool terminateUnwantedProducers(eudaq::RunControl *rc,
                                  const QStringList &wanted_names);
  QString euCliProducerPath() const;
  QString launchLogDir() const;
  QStringList configuredProducerNames() const;

  QCheckBox *m_enabled;
  QSpinBox *m_fers_count;
  QSpinBox *m_drs_count;
  QTableWidget *m_fers_table;
  QTableWidget *m_drs_table;
  QLabel *m_status;
  bool m_loading_settings;
  std::vector<QProcess*> m_processes;
};

#endif
