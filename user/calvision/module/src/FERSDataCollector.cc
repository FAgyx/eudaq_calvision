#include "eudaq/DataCollector.hh"
#include "eudaq/Event.hh"
#include "eudaq/FileNamer.hh"
#include "eudaq/FileSerializer.hh"
#include "eudaq/Logger.hh"
#include "eudaq/Utils.hh"
#include "DRS_EUDAQ.h"



#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kDefaultMonitorDrsMaxSamples = 20;
constexpr size_t kDrsCompactMonitorHeaderBytes = 9 * sizeof(uint32_t);
constexpr size_t kFastDeviceCount = 5;
constexpr size_t kFastMaxBuilders = 4;
constexpr uint32_t kFastDefaultFullMask = 0x1F;
constexpr size_t kFastDefaultBuilderCount = 4;
constexpr size_t kFastDefaultRingSize = 65536;
constexpr size_t kFastDefaultQueueSize = 65536;
constexpr size_t kFastDefaultWriterQueueMB = 512;
const char *kFastDeviceNames[kFastDeviceCount] = {
  "DRS0", "DRS1", "DRS2", "FERS0", "FERS1"
};
const char *kFastBuilderRoutedTags[kFastMaxBuilders] = {
  "FastB0Routed", "FastB1Routed", "FastB2Routed", "FastB3Routed"
};
const char *kFastBuilderQueueDepthTags[kFastMaxBuilders] = {
  "FastB0QueueDepth", "FastB1QueueDepth", "FastB2QueueDepth", "FastB3QueueDepth"
};
const char *kFastBuilderQueueMaxDepthTags[kFastMaxBuilders] = {
  "FastB0QueueMaxDepth", "FastB1QueueMaxDepth",
  "FastB2QueueMaxDepth", "FastB3QueueMaxDepth"
};
const char *kFastBuilderQueueFullTags[kFastMaxBuilders] = {
  "FastB0QueueFullN", "FastB1QueueFullN",
  "FastB2QueueFullN", "FastB3QueueFullN"
};

bool IsPowerOfTwo(size_t value){
  return value > 0 && (value & (value - 1)) == 0;
}

size_t RoundUpPowerOfTwo(size_t value){
  if(value <= 1)
    return 1;
  size_t result = 1;
  while(result < value)
    result <<= 1;
  return result;
}

uint32_t Log2PowerOfTwo(size_t value){
  uint32_t bits = 0;
  while(value > 1){
    value >>= 1;
    bits++;
  }
  return bits;
}

int ParseTrailingIndex(const std::string &name, const std::string &prefix){
  if(name.rfind(prefix, 0) != 0)
    return -1;
  int value = 0;
  bool have_digit = false;
  for(size_t i = prefix.size(); i < name.size(); ++i){
    if(name[i] < '0' || name[i] > '9')
      return -1;
    have_digit = true;
    value = value * 10 + (name[i] - '0');
  }
  return have_digit ? value : -1;
}

int DeviceIdFromConnectionName(const std::string &name){
  const int drs = ParseTrailingIndex(name, "my_drs");
  if(drs >= 0 && drs <= 2)
    return drs;
  const int fers = ParseTrailingIndex(name, "my_fers");
  if(fers >= 0 && fers <= 1)
    return 3 + fers;
  return -1;
}

std::string CurrentTimeString(){
  std::time_t time_now = std::time(nullptr);
  char time_buff[13];
  time_buff[12] = 0;
  std::strftime(time_buff, sizeof(time_buff), "%y%m%d%H%M%S",
		std::localtime(&time_now));
  return std::string(time_buff);
}

std::string MakeBuilderFileName(const std::string &pattern,
				uint32_t run_number,
				size_t builder_id,
				const std::string &time_string){
  const std::string suffix = "_builder" + std::to_string(builder_id) + ".raw";
  return std::string(eudaq::FileNamer(pattern)
		     .Set('X', suffix)
		     .Set('R', run_number)
		     .Set('D', time_string));
}

struct Fragment {
  uint32_t event_id = 0;
  uint32_t device_id = 0;
  eudaq::EventSP event;
};

struct PartialEvent {
  uint32_t event_id = 0;
  uint32_t mask = 0;
  bool active = false;
  std::array<eudaq::EventSP, kFastDeviceCount> fragments;

  void Reset(uint32_t next_event_id){
    Clear();
    event_id = next_event_id;
    active = true;
  }

  void Clear(){
    mask = 0;
    active = false;
    for(auto &fragment: fragments)
      fragment.reset();
  }
};

class FastOutputSerializer : public eudaq::Serializer {
public:
  explicit FastOutputSerializer(const std::string &file_name)
    : m_file(nullptr),
      m_filebytes(0),
      m_buffer(4 * 1024 * 1024) {
    std::filesystem::path output_path(file_name);
    if(!output_path.parent_path().empty())
      std::filesystem::create_directories(output_path.parent_path());

    m_file = std::fopen(file_name.c_str(), "wb");
    if(!m_file){
      EUDAQ_THROW("Unable to open fast builder output file: "
		  + file_name + ": " + std::strerror(errno));
    }
    if(!m_buffer.empty())
      std::setvbuf(m_file, m_buffer.data(), _IOFBF, m_buffer.size());
  }

  ~FastOutputSerializer() override{
    if(m_file)
      std::fclose(m_file);
  }

  void Flush() override{
    if(m_file)
      std::fflush(m_file);
  }

  uint64_t FileBytes() const{
    return m_filebytes;
  }

private:
  void Serialize(const uint8_t *data, size_t len) override{
#if defined(__linux__)
    const size_t written =
      ::fwrite_unlocked(reinterpret_cast<const char *>(data), 1, len, m_file);
#else
    const size_t written =
      std::fwrite(reinterpret_cast<const char *>(data), 1, len, m_file);
#endif
    m_filebytes += written;
    if(written != len){
      EUDAQ_THROW("Error writing fast builder output: "
		  + eudaq::to_string(errno) + ", " + std::strerror(errno));
    }
  }

  FILE *m_file;
  uint64_t m_filebytes;
  std::vector<char> m_buffer;
};

class EventBufferSerializer : public eudaq::Serializer {
public:
  explicit EventBufferSerializer(size_t reserve_size){
    if(reserve_size > 0)
      m_data.reserve(reserve_size);
  }

  std::vector<uint8_t> TakeData(){
    return std::move(m_data);
  }

private:
  void Serialize(const uint8_t *data, size_t len) override{
    const size_t pos = m_data.size();
    m_data.resize(pos + len);
    std::memcpy(m_data.data() + pos, data, len);
  }

  std::vector<uint8_t> m_data;
};

class AsyncOutputWriter {
public:
  AsyncOutputWriter(const std::string &file_name, size_t max_queue_bytes)
    : m_file_name(file_name),
      m_max_queue_bytes(max_queue_bytes),
      m_stop(false),
      m_failed(false),
      m_queue_bytes(0),
      m_queue_depth_status(0),
      m_queue_bytes_status(0),
      m_queue_max_depth(0),
      m_queue_max_bytes(0),
      m_queue_full_count(0),
      m_enqueue_wait_us_total(0),
      m_enqueue_wait_us_max(0),
      m_write_us_total(0),
      m_write_us_max(0),
      m_write_count(0),
      m_output_bytes(0) {
  }

  void Start(){
    m_serializer.reset(new FastOutputSerializer(m_file_name));
    m_thread = std::thread(&AsyncOutputWriter::Run, this);
  }

