// FromDust shim for xenia/cpu/processor.h: only what the GPU needs.
#ifndef XENIA_CPU_PROCESSOR_H_
#define XENIA_CPU_PROCESSOR_H_

#include <cstdint>

#include "xenia/cpu/mmio_handler_shim.h"
#include "xenia/memory.h"

namespace xe {
class ByteStream;
namespace cpu {

namespace backend {
constexpr uint32_t MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES = 32;
struct GuestPseudoStackTrace {
  uint32_t count = 0;
  uint32_t return_addrs[MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES]{};
  bool truncated_flag = false;
};
}

class Backend {
 public:
  bool PopulatePseudoStacktrace(backend::GuestPseudoStackTrace*) { return false; }
};

class Processor {
 public:
  explicit Processor(Memory* memory) : memory_(memory) {}
  Memory* memory() const { return memory_; }
  Backend* backend() { return &backend_; }

 private:
  Memory* memory_;
  Backend backend_;
};

struct StackTrace {};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PROCESSOR_H_
