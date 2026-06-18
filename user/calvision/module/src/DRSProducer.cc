#include "eudaq/Producer.hh"
#include "DRS_EUDAQ.h"
#include "CAENDigitizer.h"
extern "C" {
#include "WDconfig.h"
}
#include <iostream>
#include <fstream>
#include <ratio>
#include <chrono>
#include <thread>
#include <cstring>
#include <cctype>
#include <deque>
#include <set>
#include <utility>
//#include <random>
#ifndef _WIN32
#include <sys/file.h>
#endif

namespace {

enum class DrsPayloadMode {
  Decoded,
  Compact
};

std::string NormalizeDrsLinkType(std::string value) {
  std::string normalized;
  for (char ch : value) {
    if (ch == '-' || ch == ' ' || ch == '\t') {
      ch = '_';
    }
    normalized += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return normalized;
}

int ParseDrsLinkType(const std::string &value) {
  const std::string normalized = NormalizeDrsLinkType(value);
  if (normalized.empty()) {
    EUDAQ_THROW("DRS_LINK_TYPE is empty");
  }
  if (normalized == "USB" || normalized == "DIRECT_USB" || normalized == "USB2" || normalized == "USB2_0") {
    return CAEN_DGTZ_USB;
  }
  if (normalized == "A4818" || normalized == "USB_A4818" || normalized == "USB3_A4818") {
    return CAEN_DGTZ_USB_A4818;
  }
  if (normalized == "PCI" || normalized == "OPTICAL" || normalized == "OPTICAL_LINK") {
    return CAEN_DGTZ_OpticalLink;
  }
  if (normalized == "USB_V4718") {
    return CAEN_DGTZ_USB_V4718;
  }
  if (normalized == "ETH_V4718") {
    return CAEN_DGTZ_ETH_V4718;
  }
  EUDAQ_THROW("Unsupported DRS_LINK_TYPE '" + value
    + "'. Use USB or A4818 for this setup.");
}

DrsPayloadMode ParseDrsPayloadMode(std::string value) {
  std::string normalized;
  for (char ch : value) {
    if (ch == '-' || ch == ' ' || ch == '\t') {
      ch = '_';
    }
    normalized += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  if (normalized.empty() || normalized == "DECODED" || normalized == "EXPANDED") {
    return DrsPayloadMode::Decoded;
  }
  if (normalized == "COMPACT" || normalized == "RAW" || normalized == "BINARY" ||
      normalized == "COMPACT_RAW") {
    return DrsPayloadMode::Compact;
  }
  EUDAQ_THROW("Unsupported DRS_PAYLOAD_MODE '" + value
    + "'. Use decoded or compact.");
}

std::string DrsPayloadModeName(DrsPayloadMode mode) {
  switch (mode) {
    case DrsPayloadMode::Decoded:
      return "decoded";
    case DrsPayloadMode::Compact:
      return "compact";
  }
  return "unknown";
}

std::string DrsLinkTypeName(int link_type) {
  switch (link_type) {
    case CAEN_DGTZ_USB:
      return "USB";
    case CAEN_DGTZ_USB_A4818:
      return "USB_A4818";
    case CAEN_DGTZ_OpticalLink:
      return "PCI";
    case CAEN_DGTZ_USB_V4718:
      return "USB_V4718";
    case CAEN_DGTZ_ETH_V4718:
      return "ETH_V4718";
    default:
      return std::to_string(link_type);
  }
}

}






//----------DOC-MARK-----BEG*DEC-----DOC-MARK----------
class DRSProducer : public eudaq::Producer {
  public:
  DRSProducer(const std::string & name, const std::string & runcontrol);
  void DoInitialise() override;
  void DoConfigure() override;
  void DoStartRun() override;
  void DoStopRun() override;
  void DoTerminate() override;
  void DoReset() override;
  void RunLoop() override;
  void CleanupDrsResources(const std::string &phase);
  void ClearQueuedEvents();
  void ReleaseLock();
  //void make_evtCnt_corr(std::map<int, std::deque<CAEN_DGTZ_X742_EVENT_t>>* m_conn_evque);
  CAEN_DGTZ_X742_EVENT_t* deep_copy_event(const CAEN_DGTZ_X742_EVENT_t *src) ;
  void free_deep_copied_event(CAEN_DGTZ_X742_EVENT_t* event);


  static const uint32_t m_id_factory = eudaq::cstr2hash("DRSProducer");
private:
  // Keep the DRS/FERS sync counter separate from the CAEN hardware trigger time tag.
  struct DecodedQueuedEvent {
    uint32_t trigger_n = 0;
    CAEN_DGTZ_X742_EVENT_t *event = NULL;
  };

  struct CompactQueuedEvent {
    uint32_t trigger_n = 0;
    uint32_t event_size = 0;
    uint32_t board_id = 0;
    uint32_t pattern = 0;
    uint32_t channel_mask = 0;
    uint32_t event_counter = 0;
    uint32_t trigger_time_tag = 0;
    std::vector<uint8_t> payload;
  };

  bool m_flag_ts;
  bool m_flag_tg;
  bool m_debug_trigger_print;
  DrsPayloadMode m_payload_mode;
  uint32_t m_plane_id;
  FILE* m_file_lock;
  std::chrono::milliseconds m_ms_busy;
  std::chrono::microseconds m_us_evt_length; // fake event length used in sync
  bool m_exit_of_run;

  int handle =-1;                 // Single Board handle
  int vhandle[16];		  // All Boards handles
  int PID_DRS[16];

  WaveDumpConfig_t   WDcfg;
  int V1718_PID =1002; // yes!, hardcoded ...
  int ret, NBoardsDRS =0 ;
  int drs_group = 0;
  int drs_board_offset = 0;
  int drs_expected_serial = -1;
  uint32_t AllocatedSize, BufferSize, NumEvents;
  CAEN_DGTZ_BoardInfo_t   BoardInfo;
  CAEN_DGTZ_EventInfo_t   EventInfo;
  CAEN_DGTZ_X742_EVENT_t  *Event742=NULL;  /* custom event struct with 8 bit data (only for 8 bit digitizers) */
  char *EventPtr = NULL;
  char *buffer = NULL;
  double TTimeTag_calib = 58.59125; // 58.594 MHz from CAEN manual


  // WC DRS config
  int WC_DRS_ID = -1;
  int WC_DRS_FREQ = 0;
  uint32_t WC_DRS_RECORD_LENGTH = 1024;
  int WC_DRS_POST_TRIGGER = 1; 
  int DRS_BASELINE_CORR = 1;

  std::map<int, std::deque<DecodedQueuedEvent>> m_conn_evque;
  std::map<int, CAEN_DGTZ_X742_EVENT_t> m_conn_ev;
  std::map<int, std::deque<CompactQueuedEvent>> m_compact_evque;
  std::map<int, CompactQueuedEvent> m_compact_ev;


// Add to DRSProducer class declaration
  std::vector<CAEN_DGTZ_X742_EVENT_t*> m_allocated_events;

  struct shmseg *shmp = NULL;
  int shmid = -1;

};
//----------DOC-MARK-----END*DEC-----DOC-MARK----------
//----------DOC-MARK-----BEG*REG-----DOC-MARK----------
namespace{
  auto dummy0 = eudaq::Factory<eudaq::Producer>::
    Register<DRSProducer, const std::string&, const std::string&>(DRSProducer::m_id_factory);
}
//----------DOC-MARK-----END*REG-----DOC-MARK----------
//----------DOC-MARK-----BEG*CON-----DOC-MARK----------
DRSProducer::DRSProducer(const std::string & name, const std::string & runcontrol)
  :eudaq::Producer(name, runcontrol), m_debug_trigger_print(false),
   m_payload_mode(DrsPayloadMode::Decoded), m_file_lock(0), m_exit_of_run(false){
  for (int &drs_handle : vhandle) {
    drs_handle = -1;
  }
  for (int &pid : PID_DRS) {
    pid = 0;
  }
}

void DRSProducer::ReleaseLock() {
  if(m_file_lock){
#ifndef _WIN32
    flock(fileno(m_file_lock), LOCK_UN);
#endif
    fclose(m_file_lock);
    m_file_lock = 0;
  }
}

void DRSProducer::ClearQueuedEvents() {
  std::set<CAEN_DGTZ_X742_EVENT_t*> events_to_free;
  for (auto &conn_evque : m_conn_evque) {
    while (!conn_evque.second.empty()) {
      events_to_free.insert(conn_evque.second.front().event);
      conn_evque.second.pop_front();
    }
  }
  for (auto *event_ptr : m_allocated_events) {
    events_to_free.insert(event_ptr);
  }
  m_allocated_events.clear();
  m_conn_ev.clear();
  m_compact_evque.clear();
  m_compact_ev.clear();
  for (auto *event_ptr : events_to_free) {
    free_deep_copied_event(event_ptr);
  }
}

void DRSProducer::CleanupDrsResources(const std::string &phase) {
  m_exit_of_run = true;

  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
    if (vhandle[brd] < 0) {
      continue;
    }
    int local_ret = CAEN_DGTZ_SWStopAcquisition(vhandle[brd]);
    if (local_ret != CAEN_DGTZ_Success) {
      EUDAQ_WARN("DRS: " + phase + " SWStopAcquisition returned " + std::to_string(local_ret)
        + " on local board " + std::to_string(brd));
    }
    local_ret = CAEN_DGTZ_ClearData(vhandle[brd]);
    if (local_ret != CAEN_DGTZ_Success) {
      EUDAQ_WARN("DRS: " + phase + " ClearData returned " + std::to_string(local_ret)
        + " on local board " + std::to_string(brd));
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ClearQueuedEvents();

  if (Event742) {
    int event_handle = handle >= 0 ? handle : vhandle[0];
    if (event_handle >= 0) {
      int local_ret = CAEN_DGTZ_FreeEvent(event_handle, (void**)&Event742);
      if (local_ret != CAEN_DGTZ_Success) {
        EUDAQ_WARN("DRS: " + phase + " FreeEvent returned " + std::to_string(local_ret));
      }
    }
    Event742 = NULL;
  }
  if (buffer) {
    int local_ret = CAEN_DGTZ_FreeReadoutBuffer(&buffer);
    if (local_ret != CAEN_DGTZ_Success) {
      EUDAQ_WARN("DRS: " + phase + " FreeReadoutBuffer returned " + std::to_string(local_ret));
    }
    buffer = NULL;
  }

  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
    if (vhandle[brd] < 0) {
      continue;
    }
    int local_ret = CAEN_DGTZ_CloseDigitizer(vhandle[brd]);
    if (local_ret != CAEN_DGTZ_Success) {
      EUDAQ_WARN("DRS: " + phase + " CloseDigitizer returned " + std::to_string(local_ret)
        + " on local board " + std::to_string(brd));
    } else {
      EUDAQ_INFO("DRS: " + phase + " closed local board " + std::to_string(brd));
    }
    vhandle[brd] = -1;
  }

  handle = -1;
  NBoardsDRS = 0;
  AllocatedSize = 0;
  BufferSize = 0;
  NumEvents = 0;
  EventPtr = NULL;
  ReleaseLock();
  if (shmp && shmp != (void *) -1) {
    if (shmdt(shmp) == -1) {
      perror("shmdt");
    }
    shmp = NULL;
  }
}
//----------DOC-MARK-----BEG*INI-----DOC-MARK----------
void DRSProducer::DoInitialise(){

  shmid = shmget(SHM_KEY, sizeof(struct shmseg), 0644|IPC_CREAT);
  if (shmid == -1) {
    perror("Shared memory");
    EUDAQ_THROW("DRS shared memory creation failed");
  }
  EUDAQ_WARN("producer constructor: shmid = "+std::to_string(shmid));

  // Attach to the segment to get a pointer to it.
  shmp = (shmseg*)shmat(shmid, NULL, 0);
  if (shmp == (void *) -1) {
    perror("Shared memory attach");
    shmp = NULL;
    EUDAQ_THROW("DRS shared memory attach failed");
  }


  auto ini = GetInitConfiguration();
  std::string drs_prodid = ini->Get("DRS_PRODID", "my_drs0");
  std::string number_str;
  for (char c : drs_prodid) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      number_str += c;
    }
  }
  drs_group = number_str.empty() ? 0 : std::stoi(number_str);
  drs_board_offset = ini->Get("DRS_BOARD_OFFSET", drs_group);
  EUDAQ_WARN("DRS " + drs_prodid
    + ", GROUP = " + std::to_string(drs_group)
    + ", BOARD_OFFSET = " + std::to_string(drs_board_offset));

