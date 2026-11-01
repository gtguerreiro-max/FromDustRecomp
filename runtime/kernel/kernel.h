#pragma once

#include <cstdarg>
#include <cstdint>

#include "ppc_config.h"
#include <ppc_context.h>

#include "../memory.h"

// Helpers for implementing Xbox 360 kernel / XAM exports.
//
// Every import is recompiled as a call to `__imp__<Name>(ctx, base)`, with
// the arguments in r3..r10 (f1.. for floats) and the result returned in r3.
//
// KFUNC wraps the body so every call can be traced (FD_TRACE=1).
#define KFUNC(name)                                                         \
    static void name##_Impl(PPCContext& __restrict ctx, uint8_t* base);     \
    PPC_FUNC(__imp__##name)                                                 \
    {                                                                       \
        const bool traced = kernel::g_traceImports;                         \
        if (traced) kernel::TraceEnter(#name, ctx);                         \
        name##_Impl(ctx, base);                                             \
        if (traced) kernel::TraceLeave(#name, ctx);                         \
    }                                                                       \
    static void name##_Impl(PPCContext& __restrict ctx, [[maybe_unused]] uint8_t* base)

#define ARG0 ctx.r3.u32
#define ARG1 ctx.r4.u32
#define ARG2 ctx.r5.u32
#define ARG3 ctx.r6.u32
#define ARG4 ctx.r7.u32
#define ARG5 ctx.r8.u32
#define ARG6 ctx.r9.u32
#define ARG7 ctx.r10.u32
// Arguments past r10 live on the guest stack.
#define STACK_ARG(n) mem::Read32(ctx.r1.u32 + 0x54 + (n) * 8)
#define RETURN(value) do { ctx.r3.u64 = uint64_t(value); return; } while (0)

// NTSTATUS values used by the kernel.
constexpr uint32_t STATUS_SUCCESS = 0x00000000;
constexpr uint32_t STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t STATUS_PENDING = 0x00000103;
constexpr uint32_t STATUS_ALERTED = 0x00000101;
constexpr uint32_t STATUS_USER_APC = 0x000000C0;
constexpr uint32_t STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t STATUS_OBJECT_NAME_EXISTS = 0x40000000;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t STATUS_NOT_SUPPORTED = 0xC00000BB;

// Win32 error codes returned by XAM.
constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
constexpr uint32_t ERROR_ACCESS_DENIED = 5;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t ERROR_ALREADY_EXISTS = 183;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t ERROR_NOT_FOUND = 1168;
constexpr uint32_t ERROR_CANCELLED = 1223;
constexpr uint32_t ERROR_NO_SUCH_USER = 1317;

namespace kernel
{
    void Log(const char* fmt, ...);
    [[noreturn]] void Fatal(const char* fmt, ...);

    extern bool g_traceImports;
    void TraceEnter(const char* name, const PPCContext& ctx);
    void TraceLeave(const char* name, const PPCContext& ctx);

    // Logs the first call of every export that is still a stub.
    void LogUnimplemented(const char* name);

    // Calls a recompiled guest function by address on the current thread.
    void CallGuest(PPCContext& ctx, uint32_t address);
    PPCFunc* FindFunction(uint32_t address);

    // Function table shared by every recompiled module (ppc_overrides.h).
    bool InitFunctionTable();
}
