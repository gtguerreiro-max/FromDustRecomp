#pragma once
#include <cstdint>
namespace gpu::native {class Renderer;}
struct ID3D12Device;

namespace gpu::xenia {
bool Initialize();
bool Active();
bool HasWindow();
int RunMessageLoop();
void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2);
void EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2);
void SetInterruptCallback(uint32_t callback, uint32_t userData);
// Called right after the guest's D3D writes CP_RB_WPTR (see gpu_hooks.cpp).
void NotifyWritePointer();
// Windows I/O validates destination pages before writing and doesn't deliver
// the write faults used by the GPU texture cache.
void PrepareHostWrite(uint32_t address, uint32_t length);
void WatchPhysical(uint32_t address,uint32_t length);
uint64_t PhysicalRevision(uint32_t address,uint32_t length);
uint64_t PhysicalWriteSerial();
// Changes after every recorded write: a PhysicalRevision computed after
// reading a generation stays valid while the generation is the same.
uint64_t PhysicalWriteGeneration();
ID3D12Device* NativeDevice();
bool PresentNative(gpu::native::Renderer& renderer);
}