  std::string lock_path = ini->Get("DRS_DEV_LOCK_PATH", "drslockfile.txt");
  m_file_lock = fopen(lock_path.c_str(), "a");
#ifndef _WIN32
  if(flock(fileno(m_file_lock), LOCK_EX|LOCK_NB)){ //fail
    EUDAQ_THROW("unable to lock the lockfile: "+lock_path );
  }
#endif

  char *s, *sep, ss[20][16];

  std::string drs_conf_filename = ini->Get("DRS_CONF_FILE", "");
  if (drs_conf_filename.empty()) {
    const char *default_drs_conf = "user/calvision/misc/conf/WaveDumpConfig_X742.txt";
    FILE *f_default = fopen(default_drs_conf, "r");
    if (f_default) {
      fclose(f_default);
      drs_conf_filename = default_drs_conf;
    }
  }
  if (!drs_conf_filename.empty()) {
    FILE *f_ini = fopen(drs_conf_filename.c_str(), "r");
    if (!f_ini) {
      EUDAQ_THROW("DRS: cannot open DRS_CONF_FILE " + drs_conf_filename);
    }

    if (ParseConfigFile(f_ini, &WDcfg) < 0) {
      fclose(f_ini);
      EUDAQ_THROW("DRS: failed to parse DRS_CONF_FILE " + drs_conf_filename);
    }
    fclose(f_ini);
    std::string drs_link_type = ini->Get("DRS_LINK_TYPE", "");
    if (!drs_link_type.empty()) {
      WDcfg.LinkType = ParseDrsLinkType(drs_link_type);
    }
    WDcfg.LinkNum = ini->Get("DRS_LINK_NUM", WDcfg.LinkNum);
    WDcfg.ConetNode = ini->Get("DRS_CONET_NODE", WDcfg.ConetNode);
    drs_expected_serial = ini->Get("DRS_EXPECTED_SERIAL", -1);
    EUDAQ_INFO("DRS link map"
      + std::string(" type=") + DrsLinkTypeName(WDcfg.LinkType)
      + " link_num=" + std::to_string(WDcfg.LinkNum)
      + " conet_node=" + std::to_string(WDcfg.ConetNode)
      + " expected_serial=" + std::to_string(drs_expected_serial));

    if (drs_expected_serial >= 0 &&
        WDcfg.LinkType == CAEN_DGTZ_USB &&
        WDcfg.BaseAddress == 0) {
      const int scan_max = ini->Get("DRS_LINK_SCAN_MAX", 16);
      bool found_expected_serial = false;
      for (int candidate = 0; candidate < scan_max; ++candidate) {
        uint32_t link_num = static_cast<uint32_t>(candidate);
        int candidate_handle = -1;
        ret = CAEN_DGTZ_OpenDigitizer2(
          static_cast<CAEN_DGTZ_ConnectionType>(WDcfg.LinkType),
          &link_num,
          WDcfg.ConetNode,
          WDcfg.BaseAddress,
          &candidate_handle);
        if (ret) {
          continue;
        }

        CAEN_DGTZ_BoardInfo_t candidate_info;
        ret = CAEN_DGTZ_GetInfo(candidate_handle, &candidate_info);
        if (!ret && candidate_info.SerialNumber == drs_expected_serial) {
          handle = candidate_handle;
          BoardInfo = candidate_info;
          WDcfg.LinkNum = link_num;
          found_expected_serial = true;
          break;
        }
        CAEN_DGTZ_CloseDigitizer(candidate_handle);
      }
      if (!found_expected_serial) {
        EUDAQ_THROW("Unable to find DRS serial " + std::to_string(drs_expected_serial)
          + " while scanning USB links 0.." + std::to_string(scan_max - 1));
      }
    } else {
      const void *arg = nullptr;
      uint32_t link_num = WDcfg.LinkNum;
      if (WDcfg.LinkType == CAEN_DGTZ_ETH_V4718) {
        arg = WDcfg.HostName;
      } else {
        arg = &link_num;
      }

      ret = CAEN_DGTZ_OpenDigitizer2(static_cast<CAEN_DGTZ_ConnectionType>(WDcfg.LinkType), arg, WDcfg.ConetNode, WDcfg.BaseAddress, &handle);
      if (ret) {
        EUDAQ_THROW("Unable to open DRS from DRS_CONF_FILE " + drs_conf_filename + ", ret = " + std::to_string(ret));
      }

      ret = CAEN_DGTZ_GetInfo(handle, &BoardInfo);
      if (ret) {
        EUDAQ_THROW("DRS: CAEN_DGTZ_GetInfo failed after opening " + drs_conf_filename);
      }
      if (drs_expected_serial >= 0 && BoardInfo.SerialNumber != drs_expected_serial) {
        EUDAQ_THROW("DRS link " + std::to_string(WDcfg.LinkNum)
          + " opened serial " + std::to_string(BoardInfo.SerialNumber)
          + ", expected " + std::to_string(drs_expected_serial));
      }
    }

    NBoardsDRS = 1;
    vhandle[0] = handle;

    EUDAQ_INFO("DRS: opened from config " + drs_conf_filename
      + " | link " + std::to_string(WDcfg.LinkNum)
      + " | model " + BoardInfo.ModelName
      + " SN " + std::to_string(BoardInfo.SerialNumber)
      + " | PCB Revision " + std::to_string(BoardInfo.PCB_Revision)
      + " | ROC FirmwareRel " + std::string(BoardInfo.ROC_FirmwareRel)
      + " | AMX FirmwareRel " + std::string(BoardInfo.AMC_FirmwareRel));

    PID_DRS[0] = BoardInfo.SerialNumber;
    m_conn_evque[0].clear();
    if (drs_board_offset < 0 || drs_board_offset + NBoardsDRS > 20) {
      EUDAQ_THROW("DRS board offset is out of range: " + std::to_string(drs_board_offset));
    }
    shmp->nevtDRS[drs_board_offset] = 0;
    if (shmp->connectedboardsDRS < drs_board_offset + NBoardsDRS) {
      shmp->connectedboardsDRS = drs_board_offset + NBoardsDRS;
    }
    return;
  }

