#include "ui_euRun.h"
#include "RunControlModel.hh"
#include "scanHelper.hh"

#include <QFileDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QCloseEvent>
#include <QItemDelegate>
#include <QDir>
#include <QPainter>
#include <QTimer>
#include <QInputDialog>
#include <QSettings>
#include <QString>
#include <QGridLayout>

class CalvisionDeviceTab;
class CalvisionConfTab;
class CalvisionDrsTab;
class CalvisionFersTab;
class QTabWidget;


class RunControlGUI : public QMainWindow,
		      public Ui::wndRun{



   Q_OBJECT
public:
  RunControlGUI();
  void SetInstance(eudaq::RunControlUP rc);
  void Exec();
private slots:
  void on_checkBox_stateChanged(int arg1);

private:
  void closeEvent(QCloseEvent *event) override;
			
private slots:
  void DisplayTimer();  
  void on_btnInit_clicked();
  void on_btnConfig_clicked();
  void on_btnStart_clicked();
  void on_btnSetRunNumber_clicked();
  void on_txtNextRunNumber_returnPressed();
  void on_btnStop_clicked();
  void on_btnReset_clicked();
  void on_btnTerminate_clicked();
  void on_btnLog_clicked();
  void on_btnLoadDataPath_clicked();
  void onCustomContextMenu(const QPoint &point);

  void on_btn_LoadScanFile_clicked();
  void on_btnStartScan_clicked();
  void nextStep();

private:
  eudaq::Status::State updateInfos();
  bool loadInitFile();
  bool loadConfigFile();
  bool addStatusDisplay(std::pair<eudaq::ConnectionSPC, eudaq::StatusSPC> connection);
  bool removeStatusDisplay(std::pair<eudaq::ConnectionSPC, eudaq::StatusSPC> connection);
  bool updateStatusDisplay();
  bool addToGrid(const QString &objectName, QString displayedName="");
  bool addAdditionalStatus(std::string info);
  bool checkFile(QString file, QString usecase);
  bool applyManualRunNumber(bool warn_if_empty);

  bool readScanConfig();
  bool allConnectionsInState(eudaq::Status::State state);
  bool checkEventsInStep();
  int getEventsCurrent();
  void store_config();
  QString getSelectedDataPath() const;
  QString getRunDirectory(uint32_t run_n) const;
  void ensureDirectoryExists(const QString &path) const;
  void applyOutputPathToInitConfig();
  void applyOutputPathToRunConfig();
  void applyDrsPayloadModeToRunConfig();
  void setupDevicesTab();
  void requestFersHvMonitorUpdate();
  void requestFersHvSwitch(int board, bool on);
  void refreshConfiguredOutputTargets();
  void saveConfigurationSnapshot(const QString &path,
                                 eudaq::ConfigurationSPC conf) const;
  static std::map<int, QString> m_map_state_str;
  std::map<QString, QString> m_map_label_str;
  eudaq::RunControlUP m_rc;
  RunControlModel m_model_conns;
  QItemDelegate m_delegate;
  QTimer m_timer_display;
  std::map<QString, QLabel*> m_str_label;
  std::map<eudaq::ConnectionSPC, eudaq::StatusSPC> m_map_conn_status_last;
  uint32_t m_run_n_qsettings;
  int m_display_col, m_display_row;
  QMenu* contextMenu;
  bool m_lastexit_success;
  eudaq::LogSender m_log;
  bool m_scan_active;
  bool m_scan_interrupt_received;
  bool m_save_config_at_run_start;
  QTimer m_scanningTimer;
  std::shared_ptr<eudaq::Configuration> m_scan_config;
  Scan m_scan;
  QTabWidget *m_main_tabs;
  CalvisionConfTab *m_conf_tab;
  CalvisionDeviceTab *m_device_tab;
  CalvisionDrsTab *m_drs_tab;
  CalvisionFersTab *m_fers_tab;

  void updateProgressBar();
};