  void Stop(){
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_stop = true;
    }
    m_data_cv.notify_all();
    m_space_cv.notify_all();
    if(m_thread.joinable())
      m_thread.join();
    if(m_serializer)
      m_serializer->Flush();
  }

  bool Push(std::vector<uint8_t> &&data){
    const auto wait_begin = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(m_mutex);
    while(!m_stop && !m_failed.load(std::memory_order_acquire) &&
	  QueueWouldExceedLimit(data.size())){
      m_queue_full_count.fetch_add(1, std::memory_order_relaxed);
      m_space_cv.wait(lock);
    }
    const auto wait_end = std::chrono::steady_clock::now();
    const uint64_t wait_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
	wait_end - wait_begin).count());
    m_enqueue_wait_us_total.fetch_add(wait_us, std::memory_order_relaxed);
    UpdateMax(m_enqueue_wait_us_max, wait_us);

    if(m_stop || m_failed.load(std::memory_order_acquire))
      return false;

    m_queue_bytes += data.size();
    m_queue.push_back(std::move(data));
    PublishQueueStateLocked();
    lock.unlock();
    m_data_cv.notify_one();
    return true;
  }

  uint64_t QueueDepth() const{
    return m_queue_depth_status.load(std::memory_order_acquire);
  }

  uint64_t QueueBytes() const{
    return m_queue_bytes_status.load(std::memory_order_acquire);
  }

  uint64_t QueueMaxDepth() const{
    return m_queue_max_depth.load(std::memory_order_acquire);
  }

  uint64_t QueueMaxBytes() const{
    return m_queue_max_bytes.load(std::memory_order_acquire);
  }

  uint64_t QueueFullCount() const{
    return m_queue_full_count.load(std::memory_order_acquire);
  }

  uint64_t EnqueueWaitTotalUs() const{
    return m_enqueue_wait_us_total.load(std::memory_order_acquire);
  }

  uint64_t EnqueueWaitMaxUs() const{
    return m_enqueue_wait_us_max.load(std::memory_order_acquire);
  }

  uint64_t WriteTotalUs() const{
    return m_write_us_total.load(std::memory_order_acquire);
  }

  uint64_t WriteMaxUs() const{
    return m_write_us_max.load(std::memory_order_acquire);
  }

  uint64_t WriteCount() const{
    return m_write_count.load(std::memory_order_acquire);
  }

  uint64_t OutputBytes() const{
    return m_output_bytes.load(std::memory_order_acquire);
  }

private:
  bool QueueWouldExceedLimit(size_t next_bytes) const{
    if(m_max_queue_bytes == 0)
      return false;
    if(m_queue.empty())
      return false;
    return m_queue_bytes + next_bytes > m_max_queue_bytes;
  }

  void PublishQueueStateLocked(){
    m_queue_depth_status.store(m_queue.size(), std::memory_order_release);
    m_queue_bytes_status.store(m_queue_bytes, std::memory_order_release);
    UpdateMax(m_queue_max_depth, m_queue.size());
    UpdateMax(m_queue_max_bytes, m_queue_bytes);
  }

  void Run(){
    try{
      while(true){
	std::vector<uint8_t> data;
	{
	  std::unique_lock<std::mutex> lock(m_mutex);
	  m_data_cv.wait(lock, [this]{
	    return m_stop || !m_queue.empty();
	  });
	  if(m_queue.empty()){
	    if(m_stop)
	      break;
	    continue;
	  }
	  data = std::move(m_queue.front());
	  m_queue.pop_front();
	  m_queue_bytes -= data.size();
	  PublishQueueStateLocked();
	}
	m_space_cv.notify_one();

	const auto write_begin = std::chrono::steady_clock::now();
	m_serializer->append(data.data(), data.size());
	const auto write_end = std::chrono::steady_clock::now();
	const uint64_t write_us = static_cast<uint64_t>(
	  std::chrono::duration_cast<std::chrono::microseconds>(
	    write_end - write_begin).count());
	m_write_us_total.fetch_add(write_us, std::memory_order_relaxed);
	UpdateMax(m_write_us_max, write_us);
	m_write_count.fetch_add(1, std::memory_order_relaxed);
	m_output_bytes.fetch_add(data.size(), std::memory_order_relaxed);
      }
    }catch(const std::exception &e){
      m_failed.store(true, std::memory_order_release);
      {
	std::unique_lock<std::mutex> lock(m_mutex);
	m_stop = true;
      }
      m_space_cv.notify_all();
      m_data_cv.notify_all();
      EUDAQ_ERROR("AsyncOutputWriter stopped after exception: " + std::string(e.what()));
    }catch(...){
      m_failed.store(true, std::memory_order_release);
      {
	std::unique_lock<std::mutex> lock(m_mutex);
	m_stop = true;
      }
      m_space_cv.notify_all();
      m_data_cv.notify_all();
      EUDAQ_ERROR("AsyncOutputWriter stopped after unknown exception");
    }
  }

  static void UpdateMax(std::atomic<uint64_t> &target, uint64_t value){
    uint64_t old_value = target.load(std::memory_order_relaxed);
    while(value > old_value &&
	  !target.compare_exchange_weak(old_value, value,
					std::memory_order_release,
					std::memory_order_relaxed)){
    }
  }

  std::string m_file_name;
  size_t m_max_queue_bytes;
  std::unique_ptr<FastOutputSerializer> m_serializer;
  std::thread m_thread;
  mutable std::mutex m_mutex;
  std::condition_variable m_data_cv;
  std::condition_variable m_space_cv;
  bool m_stop;
  std::atomic<bool> m_failed;
  std::deque<std::vector<uint8_t>> m_queue;
  uint64_t m_queue_bytes;
  std::atomic<uint64_t> m_queue_depth_status;
  std::atomic<uint64_t> m_queue_bytes_status;
  std::atomic<uint64_t> m_queue_max_depth;
  std::atomic<uint64_t> m_queue_max_bytes;
  std::atomic<uint64_t> m_queue_full_count;
  std::atomic<uint64_t> m_enqueue_wait_us_total;
  std::atomic<uint64_t> m_enqueue_wait_us_max;
  std::atomic<uint64_t> m_write_us_total;
  std::atomic<uint64_t> m_write_us_max;
  std::atomic<uint64_t> m_write_count;
  std::atomic<uint64_t> m_output_bytes;
};

class SpscFragmentQueue {
public:
  explicit SpscFragmentQueue(size_t capacity)
    : m_buffer(capacity),
      m_mask(capacity - 1),
      m_head(0),
      m_tail(0),
      m_max_depth(0) {
  }

  bool TryPush(Fragment &&fragment){
    const size_t tail = m_tail.load(std::memory_order_relaxed);
    const size_t next_tail = (tail + 1) & m_mask;
    const size_t head = m_head.load(std::memory_order_acquire);
    if(next_tail == head)
      return false;

    m_buffer[tail] = std::move(fragment);
    m_tail.store(next_tail, std::memory_order_release);
    UpdateMaxDepth((next_tail - head) & m_mask);
    return true;
  }

  bool TryPop(Fragment *fragment){
    const size_t head = m_head.load(std::memory_order_relaxed);
    const size_t tail = m_tail.load(std::memory_order_acquire);
    if(head == tail)
      return false;

    *fragment = std::move(m_buffer[head]);
    m_buffer[head] = Fragment();
    m_head.store((head + 1) & m_mask, std::memory_order_release);
    return true;
  }

  size_t Size() const{
    const size_t head = m_head.load(std::memory_order_acquire);
    const size_t tail = m_tail.load(std::memory_order_acquire);
    return (tail - head) & m_mask;
  }

