#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QLabel>
#include <QTabWidget>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include "euRun.hh"
#include "CalvisionConfTab.hh"
#include "CalvisionDeviceTab.hh"
#include "CalvisionDrsTab.hh"
#include "CalvisionFersTab.hh"
#include "Colours.hh"
#include "eudaq/FileNamer.hh"
#include "eudaq/Config.hh"

using std::cout;
using std::endl;

namespace {
QString patternFileName(const std::string &pattern, const QString &fallback) {
  QFileInfo info(QString::fromStdString(pattern));
  QString filename = info.fileName();
  return filename.isEmpty() ? fallback : filename;
}

struct StatusGridTag {
  const char *tag;
  const char *label;
};

constexpr int kStatusPairsPerRow = 3;

const std::vector<StatusGridTag> kBuilderStatusTags = {
    {"_SERVER", "dc_SERVER"},
    {"FastBuilder", "Builder Enabled"},
    {"FastBuilderN", "Builders"},
    {"FastCompleteN", "Built Events"},
    {"FastIncompleteN", "Incomplete Events"},
    {"FastDuplicateN", "Duplicate Fragments"},
    {"FastQueueDepth", "Builder Queue"},
    {"FastQueueFullN", "Queue Full"},
    {"FastWriterQueueMB", "Writer Queue MB"},
    {"FastWriterQueueFullN", "Writer Queue Full"},
    {"FastEnqueueAvgUs", "Enqueue Avg us"},
    {"FastEnqueueMaxUs", "Enqueue Max us"},
    {"FastSerializeAvgUs", "Serialize Avg us"},
    {"FastSerializeMaxUs", "Serialize Max us"},
    {"FastWriteAvgUs", "Write Avg us"},
    {"FastWriteMaxUs", "Write Max us"},
    {"FastFileMB", "Output MB"},
    {"FastRouteDropN", "Route Drops"}
};
}

RunControlGUI::RunControlGUI()
  : QMainWindow(nullptr),
    m_display_col(0),
    m_scan_active(false),
    m_scan_interrupt_received(false),
    m_save_config_at_run_start(true),
    m_display_row(0),
    m_main_tabs(nullptr),
    m_conf_tab(nullptr),
    m_device_tab(nullptr),
    m_drs_tab(nullptr),
    m_fers_tab(nullptr){
    m_map_label_str = {{"RUN", "Run Number"}};
    qRegisterMetaType<QModelIndex>("QModelIndex");
    setupUi(this);
    setupDevicesTab();

    lblInit->hide();
    txtInitFileName->hide();
    btnLoadInit->hide();
    lblConfig->hide();
    txtConfigFileName->hide();
    btnLoadConf->hide();
    txtInitFileName->clear();
    txtConfigFileName->clear();
    if (auto grid = qobject_cast<QGridLayout*>(grpControl->layout())) {
      grid->removeWidget(lblInit);
      grid->removeWidget(txtInitFileName);
      grid->removeWidget(btnLoadInit);
      grid->removeWidget(lblConfig);
      grid->removeWidget(txtConfigFileName);
      grid->removeWidget(btnLoadConf);
      grid->removeWidget(btnInit);
      grid->removeWidget(btnConfig);
      grid->addWidget(btnInit, 0, 3);
      grid->addWidget(btnConfig, 0, 4);
    }

  lblCurrent->setText(m_map_state_str.at(eudaq::Status::STATE_UNINIT));
  for(auto &label_str: m_map_label_str) {
    QLabel *lblname = new QLabel(grpStatus);
    lblname->setObjectName("lbl_st_" + label_str.first);
    lblname->setText(label_str.second + ": ");
    QLabel *lblvalue = new QLabel(grpStatus);
    lblvalue->setObjectName("txt_st_" + label_str.first);
    grpGrid->addWidget(lblname, m_display_row, m_display_col * 2);
    grpGrid->addWidget(lblvalue, m_display_row, m_display_col * 2 + 1);
    m_str_label[label_str.first] = lblvalue;
    if (++m_display_col >= kStatusPairsPerRow){
      ++m_display_row;
      m_display_col = 0;
    }
  }

  viewConn->setModel(&m_model_conns);
  viewConn->setItemDelegate(&m_delegate);

  viewConn->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(viewConn, SIGNAL(customContextMenuRequested(const QPoint &)),
          this, SLOT(onCustomContextMenu(const QPoint &)));

  QRect geom(-1,-1, 150, 200);
  QRect geom_from_last_program_run;
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2");
  m_run_n_qsettings = settings.value("runnumber", 0).toUInt();
  m_lastexit_success = settings.value("successexit", 1).toUInt();
  geom_from_last_program_run.setSize(settings.value("size", geom.size()).toSize());
  geom_from_last_program_run.moveTo(settings.value("pos", geom.topLeft()).toPoint());
  txtScanFile
    ->setText(settings.value("lastScanFile", "scan file not set").toString());
  txtDataPath
    ->setText(settings.value("lastDataPath", QDir::currentPath()).toString());
  comboDrsPayloadMode->clear();
  comboDrsPayloadMode->addItem("Decoded DRS", "decoded");
  comboDrsPayloadMode->addItem("Compact DRS", "compact");
  QString drs_payload_mode =
      settings.value("drsPayloadMode", "decoded").toString().toLower();
  int drs_payload_index = comboDrsPayloadMode->findData(drs_payload_mode);
  if (drs_payload_index < 0) {
    drs_payload_index = 0;
  }
  comboDrsPayloadMode->setCurrentIndex(drs_payload_index);

  settings.endGroup();

  QSize fsize = frameGeometry().size();
  if((geom.x() == -1)||(geom.y() == -1)||(geom.width() == -1)||(geom.height() == -1)) {
    if((geom_from_last_program_run.x() == -1)||(geom_from_last_program_run.y() == -1)||(geom_from_last_program_run.width() == -1)||(geom_from_last_program_run.height() == -1)) {
      geom.setX(x());
      geom.setY(y());
      geom.setWidth(fsize.width());
      geom.setHeight(fsize.height());
      move(geom.topLeft());
      resize(geom.size());
    } else {
      move(geom_from_last_program_run.topLeft());
      resize(geom_from_last_program_run.size());
    }
  }

  setWindowTitle("eudaq Run Control " PACKAGE_VERSION);
  connect(&m_timer_display, SIGNAL(timeout()), this, SLOT(DisplayTimer()));
  connect(&m_scanningTimer,SIGNAL(timeout()), this, SLOT(nextStep()));
  m_timer_display.start(1000); // internal update time of GUI
  btnInit->setEnabled(1);
  btnConfig->setEnabled(0);
  btnStart->setEnabled(1);
  btnStop->setEnabled(1);
  btnReset->setEnabled(1);
  btnTerminate->setEnabled(1);
  btnLog->setEnabled(1);

  QSettings settings_output("EUDAQ collaboration", "EUDAQ");
  settings_output.beginGroup("euRun2");
  settings_output.setValue("successexit", 0);
  settings_output.endGroup();
}