  std::string DRS_BASE_ADDRESS = ini->Get("DRS_BASE_ADDRESS", "3210");
  // split DRS_BASE_ADDRESS into strings separated by ':'
  NBoardsDRS = 0;
  s = &DRS_BASE_ADDRESS[0];
  while(s < DRS_BASE_ADDRESS.c_str()+strlen(DRS_BASE_ADDRESS.c_str())) {
          sep = strchr(s, ':');
          if (sep == NULL) break;
          strncpy(ss[NBoardsDRS], s, sep-s);
          ss[NBoardsDRS][sep-s] = 0;
          s += sep-s+1;
          NBoardsDRS++;
          if (NBoardsDRS == 20) break;
        }
        strcpy(ss[NBoardsDRS++], s);
  EUDAQ_INFO("DRS: found "+std::to_string(NBoardsDRS)+ " addresses in the config.ini file"
                );

  char BA[100];

  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
          // std::sprintf(BA,"%s0000", ss[brd]);
	  // ret = CAEN_DGTZ_OpenDigitizer2(CAEN_DGTZ_USB_V4718, (void *)&V4718_PID, 0 , std::stoi(BA, 0, 16), &handle);
	  std::sprintf(BA,"0x%s0000", ss[brd]);
	  ret = CAEN_DGTZ_OpenDigitizer(CAEN_DGTZ_USB, 0, 0, std::stoul(std::string(BA), 0, 16), &handle);

