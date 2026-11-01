#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace kernel
{
    struct Module
    {
        std::string name;       // e.g. "GalileoKernel.xex"
        std::string path;       // relative to the game directory
        uint32_t base = 0, size = 0, entry = 0;
        uint32_t ldrEntry = 0;  // guest LDR_DATA_TABLE_ENTRY == module handle
        uint32_t xexHeader = 0; // guest copy of the XEX header
        uint32_t xexHeaderSize = 0;
        uint32_t stackSize = 0;
        uint32_t loadCount = 0;
        bool isDll = false;
        std::unordered_map<uint32_t, uint32_t> exports; // ordinal -> address
        std::vector<uint8_t> headerBytes;               // host copy of the XEX header
    };

    // Maps every XEX of the game into guest memory and resolves imports.
    bool LoadModules(const std::filesystem::path& gameDir);

    Module* MainModule();
    Module* FindModuleByName(const std::string& path); // matches the file name
    Module* FindModuleByHandle(uint32_t handle);
    Module* FindModuleByXexHeader(uint32_t xexHeader);

    // Looks up an optional XEX header field; returns its guest address or value.
    bool GetXexHeaderField(const Module& module, uint32_t key, uint32_t& result);

    // Guest address of a kernel variable export, for code that needs it.
    uint32_t KernelVariable(const char* name);

    // Refreshes KeTimeStampBundle (called periodically).
    void UpdateTimeStampBundle();
}
