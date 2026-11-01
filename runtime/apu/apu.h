#pragma once

#include <cstdint>

namespace apu
{
// Allocates the XMA context array; call after mem::Init().
void Initialize();
// Handles a write to the XMA register block (value in register byte order).
void WriteRegister(uint32_t address, uint32_t value);
}
