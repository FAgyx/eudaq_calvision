#include "eudaq/Event.hh"
#include "eudaq/FileDeserializer.hh"
#include "eudaq/Factory.hh"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Args {
  std::string raw_path;
  int max_events = -1;
};

Args ParseArgs(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    throw std::runtime_error(
        "Usage: print_event_ids <run.raw> [max_events]\n"
        "Print one TSV row per top-level event with DRS/FERS subevent IDs.");
  }

  Args args;
  args.raw_path = argv[1];
  if (argc == 3) {
    args.max_events = std::stoi(argv[2]);
    if (args.max_events < 0) {
      throw std::runtime_error("max_events must be >= 0");
    }
  }
  return args;
}

void UpdateFromEvent(const eudaq::Event &ev,
                     int &drs_event_n,
                     int &drs_trigger_n,
                     int &fers_event_n,
                     int &fers_trigger_n) {
  const std::string description = ev.GetDescription();
  if (description == "DRSProducer") {
    drs_event_n = static_cast<int>(ev.GetEventN());
    drs_trigger_n = static_cast<int>(ev.GetTriggerN());
  } else if (description == "FERSProducer") {
    fers_event_n = static_cast<int>(ev.GetEventN());
    fers_trigger_n = static_cast<int>(ev.GetTriggerN());
  }
}

void PrintValueOrNA(int value) {
  if (value < 0) {
    std::cout << "NA";
  } else {
    std::cout << value;
  }
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Args args = ParseArgs(argc, argv);

    eudaq::FileDeserializer des(args.raw_path);

    std::cout
        << "top_event_n\t"
        << "top_trigger_n\t"
        << "drs_event_n\t"
        << "drs_trigger_n\t"
        << "fers_event_n\t"
        << "fers_trigger_n\t"
        << "subevent_count\n";

    int printed = 0;
    while (des.HasData()) {
      if (args.max_events >= 0 && printed >= args.max_events) {
        break;
      }

      uint32_t type_id = 0;
      des.PreRead(type_id);
      auto ev = eudaq::Factory<eudaq::Event>::Create<eudaq::Deserializer &>(type_id, des);
      if (!ev) {
        break;
      }

      int drs_event_n = -1;
      int drs_trigger_n = -1;
      int fers_event_n = -1;
      int fers_trigger_n = -1;
      uint32_t subevent_count = 0;

      if (ev->IsFlagPacket()) {
        subevent_count = ev->GetNumSubEvent();
        for (uint32_t isub = 0; isub < subevent_count; ++isub) {
          const auto subev = ev->GetSubEvent(isub);
          if (!subev) {
            continue;
          }
          UpdateFromEvent(*subev, drs_event_n, drs_trigger_n, fers_event_n, fers_trigger_n);
        }
      } else {
        subevent_count = 1;
        UpdateFromEvent(*ev, drs_event_n, drs_trigger_n, fers_event_n, fers_trigger_n);
      }

      std::cout << ev->GetEventN() << '\t' << ev->GetTriggerN() << '\t';
      PrintValueOrNA(drs_event_n);
      std::cout << '\t';
      PrintValueOrNA(drs_trigger_n);
      std::cout << '\t';
      PrintValueOrNA(fers_event_n);
      std::cout << '\t';
      PrintValueOrNA(fers_trigger_n);
      std::cout << '\t' << subevent_count << '\n';

      ++printed;
    }

    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "print_event_ids error: " << ex.what() << '\n';
    return 1;
  }
}