	  if (ret) {
		EUDAQ_THROW("Unable to open DRS at 0x"+std::string(ss[brd])+", ret = " + std::to_string(ret));
	  }
	  vhandle[brd]=handle;
    	  ret = CAEN_DGTZ_GetInfo(handle, &BoardInfo);
    	  if (ret) {
	  	EUDAQ_THROW("DRS: CAEN_DGTZ_GetInfo failed on board"+std::string(ss[brd]));
	  }else{
	  	EUDAQ_INFO("DRS: opened at 0x"+std::string(ss[brd])+ "0000 is model "+BoardInfo.ModelName
		+" SN "+std::to_string(BoardInfo.SerialNumber)
		+" | PCB Revision "+std::to_string(BoardInfo.PCB_Revision)
		+" | ROC FirmwareRel "+std::string(BoardInfo.ROC_FirmwareRel)
		+" | AMX FirmwareRel "+std::string(BoardInfo.AMC_FirmwareRel)
		);
		PID_DRS[brd]=BoardInfo.SerialNumber;
	  }

	  m_conn_evque[brd].clear();
          int global_brd = drs_board_offset + brd;
          if (global_brd < 0 || global_brd >= 20) {
            EUDAQ_THROW("DRS board offset is out of range: " + std::to_string(global_brd));
          }
          shmp->nevtDRS[global_brd]=0;

  }
  //shmp->isEvtCntCorrDRSReady = false;
  if (shmp->connectedboardsDRS < drs_board_offset + NBoardsDRS) {
    shmp->connectedboardsDRS = drs_board_offset + NBoardsDRS;
  }
  //shmp->DRS_trigC = 0;
  //shmp->DRS_trigT_last = 0;
}