  size_t MaxDepth() const{
    return m_max_depth.load(std::memory_order_acquire);
  }

private:
  void UpdateMaxDepth(size_t depth){
    size_t old_depth = m_max_depth.load(std::memory_order_relaxed);
    while(depth > old_depth &&
	  !m_max_depth.compare_exchange_weak(old_depth, depth,
					     std::memory_order_release,
					     std::memory_order_relaxed)){
    }
  }

  std::vector<Fragment> m_buffer;
  size_t m_mask;
  alignas(64) std::atomic<size_t> m_head;
  alignas(64) std::atomic<size_t> m_tail;
  std::atomic<size_t> m_max_depth;
};

class RingEventBuilder {
public:
  RingEventBuilder(size_t builder_id,
		   uint32_t builder_shift,
		   size_t ring_size,
		   uint32_t full_mask,
		   size_t writer_queue_bytes,
		   uint32_t run_number,
		   uint32_t stream_number,
		   const std::string &file_name,
		   const std::array<SpscFragmentQueue*, kFastDeviceCount> &queues)
    : m_builder_id(builder_id),
      m_builder_shift(builder_shift),
      m_ring(ring_size),
      m_ring_mask(ring_size - 1),
      m_full_mask(full_mask),
      m_writer_queue_bytes(writer_queue_bytes),
      m_run_number(run_number),
      m_stream_number(stream_number),
      m_file_name(file_name),
      m_queues(queues),
      m_stop(false),
      m_complete_count(0),
      m_incomplete_count(0),
      m_duplicate_count(0),
      m_fragment_count(0),
      m_overwrite_count(0),
      m_serialize_us_total(0),
      m_serialize_us_max(0),
      m_last_serialized_size(0) {
  }

  void Start(){
    m_writer.reset(new AsyncOutputWriter(m_file_name, m_writer_queue_bytes));
    m_writer->Start();
    m_thread = std::thread(&RingEventBuilder::Run, this);
  }

  void Stop(){
    m_stop.store(true, std::memory_order_release);
    if(m_thread.joinable())
      m_thread.join();
    if(m_writer)
      m_writer->Stop();
  }

  uint64_t CompleteCount() const{
    return m_complete_count.load(std::memory_order_acquire);
  }

  uint64_t IncompleteCount() const{
    return m_incomplete_count.load(std::memory_order_acquire);
  }

  uint64_t DuplicateCount() const{
    return m_duplicate_count.load(std::memory_order_acquire);
  }

  uint64_t FragmentCount() const{
    return m_fragment_count.load(std::memory_order_acquire);
  }

  uint64_t OverwriteCount() const{
    return m_overwrite_count.load(std::memory_order_acquire);
  }

  uint64_t WriteTotalUs() const{
    return m_writer ? m_writer->WriteTotalUs() : 0;
  }

  uint64_t WriteMaxUs() const{
    return m_writer ? m_writer->WriteMaxUs() : 0;
  }

  uint64_t WriteCount() const{
    return m_writer ? m_writer->WriteCount() : 0;
  }

  uint64_t SerializeTotalUs() const{
    return m_serialize_us_total.load(std::memory_order_acquire);
  }

  uint64_t SerializeMaxUs() const{
    return m_serialize_us_max.load(std::memory_order_acquire);
  }

  uint64_t WriterQueueDepth() const{
    return m_writer ? m_writer->QueueDepth() : 0;
  }

  uint64_t WriterQueueBytes() const{
    return m_writer ? m_writer->QueueBytes() : 0;
  }

  uint64_t WriterQueueMaxDepth() const{
    return m_writer ? m_writer->QueueMaxDepth() : 0;
  }

  uint64_t WriterQueueMaxBytes() const{
    return m_writer ? m_writer->QueueMaxBytes() : 0;
  }

  uint64_t WriterQueueFullCount() const{
    return m_writer ? m_writer->QueueFullCount() : 0;
  }

  uint64_t EnqueueWaitTotalUs() const{
    return m_writer ? m_writer->EnqueueWaitTotalUs() : 0;
  }

  uint64_t EnqueueWaitMaxUs() const{
    return m_writer ? m_writer->EnqueueWaitMaxUs() : 0;
  }

  uint64_t OutputBytes() const{
    return m_writer ? m_writer->OutputBytes() : 0;
  }

private:
  void Run(){
    try{
      uint32_t idle_spin = 0;
      while(!m_stop.load(std::memory_order_acquire) || AnyQueueHasData()){
	bool did_work = false;
	for(size_t device_id = 0; device_id < kFastDeviceCount; ++device_id){
	  Fragment fragment;
	  while(m_queues[device_id] &&
		m_queues[device_id]->TryPop(&fragment)){
	    did_work = true;
	    ProcessFragment(std::move(fragment));
	  }
	}
	if(did_work){
	  idle_spin = 0;
	  continue;
	}
	if(idle_spin < 1000){
	  idle_spin++;
	  std::this_thread::yield();
	}else{
	  std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
      }
      FlushIncompleteSlots();
    }catch(const std::exception &e){
      EUDAQ_ERROR("RingEventBuilder " + std::to_string(m_builder_id)
		  + " stopped after exception: " + e.what());
    }catch(...){
      EUDAQ_ERROR("RingEventBuilder " + std::to_string(m_builder_id)
		  + " stopped after unknown exception");
    }
  }

  bool AnyQueueHasData() const{
    for(auto queue: m_queues){
      if(queue && queue->Size() > 0)
	return true;
    }
    return false;
  }

  void ProcessFragment(Fragment &&fragment){
    if(fragment.device_id >= kFastDeviceCount || !fragment.event)
      return;

    m_fragment_count.fetch_add(1, std::memory_order_relaxed);
    const size_t slot_id =
      (static_cast<size_t>(fragment.event_id) >> m_builder_shift) & m_ring_mask;
    PartialEvent &slot = m_ring[slot_id];

    if(!slot.active || slot.event_id != fragment.event_id){
      if(slot.active && slot.mask != 0){
	m_incomplete_count.fetch_add(1, std::memory_order_relaxed);
	m_overwrite_count.fetch_add(1, std::memory_order_relaxed);
      }
      slot.Reset(fragment.event_id);
    }

    const uint32_t device_bit = 1u << fragment.device_id;
    if(slot.mask & device_bit){
      m_duplicate_count.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    slot.fragments[fragment.device_id] = std::move(fragment.event);
    slot.mask |= device_bit;

    if((slot.mask & m_full_mask) == m_full_mask){
      WriteCompleteEvent(slot);
      slot.Clear();
    }
  }

  void WriteCompleteEvent(const PartialEvent &partial){
    auto event = eudaq::Event::MakeShared("FERSDRS");
    event->SetFlagPacket();
    event->SetRunN(m_run_number);
    event->SetEventN(partial.event_id);
    event->SetStreamN(m_stream_number);
    event->SetTriggerN(partial.event_id);

    for(size_t device_id = 0; device_id < kFastDeviceCount; ++device_id){
      const uint32_t device_bit = 1u << device_id;
      if((m_full_mask & device_bit) && partial.fragments[device_id])
	event->AddSubEvent(partial.fragments[device_id]);
    }

    const size_t reserve_size = m_last_serialized_size.load(std::memory_order_relaxed);
    EventBufferSerializer serializer(reserve_size);
    const auto serialize_begin = std::chrono::steady_clock::now();
    serializer.write(*event);
    auto data = serializer.TakeData();
    const auto serialize_end = std::chrono::steady_clock::now();
    const uint64_t serialize_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
	serialize_end - serialize_begin).count());
    m_serialize_us_total.fetch_add(serialize_us, std::memory_order_relaxed);
    UpdateMax(m_serialize_us_max, serialize_us);
    UpdateMax(m_last_serialized_size, data.size());

    if(m_writer && m_writer->Push(std::move(data)))
      m_complete_count.fetch_add(1, std::memory_order_relaxed);
  }