void RunControlGUI::SetInstance(eudaq::RunControlUP rc){
  m_rc = std::move(rc);
  if(m_lastexit_success)
    m_rc->SetRunN(m_run_n_qsettings);
  else
    m_rc->SetRunN(m_run_n_qsettings+1);
  auto thd_rc = std::thread(&eudaq::RunControl::Exec, m_rc.get());
  thd_rc.detach();
}

void RunControlGUI::setupDevicesTab() {
  QWidget *run_control_page = takeCentralWidget();
  m_main_tabs = new QTabWidget(this);
  setCentralWidget(m_main_tabs);
  if (run_control_page) {
    m_main_tabs->addTab(run_control_page, "Run Control");
  }
  m_device_tab = new CalvisionDeviceTab(m_main_tabs);
  m_main_tabs->addTab(m_device_tab, "Devices");
  m_fers_tab = new CalvisionFersTab(m_main_tabs);
  m_fers_tab->setHvMonitorUpdateCallback([this]() {
    requestFersHvMonitorUpdate();
  });
  m_fers_tab->setHvSwitchCallback([this](int board, bool on) {
    requestFersHvSwitch(board, on);
  });
  m_main_tabs->addTab(m_fers_tab, "FERS");
  m_drs_tab = new CalvisionDrsTab(m_main_tabs);
  m_main_tabs->addTab(m_drs_tab, "DRS");
  m_conf_tab = new CalvisionConfTab(m_main_tabs);
  m_main_tabs->addTab(m_conf_tab, "Conf");
}

void RunControlGUI::requestFersHvMonitorUpdate() {
  if (!m_rc) {
    return;
  }

  const auto map_conn_status = m_rc->GetActiveConnectionStatusMap();
  int requested = 0;
  for (const auto &conn_status : map_conn_status) {
    if (!conn_status.first || !conn_status.second ||
        conn_status.first->GetType() != "Producer") {
      continue;
    }
    const QString producer =
        QString::fromStdString(conn_status.first->GetName());
    if (!producer.startsWith("my_fers")) {
      continue;
    }
    if (conn_status.second->GetState() == eudaq::Status::STATE_RUNNING) {
      continue;
    }
    m_rc->SendUserCommand("FERS_UPDATE_HV_MONITOR", "", conn_status.first);
    ++requested;
  }

  if (requested == 0) {
    EUDAQ_WARN("No stopped/configured FERS producer available for HV monitor update");
  } else {
    EUDAQ_INFO("Requested FERS HV monitor update from "
               + std::to_string(requested) + " producer(s)");
  }
}

void RunControlGUI::requestFersHvSwitch(int board, bool on) {
  if (!m_rc) {
    return;
  }

  const QString target = QString("my_fers%1").arg(board);
  const auto map_conn_status = m_rc->GetActiveConnectionStatusMap();
  bool found = false;
  bool requested = false;
  for (const auto &conn_status : map_conn_status) {
    if (!conn_status.first || !conn_status.second ||
        conn_status.first->GetType() != "Producer") {
      continue;
    }
    const QString producer =
        QString::fromStdString(conn_status.first->GetName());
    if (producer != target) {
      continue;
    }
    found = true;
    if (conn_status.second->GetState() == eudaq::Status::STATE_RUNNING) {
      EUDAQ_WARN("Ignoring FERS HV switch request while DAQ is running: "
                 + producer.toStdString());
      break;
    }
    const std::string param = std::string("0 ") + (on ? "1" : "0");
    m_rc->SendUserCommand("FERS_SET_HV_ONOFF", param, conn_status.first);
    requested = true;
    break;
  }

  if (!found) {
    EUDAQ_WARN("No FERS producer found for HV switch request: "
               + target.toStdString());
  } else if (requested) {
    EUDAQ_INFO("Requested " + target.toStdString() + " HV "
               + std::string(on ? "ON" : "OFF"));
  }
}

void RunControlGUI::on_btnInit_clicked(){
  QString fers_config_path;
  if (m_fers_tab) {
    if (!m_fers_tab->saveConfig()) {
      return;
    }
    fers_config_path = m_fers_tab->configPath();
  }
  QString drs_config_path;
  if (m_drs_tab) {
    if (!m_drs_tab->saveConfig()) {
      return;
    }
    drs_config_path = m_drs_tab->configPath();
  }
  QString conf_template_path;
  if (m_conf_tab) {
    if (!m_conf_tab->saveTemplate()) {
      return;
    }
    conf_template_path = m_conf_tab->templatePath();
    txtConfigFileName->setText(conf_template_path);
  }
  if(m_device_tab &&
     !m_device_tab->prepareForInit(m_rc.get(), txtInitFileName, txtConfigFileName,
                                   conf_template_path, fers_config_path,
                                   drs_config_path))
    return;
  std::string settings = txtInitFileName->text().toStdString();
  if(!checkFile(QString::fromStdString(settings),QString::fromStdString("init file")))
      return;
  if(m_rc){
    m_rc->ReadInitilizeFile(settings);
    applyOutputPathToInitConfig();
    m_rc->Initialise();
  }
  // connect to the log collector - based on RunControl.cc implemtation
  std::map<eudaq::ConnectionSPC, eudaq::StatusSPC> map_conn_status;
  if(m_rc)
    map_conn_status= m_rc->GetActiveConnectionStatusMap();
  for(auto &conn_status: map_conn_status) {
   if((conn_status.first->GetType()== "LogCollector" && conn_status.first->GetName() == "log")) {
       std::string server_addr = conn_status.second->GetTag("_SERVER");
       std::string conn_addr = conn_status.first->GetRemote();
       std::string log_server = conn_addr.substr(0, conn_addr.find_last_not_of("0123456789"))
               + ":"
               + server_addr.substr(server_addr.find_last_not_of("0123456789")+1);
       EUDAQ_LOG_CONNECT("RunControl","RC-GUI", log_server);
    }
  }

}