//----------DOC-MARK-----BEG*CONF-----DOC-MARK----------
void DRSProducer::DoConfigure(){
  auto conf = GetConfiguration();
  conf->Print(std::cout);
  m_plane_id = conf->Get("DRS_PLANE_ID", 0);
  m_ms_busy = std::chrono::milliseconds(conf->Get("DRS_DURATION_BUSY_MS", 100));
  m_us_evt_length = std::chrono::microseconds(400); // used in sync.

  m_flag_ts = conf->Get("DRS_ENABLE_TIMESTAMP", 0);
  m_flag_tg = conf->Get("DRS_ENABLE_TRIGERNUMBER", 0);
  m_debug_trigger_print = conf->Get("DRS_DEBUG_TRIGGER_PRINT", 0);
  m_payload_mode = ParseDrsPayloadMode(conf->Get("DRS_PAYLOAD_MODE", "decoded"));
  EUDAQ_INFO("DRS payload mode: " + DrsPayloadModeName(m_payload_mode));
  if(!m_flag_ts && !m_flag_tg){
    EUDAQ_WARN("Both Timestamp and TriggerNumber are disabled. Now, Timestamp is enabled by default");
    m_flag_ts = false;
    m_flag_tg = true;
  }

  DRS_BASELINE_CORR = conf->Get("DRS_BASELINE_CORR", 1);
  WC_DRS_ID = conf->Get("WC_DRS_ID", -1);
  WC_DRS_FREQ = conf->Get("WC_DRS_FREQ", 0);
  WC_DRS_RECORD_LENGTH = conf->Get("WC_DRS_RECORD_LENGTH", 1024);
  WC_DRS_POST_TRIGGER = conf->Get("WC_DRS_POST_TRIGGER", 1);

  FILE *f_ini;
  std::string drs_conf_filename= conf->Get("DRS_CONF_FILE","NOFILE");
  f_ini = fopen(drs_conf_filename.c_str(), "r");

  ParseConfigFile(f_ini, &WDcfg);
  fclose(f_ini);

  /* *************************************************************************************** */
  /* program the digitizer                                                                   */
  /* *************************************************************************************** */
  EUDAQ_INFO("DRS: # boards in Conf "+std::to_string(NBoardsDRS));
  CAEN_DGTZ_TriggerPolarity_t Polarity;
  for( int brd = 0 ; brd<NBoardsDRS;brd++) {

     ret = CAEN_DGTZ_GetInfo(vhandle[brd], &BoardInfo);
     ret = ProgramDigitizer(vhandle[brd], WDcfg, BoardInfo);
     //EUDAQ_INFO("DRS: # boards in Conf "+std::to_string(*Polarity)+ "  " +std::to_string(WDcfg.PulsePolarity[0]));
     EUDAQ_INFO("DRS: Trigger Polarity in conf. : "+std::to_string(WDcfg.PulsePolarity[0]));

     //if (brd==0)
     //      CAEN_DGTZ_SetDRS4SamplingFrequency(vhandle[brd], (CAEN_DGTZ_DRS4Frequency_t)2);// change to 1GHz

     // Trigger on rising or falling edge
     //ret = CAEN_DGTZ_SetChannelPulsePolarity(handle, channel, CAEN_DGTZ_PulsePolarityPositive);
     for(int i=0; i<4; i++) {
        if (WDcfg.PulsePolarity[i]&&WDcfg.GroupTrgEnableMask) ret =  CAEN_DGTZ_SetTriggerPolarity(vhandle[brd], i , CAEN_DGTZ_TriggerOnFallingEdge);
     }
     ret =  CAEN_DGTZ_GetTriggerPolarity(vhandle[brd], 0 , &Polarity);
     //if (ret != CAEN_DGTZ_Success) {
     //   printf("Error setting pulse polarity\n");
     //}
     

     if (ret) {
    	EUDAQ_THROW("DRS: ProgramDigitizer failed on board"+std::to_string(BoardInfo.SerialNumber));
     }else{
        switch (Polarity) {
            case CAEN_DGTZ_TriggerOnRisingEdge:
                EUDAQ_INFO("Trigger Polarity status: Rising Edge");
                break;
            case CAEN_DGTZ_TriggerOnFallingEdge:
                EUDAQ_INFO("Trigger Polarity status: Falling Edge ");
                break;
            default:
                EUDAQ_INFO("Trigger Polarity status: Unknown "+std::to_string( Polarity));
        }
     }


  }

  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
     uint32_t status = 0;
     ret = CAEN_DGTZ_GetInfo(vhandle[brd], &BoardInfo);
     ret = CAEN_DGTZ_ReadRegister(vhandle[brd], 0x8104, &status);

     if (ret) {
    	EUDAQ_THROW("DRS: ProgramDigitizer-read failed on board"+std::to_string(BoardInfo.SerialNumber));
     }

  }
  //read twice (first read clears the previous status)
  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
     uint32_t status = 0;
     ret = CAEN_DGTZ_GetInfo(vhandle[brd], &BoardInfo);
     ret = CAEN_DGTZ_ReadRegister(vhandle[brd], 0x8104, &status);

     if (ret) {
    	EUDAQ_THROW("DRS: ProgramDigitizer-read failed on board"+std::to_string(BoardInfo.SerialNumber));
     }


     // Configure the WC DRS
     if ( brd == WC_DRS_ID){
       EUDAQ_INFO("DRS: ID for TDC = "+std::to_string(WC_DRS_ID));

       if ((ret = CAEN_DGTZ_SetDRS4SamplingFrequency(vhandle[brd], (CAEN_DGTZ_DRS4Frequency_t) WC_DRS_FREQ)) != CAEN_DGTZ_Success)
           EUDAQ_INFO("DRS: Cannot CAEN_DGTZ_SetDRS4SamplingFrequency on board "+std::to_string(brd));
       if ((ret = CAEN_DGTZ_SetRecordLength(vhandle[brd], WC_DRS_RECORD_LENGTH)) != CAEN_DGTZ_Success)
           EUDAQ_INFO("DRS: Cannot CAEN_DGTZ_SetRecordLength on board "+std::to_string(brd));
       if ((ret = CAEN_DGTZ_SetPostTriggerSize(vhandle[brd], WC_DRS_POST_TRIGGER)) != CAEN_DGTZ_Success)
           EUDAQ_INFO("DRS: Cannot CAEN_DGTZ_SetDRS4SamplingFrequency on board "+std::to_string(brd));
       if ((ret = CAEN_DGTZ_LoadDRS4CorrectionData(vhandle[brd], (CAEN_DGTZ_DRS4Frequency_t) WC_DRS_FREQ)) != CAEN_DGTZ_Success)
           EUDAQ_INFO("DRS: Cannot LoadDRS4CorrectionData on board "+std::to_string(brd));
       if ((ret = CAEN_DGTZ_EnableDRS4Correction(vhandle[brd])) != CAEN_DGTZ_Success)
          EUDAQ_INFO("DRS: Cannot EnableDRS4Correction on board "+std::to_string(brd));

     }else{
     // Load DRS factory calibration - baseline

       if (DRS_BASELINE_CORR==1) {
          if ((ret = CAEN_DGTZ_LoadDRS4CorrectionData(vhandle[brd], WDcfg.DRS4Frequency)) != CAEN_DGTZ_Success)
              EUDAQ_INFO("DRS: Cannot LoadDRS4CorrectionData on board "+std::to_string(brd));
          if ((ret = CAEN_DGTZ_EnableDRS4Correction(vhandle[brd])) != CAEN_DGTZ_Success)
             EUDAQ_INFO("DRS: Cannot EnableDRS4Correction on board "+std::to_string(brd));
       }
     }

  }
  // Allocate memory for the event data and readout buffer
  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
     ret = CAEN_DGTZ_GetInfo(vhandle[brd], &BoardInfo);
     ret = CAEN_DGTZ_AllocateEvent(vhandle[brd], (void**)&Event742);
     if (ret) {
    	EUDAQ_THROW("DRS: Allocate memory for the event data failed on board"+std::to_string(BoardInfo.SerialNumber));
     }
     ret = CAEN_DGTZ_MallocReadoutBuffer(vhandle[brd], &buffer,&AllocatedSize); /* WARNING: This malloc must be done after the digitizer programming */
     if (ret) {
    	EUDAQ_THROW("DRS: Allocate memory for the readout buffer failed on board"+std::to_string(BoardInfo.SerialNumber));
     }else{
  	EUDAQ_INFO("DRS: ProgramDigitizer successful on board SN "+std::to_string(BoardInfo.SerialNumber));
  	EUDAQ_INFO("DRS: Allocate memory for the readout buffer "+std::to_string(AllocatedSize));
     }
  }


}
//----------DOC-MARK-----BEG*RUN-----DOC-MARK----------
void DRSProducer::DoStartRun(){
  m_exit_of_run = false;
  std::chrono::time_point<std::chrono::high_resolution_clock> tp_start_aq = std::chrono::high_resolution_clock::now();
  shmp->DRS_Aqu_start_time_us=tp_start_aq;

  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
     m_conn_evque[brd].clear();
     m_compact_evque[brd].clear();
     ret = CAEN_DGTZ_GetInfo(vhandle[brd], &BoardInfo);
     ret = CAEN_DGTZ_SWStartAcquisition(vhandle[brd]);


     if (ret) {
    	EUDAQ_THROW("DRS: StartAcquisition failed on board"+std::to_string(BoardInfo.SerialNumber));
     }else{
  	EUDAQ_INFO("DRS: StartAcquisition successful on board SN "+std::to_string(BoardInfo.SerialNumber));
     }
  }



}
//----------DOC-MARK-----BEG*STOP-----DOC-MARK----------
void DRSProducer::DoStopRun(){
  m_exit_of_run = true;
  // here the hardware is told to stop data acquisition
  for( int brd = 0 ; brd<NBoardsDRS;brd++) {
     ret = CAEN_DGTZ_GetInfo(vhandle[brd], &BoardInfo);
     ret = CAEN_DGTZ_SWStopAcquisition(vhandle[brd]);
     if (ret) {
    	EUDAQ_THROW("DRS: StopAcquisition failed on board"+std::to_string(BoardInfo.SerialNumber));
     }else{
  	EUDAQ_INFO("DRS: StopAcquisition successful on board SN "+std::to_string(BoardInfo.SerialNumber));
     }

     m_conn_evque[brd].clear();
     m_compact_evque[brd].clear();
  }
  //shmp->DRS_offset_us = 0;

}
//----------DOC-MARK-----BEG*RST-----DOC-MARK----------
void DRSProducer::DoReset(){
  CleanupDrsResources("reset");
  m_ms_busy = std::chrono::milliseconds();
  m_exit_of_run = false;
  EUDAQ_INFO("DRS: reset cleanup complete");
}
//----------DOC-MARK-----BEG*TER-----DOC-MARK----------
void DRSProducer::DoTerminate(){
  CleanupDrsResources("terminate");
}
//----------DOC-MARK-----BEG*LOOP-----DOC-MARK----------
void DRSProducer::RunLoop(){
  auto tp_start_run = std::chrono::steady_clock::now();
  uint32_t trigger_n = 0;


  while(!m_exit_of_run){
    auto tp_trigger = std::chrono::steady_clock::now();
    auto tp_end_of_busy = tp_trigger + m_ms_busy;


    for( int brd = 0 ; brd<NBoardsDRS;brd++) {

       ret = CAEN_DGTZ_ReadData(vhandle[brd], CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, buffer, &BufferSize);



       NumEvents = -1;
       if (ret) {
	EUDAQ_THROW("DRS: ReadData failed ret ="+ std::to_string(ret));
       }

       ret = CAEN_DGTZ_GetNumEvents(vhandle[brd], buffer, BufferSize, &NumEvents);
       if (ret) {
 	EUDAQ_THROW("DRS: CAEN_DGTZ_GetNumEvents failed");
       }

       int global_brd = drs_board_offset + brd;
       shmp->nevtDRS[global_brd] = (int)NumEvents;


       for(int i = 0; i < (int)NumEvents; i++) {
          ret = CAEN_DGTZ_GetEventInfo(vhandle[brd], buffer, BufferSize, i, &EventInfo, &EventPtr);
          if (ret) {
	        EUDAQ_THROW("DRS: CAEN_DGTZ_GetEventInfo failed");
	  }
          if (m_payload_mode == DrsPayloadMode::Compact) {
            CompactQueuedEvent compact_event;
            compact_event.trigger_n = EventInfo.EventCounter;
            compact_event.event_size = EventInfo.EventSize;
            compact_event.board_id = EventInfo.BoardId;
            compact_event.pattern = EventInfo.Pattern;
            compact_event.channel_mask = EventInfo.ChannelMask;
            compact_event.event_counter = EventInfo.EventCounter;
            compact_event.trigger_time_tag = EventInfo.TriggerTimeTag;
            const uint8_t *payload_begin = reinterpret_cast<const uint8_t *>(EventPtr);
            compact_event.payload.assign(payload_begin, payload_begin + EventInfo.EventSize);
            m_compact_evque[brd].push_back(std::move(compact_event));
          } else {
            ret = CAEN_DGTZ_DecodeEvent(vhandle[brd], EventPtr, (void**)&Event742);
            if (ret) {
	          EUDAQ_THROW("DRS: CAEN_DGTZ_DecodeEvent failed");
	    }

	    auto copied_event = DRSProducer::deep_copy_event(Event742); // Fix for shallow copy in CAEN DIgitizer

	    if (copied_event) {
	      DecodedQueuedEvent decoded_event;
	      decoded_event.trigger_n = EventInfo.EventCounter;
	      decoded_event.event = copied_event;
	      m_conn_evque[brd].push_back(decoded_event);
	    }
          }


       }

    }
    int Nevt = 1024;
    for( int brd = 0 ; brd<NBoardsDRS;brd++) {
	int qsize = (m_payload_mode == DrsPayloadMode::Compact)
	  ? m_compact_evque[brd].size()
	  : m_conn_evque[brd].size();
	if( qsize < Nevt)
		Nevt = qsize;
    }



    // Ready to transmit the queued events with calculate Evt# offset
    for(int ievt = 0; ievt<Nevt; ievt++) {
    	    auto ev = eudaq::Event::MakeUnique("DRSProducer");
    	    ev->SetTag("Plane ID", std::to_string(m_plane_id));
	    ev->SetTag("DRS_PAYLOAD_MODE", DrsPayloadModeName(m_payload_mode));

	    if (m_payload_mode == DrsPayloadMode::Compact) {
            trigger_n = static_cast<uint32_t>(-1);
	    for(auto &conn_evque: m_compact_evque){
	        uint32_t trigger_n_ev = conn_evque.second.front().trigger_n;

		if(trigger_n_ev < trigger_n)
	          trigger_n = trigger_n_ev;
	    }
	    if(m_flag_tg)
		ev->SetTriggerN(trigger_n);
	    ev->SetTag("DRS_TRIGGER_N", std::to_string(trigger_n));

            m_compact_ev.clear(); // Just in case ...

	    for(auto &conn_evque: m_compact_evque){
		CompactQueuedEvent &ev_front = conn_evque.second.front();
		int ibrd = conn_evque.first;

		int global_brd = drs_board_offset + ibrd;
		if (ev_front.trigger_n > shmp->DRS_last_trigID[global_brd]){
	                shmp->DRS_last_event_time_us=std::chrono::high_resolution_clock::now();
			shmp->DRS_last_trigID[global_brd]= ev_front.trigger_n;
		}

		if(ev_front.trigger_n == trigger_n){
			m_compact_ev[ibrd]= std::move(ev_front);
			conn_evque.second.pop_front();

		}
	    }


	    if(m_compact_ev.size()==NBoardsDRS) {
		for( int brd = 0 ; brd<NBoardsDRS;brd++) {

	                     if( m_flag_ts && brd==0 ){
	                          auto du_ts_beg_us = std::chrono::duration_cast<std::chrono::microseconds>(shmp->DRS_Aqu_start_time_us - get_midnight_today());
			  uint64_t CTriggerTimeTag= static_cast<uint64_t>(m_compact_ev[brd].trigger_time_tag);
                          auto tp_trigger0 = std::chrono::microseconds(static_cast<long int>(CTriggerTimeTag/TTimeTag_calib/2.));
	                          du_ts_beg_us += tp_trigger0;
	                          std::chrono::microseconds du_ts_end_us(du_ts_beg_us + m_us_evt_length);
                          ev->SetTimestamp(static_cast<uint64_t>(du_ts_beg_us.count()), static_cast<uint64_t>(du_ts_end_us.count()));
                     }

		     std::vector<uint8_t> data;
		     int global_brd = drs_board_offset + brd;
			     make_header(global_brd, PID_DRS[brd], &data);

			     const auto &compact_event = m_compact_ev[brd];
			     std::string board_tag = "DRS_BOARD_" + std::to_string(global_brd);
			     ev->SetTag(board_tag + "_TRIGGER_N", std::to_string(compact_event.event_counter));
			     ev->SetTag(board_tag + "_TRIGGER_TIME_TAG", std::to_string(compact_event.trigger_time_tag));
			     if (brd == 0) {
			       ev->SetTag("DRS_TRIGGER_TIME_TAG", std::to_string(compact_event.trigger_time_tag));
			     }
			     DRSpack_compact_event(compact_event.event_size,
						   compact_event.board_id,
						   compact_event.pattern,
					   compact_event.channel_mask,
					   compact_event.event_counter,
					   compact_event.trigger_time_tag,
					   compact_event.payload.data(),
					   compact_event.payload.size(),
					   &data);

		     ev->AddBlock(m_plane_id+brd, data);
			} // loop over boards

			if (m_debug_trigger_print) {
				EUDAQ_INFO("DRS trigger debug send"
					+ std::string(" event_n=") + std::to_string(m_evt_c)
					+ " trigger_n=" + std::to_string(trigger_n)
					+ " payload=compact");
			}

			SendEvent(std::move(ev));
                m_compact_ev.clear();

	    } // if complete compact event with data from all boards

	    continue;
	    }

            trigger_n = static_cast<uint32_t>(-1);
	    for(auto &conn_evque: m_conn_evque){
	        uint32_t trigger_n_ev = conn_evque.second.front().trigger_n;

		if(trigger_n_ev < trigger_n)
	          trigger_n = trigger_n_ev;
		    }
		    if(m_flag_tg)
      		ev->SetTriggerN(trigger_n);
		    ev->SetTag("DRS_TRIGGER_N", std::to_string(trigger_n));


            m_conn_ev.clear(); // Just in case ...

	    for(auto &conn_evque: m_conn_evque){
		DecodedQueuedEvent &ev_front = conn_evque.second.front();
		CAEN_DGTZ_X742_EVENT_t* decoded_event = ev_front.event;
		int ibrd = conn_evque.first;

		int global_brd = drs_board_offset + ibrd;
		if (ev_front.trigger_n > shmp->DRS_last_trigID[global_brd]){
	                shmp->DRS_last_event_time_us=std::chrono::high_resolution_clock::now();
			shmp->DRS_last_trigID[global_brd]= ev_front.trigger_n;
		}

		//auto nowT = std::chrono::system_clock::now();
		//std::time_t time = std::chrono::system_clock::to_time_t(nowT);
		//auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(nowT.time_since_epoch()) % 1000;
		//std::tm localTime = *std::localtime(&time);
		//std::cout << std::put_time(&localTime, "%H:%M:%S") << '.' 
              	//	<< std::setfill('0') << std::setw(3) << ms.count() << '\n';


		if(ev_front.trigger_n == trigger_n){
			m_conn_ev[ibrd]= *decoded_event;
			m_allocated_events.push_back(decoded_event);
			conn_evque.second.pop_front();

	    	}
	    }


	    if(m_conn_ev.size()==NBoardsDRS) {
	    	for( int brd = 0 ; brd<NBoardsDRS;brd++) {

                     if( m_flag_ts && brd==0 ){
                          auto du_ts_beg_us = std::chrono::duration_cast<std::chrono::microseconds>(shmp->DRS_Aqu_start_time_us - get_midnight_today());
			  uint64_t CTriggerTimeTag= static_cast<uint64_t>(m_conn_ev[brd].DataGroup[0].TriggerTimeTag);
                          auto tp_trigger0 = std::chrono::microseconds(static_cast<long int>(CTriggerTimeTag/TTimeTag_calib/2.));
                          du_ts_beg_us += tp_trigger0;
                          std::chrono::microseconds du_ts_end_us(du_ts_beg_us + m_us_evt_length);
                          ev->SetTimestamp(static_cast<uint64_t>(du_ts_beg_us.count()), static_cast<uint64_t>(du_ts_end_us.count()));
                     }

			     std::vector<uint8_t> data;
			     int global_brd = drs_board_offset + brd;
			     make_header(global_brd, PID_DRS[brd], &data);
			     uint32_t trigger_time_tag = m_conn_ev[brd].DataGroup[0].TriggerTimeTag;
			     std::string board_tag = "DRS_BOARD_" + std::to_string(global_brd);
			     ev->SetTag(board_tag + "_TRIGGER_N", std::to_string(trigger_n));
			     ev->SetTag(board_tag + "_TRIGGER_TIME_TAG", std::to_string(trigger_time_tag));
			     if (brd == 0) {
			       ev->SetTag("DRS_TRIGGER_TIME_TAG", std::to_string(trigger_time_tag));
			     }

			     DRSpack_event(static_cast<void*>(&m_conn_ev[brd]),&data);

		     ev->AddBlock(m_plane_id+brd, data);
			} // loop over boards

			if (m_debug_trigger_print) {
				EUDAQ_INFO("DRS trigger debug send"
					+ std::string(" event_n=") + std::to_string(m_evt_c)
					+ " trigger_n=" + std::to_string(trigger_n));
			}


		    	SendEvent(std::move(ev));
                m_conn_ev.clear();


		for (auto event_ptr : m_allocated_events) {
		  free_deep_copied_event(event_ptr);
		}
		m_allocated_events.clear();

	    } // if complete event with data from all boards


    }// loop over all collected events in the transfer


    std::this_thread::sleep_until(tp_end_of_busy);

  } // end   while(!m_exit_of_run){

}
//----------DOC-MARK-----END*IMP-----DOC-MARK----------


