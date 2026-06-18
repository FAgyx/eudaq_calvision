/////////////////////////////////////////////////////////////////////
//                         2023 May 08                             //
//                   authors: R. Persiani & F. Tortorici           //
//                email: rinopersiani@gmail.com                    //
//                email: francesco.tortorici@enea.it               //
//                        notes:                                   //
/////////////////////////////////////////////////////////////////////


#include "eudaq/Producer.hh"
#include "FERS_Registers_520X.h"
#include "FERS_Registers_5215.h"
#include "FERS_paramparser.h"
#include "FERS_config.h"
#include "FERSlib.h"
//#include "FERSutils.h"

#undef max
#include <iostream>
#include <fstream>
#include <ratio>
#include <chrono>
#include <thread>
#include <atomic>
//#include <random>
#include "stdlib.h"
#ifndef _WIN32
#include <sys/file.h>
#endif
#include <iomanip>
#include <set>
#include <array>
#include <sstream>
#include <limits>
#include <cctype>
#include "FERS_EUDAQ.h"
#include <sys/time.h>

//#include "configure.h"
//#include "JanusC.h"
//#include "paramparser.h"



//RunVars_t RunVars;
//int SockConsole;	// 0: use stdio console, 1: use socket console
//char ErrorMsg[250];
//int NumBrd=2; // number of boards


//Janus_Config_t J_cfg;                                   // struct with all parameters

Config_t WDcfg;

struct shmseg *shmp;
int shmid;

namespace {
constexpr int kDebugUnknownQualifier = std::numeric_limits<int>::lowest();

struct ScopedAtomicFlag {
	explicit ScopedAtomicFlag(std::atomic<bool> &flag) : m_flag(flag) {
		m_flag.store(true, std::memory_order_release);
	}
	~ScopedAtomicFlag() {
		m_flag.store(false, std::memory_order_release);
	}
	std::atomic<bool> &m_flag;
};

bool IsValidSharedMemory(const shmseg *ptr) {
	return ptr != nullptr && ptr != reinterpret_cast<const shmseg *>(-1);
}

long long DebugWallNowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

long long DebugElapsedMs(const std::chrono::steady_clock::time_point &start_time) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start_time).count();
}

std::string DebugQualifierToString(int data_qualifier) {
	if (data_qualifier == kDebugUnknownQualifier) {
		return "NA";
	}
	return std::to_string(data_qualifier);
}

bool IsFersSpectData(int data_qualifier) {
	const int data_type = data_qualifier & 0xF;
	return data_type == DTQ_SPECT || data_type == DTQ_TSPECT;
}

bool IsFersServiceData(int data_qualifier) {
	return data_qualifier == DTQ_SERVICE;
}

std::string FormatFloat(float value, int precision = 3) {
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(precision) << value;
	return ss.str();
}

bool ParseOnOffToken(std::string token, bool *on) {
	for (char &ch : token) {
		ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
	}
	if (token == "1" || token == "ON" || token == "TRUE" || token == "ENABLE") {
		if (on) {
			*on = true;
		}
		return true;
	}
	if (token == "0" || token == "OFF" || token == "FALSE" || token == "DISABLE") {
		if (on) {
			*on = false;
		}
		return true;
	}
	return false;
}

std::string DebugTriggerToString(bool have_trigger, uint64_t trigger_id) {
	if (!have_trigger) {
		return "NA";
	}
	return std::to_string(trigger_id);
}

std::string FersLastErrorString() {
	char description[1024] = {};
	FERS_GetLastError(description);
	std::string detail(description);
	while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
		detail.pop_back();
	}
	return detail.empty() ? "no FERSlib detail" : detail;
}

std::string FersErrorString(const std::string &operation, int ret) {
	return operation + " failed, ret = " + std::to_string(ret)
		+ ", FERSlib detail: " + FersLastErrorString();
}

std::string BoundedCString(const char *value, size_t max_size) {
	size_t len = 0;
	while (len < max_size && value[len] != '\0') {
		len++;
	}
	return std::string(value, len);
}

std::string FormatHex32(uint32_t value, int width = 8) {
	std::ostringstream ss;
	ss << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << value;
	return ss.str();
}

std::string FormatFersFpgaFw(uint32_t fw_rev) {
	if (fw_rev == 0) {
		return "BootLoader";
	}
	std::ostringstream ss;
	ss << ((fw_rev >> 8) & 0xFF)
		<< "." << (fw_rev & 0xFF)
		<< " (Build = " << FormatHex32((fw_rev >> 16) & 0xFFFF, 4) << ")";
	return ss.str();
}
}




//----------DOC-MARK-----BEG*DEC-----DOC-MARK----------
class FERSProducer : public eudaq::Producer {
	public:
		FERSProducer(const std::string & name, const std::string & runcontrol);
		void DoInitialise() override;
		void DoConfigure() override;
		void DoStartRun() override;
		void DoStopRun() override;
		void DoTerminate() override;
		void DoReset() override;
		void OnUnrecognised(const std::string &cmd, const std::string &param) override;
		void RunLoop() override;
		void checkEntries(const std::map<int, std::deque<SpectEvent_t>>& m_conn_evque);
		size_t splitStringToIntArray(const std::string& input, char delimiter, int* result, size_t maxSize);
		int read_pedestal(const char *filename, int pid, uint16_t lgped[64], uint16_t hgped[64]);
		int check_TRIG_alignment();
		void DebugStartupCheckpoint(const std::string &phase);
		void DebugStartupSetLastDQ(int data_qualifier);
		static const uint32_t m_id_factory = eudaq::cstr2hash("FERSProducer");

	private:
		void PublishBoardReadback(const std::string &path, int board_index, int board_handle);
		void UpdateServiceReadback(int board_index, const ServEvent_t &event);
		void PublishHvMonitorStatus(bool force);
		bool RefreshHvMonitorFromHardware(const std::string &reason);
		bool SetHvOnOff(int board, bool on, const std::string &reason);
		void ReleaseLock();
		bool WaitForRunLoopBodyExit(const std::string &phase, std::chrono::milliseconds timeout);
		void CleanupFersResources(const std::string &phase, bool detach_shared_memory);

		bool m_flag_ts;
		bool m_flag_tg;
		uint32_t m_plane_id;
		FILE* m_file_lock;
		std::chrono::milliseconds m_ms_busy;
		std::chrono::microseconds m_us_evt_length; // fake event length used in sync
		std::atomic<bool> m_exit_of_run;
		std::atomic<bool> m_run_loop_active;
		int no_trigg = -1;
		int sw_trigger = 0;
		int spill_detect = 0;
		int read_boards = 0;
		int disable_ped = 0;
		bool m_debug_startup = false;
		bool m_debug_trigger_print = false;
		bool m_disable_t1_on_stop = false;
		std::string m_fers_readback_summary;
		std::array<uint32_t, FERSLIB_MAX_NBRD> m_t1_out_mask{};
		std::array<int, FERSLIB_MAX_NBRD> m_start_run_mode{};
		std::chrono::steady_clock::time_point m_debug_start_run_tp{};
		std::chrono::steady_clock::time_point m_last_hv_status_publish{};

		std::string c_ip;
	        int cnc=0;
        	int cnc_handle[FERSLIB_MAX_NCNC];               // Concentrator handles

		std::string fers_ip_address;  // TDLink IP address of the board
		std::string fers_eth_address;  // Eth IP address of the board
		std::string fers_id;
		int fers_group = 0;
		int handle =-1;		 	// Board handle
		//float fers_hv_vbias;
		//float fers_hv_imax;
		int fers_acq_mode;
                uint32_t StatusReg[FERSLIB_MAX_NBRD];   // Acquisition Status Register
		//int vhandle[FERSLIB_MAX_NBRD];
        	//int retriesTDL = 0;

		// staircase params
		//uint8_t stair_do;
		//uint16_t stair_start, stair_stop, stair_step, stair_shapingt;
		//uint32_t stair_dwell_time;


  		std::map<int, std::deque<SpectEvent_t>> m_conn_evque;
  		std::map<int, SpectEvent_t> m_conn_ev;

		//struct shmseg *shmp;
		//int shmid;
		int brd; // current board

};
//----------DOC-MARK-----END*DEC-----DOC-MARK----------
//----------DOC-MARK-----BEG*CON-----DOC-MARK----------
namespace{
	auto dummy0 = eudaq::Factory<eudaq::Producer>::
		Register<FERSProducer, const std::string&, const std::string&>(FERSProducer::m_id_factory);
}
//----------DOC-MARK-----END*REG-----DOC-MARK----------

FERSProducer::FERSProducer(const std::string & name, const std::string & runcontrol)
	:eudaq::Producer(name, runcontrol), m_file_lock(0), m_exit_of_run(false), m_run_loop_active(false)
{  
}

void FERSProducer::ReleaseLock() {
	if (!m_file_lock) {
		return;
	}
#ifndef _WIN32
	flock(fileno(m_file_lock), LOCK_UN);
#endif
	fclose(m_file_lock);
	m_file_lock = 0;
}

