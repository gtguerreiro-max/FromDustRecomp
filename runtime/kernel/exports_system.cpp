// Modules (Xex*), configuration, Rtl helpers, debug output and the C runtime
// string formatting exported by the kernel.

#include <cstring>
#include <string>

#include "guest_printf.h"
#include "kernel.h"
#include "loader.h"
#include "thread.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace kernel;

namespace
{
    // (STATUS_DLL_NOT_FOUND / STATUS_ORDINAL_NOT_FOUND come from windows.h.)
    void CallDllEntry(Module& module, uint32_t reason)
    {
        if (!module.entry)
            return;
        // Runs on the caller's guest stack, below its current frame.
        PPCContext callCtx = *CurrentThread()->ctx;
        callCtx.r3.u64 = module.ldrEntry;
        callCtx.r4.u64 = reason;
        callCtx.r5.u64 = 0;
        CallGuest(callCtx, module.entry);
        Log("%s: DllMain(%u) returned %u", module.name.c_str(), reason, callCtx.r3.u32);
    }
}

// --- Modules -------------------------------------------------------------------

KFUNC(XexLoadImage)
{
    // (module_name, flags, min_version, handle_ptr)
    const std::string name = mem::Ptr<char>(ARG0);
    Module* module = FindModuleByName(name);
    if (!module)
    {
        Log("XexLoadImage(%s): not found", name.c_str());
        RETURN(STATUS_DLL_NOT_FOUND);
    }
    Log("XexLoadImage(%s)", name.c_str());
    if (ARG3)
        mem::Write32(ARG3, module->ldrEntry);
    if (module->loadCount++ == 0 && module->isDll)
    {
        PPCContext saved = ctx;
        CallDllEntry(*module, 1 /* DLL_PROCESS_ATTACH */);
        ctx = saved;
    }
    RETURN(STATUS_SUCCESS);
}

KFUNC(XexUnloadImage)
{
    Module* module = FindModuleByHandle(ARG0);
    if (module && module->loadCount && --module->loadCount == 0 && module->isDll)
    {
        PPCContext saved = ctx;
        CallDllEntry(*module, 0 /* DLL_PROCESS_DETACH */);
        ctx = saved;
    }
    RETURN(STATUS_SUCCESS);
}

KFUNC(XexUnloadImageAndExitThread)
{
    Module* module = FindModuleByHandle(ARG0);
    if (module && module->loadCount)
        --module->loadCount;
    ExitCurrentThread(ARG1);
}

KFUNC(XexGetModuleHandle)
{
    // (module_name or null, handle_ptr)
    Module* module = ARG0 ? FindModuleByName(mem::Ptr<char>(ARG0)) : MainModule();
    if (!module)
    {
        if (ARG1)
            mem::Write32(ARG1, 0);
        RETURN(STATUS_DLL_NOT_FOUND);
    }
    if (ARG1)
        mem::Write32(ARG1, module->ldrEntry);
    RETURN(STATUS_SUCCESS);
}

KFUNC(XexGetProcedureAddress)
{
    // (handle, ordinal, out_ptr)
    Module* module = FindModuleByHandle(ARG0);
    if (!module)
        RETURN(STATUS_DLL_NOT_FOUND);
    auto it = module->exports.find(ARG1);
    if (it == module->exports.end())
    {
        Log("XexGetProcedureAddress(%s, %u): not found", module->name.c_str(), ARG1);
        RETURN(STATUS_ORDINAL_NOT_FOUND);
    }
    mem::Write32(ARG2, it->second);
    RETURN(STATUS_SUCCESS);
}

KFUNC(XexGetModuleSection)
{
    // (handle, section_name, data_ptr, size_ptr)
    Module* module = ARG0 ? FindModuleByHandle(ARG0) : MainModule();
    if (!module)
        RETURN(STATUS_DLL_NOT_FOUND);
    const char* wanted = mem::Ptr<char>(ARG1);
    const uint8_t* img = mem::Ptr<uint8_t>(module->base);
    const uint32_t pe = *(const uint32_t*)(img + 0x3C);
    const uint16_t sections = *(const uint16_t*)(img + pe + 6);
    const uint16_t optSize = *(const uint16_t*)(img + pe + 0x14);
    const uint8_t* s = img + pe + 0x18 + optSize;
    for (uint16_t i = 0; i < sections; i++, s += 40)
    {
        if (strncmp((const char*)s, wanted, 8) == 0)
        {
            mem::Write32(ARG2, module->base + *(const uint32_t*)(s + 12));
            mem::Write32(ARG3, *(const uint32_t*)(s + 8));
            RETURN(STATUS_SUCCESS);
        }
    }
    RETURN(STATUS_UNSUCCESSFUL);
}