CAEN_DGTZ_X742_EVENT_t* DRSProducer::deep_copy_event(const CAEN_DGTZ_X742_EVENT_t *src) {
    // Allocate memory for the new event
    CAEN_DGTZ_X742_EVENT_t *copy = (CAEN_DGTZ_X742_EVENT_t*)malloc(sizeof(CAEN_DGTZ_X742_EVENT_t));
    if (copy == NULL) {
        return NULL; // Memory allocation failed
    }

    // Initialize all pointers in copy to NULL for safety
    memset(copy, 0, sizeof(CAEN_DGTZ_X742_EVENT_t)); // ** Added for robustness **

    // Copy GrPresent array
    memcpy(copy->GrPresent, src->GrPresent, sizeof(src->GrPresent));

    // Loop through each group and copy the data
    for (int i = 0; i < MAX_X742_GROUP_SIZE; i++) {
        if (src->GrPresent[i]) { // If the group has data
            // Copy ChSize array
            memcpy(copy->DataGroup[i].ChSize, src->DataGroup[i].ChSize, sizeof(src->DataGroup[i].ChSize));

            // Copy TriggerTimeTag and StartIndexCell
            copy->DataGroup[i].TriggerTimeTag = src->DataGroup[i].TriggerTimeTag;
            copy->DataGroup[i].StartIndexCell = src->DataGroup[i].StartIndexCell;

            // Copy DataChannel pointers
            for (int j = 0; j < MAX_X742_CHANNEL_SIZE; j++) {
                if (src->DataGroup[i].ChSize[j] > 0) {
                    // Allocate memory for the channel data
                    copy->DataGroup[i].DataChannel[j] = (float*)malloc(src->DataGroup[i].ChSize[j] * sizeof(float));
                    if (copy->DataGroup[i].DataChannel[j] == NULL) {
                        // If allocation fails, free previously allocated memory
                        for (int k = 0; k < j; k++) { // Free previously allocated channels
                            free(copy->DataGroup[i].DataChannel[k]);
                        }
                        // Free memory from earlier groups
                        for (int g = 0; g < i; g++) { 
                            for (int c = 0; c < MAX_X742_CHANNEL_SIZE; c++) {
                                if (copy->DataGroup[g].DataChannel[c]) {
                                    free(copy->DataGroup[g].DataChannel[c]);
                                }
                            }
                        }
                        free(copy); // Free the main structure
                        return NULL; // Memory allocation failed
                    }
                    // Copy the channel data
                    memcpy(copy->DataGroup[i].DataChannel[j], src->DataGroup[i].DataChannel[j], src->DataGroup[i].ChSize[j] * sizeof(float));
                } else {
                    copy->DataGroup[i].DataChannel[j] = NULL; // Ensure unallocated channels are NULL
                }
            }
        } else {
            // If no data, set all pointers to NULL
            memset(copy->DataGroup[i].DataChannel, 0, sizeof(copy->DataGroup[i].DataChannel));
        }
    }

    return copy;
}