bool FERSProducer::WaitForRunLoopBodyExit(const std::string &phase, std::chrono::milliseconds timeout) {
	if (!m_run_loop_active.load(std::memory_order_acquire)) {
		return true;
	}

	EUDAQ_INFO("FERS: " + phase + " waiting for readout loop to leave FERS calls before hardware cleanup");
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (m_run_loop_active.load(std::memory_order_acquire) &&
	       std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (m_run_loop_active.load(std::memory_order_acquire)) {
		EUDAQ_WARN("FERS: " + phase + " readout loop did not exit within "
			+ std::to_string(timeout.count())
			+ " ms; continuing cleanup as best effort");
		return false;
	}

	EUDAQ_INFO("FERS: " + phase + " readout loop exited before hardware cleanup");
	return true;
}

void FERSProducer::CleanupFersResources(const std::string &phase, bool detach_shared_memory) {
	m_exit_of_run.store(true, std::memory_order_release);
	WaitForRunLoopBodyExit(phase, std::chrono::seconds(5));
	m_ms_busy = std::chrono::milliseconds();
	m_conn_evque.clear();
	m_conn_ev.clear();

	if (IsValidSharedMemory(shmp)) {
		int nboards = shmp->connectedboards[fers_group];
		if (nboards < 0) {
			nboards = 0;
		}
		if (nboards > FERSLIB_MAX_NBRD) {
			nboards = FERSLIB_MAX_NBRD;
		}

		if (nboards > 0) {
			int start_mode = m_start_run_mode[0];
			int ret = FERS_StopAcquisition(
				shmp->handle[fers_group],
				nboards,
				start_mode,
				GetRunNumber());
			if (ret != 0) {
				EUDAQ_WARN("FERS: " + phase + " FERS_StopAcquisition failed, ret = "
					+ std::to_string(ret) + ", FERSlib detail: " + FersLastErrorString());
			}
		}

		for (int brd = 0; brd < nboards; brd++) {
			const int board_handle = shmp->handle[fers_group][brd];
			if (board_handle < 0) {
				continue;
			}
			int ret = FERS_FlushData(board_handle);
			if (ret != 0) {
				EUDAQ_WARN("FERS: " + phase + " FERS_FlushData failed on board "
					+ std::to_string(brd) + ", ret = " + std::to_string(ret)
					+ ", FERSlib detail: " + FersLastErrorString());
			}
		}

		for (int brd = 0; brd < nboards; brd++) {
			const int board_handle = shmp->handle[fers_group][brd];
			if (board_handle < 0) {
				continue;
			}
			int ret = FERS_CloseReadout(board_handle);
			if (ret != 0) {
				EUDAQ_WARN("FERS: " + phase + " FERS_CloseReadout failed on board "
					+ std::to_string(brd) + ", ret = " + std::to_string(ret)
					+ ", FERSlib detail: " + FersLastErrorString());
			}
			ret = FERS_HV_Set_OnOff(board_handle, 0);
			if (ret != 0) {
				EUDAQ_WARN("FERS: " + phase + " FERS_HV_Set_OnOff(0) failed on board "
					+ std::to_string(brd) + ", ret = " + std::to_string(ret)
					+ ", FERSlib detail: " + FersLastErrorString());
			}
			ret = FERS_CloseDevice(board_handle);
			if (ret != 0) {
				EUDAQ_WARN("FERS: " + phase + " FERS_CloseDevice failed on board "
					+ std::to_string(brd) + ", ret = " + std::to_string(ret)
					+ ", FERSlib detail: " + FersLastErrorString());
			}
			shmp->handle[fers_group][brd] = -1;
		}
		shmp->connectedboards[fers_group] = 0;

		if (detach_shared_memory) {
			if (shmdt(shmp) == -1) {
				perror("shmdt");
			}
			shmp = nullptr;
		}
	}

	ReleaseLock();
	EUDAQ_INFO("FERS: " + phase + " cleanup complete");
}

void FERSProducer::DebugStartupCheckpoint(const std::string &phase) {
	if (!m_debug_startup) {
		return;
	}
	SetStatusTag("FERS_DBG_PHASE", phase);
	EUDAQ_INFO("FERS startup debug phase=" + phase
		+ " wall_ms=" + std::to_string(DebugWallNowMs())
		+ " elapsed_ms=" + std::to_string(DebugElapsedMs(m_debug_start_run_tp)));
}

void FERSProducer::DebugStartupSetLastDQ(int data_qualifier) {
	if (!m_debug_startup) {
		return;
	}
	SetStatusTag("FERS_DBG_LAST_DQ", DebugQualifierToString(data_qualifier));
}

void FERSProducer::PublishBoardReadback(const std::string &path,
		int board_index,
		int board_handle) {
	FERS_BoardInfo_t info = {};
	int ret = FERS_GetBoardInfo(board_handle, &info);

	uint32_t pid = FERS_pid(board_handle);
	char *model_ptr = FERS_ModelName(board_handle);
	std::string model = model_ptr ? std::string(model_ptr) : "";
	uint32_t fpga_fw = FERS_FPGA_FWrev(board_handle);
	uint32_t uc_fw = FERS_uC_FWrev(board_handle);

	if (ret == 0) {
		pid = info.pid;
		model = BoundedCString(info.ModelName, sizeof(info.ModelName));
		fpga_fw = info.FPGA_FWrev;
		uc_fw = info.uC_FWrev;
	} else {
		EUDAQ_WARN("FERS_GetBoardInfo failed for board "
			+ std::to_string(board_index)
			+ " at " + path
			+ ", ret = " + std::to_string(ret)
			+ ", using cached accessor values");
	}

	if (model.empty() && ret == 0) {
		model = BoundedCString(info.ModelCode, sizeof(info.ModelCode));
	}
	if (model.empty()) {
		model = "unknown";
	}

	const std::string fpga_fw_text = FormatFersFpgaFw(fpga_fw);
	const std::string uc_fw_text = FormatHex32(uc_fw);
	const std::string board_prefix = "FERS_BRD" + std::to_string(board_index) + "_";

	SetStatusTag(board_prefix + "PID", std::to_string(pid));
	SetStatusTag(board_prefix + "MODEL", model);
	SetStatusTag(board_prefix + "FPGA_FW_REV", fpga_fw_text);
	SetStatusTag(board_prefix + "UC_FW_REV", uc_fw_text);

	const std::string summary = "B" + std::to_string(board_index)
		+ " PID=" + std::to_string(pid)
		+ " Model=" + model
		+ " FPGA=" + fpga_fw_text
		+ " uC=" + uc_fw_text;
	if (!m_fers_readback_summary.empty()) {
		m_fers_readback_summary += "; ";
	}
	m_fers_readback_summary += summary;
	SetStatusTag("FERS_INFO", m_fers_readback_summary);

	EUDAQ_INFO("FERS readback " + path
		+ ": PID=" + std::to_string(pid)
		+ ", Brd Model=" + model
		+ ", FPGA FW Rev=" + fpga_fw_text
		+ ", uC FW Rev=" + uc_fw_text);
}

void FERSProducer::UpdateServiceReadback(int board_index,
		const ServEvent_t &event) {
	if (!IsValidSharedMemory(shmp) ||
			board_index < 0 ||
			board_index >= shmp->connectedboards[fers_group]) {
		return;
	}

	shmp->tempFPGA[fers_group][board_index] = event.tempFPGA;
	shmp->tempDet[fers_group][board_index] = event.tempDetector;
	shmp->tempBoard[fers_group][board_index] = event.tempBoard;
	shmp->hv_Vmon[fers_group][board_index] = event.hv_Vmon;
	shmp->hv_Imon[fers_group][board_index] = event.hv_Imon;
	shmp->hv_status_on[fers_group][board_index] = event.hv_status_on;
	shmp->FERS_LastSrvEvent_us[fers_group][board_index] =
		std::chrono::high_resolution_clock::now();
}

void FERSProducer::PublishHvMonitorStatus(bool force) {
	if (!IsValidSharedMemory(shmp)) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (!force &&
			now - m_last_hv_status_publish < std::chrono::milliseconds(1000)) {
		return;
	}
	m_last_hv_status_publish = now;

	float total_imon = 0.0f;
	for (int board = 0; board < shmp->connectedboards[fers_group]; ++board) {
		total_imon += shmp->hv_Imon[fers_group][board];
		const std::string prefix =
			"FERS_BRD" + std::to_string(board) + "_";
		SetStatusTag(prefix + "HV_SET_V",
			FormatFloat(shmp->HVbias[fers_group][board]));
		SetStatusTag(prefix + "HV_VMON",
			FormatFloat(shmp->hv_Vmon[fers_group][board]));
		SetStatusTag(prefix + "HV_IMON",
			FormatFloat(shmp->hv_Imon[fers_group][board]));
		SetStatusTag(prefix + "TEMP_DET",
			FormatFloat(shmp->tempDet[fers_group][board], 2));
		SetStatusTag(prefix + "TEMP_FPGA",
			FormatFloat(shmp->tempFPGA[fers_group][board], 2));
		SetStatusTag(prefix + "TEMP_BRD",
			FormatFloat(shmp->tempBoard[fers_group][board], 2));
		SetStatusTag(prefix + "HV_STATUS",
			shmp->hv_status_on[fers_group][board] ? "ON" : "OFF");
	}
	SetStatusTag("FERS_HV_TOTAL_IMON", FormatFloat(total_imon));
}

bool FERSProducer::RefreshHvMonitorFromHardware(const std::string &reason) {
	if (IsStatus(eudaq::Status::STATE_RUNNING)) {
		SetStatusTag("FERS_HV_MONITOR_UPDATE", "ignored: running");
		EUDAQ_WARN("FERS HV monitor update ignored while running");
		return false;
	}
	if (!IsValidSharedMemory(shmp) || shmp->connectedboards[fers_group] <= 0) {
		SetStatusTag("FERS_HV_MONITOR_UPDATE", "ignored: no boards");
		EUDAQ_WARN("FERS HV monitor update ignored: no connected boards");
		return false;
	}

	int failures = 0;
	for (int board = 0; board < shmp->connectedboards[fers_group]; ++board) {
		const int board_handle = shmp->handle[fers_group][board];
		if (board_handle < 0) {
			++failures;
			continue;
		}

		float hv_vmon = 0.0f;
		float hv_imon = 0.0f;
		float temp_detector = 0.0f;
		float temp_fpga = 0.0f;
		float temp_board = 0.0f;
		int hv_on = 0;
		int hv_ramping = 0;
		int hv_ovc = 0;
		int hv_ovv = 0;

		int ret = 0;
		ret |= FERS_HV_Get_Vmon(board_handle, &hv_vmon);
		ret |= FERS_HV_Get_Imon(board_handle, &hv_imon);
		ret |= FERS_HV_Get_DetectorTemp(board_handle, &temp_detector);
		ret |= FERS_Get_FPGA_Temp(board_handle, &temp_fpga);
		ret |= FERS_Get_Board_Temp(board_handle, &temp_board);
		ret |= FERS_HV_Get_Status(board_handle, &hv_on, &hv_ramping,
			&hv_ovc, &hv_ovv);
		if (ret != 0) {
			++failures;
			EUDAQ_WARN("FERS HV monitor update failed on board "
				+ std::to_string(board)
				+ ", ret = " + std::to_string(ret));
			continue;
		}

		shmp->hv_Vmon[fers_group][board] = hv_vmon;
		shmp->hv_Imon[fers_group][board] = hv_imon;
		shmp->tempDet[fers_group][board] = temp_detector;
		shmp->tempFPGA[fers_group][board] = temp_fpga;
		shmp->tempBoard[fers_group][board] = temp_board;
		shmp->hv_status_on[fers_group][board] = hv_on ? 1 : 0;
		shmp->FERS_LastSrvEvent_us[fers_group][board] =
			std::chrono::high_resolution_clock::now();
	}

	PublishHvMonitorStatus(true);
	SetStatusTag("FERS_HV_MONITOR_UPDATE",
		reason + ": " + std::to_string(shmp->connectedboards[fers_group] - failures)
		+ "/" + std::to_string(shmp->connectedboards[fers_group]) + " board(s)");
	return failures == 0;
}

bool FERSProducer::SetHvOnOff(int board, bool on, const std::string &reason) {
	if (IsStatus(eudaq::Status::STATE_RUNNING)) {
		SetStatusTag("FERS_HV_SWITCH", "ignored: running");
		EUDAQ_WARN("FERS HV switch ignored while running");
		return false;
	}
	if (!IsValidSharedMemory(shmp) || shmp->connectedboards[fers_group] <= 0) {
		SetStatusTag("FERS_HV_SWITCH", "ignored: no boards");
		EUDAQ_WARN("FERS HV switch ignored: no connected boards");
		return false;
	}
	if (board < 0 || board >= shmp->connectedboards[fers_group]) {
		SetStatusTag("FERS_HV_SWITCH", "ignored: invalid board");
		EUDAQ_WARN("FERS HV switch ignored: invalid board "
			+ std::to_string(board)
			+ " for " + std::to_string(shmp->connectedboards[fers_group])
			+ " connected board(s)");
		return false;
	}

	const int board_handle = shmp->handle[fers_group][board];
	if (board_handle < 0) {
		SetStatusTag("FERS_HV_SWITCH", "failed: closed board handle");
		EUDAQ_WARN("FERS HV switch failed on board "
			+ std::to_string(board) + ": closed board handle");
		return false;
	}

	const int ret = FERS_HV_Set_OnOff(board_handle, on ? 1 : 0);
	if (ret != 0) {
		SetStatusTag("FERS_HV_SWITCH", "failed: board "
			+ std::to_string(board) + " " + (on ? "ON" : "OFF"));
		EUDAQ_WARN("FERS_HV_Set_OnOff(" + std::string(on ? "1" : "0")
			+ ") failed on board " + std::to_string(board)
			+ ", ret = " + std::to_string(ret)
			+ ", FERSlib detail: " + FersLastErrorString());
		return false;
	}

	shmp->hv_status_on[fers_group][board] = on ? 1 : 0;
	const std::string prefix = "FERS_BRD" + std::to_string(board) + "_";
	SetStatusTag(prefix + "HV_STATUS", on ? "ON" : "OFF");
	SetStatusTag("FERS_HV_SWITCH", reason + ": board "
		+ std::to_string(board) + " " + (on ? "ON" : "OFF"));
	PublishHvMonitorStatus(true);
	RefreshHvMonitorFromHardware(reason + " hv switch");
	return true;
}

void FERSProducer::OnUnrecognised(const std::string &cmd,
		const std::string &param) {
	if (cmd == "FERS_UPDATE_HV_MONITOR") {
		RefreshHvMonitorFromHardware("manual");
		return;
	}
	if (cmd == "FERS_SET_HV_ONOFF") {
		int board = 0;
		std::string onoff_token;
		std::istringstream full_param(param);
		if (!(full_param >> board >> onoff_token)) {
			board = 0;
			std::istringstream state_only(param);
			if (!(state_only >> onoff_token)) {
				SetStatusTag("FERS_HV_SWITCH", "ignored: missing parameter");
				EUDAQ_WARN("FERS HV switch ignored: missing ON/OFF parameter");
				return;
			}
		}
		bool on = false;
		if (!ParseOnOffToken(onoff_token, &on)) {
			SetStatusTag("FERS_HV_SWITCH", "ignored: invalid parameter");
			EUDAQ_WARN("FERS HV switch ignored: invalid ON/OFF parameter '"
				+ onoff_token + "'");
			return;
		}
		SetHvOnOff(board, on, "manual");
		return;
	}
	eudaq::Producer::OnUnrecognised(cmd, param);
}

//----------DOC-MARK-----BEG*INI-----DOC-MARK----------
void FERSProducer::DoInitialise(){
	try {
	m_fers_readback_summary.clear();
	// see https://www.tutorialspoint.com/inter_process_communication/inter_process_communication_shared_memory.htm
	shmid = shmget(SHM_KEY, sizeof(struct shmseg), 0644|IPC_CREAT);
	//shmid = shmget(SHM_KEY, sizeof(struct shmseg), 0);
	if (shmid == -1) {
		perror("Shared memory");
		EUDAQ_THROW("FERS shared memory creation failed");
	}
	EUDAQ_WARN("producer constructor: shmid = "+std::to_string(shmid));

	// Attach to the segment to get a pointer to it.
	shmp = (shmseg*)shmat(shmid, NULL, 0);
	if (shmp == (void *) -1) {
		perror("Shared memory attach");
		shmp = nullptr;
		EUDAQ_THROW("FERS shared memory attach failed");
	}

	initshm( shmid );


	auto ini = GetInitConfiguration();
	std::string fers_prodid = ini->Get("FERS_PRODID","my_fers0");
	std::string number_str;
	for (char c : fers_prodid) {
        	if (std::isdigit(c)) {
            	number_str += c;
        	}
    	}
	fers_group = number_str.empty() ? 0 : std::stoi(number_str);
	shmp->connectedboards[fers_group]=0;

	EUDAQ_WARN("FERS "+fers_prodid+", GROUP = "+std::to_string(fers_group));
	std::string lock_path = ini->Get("FERS_DEV_LOCK_PATH", "ferslockfile.txt");
	m_file_lock = fopen(lock_path.c_str(), "a");
	if (!m_file_lock) {
		EUDAQ_THROW("unable to open the lockfile: " + lock_path);
	}
#ifndef _WIN32
	EUDAQ_INFO("FERS " + fers_prodid + " waiting for device lock: " + lock_path);
	if(flock(fileno(m_file_lock), LOCK_EX)){ // serialize FERSlib USB access across producers
		EUDAQ_THROW("unable to lock the lockfile: "+lock_path );
	}
	EUDAQ_INFO("FERS " + fers_prodid + " acquired device lock: " + lock_path);
#endif

	fers_ip_address = ini->Get("FERS_IP_ADDRESS", "");
	fers_eth_address = ini->Get("FERS_ETH_ADDRESS", "");
	fers_id = ini->Get("FERS_ID","");
	//memset(&handle, -1, sizeof(handle));
	//for (int i=0; i<FERSLIB_MAX_NBRD; i++)
	//	vhandle[i] = -1;


        if (!fers_eth_address.empty()) {
	   char eth_address[30];
           int eth_ip[16]={0};
           size_t count = splitStringToIntArray(fers_eth_address, ',', eth_ip, 16);
           int ROmode = ini->Get("FERS_RO_MODE",0);
           //std::string fers_prodid = ini->Get("FERS_PRODID","no prod ID");
           int allocsize;
           char tmp_path[100];
	   for (size_t iIP = 0; iIP < count; ++iIP) {
		if (eth_ip[iIP] < 1) continue;
                              //std::sprintf(tmp_path,"%s%d", connection_path,brd);
                              std::sprintf(tmp_path,"eth:192.168.50.%d",eth_ip[iIP]);// Eth IP
                              int ret = FERS_OpenDevice(tmp_path, &handle); // open conection to a board
                              if(ret==0){
                                    EUDAQ_INFO("Bords at "+std::string(tmp_path)
                                       +" connected to handle "+std::to_string(handle)
                                       );
                                    FERS_InitReadout(handle,ROmode,&allocsize);
                                    PublishBoardReadback(std::string(tmp_path),
                                                        shmp->connectedboards[fers_group],
                                                        handle);

                                    // fill shared struct
                                    //vhandle[shmp->connectedboards[fers_group]]=handle;
                                    shmp->handle[fers_group][shmp->connectedboards[fers_group]] = handle;
				    shmp->FERS_TDLink[fers_group][shmp->connectedboards[fers_group]]=0;
 				    shmp->FERS_LastSrvEvent_us[fers_group][shmp->connectedboards[fers_group]]=std::chrono::high_resolution_clock::now();


                                    EUDAQ_INFO("check shared on board "+std::to_string(shmp->connectedboards[fers_group])+": "
                                               +std::string(tmp_path)
                                               +"*"+std::to_string(shmp->handle[fers_group][shmp->connectedboards[fers_group]])
                                                );
                                                m_conn_evque[shmp->connectedboards[fers_group]].clear();
                                                shmp->connectedboards[fers_group]++;
                              }else{
                                    EUDAQ_THROW("Bords at "+std::string(tmp_path)
                                                +" error "+std::to_string(ret)
                                                +", FERSlib detail: "+FersLastErrorString()
                                                );
                              }

           }
        }

	if (fers_eth_address.empty() &&
	    (fers_ip_address.rfind("usb:", 0) == 0 || fers_ip_address.rfind("eth:", 0) == 0)) {
		int ROmode = ini->Get("FERS_RO_MODE", 0);
		int expected_pid = ini->Get("FERS_EXPECTED_PID", -1);
		int allocsize;
		char tmp_path[100];
		std::sprintf(tmp_path, "%s", fers_ip_address.c_str());
		int ret = FERS_OpenDevice(tmp_path, &handle);
		if (ret == 0) {
			uint32_t opened_pid = FERS_pid(handle);
			if (expected_pid >= 0 && opened_pid != static_cast<uint32_t>(expected_pid)) {
				FERS_CloseDevice(handle);
				EUDAQ_THROW("FERS at " + std::string(tmp_path)
					+ " opened PID " + std::to_string(opened_pid)
					+ ", expected " + std::to_string(expected_pid));
			}
			EUDAQ_INFO("Bords at " + std::string(tmp_path)
				+ " connected to handle " + std::to_string(handle)
				+ " PID " + std::to_string(opened_pid));
			FERS_InitReadout(handle, ROmode, &allocsize);
			PublishBoardReadback(std::string(tmp_path),
				shmp->connectedboards[fers_group],
				handle);

			shmp->handle[fers_group][shmp->connectedboards[fers_group]] = handle;
			shmp->FERS_TDLink[fers_group][shmp->connectedboards[fers_group]] = 0;
			shmp->FERS_LastSrvEvent_us[fers_group][shmp->connectedboards[fers_group]] =
				std::chrono::high_resolution_clock::now();

			EUDAQ_INFO("check shared on board " + std::to_string(shmp->connectedboards[fers_group]) + ": "
				+ std::string(tmp_path)
				+ "*" + std::to_string(shmp->handle[fers_group][shmp->connectedboards[fers_group]]));
			m_conn_evque[shmp->connectedboards[fers_group]].clear();
			shmp->connectedboards[fers_group]++;
		} else {
			EUDAQ_THROW("Bords at " + std::string(tmp_path)
				+ " error " + std::to_string(ret)
				+ ", FERSlib detail: " + FersLastErrorString());
		}
	}



        if (fers_ip_address.find("tdl") != std::string::npos) {
	  char ip_address[30];
	  char connection_path[50];

	  strcpy(ip_address, fers_ip_address.c_str());
	  sprintf(connection_path,"usb:%s",ip_address);

          char cpath[100];
	  FERS_Get_CncPath(connection_path, cpath);
	  std::string str_cpath(cpath);
          c_ip = str_cpath.substr(0,str_cpath.length() - 4); // removing ":cnc"
	  EUDAQ_INFO("cpath "+std::string(cpath) + " c_ip "+c_ip );

	  int ret ;

	  if (!FERS_IsOpen(cpath)) {
                FERS_CncInfo_t CncInfo;
                ret = FERS_OpenDevice(cpath, &cnc_handle[cnc]);  // open connection to the concetrator
                ret |= FERS_ReadConcentratorInfo(cnc_handle[cnc], &CncInfo);
                for (int i = 0; i < 8; i++) { // Loop over all the TDlink chains
                	if (CncInfo.ChainInfo[i].BoardCount > 0) {
   				EUDAQ_INFO("TDlink "+std::to_string(i)
					+" Connected Boards Count "+std::to_string(CncInfo.ChainInfo[i].BoardCount)
		 		);
				//connection_path[strlen(connection_path) - 1] = '\0';
				int ROmode = ini->Get("FERS_RO_MODE",0);
				int allocsize;
				char tmp_path[100];
                		for (brd = 0; brd < CncInfo.ChainInfo[i].BoardCount; brd++) { // Loop over all the boards
					//std::sprintf(tmp_path,"%s%d", connection_path,brd);
					std::sprintf(tmp_path,"%s:tdl:%d:%d", c_ip.c_str(),i,brd);// CNC IP : chain#: Brd#
				        ret = FERS_OpenDevice(tmp_path, &handle); // open conection to a board
					if(ret==0){
		   				EUDAQ_INFO("Bords at "+std::string(tmp_path)
						+" connected to handle "+std::to_string(handle)
				 		);
						FERS_InitReadout(handle,ROmode,&allocsize);
						PublishBoardReadback(std::string(tmp_path),
							shmp->connectedboards[fers_group],
							handle);

						// fill shared struct
						//vhandle[shmp->connectedboards[fers_group]]=handle;
						shmp->handle[fers_group][shmp->connectedboards[fers_group]] = handle;
						shmp->FERS_TDLink[fers_group][shmp->connectedboards[fers_group]]=1;
						shmp->FERS_LastSrvEvent_us[fers_group][shmp->connectedboards[fers_group]]=std::chrono::high_resolution_clock::now();

						EUDAQ_INFO("check shared on board "+std::to_string(shmp->connectedboards[fers_group])+": "
							+std::string(tmp_path)
						);
						m_conn_evque[shmp->connectedboards[fers_group]].clear();
						shmp->connectedboards[fers_group]++;
					}else{
		   				EUDAQ_THROW("Bords at "+std::string(tmp_path)
						+" error "+std::to_string(ret)
						+", FERSlib detail: "+FersLastErrorString()
				 		);
					}

				}
			}
		}
	  }
	}


	EUDAQ_WARN("FERS: # connected boards is "+std::to_string(shmp->connectedboards[fers_group]));
	SetStatusTag("FERS_BOARD_COUNT", std::to_string(shmp->connectedboards[fers_group]));
	} catch (...) {
		CleanupFersResources("init_error", true);
		throw;
	}

}

//----------DOC-MARK-----BEG*CONF-----DOC-MARK----------
void FERSProducer::DoConfigure(){
	int bcnt=0;
	if (fers_group==0) {  // create flat index
		for (int igr=0;igr<MAX_NGR;igr++){
			for(int ibrd=0;ibrd<shmp->connectedboards[igr];ibrd++){
				shmp->flat_idx[igr][ibrd]=bcnt;
				bcnt++;
			}
		}
	}
	auto conf = GetConfiguration();
	conf->Print(std::cout);
	read_boards = conf->Get("FERS_DIRECT_READ", 0);
	no_trigg = conf->Get("FERS_NO_TRIGG", 1);
	spill_detect = conf->Get("FERS_SPILL_DETECT", 0);
	sw_trigger = conf->Get("FERS_SW_TRIGGER", 0);
	m_debug_startup = conf->Get("FERS_DEBUG_STARTUP", 0);
	m_debug_trigger_print = conf->Get("FERS_DEBUG_TRIGGER_PRINT", 0);
	m_disable_t1_on_stop = conf->Get("FERS_DISABLE_T1_ON_STOP", 0);
	m_plane_id = conf->Get("EX0_PLANE_ID", 100);
	m_ms_busy = std::chrono::milliseconds(conf->Get("EX0_DURATION_BUSY_MS", 1000));
	m_us_evt_length = std::chrono::microseconds(60); // used in readou.

	m_flag_ts = conf->Get("EX0_ENABLE_TIMESTAMP", 0);
	m_flag_tg = conf->Get("EX0_ENABLE_TRIGERNUMBER", 0);
	if(!m_flag_ts && !m_flag_tg){
		EUDAQ_WARN("Both Timestamp and TriggerNumber are disabled. Now, Timestamp is enabled by default");
		m_flag_ts = false;
		m_flag_tg = true;
	}
	std::string fers_conf_filename= conf->Get("FERS_CONF_FILE","NOFILE");
	std::string fers_ped_filename= conf->Get("FERS_PED_FILE","");
	disable_ped = conf->Get("FERS_DisablePedestalCalibration", 0);

	fers_acq_mode = conf->Get("FERS_ACQ_MODE",0);

	int ret = -1; // to store return code from calls to fers
	ret = FERS_LoadConfigFile(const_cast<char*>(fers_conf_filename.c_str()));

    if (ret != 0)
       EUDAQ_THROW(FersErrorString("Cannot load FERS configuration from file " + fers_conf_filename, ret));


	//fers_hv_vbias = conf->Get("FERS_HV_Vbias", 28.);
	//fers_hv_imax = conf->Get("FERS_HV_IMax", 1.);
	//std:: cout<< " FERS_HV_Vbias " << fers_hv_vbias <<std::endl;
	float fers_dummyvar = 0;
	int ret_dummy = 0;

        //FERS_SetEnergyBitsRange(0);

        for(int kbrd = 0; kbrd < shmp->connectedboards[fers_group]; kbrd++) { // loop over boards

                //std::cout << "FERS_configure vhandle = "<<shmp->handle[fers_group][kbrd]<<std::endl;
		ret |= FERS_SendCommand(shmp->handle[fers_group][kbrd], CMD_RESET);  // Reset
		//std::this_thread::sleep_for(std::chrono::milliseconds(15));


		if (!fers_ped_filename.empty() && !disable_ped) {
			uint16_t LGped[64] = {0};
			uint16_t HGped[64] = {0};

	        char date[20];
	        uint16_t DCoffset[4];
			uint16_t LGped_check[64] = {0};
			uint16_t HGped_check[64] = {0};
			ret = read_pedestal(fers_ped_filename.c_str(), FERS_pid(shmp->handle[fers_group][kbrd]), LGped, HGped);

			FERS_WritePedestals(shmp->handle[fers_group][kbrd], LGped, HGped, NULL);
			//FERS_EnablePedestalCalibration(shmp->handle[fers_group][brd], 1);


            FERS_ReadPedestalsFromFlash(shmp->handle[fers_group][kbrd], date, LGped_check, HGped_check, DCoffset);
	        std::cout <<"Ped FERS PID = "<<FERS_pid(shmp->handle[fers_group][kbrd])<<std::endl;
			for (int kk = 0;kk<64;kk++)
				std::cout <<kk<< " LG = "<<LGped[kk]<<", HG = "<<HGped[kk]
				<< " LGread = "<<LGped_check[kk]<<", HGread = "<<HGped_check[kk]
				<<std::endl;
		}


	    ret |= FERS_configure(shmp->handle[fers_group][kbrd], CFG_HARD);
		//std::cout<<"3333 - 3333 FERScfg[6]->HV_IndivAdj[52] = "<<FERScfg[6]->HV_IndivAdj[52]<<std::endl;

		if (ret == 0) {
			EUDAQ_INFO("FERS_configure done");
		} else {
			EUDAQ_THROW(FersErrorString("FERS_configure", ret));
		}


		if(disable_ped)
			FERS_EnablePedestalCalibration(shmp->handle[fers_group][kbrd], 0);
/*
		// Pedestal calibration: check the factory ped data stored in flash

                char date[20];
                uint16_t pedLG[FERSLIB_MAX_NCH], pedHG[FERSLIB_MAX_NCH];
                uint16_t DCoffset[4];
                FERS_ReadPedestalsFromFlash(shmp->handle[fers_group][brd], date, pedLG, pedHG, DCoffset);
                std::cout <<"FERS PID = "<<FERS_pid(shmp->handle[fers_group][brd])<<std::endl;
                std::cout <<"CH   PedLG   PedHG        CH   PedLG   PedHG"<<std::endl;
                for(int ch=0; ch<32; ch++) {
                     std::cout <<ch<<", "<<  pedLG[ch]<<", "<<  pedHG[ch]<<", "<<  32+ch<<", "<<  pedLG[32+ch]<<", "<<  pedHG[32+ch]<<std::endl;
                }
                std::cout <<"DCoffset: "<<DCoffset[0]<<", "<< DCoffset[1]<<", "<<DCoffset[2]<<", "<<DCoffset[3]<<std::endl;
*/


		ret_dummy = FERS_HV_Get_Imax( shmp->handle[fers_group][kbrd], &fers_dummyvar); // read back from fers
		if (ret == 0) {
			EUDAQ_INFO("I max set!");
			std::cout << "**** readback Imax value: "<< fers_dummyvar << std::endl;
		} else {
			EUDAQ_THROW("I max NOT set");
		}
		ret_dummy = FERS_HV_Get_Vbias( shmp->handle[fers_group][kbrd], &fers_dummyvar); // read back from fers
		if (ret == 0) {
			EUDAQ_INFO("HV bias set!");
			shmp->HVbias[fers_group][kbrd] = fers_dummyvar;
			std::string temp=conf->Get("EUDAQ_DC","no data collector");
			EUDAQ_WARN("check shared in board "+std::to_string(kbrd)
			+": HVbias = "+std::to_string(shmp->HVbias[fers_group][kbrd])
			+" acqmode="+std::to_string(WDcfg.AcquisitionMode));
			sleep(0.5);
			FERS_HV_Set_OnOff(shmp->handle[fers_group][kbrd], shmp->HVbias[fers_group][kbrd]); // set HV on
			sleep(3);
			} else {
				EUDAQ_THROW("HV bias NOT set");
			}

			float hv_vmon = 0.0f;
			float hv_imon = 0.0f;
			float temp_detector = 0.0f;
			float temp_fpga = 0.0f;
			float temp_board = 0.0f;
			int mon_ret = 0;
			mon_ret |= FERS_HV_Get_Vmon(shmp->handle[fers_group][kbrd], &hv_vmon);
			mon_ret |= FERS_HV_Get_Imon(shmp->handle[fers_group][kbrd], &hv_imon);
			mon_ret |= FERS_HV_Get_DetectorTemp(shmp->handle[fers_group][kbrd],
				&temp_detector);
			mon_ret |= FERS_Get_FPGA_Temp(shmp->handle[fers_group][kbrd],
				&temp_fpga);
			mon_ret |= FERS_Get_Board_Temp(shmp->handle[fers_group][kbrd],
				&temp_board);
			if (mon_ret == 0) {
				shmp->hv_Vmon[fers_group][kbrd] = hv_vmon;
				shmp->hv_Imon[fers_group][kbrd] = hv_imon;
				shmp->tempDet[fers_group][kbrd] = temp_detector;
				shmp->tempFPGA[fers_group][kbrd] = temp_fpga;
				shmp->tempBoard[fers_group][kbrd] = temp_board;
				shmp->hv_status_on[fers_group][kbrd] = hv_vmon > 1.0f ? 1 : 0;
			} else {
				EUDAQ_WARN("FERS HV monitor readback failed on board "
					+ std::to_string(kbrd)
					+ ", ret = " + std::to_string(mon_ret));
			}

			// Preserve the low-level T1 LEMO routing loaded from the Janus/FERS config.
			m_t1_out_mask[kbrd] = FERScfg[FERS_INDEX(shmp->handle[fers_group][kbrd])]->T1_outMask;
			m_start_run_mode[kbrd] = FERScfg[FERS_INDEX(shmp->handle[fers_group][kbrd])]->StartRunMode;
		} // end loop over boards
	PublishHvMonitorStatus(true);
	//stair_do = (bool)(conf->Get("stair_do",0));
	//stair_shapingt = (uint16_t)(conf->Get("stair_shapingt",0));
	///stair_start = (uint16_t)(conf->Get("stair_start",0));
	//stair_stop  = (uint16_t)(conf->Get("stair_stop",0));
	//stair_step  = (uint16_t)(conf->Get("stair_step",0));
	//stair_dwell_time  = (uint32_t)(conf->Get("stair_dwell_time",0));






}

//----------DOC-MARK-----BEG*RUN-----DOC-MARK----------
void FERSProducer::DoStartRun(){
	m_exit_of_run.store(false, std::memory_order_release);
	m_debug_start_run_tp = std::chrono::steady_clock::now();
	if (m_debug_startup) {
		SetStatusTag("FERS_DBG_LAST_DQ", "NA");
		DebugStartupCheckpoint("start_enter");
	}

	std::chrono::time_point<std::chrono::high_resolution_clock> tp_start_aq = std::chrono::high_resolution_clock::now();
	shmp->FERS_Aqu_start_time_us=tp_start_aq;

	if (!sw_trigger) {
		// Restore T1 routing before acquisition starts to avoid slow USB register writes on an active stream.
		DebugStartupCheckpoint("start_before_restore_t1");
		for (int brd = 0; brd < shmp->connectedboards[fers_group]; brd++) {
			FERS_WriteRegister(shmp->handle[fers_group][brd], a_t1_out_mask, m_t1_out_mask[brd]);
		}
		EUDAQ_INFO("FERS: Restored configured T1 LEMO routing");
		DebugStartupCheckpoint("start_after_restore_t1");
	} else {
		DebugStartupCheckpoint("start_skip_restore_t1");
	}

	int start_mode = shmp->connectedboards[fers_group] > 0 ? m_start_run_mode[0] : STARTRUN_ASYNC;
	DebugStartupCheckpoint("start_before_acq");
	int ret = FERS_StartAcquisition(
		shmp->handle[fers_group],
		shmp->connectedboards[fers_group],
		start_mode,
		GetRunNumber());
	if (ret != 0) {
		EUDAQ_THROW("FERS_StartAcquisition failed with ret = " + std::to_string(ret));
	}
	DebugStartupCheckpoint("start_after_acq");

	for (int brd = 0; brd < shmp->connectedboards[fers_group]; brd++) { // loop over boards
		m_conn_evque[brd].clear();
	}
	DebugStartupCheckpoint("start_after_queue_clear");
	EUDAQ_INFO("FERS_ReadoutStatus (0=idle, 1=running) = "+std::to_string(FERS_ReadoutStatus));
	DebugStartupCheckpoint("start_post_sleep_removed");
	shmp->FERS_last_event_time_us=std::chrono::high_resolution_clock::now();
	shmp->DRS_last_event_time_us=std::chrono::high_resolution_clock::now();
	DebugStartupCheckpoint("start_return");

/*
                        if (streq(value, "T1-IN"))                      FERScfg[brd]->T1_outMask = 0x001;
                        else if (streq(value, "BUNCHTRG"))              FERScfg[brd]->T1_outMask = 0x002;
                        else if (streq(value, "Q-OR"))                  FERScfg[brd]->T1_outMask = 0x004;
                        else if (streq(value, "RUN"))                   FERScfg[brd]->T1_outMask = 0x008;
                        else if (streq(value, "PTRG"))                  FERScfg[brd]->T1_outMask = 0x010;
                        else if (streq(value, "BUSY"))                  FERScfg[brd]->T1_outMask = 0x020;
                        else if (streq(value, "DPROBE"))                FERScfg[brd]->T1_outMask = 0x040;
                        else if (streq(value, "TLOGIC"))                FERScfg[brd]->T1_outMask = 0x080;
                        else if (streq(value, "SQ_WAVE"))               FERScfg[brd]->T1_outMask = 0x100;
                        else if (streq(value, "TDL_SYNC"))              FERScfg[brd]->T1_outMask = 0x200;
                        else if (streq(value, "RUN_SYNC"))              FERScfg[brd]->T1_outMask = 0x400;
                        else if (streq(value, "ZERO"))                  FERScfg[brd]->T1_outMask = 0x000;
*/
}

//----------DOC-MARK-----BEG*STOP-----DOC-MARK----------
void FERSProducer::DoStopRun(){
	m_exit_of_run.store(true, std::memory_order_release);
	EUDAQ_INFO("FERS: Stop requested; waiting for readout loop before stopping acquisition");
	WaitForRunLoopBodyExit("stop", std::chrono::seconds(2));

	if (IsValidSharedMemory(shmp)) {
		int nboards = shmp->connectedboards[fers_group];
		if (nboards < 0) {
			nboards = 0;
		}
		if (nboards > FERSLIB_MAX_NBRD) {
			nboards = FERSLIB_MAX_NBRD;
		}
		int start_mode = nboards > 0 ? m_start_run_mode[0] : STARTRUN_ASYNC;
		int ret = FERS_StopAcquisition(
			shmp->handle[fers_group],
			nboards,
			start_mode,
			GetRunNumber());
		if (ret != 0) {
			EUDAQ_THROW("FERS_StopAcquisition failed with ret = " + std::to_string(ret));
		}
		if (m_disable_t1_on_stop && !sw_trigger) {
			for (int brd = 0; brd < nboards; brd++) {
				FERS_WriteRegister(shmp->handle[fers_group][brd], a_t1_out_mask, 0);
			}
			EUDAQ_INFO("FERS: T1 LEMO routing disabled after acquisition stop");
		}
	}
	m_conn_evque.clear();
	m_conn_ev.clear();
}

//----------DOC-MARK-----BEG*RST-----DOC-MARK----------
void FERSProducer::DoReset(){
	CleanupFersResources("reset", true);
}

//----------DOC-MARK-----BEG*TER-----DOC-MARK----------
void FERSProducer::DoTerminate(){
	CleanupFersResources("terminate", true);
}


//----------DOC-MARK-----BEG*LOOP-----DOC-MARK----------
void FERSProducer::RunLoop(){
	ScopedAtomicFlag run_loop_active(m_run_loop_active);
	auto tp_start_run = std::chrono::steady_clock::now();


	//std::chrono::time_point<std::chrono::high_resolution_clock> runloop_time = std::chrono::high_resolution_clock::now();
	//auto run_time =  std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::high_resolution_clock::now() - runloop_time);

        std::set<int> expected_boards; 
        for (int i = 0; i < shmp->connectedboards[fers_group]; ++i) {
            expected_boards.insert(i);
        }

	int newData =0; 
	uint64_t debug_read_attempts = 0;
	uint64_t debug_empty_reads = 0;
	uint64_t debug_accepted_reads = 0;
	uint64_t debug_rejected_reads = 0;
	int debug_last_dq = kDebugUnknownQualifier;
	int debug_last_nb = -1;
	int debug_last_status = -1;
	int debug_last_board = -1;
	uint64_t debug_last_trigger = 0;
	bool debug_have_last_trigger = false;
	bool debug_logged_first_accepted = false;
	bool debug_logged_first_send = false;
	long long debug_last_read_ms = -1;
	long long debug_max_read_ms = -1;
	std::string debug_last_read_api = "NA";
	std::string debug_max_read_api = "NA";
	auto debug_next_summary = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	if (m_debug_startup) {
		DebugStartupCheckpoint("runloop_enter");
		SetStatusTag("FERS_DBG_PHASE", "runloop_wait_first_send");
	}
	auto debug_note_read = [&](const std::string &read_api,
		int board_index,
		int data_qualifier,
		int data_words,
		int read_status,
		long long read_call_ms,
		void *event_ptr) {
		if (!m_debug_startup || debug_logged_first_send) {
			return;
		}
		debug_read_attempts++;
		debug_last_read_api = read_api;
		debug_last_board = board_index;
		debug_last_dq = data_qualifier;
		debug_last_nb = data_words;
		debug_last_status = read_status;
		debug_last_read_ms = read_call_ms;
		if (read_call_ms > debug_max_read_ms) {
			debug_max_read_ms = read_call_ms;
			debug_max_read_api = read_api;
		}
		DebugStartupSetLastDQ(data_qualifier);
		EUDAQ_INFO("FERS startup debug read_call"
			+ std::string(" wall_ms=") + std::to_string(DebugWallNowMs())
			+ " elapsed_ms=" + std::to_string(DebugElapsedMs(m_debug_start_run_tp))
			+ " api=" + read_api
			+ " board=" + std::to_string(board_index)
			+ " call_ms=" + std::to_string(read_call_ms)
			+ " status=" + std::to_string(read_status)
			+ " nb=" + std::to_string(data_words)
			+ " dq=" + DebugQualifierToString(data_qualifier)
			+ " trigger=" + DebugTriggerToString(debug_have_last_trigger, debug_last_trigger));
		if (data_words == 0) {
			debug_empty_reads++;
			return;
		}
		if (data_words > 0 && IsFersSpectData(data_qualifier)) {
			debug_accepted_reads++;
			if (event_ptr != nullptr) {
				SpectEvent_t* event_spect = static_cast<SpectEvent_t*>(event_ptr);
				debug_last_trigger = event_spect->trigger_id;
				debug_have_last_trigger = true;
			}
			return;
		}
		if (data_words > 0) {
			debug_rejected_reads++;
		}
	};
	auto debug_log_first_accepted = [&](int board_index, int data_qualifier, const SpectEvent_t &event_spect) {
		if (!m_debug_startup || debug_logged_first_accepted) {
			return;
		}
		debug_logged_first_accepted = true;
		SetStatusTag("FERS_DBG_PHASE", "first_accepted");
		DebugStartupSetLastDQ(data_qualifier);
		EUDAQ_INFO("FERS startup debug first_accepted"
			+ std::string(" wall_ms=") + std::to_string(DebugWallNowMs())
			+ " elapsed_ms=" + std::to_string(DebugElapsedMs(m_debug_start_run_tp))
			+ " board=" + std::to_string(board_index)
			+ " dq=" + std::to_string(data_qualifier)
			+ " trigger_id=" + std::to_string(event_spect.trigger_id));
	};
	auto debug_maybe_log_summary = [&](const std::string &phase) {
		if (!m_debug_startup || debug_logged_first_send) {
			return;
		}
		auto now = std::chrono::steady_clock::now();
		if (now < debug_next_summary) {
			return;
		}
		SetStatusTag("FERS_DBG_PHASE", phase);
		EUDAQ_INFO("FERS startup debug summary phase=" + phase
			+ " wall_ms=" + std::to_string(DebugWallNowMs())
			+ " elapsed_ms=" + std::to_string(DebugElapsedMs(m_debug_start_run_tp))
			+ " read_attempts=" + std::to_string(debug_read_attempts)
			+ " nb_zero=" + std::to_string(debug_empty_reads)
			+ " dq_spect=" + std::to_string(debug_accepted_reads)
			+ " dq_other=" + std::to_string(debug_rejected_reads)
			+ " last_status=" + std::to_string(debug_last_status)
			+ " last_nb=" + std::to_string(debug_last_nb)
			+ " last_dq=" + DebugQualifierToString(debug_last_dq)
			+ " last_api=" + debug_last_read_api
			+ " last_call_ms=" + std::to_string(debug_last_read_ms)
			+ " max_call_ms=" + std::to_string(debug_max_read_ms)
			+ " max_api=" + debug_max_read_api
			+ " last_board=" + std::to_string(debug_last_board)
			+ " last_trigger=" + DebugTriggerToString(debug_have_last_trigger, debug_last_trigger));
		debug_next_summary = now + std::chrono::seconds(1);
	};
	// Skip proactive HV/temperature register polling before the first readout event.
	// Janus relies on service data during the run and avoids this eager USB access.

	while(!m_exit_of_run.load(std::memory_order_acquire)){


			auto tp_trigger = std::chrono::steady_clock::now();
			auto tp_end_of_busy = tp_trigger + m_ms_busy;

			int nchan = 64;
			int DataQualifier = -1;
			void *Event;

			double tstamp_us = -1;
			int nb = -1;
			int bindex = -1;
			int status = -1;

			int r_status=0;

			auto now = std::chrono::high_resolution_clock::now();
			auto elapsedFERS = std::chrono::duration_cast<std::chrono::milliseconds>( now - shmp->FERS_last_event_time_us);
			auto elapsedDRS = std::chrono::duration_cast<std::chrono::milliseconds>( now - shmp->DRS_last_event_time_us);


			if (spill_detect && elapsedFERS.count() > 2500 && elapsedFERS.count() < 3000 && elapsedDRS.count() > 2000) {
				//std::cout<<"---3333---  End od spill" <<std::endl;

				int ret = check_TRIG_alignment(); // All trigger IDs should match here.
				if( ret <0 ){
				        for(int cbrd =0; cbrd < shmp->connectedboards[fers_group]; cbrd++) { // loop over boards
				                FERS_SendCommand( shmp->handle[fers_group][cbrd], CMD_ACQ_STOP );
			        	}

					EUDAQ_THROW("FERS and DRS Trigger IDs are not aligned between spills.");
				}
				auto time_diff = std::chrono::system_clock::now() - shmp->FERS_LastSrvEvent_us[fers_group][0];
				auto time_diff_sec = std::chrono::duration_cast<std::chrono::seconds>(time_diff);
				if (time_diff_sec.count()>2){
					for (int ibrd = 0; ibrd < shmp->connectedboards[fers_group]; ibrd++) {
						int ret = 0;
						float tempFPGA,tempDetector,tempBoard,hv_Vmon,hv_Imon;
			                	ret |= FERS_HV_Get_Vmon(shmp->handle[fers_group][ibrd], &hv_Vmon);
				                ret |= FERS_HV_Get_Imon(shmp->handle[fers_group][ibrd], &hv_Imon);
        				        ret |= FERS_HV_Get_DetectorTemp(shmp->handle[fers_group][ibrd], &tempDetector);
			        	        ret |= FERS_Get_FPGA_Temp(shmp->handle[fers_group][ibrd], &tempFPGA);
						ret |= FERS_Get_Board_Temp(shmp->handle[fers_group][ibrd], &tempBoard);
						shmp->tempFPGA[fers_group][ibrd]=tempFPGA;
						shmp->tempDet[fers_group][ibrd]=tempDetector;
						shmp->tempBoard[fers_group][ibrd]=tempBoard;
						shmp->hv_Vmon[fers_group][ibrd]=hv_Vmon;
						shmp->hv_Imon[fers_group][ibrd]=hv_Imon;

						shmp->FERS_LastSrvEvent_us[fers_group][ibrd]=std::chrono::high_resolution_clock::now();
					}
					PublishHvMonitorStatus(false);
				}
			}


			if (read_boards) {
			   for (int ibrd = 0; ibrd < shmp->connectedboards[fers_group]; ibrd++) {
				if (m_exit_of_run.load(std::memory_order_acquire)) {
					break;
				}
				//run_time =  std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::high_resolution_clock::now() - runloop_time);
				//std::cout<<"---3333--- time in ms before FERS_GetEvent " <<run_time.count()/1000.<<std::endl;

				int iibrd=ibrd;
				if (sw_trigger) {
					FERS_SendCommand(shmp->handle[fers_group][ibrd], CMD_TRG);   // SW trg
				}

				const std::string debug_read_api = shmp->FERS_TDLink[fers_group][ibrd]
					? "FERS_GetEvent"
					: "FERS_GetEventFromBoard";
				auto debug_read_tp = std::chrono::steady_clock::now();
				if (shmp->FERS_TDLink[fers_group][ibrd]) {
					status = FERS_GetEvent(shmp->handle[fers_group], &iibrd, &DataQualifier, &tstamp_us, &Event, &nb);
				}else{
					status = FERS_GetEventFromBoard(shmp->handle[fers_group][ibrd], &DataQualifier, &tstamp_us, &Event, &nb);
				}
				long long debug_read_call_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - debug_read_tp).count();

				if (m_exit_of_run.load(std::memory_order_acquire)) {
					break;
				}

				if(status<0){
			            EUDAQ_THROW("FERS: Readout failure,  ret = " + std::to_string(status)+" board = "+std::to_string(iibrd));
				}

				debug_note_read(debug_read_api, iibrd, DataQualifier, nb, status, debug_read_call_ms, Event);

				if(nb>0 && IsFersServiceData(DataQualifier) && Event) {
					UpdateServiceReadback(iibrd, *static_cast<ServEvent_t*>(Event));
					PublishHvMonitorStatus(false);
					continue;
				}

				if(nb>0 && IsFersSpectData(DataQualifier)) { // Data event in spectroscopy mode
					newData++; // data - events*boards
					SpectEvent_t* EventSpect = (SpectEvent_t*)Event;
					debug_log_first_accepted(iibrd, DataQualifier, *EventSpect);
					m_conn_evque[iibrd].push_back(*EventSpect);
					if(EventSpect->trigger_id > shmp->FERS_last_trigID[fers_group][iibrd]){
						shmp->FERS_last_event_time_us=std::chrono::high_resolution_clock::now();
						shmp->FERS_last_trigID[fers_group][iibrd] = EventSpect->trigger_id;
					}
				}

				//run_time =  std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::high_resolution_clock::now() - runloop_time);
				//std::cout<<"---3333--- time in ms after FERS_GetEvent " <<run_time.count()/1000.<<std::endl;

			   }
			}else{
 			   DataQualifier=1000;
		           while(newData < shmp->connectedboards[fers_group] &&
				  DataQualifier > 0 &&
				  !m_exit_of_run.load(std::memory_order_acquire)) { // read all data from the boards
				auto debug_read_tp = std::chrono::steady_clock::now();
				status = FERS_GetEvent(shmp->handle[fers_group], &bindex, &DataQualifier, &tstamp_us, &Event, &nb);
				long long debug_read_call_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - debug_read_tp).count();
				if(status<0){
			            EUDAQ_THROW("FERS: Readout failure,  ret = " + std::to_string(status));
				    
				}
				debug_note_read("FERS_GetEvent", bindex, DataQualifier, nb, status, debug_read_call_ms, Event);
				if(nb>0 && IsFersServiceData(DataQualifier) && Event) {
					UpdateServiceReadback(bindex, *static_cast<ServEvent_t*>(Event));
					PublishHvMonitorStatus(false);
					continue;
				}
				if(nb>0 && IsFersSpectData(DataQualifier)) { // Data event in spectroscopy mode
					newData++; // data - events*boards
					SpectEvent_t* EventSpect = (SpectEvent_t*)Event;
					debug_log_first_accepted(bindex, DataQualifier, *EventSpect);
					m_conn_evque[bindex].push_back(*EventSpect);
				}
			   } // end of - read all data from the boards
			} // choose read method
		debug_maybe_log_summary("runloop_wait_first_send");
		if (m_exit_of_run.load(std::memory_order_acquire)) {
			break;
		}
		if(no_trigg>0) checkEntries(m_conn_evque); // detect no data sent by FERS board(s)


		if(!m_exit_of_run.load(std::memory_order_acquire) &&
		   newData >= shmp->connectedboards[fers_group]) {  // if evt*boards >= boards, i.e. at least one complete event candidate

			newData=0;
    			int Nevt = 1000;

			// check if there is enough FERS data to asamble an event
			// Find the min number of events that could be assambled - Nevt
    			for( int brd = 0 ; brd<shmp->connectedboards[fers_group];brd++) {  
                		int qsize = m_conn_evque[brd].size();
               			if( qsize < Nevt) Nevt = qsize;
    			}


			// Ready to transmit the queued events with calculate Evt# offset
			for(int ievt = 0; ievt<Nevt; ievt++) {

				auto ev = eudaq::Event::MakeUnique("FERSProducer");
				ev->SetTag("Plane ID", std::to_string(m_plane_id));

	            		uint64_t trigger_n = std::numeric_limits<uint64_t>::max();
	            		bool have_trigger = false;
        	    		for(auto &conn_evque: m_conn_evque){ // find the min trigger cnt in the queue
					if (conn_evque.second.empty()) {
						continue;
					}
                			uint64_t trigger_n_ev = conn_evque.second.front().trigger_id;
       	        			if(!have_trigger || trigger_n_ev < trigger_n) {
               	  				trigger_n = trigger_n_ev;
						have_trigger = true;
					}
				}

				if (!have_trigger) {
					break;
				}

				if(m_flag_tg)
					ev->SetTriggerN(static_cast<uint32_t>(trigger_n));



				m_conn_ev.clear(); // Just in case ...

				int bCntr = 0;
				std::set<int> processed_boards;

				// assemble one event with same trig count for all boards
		            	for(auto &conn_evque: m_conn_evque){
					if (conn_evque.second.empty()) {
						continue;
					}
        	        		auto &ev_front = conn_evque.second.front();
                			int ibrd = conn_evque.first;

                			if(ev_front.trigger_id == trigger_n){
                        			m_conn_ev[ibrd]=ev_front;
                        			conn_evque.second.pop_front();
						bCntr++;
						processed_boards.insert(ibrd);
	                		}
        	    		}

	            		if(bCntr!=shmp->connectedboards[fers_group]) {  // check for missing data from some of the FERS boards
					std::ostringstream missing_boards_stream;

					for (int expected : expected_boards) {
					    if (processed_boards.find(expected) == processed_boards.end()) {
					        if (!missing_boards_stream.str().empty()) {
					            missing_boards_stream << ", ";
					        }
					        missing_boards_stream << expected;
					    }
					}

					std::string missing_boards = missing_boards_stream.str();

        	        		//EUDAQ_WARN("FERS: Event alignment failed with "+std::to_string(m_conn_ev.size())
                	        	//	+" board's records instead of "+std::to_string(shmp->connectedboards) );

					EUDAQ_WARN("FERS: Event alignment failed with " + std::to_string(m_conn_ev.size())
					        + " board's records instead of " + std::to_string(shmp->connectedboards[fers_group]) +
					        ". Missing boards: " + missing_boards);

					m_conn_ev.clear();
            			}else{
                			for( int brd = 0 ; brd<shmp->connectedboards[fers_group];brd++) {
						if( m_flag_ts && brd==0 ){
							auto du_ts_beg_us = std::chrono::duration_cast<std::chrono::microseconds>(shmp->FERS_Aqu_start_time_us - get_midnight_today());
							auto tp_trigger0 = std::chrono::microseconds(static_cast<long int>(m_conn_ev[brd].tstamp_us));
							du_ts_beg_us += tp_trigger0;
							std::chrono::microseconds du_ts_end_us(du_ts_beg_us + m_us_evt_length);
							ev->SetTimestamp(static_cast<uint64_t>(du_ts_beg_us.count()), static_cast<uint64_t>(du_ts_end_us.count()));
						}

                        			std::vector<uint8_t> data;
						make_headerFERS(shmp->flat_idx[fers_group][brd], FERS_pid(shmp->handle[fers_group][brd]),
							shmp->hv_Vmon[fers_group][brd],shmp->hv_Imon[fers_group][brd], shmp->tempDet[fers_group][brd],shmp->tempFPGA[fers_group][brd], &data);
        	                		// Add data here
						FERSpackevent( static_cast<void*>(&m_conn_ev[brd]), 1, &data);
						uint32_t block_id = m_plane_id + shmp->flat_idx[fers_group][brd];

						int n_blocks = ev->AddBlock(block_id, data);

	                		}

					//ev->Print(std::cout);
						if (m_debug_startup && !debug_logged_first_send) {
							debug_logged_first_send = true;
							SetStatusTag("FERS_DBG_PHASE", "first_send");
						EUDAQ_INFO("FERS startup debug first_send"
							+ std::string(" wall_ms=") + std::to_string(DebugWallNowMs())
							+ " elapsed_ms=" + std::to_string(DebugElapsedMs(m_debug_start_run_tp))
							+ " trigger_id=" + std::to_string(trigger_n)
								+ " complete_queue_depth=" + std::to_string(Nevt - ievt));
						}
						if (m_debug_trigger_print) {
							EUDAQ_INFO("FERS trigger debug send"
								+ std::string(" event_n=") + std::to_string(m_evt_c)
								+ " trigger_n=" + std::to_string(trigger_n));
						}
						SendEvent(std::move(ev));

					m_conn_ev.clear(); // clear single-event buffer

        	    		}//m_conn_ev.size check
			} // Nevt

		} // End NewData


		if (!m_exit_of_run.load(std::memory_order_acquire)) {
			std::this_thread::sleep_until(tp_end_of_busy);
		}

	}// while !m_exit_of_run 
}
//----------DOC-MARK-----END*IMP-----DOC-MARK----------
void FERSProducer::checkEntries(const std::map<int, std::deque<SpectEvent_t>>& m_conn_evque) {
    size_t max_size = 0;

    // Find the maximum size among all deques
    for (const auto& pair : m_conn_evque) {
        max_size = std::max(max_size, pair.second.size());
    }

    // Check for deques with sizes 30 less than max_size
    for (const auto& pair : m_conn_evque) {
        size_t size_diff = max_size - pair.second.size();

        if (size_diff >= no_trigg+500) {
            EUDAQ_THROW("FERS Board missed 500 events !");
        }
    }
}


