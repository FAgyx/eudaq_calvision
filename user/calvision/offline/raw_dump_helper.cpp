#include "eudaq/Event.hh"
#include "eudaq/FileDeserializer.hh"
#include "eudaq/Factory.hh"
#include "eudaq/Utils.hh"

#include "CAENDigitizer.h"
#include "FERS_Registers_5202.h"
#include "FERSlib.h"
#undef max
#undef min
#include "DRS_EUDAQ.h"
#include "FERS_EUDAQ.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Config {
  std::string raw_path;
  std::string drs_dump_path;
  std::string fers_dump_path;
  int drs_board = 0;
  int drs_group = 0;
  int drs_channel = 0;
  int fers_board = 0;
  int fers_channel = 0;
  int drs_limit = 20;
};

Config ParseArgs(int argc, char **argv) {
  if (argc != 10) {
    throw std::runtime_error(
        "Usage: raw_dump_helper <raw> <drs.tsv> <fers.tsv> "
        "<drs_board> <drs_group> <drs_channel> <fers_board> <fers_channel> <drs_limit>");
  }

  Config cfg;
  cfg.raw_path = argv[1];
  cfg.drs_dump_path = argv[2];
  cfg.fers_dump_path = argv[3];
  cfg.drs_board = std::stoi(argv[4]);
  cfg.drs_group = std::stoi(argv[5]);
  cfg.drs_channel = std::stoi(argv[6]);
  cfg.fers_board = std::stoi(argv[7]);
  cfg.fers_channel = std::stoi(argv[8]);
  cfg.drs_limit = std::stoi(argv[9]);

  if (cfg.drs_group < 0 || cfg.drs_group >= MAX_X742_GROUP_SIZE) {
    throw std::runtime_error("DRS group out of range");
  }
  if (cfg.drs_channel < -1 || cfg.drs_channel >= MAX_X742_CHANNEL_SIZE) {
    throw std::runtime_error("DRS channel out of range");
  }
  if (cfg.fers_channel < -1 || cfg.fers_channel >= 64) {
    throw std::runtime_error("FERS channel out of range");
  }
  if (cfg.drs_limit <= 0) {
    throw std::runtime_error("DRS event limit must be positive");
  }
  return cfg;
}

void ProcessSubEvent(const eudaq::Event &ev,
                     const Config &cfg,
                     std::ofstream &drs_out,
                     std::ofstream &fers_out,
                     int &drs_written,
                     int &fers_written) {
  const std::string description = ev.GetDescription();

  if (cfg.drs_channel >= 0 && description == "DRSProducer") {
    if (drs_written >= cfg.drs_limit) {
      return;
    }
    for (auto block_n : ev.GetBlockNumList()) {
      std::vector<uint8_t> block = ev.GetBlock(block_n);
      int board = -1;
      int pid = -1;
      const int index = read_header(&block, &board, &pid);
      if (board != cfg.drs_board) {
        continue;
      }

      std::vector<uint8_t> data(block.begin() + index, block.end());
      if (data.empty()) {
        continue;
      }

      CAEN_DGTZ_X742_EVENT_S_t unpacked = DRSunpack_event_S(&data);
      if (!unpacked.GrPresent[cfg.drs_group]) {
        continue;
      }

      const auto &group = unpacked.DataGroup[cfg.drs_group];
      const uint32_t sample_count = group.ChSize[cfg.drs_channel];
      if (sample_count == 0) {
        continue;
      }

      drs_out << ev.GetEventN() << '\t' << ev.GetTriggerN() << '\t' << sample_count;
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        drs_out << '\t' << group.DataChannel[cfg.drs_channel][sample];
      }
      drs_out << '\n';
      ++drs_written;
      return;
    }
    return;
  }

  if (cfg.fers_channel >= 0 && description == "FERSProducer") {
    for (auto block_n : ev.GetBlockNumList()) {
      std::vector<uint8_t> block = ev.GetBlock(block_n);
      int board = -1;
      int pid = -1;
      float hv = 0.0f;
      float isipm = 0.0f;
      float temp_det = 0.0f;
      float temp_fpga = 0.0f;
      const int index = read_headerFERS(&block, &board, &pid, &hv, &isipm, &temp_det, &temp_fpga);
      if (board != cfg.fers_board) {
        continue;
      }

      std::vector<uint8_t> data(block.begin() + index, block.end());
      if (data.empty()) {
        continue;
      }

      SpectEvent_t unpacked = FERSunpack_spectevent(&data);
      fers_out << ev.GetEventN() << '\t' << ev.GetTriggerN() << '\t'
               << unpacked.energyHG[cfg.fers_channel] << '\t'
               << unpacked.energyLG[cfg.fers_channel] << '\n';
      ++fers_written;
      return;
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Config cfg = ParseArgs(argc, argv);

    std::ofstream drs_out(cfg.drs_dump_path);
    std::ofstream fers_out(cfg.fers_dump_path);
    if (!drs_out) {
      throw std::runtime_error("Failed to open DRS output file");
    }
    if (!fers_out) {
      throw std::runtime_error("Failed to open FERS output file");
    }

    eudaq::FileDeserializer des(cfg.raw_path);

    int drs_written = 0;
    int fers_written = 0;

    while (des.HasData()) {
      eudaq::EventUP ev;
      uint32_t id = 0;
      des.PreRead(id);
      ev = eudaq::Factory<eudaq::Event>::Create<eudaq::Deserializer &>(id, des);
      if (!ev) {
        break;
      }

      if (ev->IsFlagPacket()) {
        const uint32_t nsub = ev->GetNumSubEvent();
        for (uint32_t isub = 0; isub < nsub; ++isub) {
          ProcessSubEvent(*ev->GetSubEvent(isub), cfg, drs_out, fers_out, drs_written, fers_written);
        }
      } else {
        ProcessSubEvent(*ev, cfg, drs_out, fers_out, drs_written, fers_written);
      }
    }

    std::cout << "DRS_ROWS=" << drs_written << std::endl;
    std::cout << "FERS_ROWS=" << fers_written << std::endl;
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "raw_dump_helper error: " << ex.what() << std::endl;
    return 1;
  }
}
