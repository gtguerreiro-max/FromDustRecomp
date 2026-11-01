#pragma once

// Force-included into every recompiled module (see CMakeLists.txt).
//
// From Dust is split across several XEX images (Default.xex plus the Galileo
// and AI DLLs) loaded between 0x82000000 and 0x8A300000. XenonRecomp's default
// indirect-call table lives in guest memory right after one image, which would
// overlap the DLLs, so all modules share a single host-side table instead.

#define FD_GUEST_CODE_LOW  0x82000000u
#define FD_GUEST_CODE_HIGH 0x8A300000u

extern "C" void** g_ppcFuncTable;

#define PPC_LOOKUP_FUNC(x, y) (*(PPCFunc**)&g_ppcFuncTable[(uint32_t(y) - FD_GUEST_CODE_LOW) >> 2])

// Stores followed by eieio are hardware register writes; XMA registers are
// emulated by the runtime (runtime/apu/apu.cpp).
#include <cstdint>
extern "C" void FdMmioStore32(uint8_t* base, uint32_t address, uint32_t value);
#define PPC_MM_STORE_U32(x, y) FdMmioStore32(base, uint32_t(x), uint32_t(y))
