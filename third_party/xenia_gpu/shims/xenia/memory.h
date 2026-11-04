// FromDust shim for xenia/memory.h: the GPU's view of guest memory, backed by
// the FromDust runtime's 4 GB guest block (physical memory at 0xA0000000).
// Implemented in runtime/gpu/xenia_bridge.cpp.
#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_
#define FROMDUST_GPU_SHIM 1

#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/mmio_handler_shim.h"

namespace xe {

constexpr uint32_t kMemoryAllocationCommit = 1 << 1;
constexpr uint32_t kMemoryProtectWrite = 1 << 1;
struct HeapAllocationInfo {
  uint32_t base_address = 0;
  uint32_t region_size = 0;
  uint32_t state = kMemoryAllocationCommit;
  uint32_t protect = kMemoryProtectWrite;
};

class PhysicalHeap {
 public:
  // Everything is committed read/write in the FromDust block.
  xe::memory::PageAccess QueryRangeAccess(uint32_t low_address, uint32_t high_address) {
    return xe::memory::PageAccess::kReadWrite;
  }
  uint32_t GetPhysicalAddress(uint32_t address) const { return (address&0x1FFFFFFF)+(address>=0xE0000000u?0x1000:0); }
  bool QueryRegionInfo(uint32_t address, HeapAllocationInfo* info) {
    if (address >= 0x20000000) return false;
    info->base_address = 0;
    info->region_size = 0x20000000;
    info->state = kMemoryAllocationCommit;
    info->protect = kMemoryProtectWrite;
    return true;
  }
};
using VirtualHeap = PhysicalHeap;

class Memory {
 public:
  Memory(uint8_t* virtual_membase, uint8_t* physical_membase)
      : virtual_membase_(virtual_membase), physical_membase_(physical_membase) {}

  uint8_t* virtual_membase() const { return virtual_membase_; }
  uint8_t* physical_membase() const { return physical_membase_; }

  template <typename T = uint8_t*>
  inline T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(virtual_membase_ + guest_address);
  }
  template <typename T>
  inline xe::be<T>* TranslateVirtualBE(uint32_t guest_address) const {
    return TranslateVirtual<xe::be<T>*>(guest_address);
  }
  template <typename T = uint8_t*>
  inline T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & 0x1FFFFFFF));
  }
  uint32_t HostToGuestVirtual(const void* host_address) const {
    return uint32_t(static_cast<const uint8_t*>(host_address) - virtual_membase_);
  }
  uint32_t GetPhysicalAddress(uint32_t address) const { return (address&0x1FFFFFFF)+(address>=0xE0000000u?0x1000:0); }

  // MMIO is plain memory in FromDust; register traffic is bridged separately.
  bool AddVirtualMappedRange(uint32_t, uint32_t, uint32_t, void*, cpu::MMIOReadCallback, cpu::MMIOWriteCallback) {
    return true;
  }

  PhysicalHeap* GetPhysicalHeap() { return &physical_heap_; }
  xe::memory::PageAccess QueryRangeAccess(uint32_t, uint32_t) { return xe::memory::PageAccess::kReadWrite; }

  // Write watches on physical memory: watched pages are made read-only, and
  // the first CPU write to one notifies every callback (textures, buffers).
  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);
  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback, void* callback_context);
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);
  void EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                           bool enable_invalidation_notifications, bool enable_data_providers);

  // Called by the FromDust fault handler / host writers. Returns true if the
  // write hit a watched page (which is then unwatched).
  bool OnPhysicalWrite(uint32_t physical_address, uint32_t length);

  void SetGpuTelemetry(std::atomic<uint64_t>* frames, std::atomic<uint64_t>* draws) {
    frames_ = frames;
    draws_ = draws;
  }
  void SetGpuFramePacer(void (*pacer)()) { frame_pacer_ = pacer; }
  void SetGpuFrameObserver(void (*observer)()) { frame_observer_ = observer; }
  void OnGpuSwap() { if (frame_pacer_) frame_pacer_(); if (frame_observer_) frame_observer_(); if (frames_) ++*frames_; }
  void OnGpuDraw() { if (draws_) ++*draws_; }

 private:
  uint8_t* virtual_membase_;
  uint8_t* physical_membase_;
  PhysicalHeap physical_heap_;
  std::vector<std::pair<PhysicalMemoryInvalidationCallback, void*>*> invalidation_callbacks_;
  std::mutex watch_mutex_;
  std::array<bool, 0x20000000 / 0x1000> watched_pages_{};
  std::atomic<uint64_t>* frames_ = nullptr;
  std::atomic<uint64_t>* draws_ = nullptr;
  void (*frame_pacer_)() = nullptr;
  void (*frame_observer_)() = nullptr;
};

}  // namespace xe

#endif  // XENIA_MEMORY_H_