KFUNC(XexCheckExecutablePrivilege)
{
    // Privileges are bits of the system flags header field.
    uint32_t flags = 0;
    if (GetXexHeaderField(*MainModule(), 0x00030000, flags))
        RETURN((flags >> (ARG0 & 31)) & 1);
    RETURN(0);
}

KFUNC(RtlImageXexHeaderField)
{
    // (xex_header_base, key)
    Module* module = FindModuleByXexHeader(ARG0);
    uint32_t result = 0;
    if (module)
        GetXexHeaderField(*module, ARG1, result);
    RETURN(result);
}

KFUNC(XexLoadImageHeaders) { RETURN(STATUS_NOT_IMPLEMENTED); }

// --- System configuration --------------------------------------------------------

KFUNC(ExGetXConfigSetting)
{
    // (category, setting, buffer, buffer_size, required_size_ptr)
    const uint16_t category = uint16_t(ARG0), setting = uint16_t(ARG1);
    const uint32_t buffer = ARG2, size = ARG3, requiredPtr = ARG4;
    uint32_t value = 0, valueSize = 4;
    switch ((category << 16) | setting)
    {
    case 0x00020002: value = 0x00001000; break;          // AV region: NTSC-U
    case 0x00030001: value = 0; break;                   // time zone bias
    case 0x00030009: value = 1; break;                   // language: English
    case 0x0003000A: value = 0x00040000 | 0x00010000; break; // video flags: widescreen, 720p
    case 0x0003000B: value = 0x00010001; break;          // audio flags: Dolby Digital
    case 0x0003000E: value = 0x40; break;                // retail flags
    case 0x00030010: value = 0x67; valueSize = 1; break; // country: United States
    default:
        Log("ExGetXConfigSetting(%u, %u): unknown, returning zeros", category, setting);
        break;
    }
    if (buffer && size)
    {
        memset(mem::Translate(buffer), 0, size);
        if (valueSize == 1)
            *mem::Ptr<uint8_t>(buffer) = uint8_t(value);
        else if (size >= 4)
            mem::Write32(buffer, value);
        else if (size >= 2)
            *mem::Ptr<be<uint16_t>>(buffer) = uint16_t(value);
    }
    if (requiredPtr)
        *mem::Ptr<be<uint16_t>>(requiredPtr) = uint16_t(valueSize);
    RETURN(STATUS_SUCCESS);
}

KFUNC(XGetAVPack) { RETURN(6); }          // HDMI
KFUNC(XGetGameRegion) { RETURN(0xFFFF); } // all regions
KFUNC(XGetLanguage) { RETURN(1); }        // English
KFUNC(XamGetSystemVersion) { RETURN(0x20D35380); }

KFUNC(XGetVideoMode)
{
    // XVIDEO_MODE: width, height, interlaced, widescreen, hidef, refresh (float), standard, ...
    const uint32_t mode = ARG0;
    memset(mem::Translate(mode), 0, 48);
    mem::Write32(mode + 0x00, 1280);
    mem::Write32(mode + 0x04, 720);
    mem::Write32(mode + 0x08, 0);
    mem::Write32(mode + 0x0C, 1);
    mem::Write32(mode + 0x10, 1);
    const float refresh = 60.0f;
    uint32_t bits;
    memcpy(&bits, &refresh, 4);
    mem::Write32(mode + 0x14, bits);
    mem::Write32(mode + 0x18, 1); // NTSC-M
    RETURN(0);
}

KFUNC(XamGetExecutionId)
{
    uint32_t id = 0;
    GetXexHeaderField(*MainModule(), 0x00040006, id);
    mem::Write32(ARG0, id);
    RETURN(STATUS_SUCCESS);
}

KFUNC(HalReturnToFirmware)
{
    Log("HalReturnToFirmware(%u): exiting", ARG0);
    ExitProcess(0);
}

KFUNC(XamLoaderTerminateTitle)
{
    Log("XamLoaderTerminateTitle: exiting");
    ExitProcess(0);
}