void DRSProducer::free_deep_copied_event(CAEN_DGTZ_X742_EVENT_t* event) {
    if (!event) return;

    for (int i = 0; i < MAX_X742_GROUP_SIZE; ++i) {
        for (int j = 0; j < MAX_X742_CHANNEL_SIZE; ++j) {
            if (event->DataGroup[i].DataChannel[j]) {
                free(event->DataGroup[i].DataChannel[j]);
            }
        }
    }
    free(event);
}



/*

typedef struct
{
    uint32_t             EventSize;
    uint32_t             BoardId;
    uint32_t             Pattern;
    uint32_t             ChannelMask;
    uint32_t             EventCounter;
    uint32_t             TriggerTimeTag;
} CAEN_DGTZ_EventInfo_t;

typedef struct
{
    uint32_t                 ChSize[MAX_X742_CHANNEL_SIZE];           // the number of samples stored in DataChannel array
    float                    *DataChannel[MAX_X742_CHANNEL_SIZE];     // the array of ChSize samples
    uint32_t                 TriggerTimeTag;
    uint16_t                 StartIndexCell;
} CAEN_DGTZ_X742_GROUP_t;

typedef struct
{
    uint8_t                    GrPresent[MAX_X742_GROUP_SIZE]; // If the group has data the value is 1 otherwise is 0
    CAEN_DGTZ_X742_GROUP_t    DataGroup[MAX_X742_GROUP_SIZE]; // the array of ChSize samples
} CAEN_DGTZ_X742_EVENT_t;




*/
