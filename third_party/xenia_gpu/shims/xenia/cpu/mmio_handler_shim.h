// FromDust shim: MMIO callback types referenced by the GPU.
#pragma once

#include <cstdint>

namespace xe {
namespace cpu {
typedef uint32_t (*MMIOReadCallback)(void* ppc_context, void* callback_context, uint32_t addr);
typedef void (*MMIOWriteCallback)(void* ppc_context, void* callback_context, uint32_t addr, uint32_t value);
}  // namespace cpu
}  // namespace xe