void RunControlGUI::on_btnTerminate_clicked(){
  close();
}

void RunControlGUI::on_btnConfig_clicked(){
  std::string settings = txtConfigFileName->text().toStdString();
  if(txtConfigFileName->text().trimmed().isEmpty()){
    QMessageBox::warning(this, "Config",
                         "Run Init first to generate the runtime config file.");
    return;
  }
  if(!checkFile(QString::fromStdString(settings),QString::fromStdString("Config file")))
   {
     EUDAQ_ERROR(settings+" cannot be read");
      return;
  }
  if(m_rc){
    m_rc->ReadConfigureFile(settings);
    applyDrsPayloadModeToRunConfig();
    applyOutputPathToRunConfig();
    m_rc->Configure();
  }
  if(m_rc)
  {
  eudaq::ConfigurationSPC conf = m_rc->GetConfiguration();
  conf->SetSection("RunControl");
  std::string additionalDisplays = conf->Get("ADDITIONAL_DISPLAY_NUMBERS","");
  if(additionalDisplays!="")
    addAdditionalStatus(additionalDisplays);
  }
}

bool RunControlGUI::applyManualRunNumber(bool warn_if_empty){
  QString qs_next_run = txtNextRunNumber->text().trimmed();
  if(qs_next_run.isEmpty()){
    if(warn_if_empty)
      QMessageBox::information(this, "Run number", "Enter a run number first.");
    return !warn_if_empty;
  }

  bool succ = false;
  uint32_t run_n = qs_next_run.toUInt(&succ);
  if(!succ){
    QMessageBox::warning(this, "Invalid run number",
                         "Run number must be a non-negative integer.");
    return false;
  }

  if(m_rc)
    m_rc->SetRunN(run_n);
  m_run_n_qsettings = run_n;

  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2");
  settings.setValue("runnumber", m_run_n_qsettings);
  settings.endGroup();

  txtNextRunNumber->clear();
  if(m_str_label.count("RUN"))
    m_str_label.at("RUN")->setText(QString::number(run_n) + " (next run)");
  EUDAQ_INFO("RunControl GUI manually set next run number to "
             + std::to_string(run_n));
  return true;
}

void RunControlGUI::on_btnStart_clicked(){
  if(!applyManualRunNumber(false))
    return;
  applyOutputPathToInitConfig();
  applyOutputPathToRunConfig();
  refreshConfiguredOutputTargets();
  if(m_save_config_at_run_start)
    store_config();
  if(m_rc)
    m_rc->StartRun();
}

void RunControlGUI::on_btnSetRunNumber_clicked(){
  applyManualRunNumber(true);
}

void RunControlGUI::on_txtNextRunNumber_returnPressed(){
  applyManualRunNumber(true);
}

void RunControlGUI::on_btnStop_clicked() {
  if(m_rc)
    m_rc->StopRun();
    //update_infos();
}

void RunControlGUI::on_btnReset_clicked() {
  if(m_rc)
    m_rc->Reset();
}

void RunControlGUI::on_btnLog_clicked() {
    std::string msg = txtLogmsg->text().toStdString();
    EUDAQ_USER(msg);
}