  void UpdateMax(std::atomic<uint64_t> &target, uint64_t value){
    uint64_t old_value = target.load(std::memory_order_relaxed);
    while(value > old_value &&
	  !target.compare_exchange_weak(old_value, value,
					std::memory_order_release,
					std::memory_order_relaxed)){
    }
  }

  void FlushIncompleteSlots(){
    for(auto &slot: m_ring){
      if(slot.active && slot.mask != 0)
	m_incomplete_count.fetch_add(1, std::memory_order_relaxed);
      slot.Clear();
    }
  }

  size_t m_builder_id;
  uint32_t m_builder_shift;
  std::vector<PartialEvent> m_ring;
  size_t m_ring_mask;
  uint32_t m_full_mask;
  size_t m_writer_queue_bytes;
  uint32_t m_run_number;
  uint32_t m_stream_number;
  std::string m_file_name;
  std::array<SpscFragmentQueue*, kFastDeviceCount> m_queues;
  std::unique_ptr<AsyncOutputWriter> m_writer;
  std::thread m_thread;
  std::atomic<bool> m_stop;
  std::atomic<uint64_t> m_complete_count;
  std::atomic<uint64_t> m_incomplete_count;
  std::atomic<uint64_t> m_duplicate_count;
  std::atomic<uint64_t> m_fragment_count;
  std::atomic<uint64_t> m_overwrite_count;
  std::atomic<uint64_t> m_serialize_us_total;
  std::atomic<uint64_t> m_serialize_us_max;
  std::atomic<uint64_t> m_last_serialized_size;
};