KFUNC(XamLoaderLaunchTitle)
{
    Log("XamLoaderLaunchTitle(%s): exiting", ARG0 ? mem::Ptr<char>(ARG0) : "");
    ExitProcess(0);
}

// --- Debug / errors -----------------------------------------------------------------

KFUNC(DbgPrint)
{
    GuestArgs args = GuestArgs::FromRegisters(ctx, 4);
    std::string text = FormatGuest(mem::Ptr<char>(ARG0), args);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    Log("[guest] %s", text.c_str());
    RETURN(STATUS_SUCCESS);
}

KFUNC(DbgBreakPoint) { Log("DbgBreakPoint"); RETURN(0); }

KFUNC(RtlRaiseException)
{
    // EXCEPTION_RECORD: code @0, flags @4, record @8, address @0xC, nparams @0x10, info @0x14
    const uint32_t record = ARG0;
    const uint32_t code = mem::Read32(record);
    if (code == 0x406D1388) // MS_VC_EXCEPTION: SetThreadName
    {
        const uint32_t info = record + 0x14;
        const uint32_t name = mem::Read32(info + 4);
        Log("thread name: %s", name ? mem::Ptr<char>(name) : "?");
        RETURN(0);
    }
    Fatal("RtlRaiseException(code 0x%08X, address 0x%08X) - guest exceptions are not supported", code,
        mem::Read32(record + 0x0C));
}

KFUNC(RtlUnwind) { Fatal("RtlUnwind - guest exceptions are not supported"); }

KFUNC(__C_specific_handler)
{
    Log("__C_specific_handler called");
    RETURN(1); // ExceptionContinueSearch
}

KFUNC(RtlCaptureContext) { RETURN(0); }

KFUNC(RtlNtStatusToDosError)
{
    switch (ARG0)
    {
    case STATUS_SUCCESS: RETURN(ERROR_SUCCESS);
    case STATUS_OBJECT_NAME_NOT_FOUND: RETURN(ERROR_FILE_NOT_FOUND);
    case STATUS_OBJECT_PATH_NOT_FOUND: RETURN(ERROR_PATH_NOT_FOUND);
    case STATUS_ACCESS_DENIED: RETURN(ERROR_ACCESS_DENIED);
    case STATUS_INVALID_PARAMETER: RETURN(ERROR_INVALID_PARAMETER);
    case STATUS_NO_MORE_FILES: RETURN(ERROR_NO_MORE_FILES);
    case STATUS_OBJECT_NAME_COLLISION: RETURN(ERROR_ALREADY_EXISTS);
    case STATUS_PENDING: RETURN(ERROR_IO_PENDING);
    case STATUS_END_OF_FILE: RETURN(38); // ERROR_HANDLE_EOF
    }
    RETURN(ARG0 & 0xFFFF);
}

// --- C runtime string formatting -----------------------------------------------------

namespace
{
    uint32_t StoreFormatted(uint32_t buffer, uint32_t count, const std::string& text, bool truncateMinusOne)
    {
        if (count == 0xFFFFFFFF)
            count = uint32_t(text.size() + 1);
        if (text.size() < count)
        {
            memcpy(mem::Translate(buffer), text.c_str(), text.size() + 1);
            return uint32_t(text.size());
        }
        if (count)
            memcpy(mem::Translate(buffer), text.data(), count);
        return truncateMinusOne ? 0xFFFFFFFF : count;
    }
}

KFUNC(sprintf)
{
    GuestArgs args = GuestArgs::FromRegisters(ctx, 5);
    const std::string text = FormatGuest(mem::Ptr<char>(ARG1), args);
    RETURN(StoreFormatted(ARG0, 0xFFFFFFFF, text, false));
}

KFUNC(_snprintf)
{
    GuestArgs args = GuestArgs::FromRegisters(ctx, 6);
    const std::string text = FormatGuest(mem::Ptr<char>(ARG2), args);
    RETURN(StoreFormatted(ARG0, ARG1, text, true));
}

KFUNC(_vsnprintf)
{
    GuestArgs args = GuestArgs::FromVaList(ARG3);
    const std::string text = FormatGuest(mem::Ptr<char>(ARG2), args);
    RETURN(StoreFormatted(ARG0, ARG1, text, true));
}