size_t FERSProducer::splitStringToIntArray(const std::string& input, char delimiter, int* result, size_t maxSize) {
    std::stringstream ss(input);
    std::string item;
    size_t index = 0;
    size_t index1 = 0;

    while (std::getline(ss, item, delimiter)) {
        if (index < maxSize) {
            try {
                result[index++] = std::stoi(item); // Convert to int and store in the array
            } catch (const std::invalid_argument& e) {
                std::cerr << "Invalid input: " << item << " is not an integer." << std::endl;
            }
        } else {
            std::cerr << "Warning: Too many elements, ignoring excess." << std::endl;
            break;
        }
    }

    // Fill remaining array slots with zeros
    index1 = index;
    while (index1 < maxSize) {
        result[index1++] = 0;
    }

    return index; // Return the count of successfully populated entries
}

int FERSProducer::read_pedestal(const char *filename, int pid, uint16_t lgped[64], uint16_t hgped[64]) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open file");
        return -1;
    }

    char line[4096]; // Large enough buffer for one line

    while (fgets(line, sizeof(line), file)) {
        // Parse PID
        char *pid_str = strtok(line, ":");
        if (!pid_str) continue;

        int current_pid = atoi(pid_str);
        if (current_pid != pid) continue;

        // Parse LGped and HGped parts
        char *lgped_str = strtok(NULL, ";");
        char *hgped_str = strtok(NULL, "\n");
        if (!lgped_str || !hgped_str) {
            fclose(file);
            fprintf(stderr, "Malformed line for PID %d\n", pid);
            return -1;
        }

        // Parse LGped values
        int count = 0;
        char *token = strtok(lgped_str, "|");
        while (token && count < 64) {
            lgped[count++] = (uint32_t)atoi(token);
            token = strtok(NULL, "|");
        }
        if (count != 64) {
            fclose(file);
            fprintf(stderr, "LGped must have exactly %d values, got %d\n", 64, count);
            return -1;
        }

        // Parse HGped values
        count = 0;
        token = strtok(hgped_str, "|");
        while (token && count < 64) {
            hgped[count++] = (uint32_t)atoi(token);
            token = strtok(NULL, "|");
        }
        if (count != 64) {
            fclose(file);
            fprintf(stderr, "HGped must have exactly %d values, got %d\n", 64, count);
            return -1;
        }

        fclose(file);
        return 0; // Success
    }

    fclose(file);
    fprintf(stderr, "PID %d not found in file\n", pid);
    return -1; // PID not found
}