void AppendU32LE(uint32_t value, std::vector<uint8_t> *out){
  for(size_t i = 0; i < sizeof(value); ++i){
    out->push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

bool ReadU32LE(const std::vector<uint8_t> &data, size_t offset, uint32_t *value){
  if(offset + sizeof(uint32_t) > data.size())
    return false;
  *value = static_cast<uint32_t>(data[offset]) |
    (static_cast<uint32_t>(data[offset + 1]) << 8) |
    (static_cast<uint32_t>(data[offset + 2]) << 16) |
    (static_cast<uint32_t>(data[offset + 3]) << 24);
  return true;
}

eudaq::EventSP CloneEventShell(eudaq::EventSPC src){
  auto dst = eudaq::Event::MakeShared(src->GetDescription());
  dst->SetType(src->GetType());
  dst->SetVersion(src->GetVersion());
  dst->SetFlag(src->GetFlag());
  dst->SetRunN(src->GetRunN());
  dst->SetEventN(src->GetEventN());
  dst->SetStreamN(src->GetStreamN());
  dst->SetTriggerN(src->GetTriggerN(), false);
  dst->SetExtendWord(src->GetExtendWord());
  dst->SetTimestamp(src->GetTimestampBegin(), src->GetTimestampEnd(), false);
  for(const auto &tag: src->GetTags()){
    dst->SetTag(tag.first, tag.second);
  }
  return dst;
}

eudaq::EventSP CloneEventFull(eudaq::EventSPC src){
  auto dst = CloneEventShell(src);
  for(auto block_n: src->GetBlockNumList()){
    dst->AddBlock(block_n, src->GetBlock(block_n));
  }
  for(auto subev: src->GetSubEvents()){
    dst->AddSubEvent(CloneEventFull(subev));
  }
  return dst;
}

std::vector<uint8_t> TrimDrsBlockForMonitor(const std::vector<uint8_t> &block,
					    uint32_t max_samples){
  if(block.empty())
    return block;

  const size_t header_size = block[0];
  const size_t data_begin = header_size + 1;
  if(block.size() < data_begin + MAX_X742_GROUP_SIZE)
    return block;

  std::vector<uint8_t> out(block.begin(), block.begin() + data_begin);
  std::vector<uint8_t> data(block.begin() + data_begin, block.end());
  if(DRSis_compact_event(&data)){
    if(data.size() < kDrsCompactMonitorHeaderBytes)
      return out;
    out.insert(out.end(), data.begin(), data.begin() + 8 * sizeof(uint32_t));
    AppendU32LE(0, &out);
    return out;
  }

  size_t in = data_begin;
  uint8_t gr_present[MAX_X742_GROUP_SIZE] = {};
  for(int ig = 0; ig < MAX_X742_GROUP_SIZE; ++ig){
    gr_present[ig] = block[in++];
    out.push_back(gr_present[ig]);
  }

  for(int ig = 0; ig < MAX_X742_GROUP_SIZE; ++ig){
    if(!gr_present[ig])
      continue;

    uint32_t ch_size[MAX_X742_CHANNEL_SIZE] = {};
    for(int ich = 0; ich < MAX_X742_CHANNEL_SIZE; ++ich){
      if(!ReadU32LE(block, in, &ch_size[ich]))
	return block;
      in += sizeof(uint32_t);
      AppendU32LE(std::min<uint32_t>(ch_size[ich], max_samples), &out);
    }

    for(int ich = 0; ich < MAX_X742_CHANNEL_SIZE; ++ich){
      const size_t sample_bytes = static_cast<size_t>(ch_size[ich]) * sizeof(uint32_t);
      if(in + sample_bytes > block.size())
	return block;
      const size_t keep_bytes =
	static_cast<size_t>(std::min<uint32_t>(ch_size[ich], max_samples)) * sizeof(uint32_t);
      out.insert(out.end(), block.begin() + in, block.begin() + in + keep_bytes);
      in += sample_bytes;
    }

    constexpr size_t trailer_bytes = sizeof(uint32_t) + sizeof(uint16_t);
    if(in + trailer_bytes > block.size())
      return block;
    out.insert(out.end(), block.begin() + in, block.begin() + in + trailer_bytes);
    in += trailer_bytes;
  }

  return out;
}

eudaq::EventSP CloneDrsMonitorEvent(eudaq::EventSPC src, uint32_t max_samples){
  auto dst = CloneEventShell(src);
  dst->SetTag("DRS_MONITOR_MAX_SAMPLES", std::to_string(max_samples));
  for(auto block_n: src->GetBlockNumList()){
    dst->AddBlock(block_n, TrimDrsBlockForMonitor(src->GetBlock(block_n), max_samples));
  }
  return dst;
}

} // namespace

class FERSDataCollector:public eudaq::DataCollector{
public:
  FERSDataCollector(const std::string &name,
		   const std::string &rc);
  void DoConnect(eudaq::ConnectionSPC id) override;
  void DoDisconnect(eudaq::ConnectionSPC id) override;
  void DoConfigure() override;
  void DoStartRun() override;
  void DoStopRun() override;
  void DoReset() override;
  void DoTerminate() override;
  void DoStatus() override;
  void DoReceive(eudaq::ConnectionSPC id, eudaq::EventSP ev) override;
  eudaq::EventSPC GetMonitorEvent(eudaq::EventSPC ev) override;

  static const uint32_t m_id_factory = eudaq::cstr2hash("FERSDataCollector");
private:
  void ResetFastCounters();
  void StartFastBuilders();
  void StopFastBuilders();
  void PublishFastBuilderStatus();
  bool RouteFastFragment(eudaq::ConnectionSPC id, eudaq::EventSP ev);
  uint64_t TotalQueueFullCount() const;
  uint64_t TotalQueueDepth() const;
  uint64_t TotalQueueMaxDepth() const;
  uint64_t QueueFullCountForBuilder(size_t builder_id) const;
  uint64_t QueueDepthForBuilder(size_t builder_id) const;
  uint64_t QueueMaxDepthForBuilder(size_t builder_id) const;

  std::mutex m_mtx_map;
  std::map<eudaq::ConnectionSPC, std::deque<eudaq::EventSPC>> m_conn_evque;
  std::set<eudaq::ConnectionSPC> m_conn_inactive;
  std::set<eudaq::ConnectionSPC> m_expected_connections;

  uint32_t m_noprint;
  uint32_t m_required_connections;
  uint32_t m_sync_drop_warn_every;
  uint64_t m_sync_drop_count;
  bool m_sync_events;
  uint32_t m_monitor_drs_max_samples;

  bool m_fast_builder_enabled;
  bool m_fast_drop_on_full;
  size_t m_fast_builder_count;
  size_t m_fast_ring_size;
  size_t m_fast_queue_size;
  size_t m_fast_writer_queue_bytes;
  uint32_t m_fast_full_mask;
  uint32_t m_fast_builder_shift;
  std::atomic<bool> m_fast_accepting;
  std::array<std::array<std::unique_ptr<SpscFragmentQueue>, kFastMaxBuilders>,
	     kFastDeviceCount> m_fast_queues;
  std::vector<std::unique_ptr<RingEventBuilder>> m_fast_builders;
  std::array<std::atomic<uint64_t>, kFastDeviceCount> m_fast_fragments_received;
  std::array<std::atomic<uint64_t>, kFastMaxBuilders> m_fast_fragments_routed;
  std::array<std::array<std::atomic<uint64_t>, kFastMaxBuilders>,
	     kFastDeviceCount> m_fast_queue_full_count;
  std::atomic<uint64_t> m_fast_unknown_device_count;
  std::atomic<uint64_t> m_fast_nontrigger_count;
  std::atomic<uint64_t> m_fast_route_drop_count;
};

namespace{
  auto dummy0 = eudaq::Factory<eudaq::DataCollector>::
    Register<FERSDataCollector, const std::string&, const std::string&>
    (FERSDataCollector::m_id_factory);
}

FERSDataCollector::FERSDataCollector(const std::string &name,
					       const std::string &rc):
  DataCollector(name, rc),
  m_required_connections(0),
  m_sync_drop_warn_every(1000),
  m_sync_drop_count(0),
  m_sync_events(true),
  m_monitor_drs_max_samples(kDefaultMonitorDrsMaxSamples),
  m_fast_builder_enabled(false),
  m_fast_drop_on_full(false),
  m_fast_builder_count(kFastDefaultBuilderCount),
  m_fast_ring_size(kFastDefaultRingSize),
  m_fast_queue_size(kFastDefaultQueueSize),
  m_fast_writer_queue_bytes(kFastDefaultWriterQueueMB * 1024 * 1024),
  m_fast_full_mask(kFastDefaultFullMask),
  m_fast_builder_shift(2),
  m_fast_accepting(false),
  m_fast_unknown_device_count(0),
  m_fast_nontrigger_count(0),
  m_fast_route_drop_count(0)
{
  ResetFastCounters();
}

void FERSDataCollector::ResetFastCounters(){
  for(auto &counter: m_fast_fragments_received)
    counter.store(0, std::memory_order_relaxed);
  for(auto &counter: m_fast_fragments_routed)
    counter.store(0, std::memory_order_relaxed);
  for(auto &device_counts: m_fast_queue_full_count){
    for(auto &counter: device_counts)
      counter.store(0, std::memory_order_relaxed);
  }
  m_fast_unknown_device_count.store(0, std::memory_order_relaxed);
  m_fast_nontrigger_count.store(0, std::memory_order_relaxed);
  m_fast_route_drop_count.store(0, std::memory_order_relaxed);
}

uint64_t FERSDataCollector::TotalQueueFullCount() const{
  uint64_t total = 0;
  for(const auto &device_counts: m_fast_queue_full_count){
    for(const auto &counter: device_counts)
      total += counter.load(std::memory_order_relaxed);
  }
  return total;
}

uint64_t FERSDataCollector::TotalQueueDepth() const{
  uint64_t total = 0;
  for(const auto &device_queues: m_fast_queues){
    for(const auto &queue: device_queues){
      if(queue)
	total += queue->Size();
    }
  }
  return total;
}

uint64_t FERSDataCollector::TotalQueueMaxDepth() const{
  uint64_t total = 0;
  for(const auto &device_queues: m_fast_queues){
    for(const auto &queue: device_queues){
      if(queue)
	total += queue->MaxDepth();
    }
  }
  return total;
}

uint64_t FERSDataCollector::QueueFullCountForBuilder(size_t builder_id) const{
  if(builder_id >= kFastMaxBuilders)
    return 0;

  uint64_t total = 0;
  for(const auto &device_counts: m_fast_queue_full_count)
    total += device_counts[builder_id].load(std::memory_order_relaxed);
  return total;
}

uint64_t FERSDataCollector::QueueDepthForBuilder(size_t builder_id) const{
  if(builder_id >= kFastMaxBuilders)
    return 0;

  uint64_t total = 0;
  for(const auto &device_queues: m_fast_queues){
    const auto &queue = device_queues[builder_id];
    if(queue)
      total += queue->Size();
  }
  return total;
}

uint64_t FERSDataCollector::QueueMaxDepthForBuilder(size_t builder_id) const{
  if(builder_id >= kFastMaxBuilders)
    return 0;

  uint64_t total = 0;
  for(const auto &device_queues: m_fast_queues){
    const auto &queue = device_queues[builder_id];
    if(queue)
      total += queue->MaxDepth();
  }
  return total;
}

void FERSDataCollector::StartFastBuilders(){
  StopFastBuilders();
  ResetFastCounters();

  if(!m_fast_builder_enabled)
    return;

  const auto conf = GetConfiguration();
  const std::string file_pattern =
    conf ? conf->Get("EUDAQ_FW_PATTERN", std::string("run$3R$X"))
	 : std::string("run$3R$X");
  const std::string time_string = CurrentTimeString();
  const uint32_t run_number = GetRunNumber();
  const uint32_t stream_base = eudaq::str2hash(GetFullName());
  m_fast_builder_shift = Log2PowerOfTwo(m_fast_builder_count);

  for(size_t device_id = 0; device_id < kFastDeviceCount; ++device_id){
    for(size_t builder_id = 0; builder_id < m_fast_builder_count; ++builder_id){
      m_fast_queues[device_id][builder_id].reset(
	new SpscFragmentQueue(m_fast_queue_size));
    }
  }

  try{
    m_fast_builders.clear();
    m_fast_builders.reserve(m_fast_builder_count);
    for(size_t builder_id = 0; builder_id < m_fast_builder_count; ++builder_id){
      std::array<SpscFragmentQueue*, kFastDeviceCount> queues = {};
      for(size_t device_id = 0; device_id < kFastDeviceCount; ++device_id)
	queues[device_id] = m_fast_queues[device_id][builder_id].get();

      const std::string file_name =
	MakeBuilderFileName(file_pattern, run_number, builder_id, time_string);
      const uint32_t stream_number = stream_base ^ static_cast<uint32_t>(builder_id + 1);
      m_fast_builders.emplace_back(new RingEventBuilder(
	builder_id,
	m_fast_builder_shift,
	m_fast_ring_size,
	m_fast_full_mask,
	m_fast_writer_queue_bytes,
	run_number,
	stream_number,
	file_name,
	queues));
      m_fast_builders.back()->Start();
      EUDAQ_INFO("FERSDataCollector fast builder "
		 + std::to_string(builder_id)
		 + " writing " + file_name);
    }
    m_fast_accepting.store(true, std::memory_order_release);
  }catch(...){
    StopFastBuilders();
    throw;
  }
}

void FERSDataCollector::StopFastBuilders(){
  m_fast_accepting.store(false, std::memory_order_release);
  for(auto &builder: m_fast_builders){
    if(builder)
      builder->Stop();
  }

  for(auto &device_queues: m_fast_queues){
    for(auto &queue: device_queues)
      queue.reset();
  }
}

void FERSDataCollector::PublishFastBuilderStatus(){
  uint64_t complete_total = 0;
  uint64_t incomplete_total = 0;
  uint64_t duplicate_total = 0;
  uint64_t overwrite_total = 0;
  uint64_t serialize_us_total = 0;
  uint64_t serialize_us_max = 0;
  uint64_t write_us_total = 0;
  uint64_t write_us_max = 0;
  uint64_t write_count_total = 0;
  uint64_t output_bytes_total = 0;
  uint64_t writer_queue_depth_total = 0;
  uint64_t writer_queue_bytes_total = 0;
  uint64_t writer_queue_max_depth_total = 0;
  uint64_t writer_queue_max_bytes_total = 0;
  uint64_t writer_queue_full_total = 0;
  uint64_t enqueue_wait_us_total = 0;
  uint64_t enqueue_wait_us_max = 0;

  for(size_t builder_id = 0; builder_id < kFastMaxBuilders; ++builder_id){
    uint64_t complete = 0;
    uint64_t fragments = 0;
    uint64_t incomplete = 0;
    uint64_t duplicate = 0;
    uint64_t overwrite = 0;
    uint64_t serialize_us = 0;
    uint64_t serialize_max_us = 0;
    uint64_t write_us = 0;
    uint64_t write_max_us = 0;
    uint64_t write_count = 0;
    uint64_t output_bytes = 0;
    uint64_t writer_queue_depth = 0;
    uint64_t writer_queue_bytes = 0;
    uint64_t writer_queue_max_depth = 0;
    uint64_t writer_queue_max_bytes = 0;
    uint64_t writer_queue_full = 0;
    uint64_t enqueue_wait_us = 0;
    uint64_t enqueue_wait_max_us = 0;

    if(builder_id < m_fast_builders.size()){
      const auto &builder = m_fast_builders[builder_id];
      if(builder){
	complete = builder->CompleteCount();
	fragments = builder->FragmentCount();
	incomplete = builder->IncompleteCount();
	duplicate = builder->DuplicateCount();
	overwrite = builder->OverwriteCount();
	serialize_us = builder->SerializeTotalUs();
	serialize_max_us = builder->SerializeMaxUs();
	write_us = builder->WriteTotalUs();
	write_max_us = builder->WriteMaxUs();
	write_count = builder->WriteCount();
	output_bytes = builder->OutputBytes();
	writer_queue_depth = builder->WriterQueueDepth();
	writer_queue_bytes = builder->WriterQueueBytes();
	writer_queue_max_depth = builder->WriterQueueMaxDepth();
	writer_queue_max_bytes = builder->WriterQueueMaxBytes();
	writer_queue_full = builder->WriterQueueFullCount();
	enqueue_wait_us = builder->EnqueueWaitTotalUs();
	enqueue_wait_max_us = builder->EnqueueWaitMaxUs();
      }
    }

    complete_total += complete;
    incomplete_total += incomplete;
    duplicate_total += duplicate;
    overwrite_total += overwrite;
    serialize_us_total += serialize_us;
    if(serialize_max_us > serialize_us_max)
      serialize_us_max = serialize_max_us;
    write_us_total += write_us;
    if(write_max_us > write_us_max)
      write_us_max = write_max_us;
    write_count_total += write_count;
    output_bytes_total += output_bytes;
    writer_queue_depth_total += writer_queue_depth;
    writer_queue_bytes_total += writer_queue_bytes;
    writer_queue_max_depth_total += writer_queue_max_depth;
    writer_queue_max_bytes_total += writer_queue_max_bytes;
    writer_queue_full_total += writer_queue_full;
    enqueue_wait_us_total += enqueue_wait_us;
    if(enqueue_wait_max_us > enqueue_wait_us_max)
      enqueue_wait_us_max = enqueue_wait_max_us;

    SetStatusTag("FastB" + std::to_string(builder_id) + "Complete",
		 std::to_string(complete));
    SetStatusTag("FastB" + std::to_string(builder_id) + "Fragments",
		 std::to_string(fragments));
    SetStatusTag("FastB" + std::to_string(builder_id) + "Incomplete",
		 std::to_string(incomplete));
    SetStatusTag("FastB" + std::to_string(builder_id) + "Duplicate",
		 std::to_string(duplicate));
    SetStatusTag(kFastBuilderRoutedTags[builder_id],
		 std::to_string(m_fast_fragments_routed[builder_id].load(
		   std::memory_order_relaxed)));
    SetStatusTag(kFastBuilderQueueDepthTags[builder_id],
		 std::to_string(QueueDepthForBuilder(builder_id)));
    SetStatusTag(kFastBuilderQueueMaxDepthTags[builder_id],
		 std::to_string(QueueMaxDepthForBuilder(builder_id)));
    SetStatusTag(kFastBuilderQueueFullTags[builder_id],
		 std::to_string(QueueFullCountForBuilder(builder_id)));
    SetStatusTag("FastB" + std::to_string(builder_id) + "WriteAvgUs",
		 std::to_string(write_count > 0 ? write_us / write_count : 0));
    SetStatusTag("FastB" + std::to_string(builder_id) + "WriteMaxUs",
		 std::to_string(write_max_us));
    SetStatusTag("FastB" + std::to_string(builder_id) + "FileMB",
		 std::to_string(output_bytes / (1024 * 1024)));
    SetStatusTag("FastB" + std::to_string(builder_id) + "SerializeAvgUs",
		 std::to_string(complete > 0 ? serialize_us / complete : 0));
    SetStatusTag("FastB" + std::to_string(builder_id) + "SerializeMaxUs",
		 std::to_string(serialize_max_us));
    SetStatusTag("FastB" + std::to_string(builder_id) + "WriterQueueDepth",
		 std::to_string(writer_queue_depth));
    SetStatusTag("FastB" + std::to_string(builder_id) + "WriterQueueMB",
		 std::to_string(writer_queue_bytes / (1024 * 1024)));
    SetStatusTag("FastB" + std::to_string(builder_id) + "WriterQueueMaxMB",
		 std::to_string(writer_queue_max_bytes / (1024 * 1024)));
    SetStatusTag("FastB" + std::to_string(builder_id) + "WriterQueueFullN",
		 std::to_string(writer_queue_full));
    SetStatusTag("FastB" + std::to_string(builder_id) + "EnqueueAvgUs",
		 std::to_string(complete > 0 ? enqueue_wait_us / complete : 0));
    SetStatusTag("FastB" + std::to_string(builder_id) + "EnqueueMaxUs",
		 std::to_string(enqueue_wait_max_us));
  }

  for(size_t device_id = 0; device_id < kFastDeviceCount; ++device_id){
    SetStatusTag(std::string("FastFrag") + kFastDeviceNames[device_id],
		 std::to_string(m_fast_fragments_received[device_id].load(
		   std::memory_order_relaxed)));
  }

  SetStatusTag("FastBuilder", m_fast_builder_enabled ? "1" : "0");
  SetStatusTag("FastBuilderN", std::to_string(m_fast_builder_count));
  SetStatusTag("FastFullMask", std::to_string(m_fast_full_mask));
  if(m_fast_builder_enabled)
    SetStatusTag("EventN", std::to_string(complete_total));
  SetStatusTag("FastCompleteN", std::to_string(complete_total));
  SetStatusTag("FastIncompleteN", std::to_string(incomplete_total));
  SetStatusTag("FastDuplicateN", std::to_string(duplicate_total));
  SetStatusTag("FastOverwriteN", std::to_string(overwrite_total));
  SetStatusTag("FastQueueDepth", std::to_string(TotalQueueDepth()));
  SetStatusTag("FastQueueMaxDepth", std::to_string(TotalQueueMaxDepth()));
  SetStatusTag("FastQueueFullN", std::to_string(TotalQueueFullCount()));
  SetStatusTag("FastSerializeAvgUs",
	       std::to_string(complete_total > 0 ?
			      serialize_us_total / complete_total : 0));
  SetStatusTag("FastSerializeMaxUs", std::to_string(serialize_us_max));
  SetStatusTag("FastEnqueueAvgUs",
	       std::to_string(complete_total > 0 ?
			      enqueue_wait_us_total / complete_total : 0));
  SetStatusTag("FastEnqueueMaxUs", std::to_string(enqueue_wait_us_max));
  SetStatusTag("FastWriterQueueDepth", std::to_string(writer_queue_depth_total));
  SetStatusTag("FastWriterQueueMB",
	       std::to_string(writer_queue_bytes_total / (1024 * 1024)));
  SetStatusTag("FastWriterQueueMaxDepth",
	       std::to_string(writer_queue_max_depth_total));
  SetStatusTag("FastWriterQueueMaxMB",
	       std::to_string(writer_queue_max_bytes_total / (1024 * 1024)));
  SetStatusTag("FastWriterQueueFullN", std::to_string(writer_queue_full_total));
  SetStatusTag("FastWriteAvgUs",
	       std::to_string(write_count_total > 0 ?
			      write_us_total / write_count_total : 0));
  SetStatusTag("FastWriteMaxUs", std::to_string(write_us_max));
  SetStatusTag("FastFileMB",
	       std::to_string(output_bytes_total / (1024 * 1024)));
  SetStatusTag("FastRouteDropN",
	       std::to_string(m_fast_route_drop_count.load(
		 std::memory_order_relaxed)));
  SetStatusTag("FastUnknownDeviceN",
	       std::to_string(m_fast_unknown_device_count.load(
		 std::memory_order_relaxed)));
  SetStatusTag("FastNonTriggerN",
	       std::to_string(m_fast_nontrigger_count.load(
		 std::memory_order_relaxed)));
}

void FERSDataCollector::DoConnect(eudaq::ConnectionSPC idx){
  std::unique_lock<std::mutex> lk(m_mtx_map);
  m_conn_evque[idx].clear();
  m_conn_inactive.erase(idx);
  m_expected_connections.insert(idx);
  if(m_fast_builder_enabled){
    const int device_id = DeviceIdFromConnectionName(idx->GetName());
    if(device_id < 0){
      EUDAQ_WARN("FERSDataCollector fast builder got unknown producer connection: "
		 + idx->GetName());
    }
  }
}

void FERSDataCollector::DoDisconnect(eudaq::ConnectionSPC idx){
  std::unique_lock<std::mutex> lk(m_mtx_map);
  m_conn_inactive.insert(idx);
  m_expected_connections.erase(idx);
  m_conn_evque.erase(idx);
  if(m_expected_connections.empty()){
    m_conn_inactive.clear();
    m_conn_evque.clear();
  }
}

void FERSDataCollector::DoConfigure(){
  StopFastBuilders();
  m_fast_builders.clear();
  m_noprint = 0;
  m_required_connections = 0;
  m_sync_drop_warn_every = 1000;
  m_sync_drop_count = 0;
  m_sync_events = true;
  m_monitor_drs_max_samples = kDefaultMonitorDrsMaxSamples;
  m_fast_builder_enabled = false;
  m_fast_drop_on_full = false;
  m_fast_builder_count = kFastDefaultBuilderCount;
  m_fast_ring_size = kFastDefaultRingSize;
  m_fast_queue_size = kFastDefaultQueueSize;
  m_fast_writer_queue_bytes = kFastDefaultWriterQueueMB * 1024 * 1024;
  m_fast_full_mask = kFastDefaultFullMask;
  m_fast_builder_shift = 2;
  ResetFastCounters();
  auto conf = GetConfiguration();
  if(conf){
    conf->Print();
    m_noprint = conf->Get("FERS_DISABLE_PRINT", 0);
    m_sync_events = conf->Get("FERS_ENABLE_EVENT_SYNC", 1);
    m_required_connections = conf->Get("FERS_SYNC_EXPECTED_CONNECTIONS", 0);
    m_sync_drop_warn_every = conf->Get("FERS_SYNC_DROP_WARN_EVERY", 1000);
    m_monitor_drs_max_samples =
      conf->Get("FERS_MONITOR_DRS_MAX_SAMPLES", kDefaultMonitorDrsMaxSamples);
    m_fast_builder_enabled = conf->Get("FERS_PARALLEL_EVENT_BUILDER", 0);
    m_fast_drop_on_full = conf->Get("FERS_EVENT_BUILDER_DROP_ON_FULL", 0);
    m_fast_builder_count =
      static_cast<size_t>(conf->Get("FERS_EVENT_BUILDER_THREADS",
				    static_cast<int>(kFastDefaultBuilderCount)));
    m_fast_ring_size =
      static_cast<size_t>(conf->Get("FERS_EVENT_BUILDER_RING_SIZE",
				    static_cast<int>(kFastDefaultRingSize)));
    m_fast_queue_size =
      static_cast<size_t>(conf->Get("FERS_EVENT_BUILDER_QUEUE_SIZE",
				    static_cast<int>(kFastDefaultQueueSize)));
    m_fast_writer_queue_bytes =
      static_cast<size_t>(conf->Get("FERS_EVENT_WRITER_QUEUE_MB",
				    static_cast<int>(kFastDefaultWriterQueueMB)))
      * 1024 * 1024;
    m_fast_full_mask =
      static_cast<uint32_t>(conf->Get("FERS_EVENT_BUILDER_FULL_MASK",
				      static_cast<int>(kFastDefaultFullMask)));
  }

  if(m_fast_builder_count == 0 || m_fast_builder_count > kFastMaxBuilders ||
     !IsPowerOfTwo(m_fast_builder_count)){
    EUDAQ_WARN("FERS_EVENT_BUILDER_THREADS must be a power of two from 1 to "
	       + std::to_string(kFastMaxBuilders)
	       + "; using " + std::to_string(kFastDefaultBuilderCount));
    m_fast_builder_count = kFastDefaultBuilderCount;
  }
  if(m_fast_ring_size < m_fast_builder_count * 2)
    m_fast_ring_size = m_fast_builder_count * 2;
  if(!IsPowerOfTwo(m_fast_ring_size)){
    const size_t rounded = RoundUpPowerOfTwo(m_fast_ring_size);
    EUDAQ_WARN("FERS_EVENT_BUILDER_RING_SIZE must be a power of two; using "
	       + std::to_string(rounded));
    m_fast_ring_size = rounded;
  }
  if(m_fast_queue_size < 2)
    m_fast_queue_size = 2;
  if(!IsPowerOfTwo(m_fast_queue_size)){
    const size_t rounded = RoundUpPowerOfTwo(m_fast_queue_size);
    EUDAQ_WARN("FERS_EVENT_BUILDER_QUEUE_SIZE must be a power of two; using "
	       + std::to_string(rounded));
    m_fast_queue_size = rounded;
  }
  if((m_fast_full_mask & kFastDefaultFullMask) == 0){
    EUDAQ_WARN("FERS_EVENT_BUILDER_FULL_MASK has no known device bits; using 31");
    m_fast_full_mask = kFastDefaultFullMask;
  }
  SetStatusTag("SyncDropN", "0");
  PublishFastBuilderStatus();
}

void FERSDataCollector::DoStartRun(){
  if(m_fast_builder_enabled)
    StartFastBuilders();
}

void FERSDataCollector::DoStopRun(){
  StopFastBuilders();
}

void FERSDataCollector::DoReset(){
  StopFastBuilders();
  m_fast_builders.clear();
  std::unique_lock<std::mutex> lk(m_mtx_map);
  m_noprint = 0;
  m_required_connections = 0;
  m_sync_drop_warn_every = 1000;
  m_sync_drop_count = 0;
  m_sync_events = true;
  m_monitor_drs_max_samples = kDefaultMonitorDrsMaxSamples;
  m_fast_builder_enabled = false;
  m_fast_accepting.store(false, std::memory_order_release);
  ResetFastCounters();
  m_conn_evque.clear();
  m_conn_inactive.clear();
  m_expected_connections.clear();
  SetStatusTag("SyncDropN", "0");
  PublishFastBuilderStatus();
}

void FERSDataCollector::DoTerminate(){
  StopFastBuilders();
}

void FERSDataCollector::DoStatus(){
  PublishFastBuilderStatus();
}

eudaq::EventSPC FERSDataCollector::GetMonitorEvent(eudaq::EventSPC ev){
  auto mon_ev = CloneEventShell(ev);
  mon_ev->SetTag("MONITOR_SLIMMED", "1");
  mon_ev->SetTag("MONITOR_DRS_MAX_SAMPLES", std::to_string(m_monitor_drs_max_samples));

  for(auto subev: ev->GetSubEvents()){
    const std::string desc = subev->GetDescription();
    if(desc == "FERSProducer"){
      mon_ev->AddSubEvent(CloneEventFull(subev));
      continue;
    }
    if(desc == "DRSProducer"){
      mon_ev->AddSubEvent(CloneDrsMonitorEvent(subev, m_monitor_drs_max_samples));
      continue;
    }
    mon_ev->AddSubEvent(CloneEventFull(subev));
  }

  return mon_ev;
}

bool FERSDataCollector::RouteFastFragment(eudaq::ConnectionSPC idx,
					  eudaq::EventSP evsp){
  if(!evsp->IsFlagTrigger()){
    m_fast_nontrigger_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const int device_id_signed = DeviceIdFromConnectionName(idx->GetName());
  if(device_id_signed < 0){
    m_fast_unknown_device_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const size_t device_id = static_cast<size_t>(device_id_signed);
  const uint32_t event_id = evsp->GetTriggerN();
  const size_t builder_id = event_id & (m_fast_builder_count - 1);
  if(builder_id >= kFastMaxBuilders || !m_fast_queues[device_id][builder_id]){
    m_fast_route_drop_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  Fragment fragment;
  fragment.event_id = event_id;
  fragment.device_id = static_cast<uint32_t>(device_id);
  fragment.event = std::move(evsp);

  m_fast_fragments_received[device_id].fetch_add(1, std::memory_order_relaxed);
  auto &queue = m_fast_queues[device_id][builder_id];
  while(!queue->TryPush(std::move(fragment))){
    m_fast_queue_full_count[device_id][builder_id].fetch_add(
      1, std::memory_order_relaxed);
    if(m_fast_drop_on_full ||
       !m_fast_accepting.load(std::memory_order_acquire)){
      m_fast_route_drop_count.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    std::this_thread::yield();
  }

  m_fast_fragments_routed[builder_id].fetch_add(1, std::memory_order_relaxed);
  return true;
}

void FERSDataCollector::DoReceive(eudaq::ConnectionSPC idx, eudaq::EventSP evsp){
  if(m_fast_builder_enabled){
    if(m_fast_accepting.load(std::memory_order_acquire))
      RouteFastFragment(idx, std::move(evsp));
    else
      m_fast_route_drop_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  std::unique_lock<std::mutex> lk(m_mtx_map);
  if(!evsp->IsFlagTrigger()){
    EUDAQ_THROW("!evsp->IsFlagTrigger()");
  }

  if(!m_sync_events){
    auto ev_async = eudaq::Event::MakeUnique("FERSDRS");
    ev_async->SetFlagPacket();
    ev_async->SetTriggerN(evsp->GetTriggerN());
    ev_async->AddSubEvent(evsp);
    if(!m_noprint)
      ev_async->Print(std::cout);
    WriteEvent(std::move(ev_async));
    return;
  }

  m_conn_evque[idx].push_back(evsp);

  if(m_required_connections > 0 && m_expected_connections.size() < m_required_connections){
    return;
  }

  while(true){
    uint32_t min_trigger = UINT32_MAX;
    uint32_t max_trigger = 0;
    for(auto &conn: m_expected_connections){
      auto conn_evque = m_conn_evque.find(conn);
      if(conn_evque == m_conn_evque.end() || conn_evque->second.empty())
	return;
      uint32_t trigger_n_ev = conn_evque->second.front()->GetTriggerN();
      if(trigger_n_ev < min_trigger)
	min_trigger = trigger_n_ev;
      if(trigger_n_ev > max_trigger)
	max_trigger = trigger_n_ev;
    }

    if(m_expected_connections.empty())
      return;

    if(min_trigger != max_trigger){
      uint64_t dropped_now = 0;
      for(auto &conn: m_expected_connections){
	auto &queue = m_conn_evque[conn];
	while(!queue.empty() && queue.front()->GetTriggerN() < max_trigger){
	  queue.pop_front();
	  dropped_now++;
	}
      }

      if(dropped_now > 0){
	m_sync_drop_count += dropped_now;
	SetStatusTag("SyncDropN", std::to_string(m_sync_drop_count));
	bool should_warn = (m_sync_drop_count == dropped_now);
	if(m_sync_drop_warn_every > 0){
	  should_warn = should_warn ||
	    ((m_sync_drop_count / m_sync_drop_warn_every) !=
	     ((m_sync_drop_count - dropped_now) / m_sync_drop_warn_every));
	}
	if(should_warn){
	  EUDAQ_WARN("FERSDataCollector sync dropped "
	    + std::to_string(dropped_now)
	    + " unmatched producer event(s) while realigning to trigger "
	    + std::to_string(max_trigger)
	    + "; total dropped=" + std::to_string(m_sync_drop_count));
	}
      }
      continue;
    }

    auto ev_sync = eudaq::Event::MakeUnique("FERSDRS");
    ev_sync->SetFlagPacket();
    ev_sync->SetTriggerN(min_trigger);
    for(auto &conn: m_expected_connections){
      auto &ev_front = m_conn_evque[conn].front();
      ev_sync->AddSubEvent(ev_front);
      m_conn_evque[conn].pop_front();
    }

    if(!m_noprint)
      ev_sync->Print(std::cout);
    if(ev_sync->GetSubEvents().size() == m_expected_connections.size()){
      WriteEvent(std::move(ev_sync));
    }
  }
}
