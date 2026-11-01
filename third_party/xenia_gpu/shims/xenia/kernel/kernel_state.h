// FromDust shim for xenia/kernel/kernel_state.h: the few kernel services the
// GPU uses (interrupt delivery, title info, display configuration).
#ifndef XENIA_KERNEL_KERNEL_STATE_H_
#define XENIA_KERNEL_KERNEL_STATE_H_

#include <cstdint>

#include "xenia/cpu/processor.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe {
class Emulator;
namespace kernel {

constexpr uint16_t XCONFIG_USER_CATEGORY = 0x0003;
constexpr uint16_t XCONFIG_USER_AV_COMPOSITE_SCREENSZ = 0x0013;

struct Resolution {
  explicit Resolution(uint32_t packed) : width_(packed >> 16), height_(packed & 0xFFFF) {}
  uint32_t width_, height_;
};

class XConfig {
 public:
  template <typename T>
  T ReadSetting(uint16_t category, uint16_t setting) {
    // Packed width << 16 | height of the output (720p).
    return T((1280u << 16) | 720u);
  }
};

class KernelState {
 public:
  KernelState(Emulator* emulator, cpu::Processor* processor)
      : emulator_(emulator), processor_(processor) {}

  Emulator* emulator() const { return emulator_; }
  cpu::Processor* processor() const { return processor_; }
  Memory* memory() const { return processor_->memory(); }
  bool is_title_open() const { return true; }
  uint32_t GetIdleProcess() const { return 0; }
  XConfig* xconfig() { return &xconfig_; }
  UserModule* GetExecutableModule() { return &executable_module_; }

  // Runs the guest graphics interrupt callback as if on hardware thread `cpu`.
  void EmulateCPInterruptDPC(uint32_t interrupt_callback, uint32_t interrupt_callback_data,
                             uint32_t source, uint32_t cpu);

 private:
  Emulator* emulator_;
  cpu::Processor* processor_;
  XConfig xconfig_;
  UserModule executable_module_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_STATE_H_