int FERSProducer::check_TRIG_alignment() {
    int max_fers_val = 0;
    int max_drs_val = 0;

    // Find the maximum FERS trigger ID
    for (int i = 0; i < MAX_NGR; i++) {
        for (int j = 0; j < shmp->connectedboards[i]; j++) {
            int fers_val = static_cast<int>(shmp->FERS_last_trigID[i][j]);
            if (fers_val > max_fers_val) {
                max_fers_val = fers_val;
            }
        }
    }

    // Find the maximum DRS trigger ID
    for (int index = 0; index < shmp->connectedboardsDRS; index++) {
        int drs_val = static_cast<int>(shmp->DRS_last_trigID[index]);
        if (drs_val > max_drs_val) {
            max_drs_val = drs_val;
        }
    }

    // Compare the maximum trigger IDs
    if (max_fers_val > 0 && max_drs_val > 0) { // Ensure non-zero values
        if (max_fers_val != max_drs_val) {
            std::cerr << "Trigger ID mismatch"
                      << ": Max DRS=" << max_drs_val
                      << ", Max FERS=" << max_fers_val << "\n";
            auto nowT = std::chrono::system_clock::now();
            std::time_t time = std::chrono::system_clock::to_time_t(nowT);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(nowT.time_since_epoch()) % 1000;
            std::tm localTime = *std::localtime(&time);
            std::cout << std::put_time(&localTime, "%H:%M:%S") << '.'
                << std::setfill('0') << std::setw(3) << ms.count() << '\n';



            return -1;
        }
    } 

    return 0; // All good
}
