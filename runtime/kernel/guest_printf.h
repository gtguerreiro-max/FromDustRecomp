#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "kernel.h"

namespace kernel
{
    // Source of printf arguments: 64-bit values in guest call order.
    class GuestArgs
    {
    public:
        // Variadic call: arguments start at register r<firstReg> and continue
        // on the stack (8-byte big-endian slots from r1+0x50).
        static GuestArgs FromRegisters(const PPCContext& ctx, int firstReg);
        // va_list: pointer to consecutive 8-byte big-endian slots.
        static GuestArgs FromVaList(uint32_t vaList);

        uint64_t Next();

    private:
        const PPCContext* ctx_ = nullptr;
        int reg_ = 0;
        uint32_t stack_ = 0;
    };

    std::string FormatGuest(const char* format, GuestArgs& args);
}