void RunControlGUI::on_btnLoadDataPath_clicked() {
  QString usedpath = txtDataPath->text().trimmed();
  if (usedpath.isEmpty()) {
    usedpath = QDir::currentPath();
  }
  QString dirname = QFileDialog::getExistingDirectory(
      this, tr("Select Data Directory"), usedpath,
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (!dirname.isNull()) {
    txtDataPath->setText(dirname);
  }
}

void RunControlGUI::DisplayTimer(){
  auto state = updateInfos();
  updateStatusDisplay();
  if(state == eudaq::Status::STATE_RUNNING)
      updateProgressBar();

  if(!m_scan.scanIsTimeBased()&& m_scan_active == true)
      if(checkEventsInStep())
          nextStep();
}

eudaq::Status::State RunControlGUI::updateInfos(){
    std::map<eudaq::ConnectionSPC, eudaq::StatusSPC> map_conn_status;
    auto state = eudaq::Status::STATE_RUNNING;
    if(m_rc)
      map_conn_status= m_rc->GetActiveConnectionStatusMap();

    for(auto &conn_status_last: m_map_conn_status_last){
      if(!map_conn_status.count(conn_status_last.first)){
        m_model_conns.disconnected(conn_status_last.first);
        removeStatusDisplay(conn_status_last);
      }
    }
    for(auto &conn_status: map_conn_status){
      if(!m_map_conn_status_last.count(conn_status.first)){
        m_model_conns.newconnection(conn_status.first);
        if(! (conn_status.first->GetType()== "LogCollector"))
            addStatusDisplay(conn_status);
      }
      if(conn_status.first && conn_status.first->GetType() == "DataCollector"){
        addStatusDisplay(conn_status);
      }
    }
    if(map_conn_status.empty()){
      state = eudaq::Status::STATE_UNINIT;
    }
    else{
      state = eudaq::Status::STATE_RUNNING;
      for(auto &conn_status: map_conn_status){
        if(!conn_status.second)
      continue;
        auto state_conn = conn_status.second->GetState();
        state_conn < state ? state = eudaq::Status::State(state_conn) : state = state ;
        m_model_conns.SetStatus(conn_status.first, conn_status.second);
      }
    }

    bool have_fers_producer = false;
    if (m_device_tab) {
      QMap<QString, CalvisionDeviceTab::FersReadback> fers_readbacks;
      std::map<int, CalvisionFersTab::HvReadback> fers_hv_readbacks;
      for (auto &conn_status : map_conn_status) {
        if (!conn_status.first || !conn_status.second ||
            conn_status.first->GetType() != "Producer") {
          continue;
        }
        const QString producer =
            QString::fromStdString(conn_status.first->GetName());
        if (!producer.startsWith("my_fers")) {
          continue;
        }
        have_fers_producer = true;
        bool producer_index_ok = false;
        const int producer_index =
            producer.mid(QString("my_fers").size()).toInt(&producer_index_ok);

        const auto tags = conn_status.second->GetTags();
        auto get_tag = [&tags](const std::string &key) -> QString {
          const auto it = tags.find(key);
          return it == tags.end() ? QString() : QString::fromStdString(it->second);
        };

        CalvisionDeviceTab::FersReadback readback;
        readback.pid = get_tag("FERS_BRD0_PID");
        readback.model = get_tag("FERS_BRD0_MODEL");
        readback.fpga_fw = get_tag("FERS_BRD0_FPGA_FW_REV");
        readback.uc_fw = get_tag("FERS_BRD0_UC_FW_REV");
        readback.summary = get_tag("FERS_INFO");
        if (!readback.pid.isEmpty() || !readback.model.isEmpty() ||
            !readback.fpga_fw.isEmpty() || !readback.uc_fw.isEmpty() ||
            !readback.summary.isEmpty()) {
          fers_readbacks.insert(producer, readback);
        }
        if (producer_index_ok && producer_index >= 0) {
          CalvisionFersTab::HvReadback hv;
          hv.hv_set = get_tag("FERS_BRD0_HV_SET_V");
          hv.vmon = get_tag("FERS_BRD0_HV_VMON");
          hv.imon = get_tag("FERS_BRD0_HV_IMON");
          hv.det_temp = get_tag("FERS_BRD0_TEMP_DET");
          hv.fpga_temp = get_tag("FERS_BRD0_TEMP_FPGA");
          hv.board_temp = get_tag("FERS_BRD0_TEMP_BRD");
          hv.status = get_tag("FERS_BRD0_HV_STATUS");
          if (!hv.hv_set.isEmpty() || !hv.vmon.isEmpty() ||
              !hv.imon.isEmpty() || !hv.det_temp.isEmpty() ||
              !hv.fpga_temp.isEmpty() || !hv.board_temp.isEmpty() ||
              !hv.status.isEmpty()) {
            fers_hv_readbacks[producer_index] = hv;
          }
        }
      }
      m_device_tab->updateFersReadbacks(fers_readbacks);
      if (m_fers_tab) {
        m_fers_tab->updateHvReadbacks(fers_hv_readbacks);
      }
    }
    if (m_fers_tab) {
      m_fers_tab->setHvMonitorUpdateEnabled(
          have_fers_producer && state != eudaq::Status::STATE_RUNNING);
      m_fers_tab->setHvControlsEnabled(
          have_fers_producer && state != eudaq::Status::STATE_RUNNING);
    }

    const bool confLoaded = QFileInfo(txtConfigFileName->text()).exists() &&
                            txtConfigFileName->text().endsWith(".conf");

    btnInit->setEnabled(state == eudaq::Status::STATE_UNINIT);
    btnConfig->setEnabled((state == eudaq::Status::STATE_UNCONF ||
               state == eudaq::Status::STATE_CONF ||
               state == eudaq::Status::STATE_STOPPED)&& confLoaded);
    comboDrsPayloadMode->setEnabled(state != eudaq::Status::STATE_RUNNING);
    btnStart->setEnabled(state == eudaq::Status::STATE_CONF || state == eudaq::Status::STATE_STOPPED);
    btnSetRunNumber->setEnabled(state != eudaq::Status::STATE_RUNNING);
    txtNextRunNumber->setEnabled(state != eudaq::Status::STATE_RUNNING);
    btnStop->setEnabled(state == eudaq::Status::STATE_RUNNING && !m_scan_active);
    btnReset->setEnabled(state != eudaq::Status::STATE_RUNNING);
    btnTerminate->setEnabled(state != eudaq::Status::STATE_RUNNING);

    lblCurrent->setText(m_map_state_str.at(state));

    uint32_t run_n = m_rc->GetRunN();
    if(m_run_n_qsettings != run_n){
      m_run_n_qsettings = run_n;
      QSettings settings("EUDAQ collaboration", "EUDAQ");
      settings.beginGroup("euRun2");
      settings.setValue("runnumber", m_run_n_qsettings);
      settings.endGroup();
    }
    if(m_rc&&m_str_label.count("RUN")){
      if(state == eudaq::Status::STATE_RUNNING){
        m_str_label.at("RUN")->setText(QString::number(run_n));
      } else {
        m_str_label.at("RUN")->setText(QString::number(run_n)+" (next run)");
      }
    }
    m_map_conn_status_last = map_conn_status;
    return state;
}

void RunControlGUI::closeEvent(QCloseEvent *event) {
  if (QMessageBox::question(this, "Quitting",
                "Terminate all connections and quit?",
                QMessageBox::Ok | QMessageBox::Cancel)
      == QMessageBox::Cancel){
    event->ignore();
  } else {
    m_timer_display.stop();
    m_scanningTimer.stop();
    m_scan_active = false;
    m_scan_interrupt_received = true;

    QSettings settings("EUDAQ collaboration", "EUDAQ");
    settings.beginGroup("euRun2");
    if(m_rc)
      settings.setValue("runnumber", m_rc->GetRunN());
    else
      settings.setValue("runnumber", m_run_n_qsettings);
    settings.setValue("size", size());
    settings.setValue("pos", pos());
    settings.setValue("lastScanFile", txtScanFile->text());
    settings.setValue("lastDataPath", txtDataPath->text());
    settings.setValue("drsPayloadMode", comboDrsPayloadMode->currentData().toString());
    settings.setValue("successexit", 1);
    settings.endGroup();
    if (m_device_tab) {
      m_device_tab->saveSettings();
    }
    if (m_conf_tab) {
      m_conf_tab->saveSettings();
    }
    if (m_fers_tab) {
      m_fers_tab->saveSettings();
    }
    if(m_rc)
      m_rc->Terminate();
    if (m_device_tab) {
      m_device_tab->terminateOwnedProcesses();
    }
    event->accept();
  }
}

void RunControlGUI::Exec(){
  show();
  if(QApplication::instance())
    QApplication::instance()->exec();
  else
    std::cerr<<"ERROR: RUNContrlGUI::EXEC\n";
}


std::map<int, QString> RunControlGUI::m_map_state_str ={
    {eudaq::Status::STATE_UNINIT,
     "<font size=12 color='red'><b>Current State: Uninitialised </b></font>"},
    {eudaq::Status::STATE_UNCONF,
     "<font size=12 color='red'><b>Current State: Unconfigured </b></font>"},
    {eudaq::Status::STATE_CONF,
     "<font size=12 color='orange'><b>Current State: Configured </b></font>"},
    {eudaq::Status::STATE_STOPPED,
     "<font size=12 color='blue'><b>Current State: Stopped </b></font>"},
    {eudaq::Status::STATE_RUNNING,
     "<font size=12 color='green'><b>Current State: Running </b></font>"},
    {eudaq::Status::STATE_ERROR,
     "<font size=12 color='darkred'><b>Current State: Error </b></font>"}
};


void RunControlGUI::onCustomContextMenu(const QPoint &point)
{
    QModelIndex index = viewConn->indexAt(point);
    if(index.isValid()) {
    QMenu *contextMenu = new QMenu(viewConn);
    // load an eventually updated ini file
    if(m_rc){
    loadInitFile();
    }
    if(m_rc->GetInitConfiguration()){
    QAction *initialiseAction = new QAction("Initialise", this);
    connect(initialiseAction, &QAction::triggered, this, [this,index]() {m_rc->InitialiseSingleConnection(m_model_conns.getConnection(index));});
    contextMenu->addAction(initialiseAction);
    }

    // load an eventually updated config file
    if(m_rc){
    loadConfigFile();
    }
    if(m_rc->GetConfiguration()){
    QAction *configureAction = new QAction("Configure", this);
    connect(configureAction, &QAction::triggered, this, [this,index]() {m_rc->ConfigureSingleConnection(m_model_conns.getConnection(index));});
    contextMenu->addAction(configureAction);
    }

    QAction *startAction = new QAction("Start", this);
    connect(startAction, &QAction::triggered, this, [this,index]() {m_rc->StartSingleConnection(m_model_conns.getConnection(index));});
    contextMenu->addAction(startAction);

    QAction *stopAction = new QAction("Stop", this);
    connect(stopAction, &QAction::triggered, this, [this,index]() {m_rc->StopSingleConnection(m_model_conns.getConnection(index));});
    contextMenu->addAction(stopAction);

    QAction *resetAction = new QAction("Reset", this);
    connect(resetAction, &QAction::triggered, this, [this,index]() {m_rc->ResetSingleConnection(m_model_conns.getConnection(index));});
    contextMenu->addAction(resetAction);

    QAction *terminateAction = new QAction("Terminate", this);
    connect(terminateAction, &QAction::triggered, this, [this,index]() {m_rc->TerminateSingleConnection(m_model_conns.getConnection(index));});
    contextMenu->addAction(terminateAction);

    contextMenu->exec(viewConn->viewport()->mapToGlobal(point));
    }

}


bool RunControlGUI::loadInitFile() {
  std::string settings = txtInitFileName->text().toStdString();
  QFileInfo check_file(txtInitFileName->text());
  if(!check_file.exists() || !check_file.isFile()){
    QMessageBox::warning(NULL, "ERROR", "Init file does not exist.");
    return false;
  }
  if(m_rc){
    m_rc->ReadInitilizeFile(settings);
  }
  return true;
}

bool RunControlGUI::loadConfigFile() {
  std::string settings = txtConfigFileName->text().toStdString();
  QFileInfo check_file(txtConfigFileName->text());
  if(!check_file.exists() || !check_file.isFile()){
    QMessageBox::warning(NULL, "ERROR", "Config file does not exist.");
    return false;
  }
  if(m_rc){
    m_rc->ReadConfigureFile(settings);
  }
  return true;
}

bool RunControlGUI::addStatusDisplay(std::pair<eudaq::ConnectionSPC, eudaq::StatusSPC> connection) {
    if(!connection.first)
      return false;
    if(connection.first->GetType() == "DataCollector") {
      const QString connection_name =
          QString::fromStdString(connection.first->GetName());
      for(const auto &tag: kBuilderStatusTags) {
        addToGrid(connection_name + ":" + tag.tag, tag.label);
      }
    }
    return true;
}

bool RunControlGUI::removeStatusDisplay(std::pair<eudaq::ConnectionSPC, eudaq::StatusSPC> connection) {
    if(!connection.first)
      return false;
    const QString prefix =
        QString::fromStdString(connection.first->GetName()) + ":";
    QStringList object_names;
    for(const auto &label: m_str_label) {
      if(label.first.startsWith(prefix)) {
        object_names << label.first;
      }
    }

    for(const QString &object_name: object_names) {
      const QString value_name = "val_" + object_name;
      for(int idx = grpGrid->count() - 1; idx >= 0; --idx) {
        QLayoutItem *item = grpGrid->itemAt(idx);
        QLabel *label = item ? dynamic_cast<QLabel *>(item->widget()) : nullptr;
        if(label && (label->objectName() == object_name ||
                     label->objectName() == value_name)) {
          grpGrid->removeWidget(label);
          delete label;
        }
      }
      m_map_label_str.erase(object_name);
      m_str_label.erase(object_name);
    }
    return true;
}
bool RunControlGUI::addToGrid(const QString & objectName, QString displayedName) {

    if(m_str_label.count(objectName)==1) {
        //QMessageBox::warning(NULL,"ERROR - Status display","Duplicating display entry request: "+objectName);
        return false;
    }
    if(displayedName=="")
        displayedName = objectName;
    QLabel *lblname = new QLabel(grpStatus);
    lblname->setObjectName(objectName);
    lblname->setText(displayedName+": ");
    QLabel *lblvalue = new QLabel(grpStatus);
    lblvalue->setObjectName("val_"+objectName);
    lblvalue->setText("--");

    int colPos = 0, rowPos = 0;
    if( 2* (m_str_label.size()+1) < grpGrid->rowCount() * grpGrid->columnCount() ) {
        colPos = m_display_col;
        rowPos = m_display_row;
        if (++m_display_col >= kStatusPairsPerRow) {
            ++m_display_row;
            m_display_col = 0;
        }
    }
    else {
        colPos = m_display_col;
        rowPos = m_display_row;
        if (++m_display_col >= kStatusPairsPerRow){
            ++m_display_row;
            m_display_col = 0;
        }
    }
    m_map_label_str.insert(std::pair<QString, QString>(objectName,objectName+": "));
    m_str_label.insert(std::pair<QString, QLabel*>(objectName, lblvalue));
    grpGrid->addWidget(lblname, rowPos, colPos * 2);
    grpGrid->addWidget(lblvalue, rowPos, colPos * 2 + 1);
    return true;
}
/**
 * @brief RunControlGUI::updateStatusDisplay
 * @return true if success, false otherwise (cannot happen currently)
 */
bool RunControlGUI::updateStatusDisplay() {
    auto map_conn_status = m_map_conn_status_last;
    if(m_rc)
      map_conn_status = m_rc->GetActiveConnectionStatusMap();

    auto it = map_conn_status.begin();
    while(it!=map_conn_status.end()) {
        // elements might not be existing at startup/beeing asynchronously changed
        if(it->first && it->second) {
            auto labelit = m_str_label.begin();
            while(labelit!=m_str_label.end()) {
                const std::string label_key = labelit->first.toStdString();
                const size_t sep = label_key.find(":");
                if(sep == std::string::npos){
                    ++labelit;
                    continue;
                }
                std::string labelname = label_key.substr(0, sep);
                std::string displayedItem = label_key.substr(sep + 1);
                const std::string conn_name = it->first->GetName();
                const std::string conn_full_name =
                    it->first->GetType() + "." + it->first->GetName();
                if(conn_name==labelname || conn_full_name==labelname) {
                    auto tags = it->second->GetTags();
                    const auto tag = tags.find(displayedItem);
                    if(tag == tags.end()){
                        labelit->second->setText("--");
                    }else if(displayedItem=="EventN"){
                        labelit->second->setText(QString::fromStdString(tag->second+" Events"));
                    }else if(displayedItem=="Freq. (avg.) [kHz]"){
                        labelit->second->setText(QString::fromStdString(tag->second+" kHz"));
                    }else{
                        labelit->second->setText(QString::fromStdString(tag->second));
                    }
                }
                labelit++;
            }
        }
        it++;
    }
       return true;
}

bool RunControlGUI::addAdditionalStatus(std::string info) {
    std::vector<std::string> results = eudaq::splitString(info,',');
    if(results.size()%2!=0) {
        QMessageBox::warning(NULL,"ERROR","Additional Status Display inputs are not correctly formatted - please check");
       return false;
    } else {
        for(auto c = 0; c < results.size();c+=2) {
            // check if the connection exists, otherwise do not display
            auto it = m_map_conn_status_last.begin();
            bool found = false;
            while(it != m_map_conn_status_last.end()) {
                if(it->first && it->first->GetName()==results.at(c)){
                    addToGrid(QString::fromStdString(results.at(c)+":"+results.at(c+1)));
                    found = true;
                }
                it++;
            }
            if(!found) {
                QMessageBox::warning(NULL,"ERROR",QString::fromStdString("Element \""+results.at(c)+ "\" is not connected"));
                return false;
            }
        }
    }
    return true;
}

bool RunControlGUI::checkFile(QString file, QString usecase)
{
    QFileInfo check_file(file);
    if(!check_file.exists() || !check_file.isFile()){
      QMessageBox::warning(NULL, "ERROR",QString(usecase + " file does not exist."));
      return false;
    }
    else
        return true;
}

/**
 * @brief RunControlGUI::on_btn_LoadScanFile_clicked
 * @abstract push Button to open file dialog to select the scan configuration
 * file.
 * @group Scanning utils, RunControlGUI
 */
void RunControlGUI::on_btn_LoadScanFile_clicked()
{
    QString usedpath =QFileInfo(txtScanFile->text()).path();
    QString filename =QFileDialog::getOpenFileName(this, tr("Open File"),
                           usedpath,
                           tr("*.scan (*.scan)"));
    if (!filename.isNull()){
      txtScanFile->setText(filename);
    }

}

/**
 * @brief RunControlGUI::on_btnStartScan_clicked
 * @abstract Button to control the scanning procedure. Does not implement any real
 * functionality, only changes status bools and texts
 *
 */
void RunControlGUI::on_btnStartScan_clicked()
{
   if(m_scan_active == true){
       QMessageBox::StandardButton reply;
       reply = QMessageBox::question(NULL,"Interrupt Scan","Do you want to stop immediately?\n Hitting no will stop after finishing the current step",
                                     QMessageBox::Yes|QMessageBox::No|QMessageBox::Abort);
       if(reply==QMessageBox::Yes) {
           m_scan_active = false;
           m_scanningTimer.stop();
           nextStep();
           return;
       } else if(reply==QMessageBox::Abort) {
           m_scan_active = true;
           btnStartScan->setText("Interrupt scan");
       } else if(reply==QMessageBox::No) {
           m_scan_interrupt_received = true;
           btnStartScan->setText("Scan stops after current step");
       }
   } else {
       if(!readScanConfig())
          return;
       m_scan_active = true;
       m_scan_interrupt_received = false;
       EUDAQ_INFO("STARTING SCAN");
       btnStartScan->setText("Interrupt Scan");
       nextStep();
   return;
   }
}

/**
 * @brief RunControlGUI::prepareAndStartStep
 * @abstract stop the data taking, update the configuration and start a new run
 * @return Returns true if step has been successfull
 */
void RunControlGUI::nextStep()
{
    if(!m_scan_active){
        btnStartScan->setText("Start scan");
        std::cout << "Stopping scan" << std::endl;
        m_scan_interrupt_received = false;
        m_scanningTimer.stop();
        if(!allConnectionsInState(eudaq::Status::STATE_STOPPED))
            on_btnStop_clicked();
        return;
    }
    if(m_scan.currentStep()!=0)
        on_btnStop_clicked();
    std::string conf = m_scan.nextConfig();
    EUDAQ_USER("Next file ("+std::to_string(m_scan.currentStep())+"): "+conf );
    if(m_scan_interrupt_received ==false && m_scan_active==true && conf !="finished") {
        std::cout << "Next step" << std::endl;
        txtConfigFileName->setText(QString(conf.c_str()));
        QCoreApplication::processEvents();
        while((!allConnectionsInState(eudaq::Status::STATE_STOPPED) && m_scan.scanHasbeenStarted())
              ||(!allConnectionsInState(eudaq::Status::STATE_CONF) && !m_scan_active)){
            updateInfos();
            QCoreApplication::processEvents();
            std::this_thread::sleep_for (std::chrono::seconds(1));
            std::cout << "Waiting until all components are stopped"<<std::endl;
        }

        updateInfos();
        std::this_thread::sleep_for (std::chrono::seconds(3));
        on_btnConfig_clicked();
        while(!allConnectionsInState(eudaq::Status::STATE_CONF) && m_scan_active){
            updateInfos();
            QCoreApplication::processEvents();
            std::this_thread::sleep_for (std::chrono::seconds(1));
            std::cout << "Waiting until all components are (re)configured"<<std::endl;
        }
        updateInfos();
        std::cout << "Ready for next step"<<std::endl;

        on_btnStart_clicked();
        while(!allConnectionsInState(eudaq::Status::STATE_RUNNING)){
            updateInfos();
            QCoreApplication::processEvents();
            std::this_thread::sleep_for (std::chrono::seconds(1));
            std::cout << "Waiting until all components are running"<<std::endl;

        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        updateInfos();

        if(m_scan.scanIsTimeBased())
        {
            m_scanningTimer.start(1000*m_scan.timePerStep());
            EUDAQ_USER("Time based scan next step");}
        else {
            EUDAQ_USER("Event based scan next step");
        }
        // stop the scan here
    } else {
        btnStartScan->setText("Start scan");
        m_scan_active = false;
        m_scan_interrupt_received = false;
        m_scanningTimer.stop();

    }
    m_scan.scanStarted();
    return;
}
/**
 * @brief RunControlGUI::allConnectionsInState
 * @param state to be checked
 * @return true if all connections are in state, false otherwise
 */
bool RunControlGUI::allConnectionsInState(eudaq::Status::State state){
    std::map<eudaq::ConnectionSPC, eudaq::StatusSPC> map_conn_status;
    if(m_rc)
      map_conn_status= m_rc->GetActiveConnectionStatusMap();
    else
        return false;
    for(auto &conn_status: map_conn_status){
        if(!conn_status.second)
            continue;
        auto state_conn = conn_status.second->GetState();

        if(state_conn == eudaq::Status::STATE_ERROR)
        {
            EUDAQ_ERROR("Automatical config failed - retry...");
            // private reset here....
            m_rc->ResetSingleConnection(conn_status.first);
            if( state <= eudaq::Status::STATE_UNCONF && conn_status.second->GetState() == eudaq::Status::STATE_UNINIT)
                m_rc->InitialiseSingleConnection(conn_status.first);
            if(state <= eudaq::Status::STATE_CONF && (conn_status.second->GetState() == eudaq::Status::STATE_UNCONF))
                m_rc->ConfigureSingleConnection(conn_status.first);
            return false;

        }
        if((int)state_conn != (int)state)
            return false;
    }
    return true;
}

/**
 * @brief RunControlGUI::readScanConfig
 * @abstract Read the scan config file and prepare all parameters
 * @return true if sucessfull
 */
bool RunControlGUI::readScanConfig(){
    m_scan.reset();
    return m_scan.setupScan(txtConfigFileName->text().toStdString(),txtScanFile->text().toStdString());
}
/**
 * @brief RunControlGUI::checkEventsInStep
 * @abstract check if the reuqested number of events for a certain step is recorded
 * @return true if reached/surpassed, false otherwise
 */
bool RunControlGUI::checkEventsInStep(){
    int events = getEventsCurrent();
    return ( (events > 0 ? events : (m_scan.eventsPerStep()-2))>m_scan.eventsPerStep());
}

/**
 * @brief RunControlGUI::getEventsCurrent
 * @return Number of events in current step of scans
 */

int RunControlGUI::getEventsCurrent(){
    std::map<eudaq::ConnectionSPC, eudaq::StatusSPC> map_conn_status;
    if(m_scan.scanIsTimeBased())
        return m_scan.eventsPerStep()+1;
    if(m_rc)
        map_conn_status= m_rc->GetActiveConnectionStatusMap();
    else
        return -2;
    for(auto conn : map_conn_status) {
        if((conn.first->GetType()+"."+conn.first->GetName())==m_scan.currentCountingComponent()){
            auto tags = conn.second->GetTags();
            for(auto &tag: tags)
                if(tag.first=="EventN")
                    return std::stoi(tag.second);
        }
    }
    return -1;
}

void RunControlGUI::store_config()
{
    if(!m_rc)
        return;
    uint32_t run_n = m_rc->GetRunN();
    QString run_dir = getRunDirectory(run_n);
    ensureDirectoryExists(run_dir);
    QString run_tag = QString::fromStdString(eudaq::to_string(run_n, 3));
    saveConfigurationSnapshot(QDir(run_dir).filePath("init_run" + run_tag + ".ini"),
                              m_rc->GetInitConfiguration());
    saveConfigurationSnapshot(QDir(run_dir).filePath("config_run" + run_tag + ".conf"),
                              m_rc->GetConfiguration());
}

void RunControlGUI::updateProgressBar(){
    double scanProgress = 0;
    if(m_scan_active){
        scanProgress = ((m_scan.currentStep()-1)%m_scan.nSteps())/double(std::max(1,m_scan.nSteps()))*100;
    if(m_scan.scanIsTimeBased())
        scanProgress+= ((m_scanningTimer.interval()-m_scanningTimer.remainingTime())/double(std::max(1,m_scanningTimer.interval())) *100./std::max(1,m_scan.nSteps()));
    else
        scanProgress += getEventsCurrent()/double(m_scan.eventsPerStep())*100./std::max(1,m_scan.nSteps());
    }
    progressBar_scan->setValue(scanProgress);

}


void RunControlGUI::on_checkBox_stateChanged(int arg1)
{
m_save_config_at_run_start = arg1;
}

QString RunControlGUI::getSelectedDataPath() const {
  QString data_path = txtDataPath->text().trimmed();
  if (data_path.isEmpty()) {
    data_path = QDir::currentPath();
  } else if (data_path == "~") {
    data_path = QDir::homePath();
  } else if (data_path.startsWith("~/")) {
    data_path = QDir::homePath() + data_path.mid(1);
  }
  return QDir(data_path).absolutePath();
}

QString RunControlGUI::getRunDirectory(uint32_t run_n) const {
  QString run_folder =
      QString::fromStdString(std::string(eudaq::FileNamer("run$3R").Set('R', run_n)));
  return QDir(getSelectedDataPath()).filePath(run_folder);
}

void RunControlGUI::ensureDirectoryExists(const QString &path) const {
  if (!QDir().mkpath(path)) {
    EUDAQ_THROW("Unable to create directory: " + path.toStdString());
  }
}

void RunControlGUI::applyOutputPathToInitConfig() {
  if (!m_rc) {
    return;
  }
  auto conf = std::const_pointer_cast<eudaq::Configuration>(m_rc->GetInitConfiguration());
  if (!conf) {
    return;
  }
  const QString base_dir = getSelectedDataPath();
  const QString run_dir_pattern = "run$3R";
  const QString cur_section = QString::fromStdString(conf->GetCurrentSectionName());
  auto active_connections = m_rc->GetActiveConnections();
  for (const auto &conn : active_connections) {
    if (!conn || conn->GetType() != "LogCollector") {
      continue;
    }
    const std::string section = conn->GetType() + "." + conn->GetName();
    conf->SetSection(section);
    QString file_name =
        patternFileName(conf->Get("EULOG_GUI_LOG_FILE_PATTERN",
                                  conf->Get("FILE_PATTERN", "EULog_$4R_$12D.log")),
                        "EULog_$4R_$12D.log");
    QString full_pattern = QDir(base_dir).filePath(run_dir_pattern + "/" + file_name);
    conf->SetString("EULOG_GUI_LOG_FILE_PATTERN", full_pattern.toStdString());
    conf->SetString("FILE_PATTERN", full_pattern.toStdString());
  }
  if (!cur_section.isEmpty()) {
    conf->SetSection(cur_section.toStdString());
  }
}

void RunControlGUI::applyOutputPathToRunConfig() {
  if (!m_rc) {
    return;
  }
  auto conf = std::const_pointer_cast<eudaq::Configuration>(m_rc->GetConfiguration());
  if (!conf) {
    return;
  }
  const QString base_dir = getSelectedDataPath();
  const QString run_dir_pattern = "run$3R";
  const QString cur_section = QString::fromStdString(conf->GetCurrentSectionName());
  auto active_connections = m_rc->GetActiveConnections();
  for (const auto &conn : active_connections) {
    if (!conn) {
      continue;
    }
    const std::string section = conn->GetType() + "." + conn->GetName();
    if (conn->GetType() == "DataCollector") {
      conf->SetSection(section);
      QString file_name =
          patternFileName(conf->Get("EUDAQ_FW_PATTERN", "run$3R$X"), "run$3R$X");
      QString full_pattern = QDir(base_dir).filePath(run_dir_pattern + "/" + file_name);
      conf->SetString("EUDAQ_FW_PATTERN", full_pattern.toStdString());
    } else if (conn->GetType() == "LogCollector") {
      conf->SetSection(section);
      QString file_name =
          patternFileName(conf->Get("EULOG_GUI_LOG_FILE_PATTERN",
                                    conf->Get("FILE_PATTERN", "EULog_$4R_$12D.log")),
                          "EULog_$4R_$12D.log");
      QString full_pattern = QDir(base_dir).filePath(run_dir_pattern + "/" + file_name);
      conf->SetString("EULOG_GUI_LOG_FILE_PATTERN", full_pattern.toStdString());
      conf->SetString("FILE_PATTERN", full_pattern.toStdString());
    }
  }
  conf->SetSection("RunControl");
  conf->SetString("config_log_path", (base_dir + "/").toStdString());
  if (!cur_section.isEmpty()) {
    conf->SetSection(cur_section.toStdString());
  }
}

void RunControlGUI::applyDrsPayloadModeToRunConfig() {
  if (!m_rc) {
    return;
  }
  auto conf = std::const_pointer_cast<eudaq::Configuration>(m_rc->GetConfiguration());
  if (!conf) {
    return;
  }

  QString mode = comboDrsPayloadMode->currentData().toString().toLower();
  if (mode.isEmpty()) {
    mode = "decoded";
  }

  const QString cur_section = QString::fromStdString(conf->GetCurrentSectionName());
  auto active_connections = m_rc->GetActiveConnections();
  for (const auto &conn : active_connections) {
    if (!conn || conn->GetType() != "Producer") {
      continue;
    }
    std::string name = conn->GetName();
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower_name.find("drs") == std::string::npos) {
      continue;
    }
    const std::string section = conn->GetType() + "." + name;
    if (!conf->HasSection(section)) {
      continue;
    }
    conf->SetSection(section);
    conf->SetString("DRS_PAYLOAD_MODE", mode.toStdString());
  }
  if (!cur_section.isEmpty()) {
    conf->SetSection(cur_section.toStdString());
  }

  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("euRun2");
  settings.setValue("drsPayloadMode", mode);
  settings.endGroup();
}

void RunControlGUI::refreshConfiguredOutputTargets() {
  if (!m_rc) {
    return;
  }
  std::vector<eudaq::ConnectionSPC> targets;
  auto map_conn_status = m_rc->GetActiveConnectionStatusMap();
  for (const auto &conn_status : map_conn_status) {
    if (!conn_status.first || !conn_status.second) {
      continue;
    }
    const auto &conn = conn_status.first;
    const auto state = conn_status.second->GetState();
    if ((conn->GetType() == "DataCollector" || conn->GetType() == "LogCollector") &&
        (state == eudaq::Status::STATE_CONF || state == eudaq::Status::STATE_STOPPED)) {
      m_rc->ConfigureSingleConnection(conn);
      targets.push_back(conn);
    }
  }
  if (targets.empty()) {
    return;
  }
  auto deadline = QDateTime::currentDateTime().addSecs(5);
  while (QDateTime::currentDateTime() < deadline) {
    bool all_ready = true;
    auto current_status = m_rc->GetActiveConnectionStatusMap();
    for (const auto &conn : targets) {
      auto it = current_status.find(conn);
      if (it == current_status.end() || !it->second) {
        all_ready = false;
        break;
      }
      auto state = it->second->GetState();
      if (state == eudaq::Status::STATE_ERROR) {
        return;
      }
      if (state != eudaq::Status::STATE_CONF) {
        all_ready = false;
        break;
      }
    }
    if (all_ready) {
      return;
    }
    QApplication::processEvents();
    eudaq::mSleep(50);
  }
}

void RunControlGUI::saveConfigurationSnapshot(const QString &path,
                                              eudaq::ConfigurationSPC conf) const {
  if (!conf) {
    return;
  }
  QFileInfo info(path);
  ensureDirectoryExists(info.dir().absolutePath());
  std::ofstream out(path.toStdString());
  if (!out.is_open()) {
    EUDAQ_THROW("Unable to open configuration snapshot: " + path.toStdString());
  }
  conf->Save(out);
}
