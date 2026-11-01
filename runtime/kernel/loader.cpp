#include "loader.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>

#include <image.h>

#include "kernel.h"
#include "thread.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Ordinal -> name tables from XenonUtils (generated from Xenia's export lists).
extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

namespace kernel
{
    namespace
    {
        // Every XEX shipped with the game. They load at fixed, non-overlapping
        // addresses, so all of them are mapped up front; XexLoadImage then
        // only has to run the DLL entry point.
        const char* const kModulePaths[] = {
            "Default.xex",
            "GalileoKernel.xex",
            "GalileoPathfind_9_9.xex",
            "GalileoSimulatorWater.xex",
            "GalileoSimulatorWater_9_9.xex",
            "GalileoTexture.xex",
            "AInative/bin/dll/dustai.x360.f.xex",
        };

        std::vector<std::unique_ptr<Module>> g_modules;
        std::unordered_map<std::string, uint32_t> g_variables;

        uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

        uint32_t AllocString(const std::string& s)
        {
            uint32_t addr = mem::AllocSystem(uint32_t(s.size() + 1));
            memcpy(mem::Translate(addr), s.c_str(), s.size() + 1);
            return addr;
        }

        // UTF-16BE string + X_UNICODE_STRING {u16 length, u16 max, u32 buffer}.
        void WriteUnicodeString(uint32_t target, const std::string& s)
        {
            const uint32_t buffer = mem::AllocSystem(uint32_t(s.size() + 1) * 2);
            for (size_t i = 0; i < s.size(); i++)
                *mem::Ptr<be<uint16_t>>(buffer + uint32_t(i) * 2) = uint16_t(uint8_t(s[i]));
            *mem::Ptr<be<uint16_t>>(target) = uint16_t(s.size() * 2);
            *mem::Ptr<be<uint16_t>>(target + 2) = uint16_t((s.size() + 1) * 2);
            mem::Write32(target + 4, buffer);
        }

        void CreateKernelVariables()
        {
            auto var = [](const char* name, uint32_t size) {
                uint32_t addr = mem::AllocSystem(size < 16 ? 16 : size);
                g_variables[name] = addr;
                return addr;
            };

            // XboxHardwareInfo: flags, CPU count.
            uint32_t hw = var("XboxHardwareInfo", 16);
            mem::Write32(hw, 0x00000020);
            mem::Ptr<uint8_t>(hw)[4] = 6;

            // XboxKrnlVersion: 2.0.20353.0 (the XDK the game was built with).
            uint32_t ver = var("XboxKrnlVersion", 8);
            *mem::Ptr<be<uint16_t>>(ver) = 2;
            *mem::Ptr<be<uint16_t>>(ver + 2) = 0;
            *mem::Ptr<be<uint16_t>>(ver + 4) = 20353;
            *mem::Ptr<be<uint16_t>>(ver + 6) = 0;

            var("KeTimeStampBundle", 24);
            UpdateTimeStampBundle();

            // ExLoadedCommandLine is a char array, not a pointer.
            uint32_t cmd = var("ExLoadedCommandLine", 1024);
            strcpy(mem::Ptr<char>(cmd), "\"default.xex\"");

            mem::Write32(var("VdGpuClockInMHz", 4), 500);
            var("VdGlobalDevice", 4);
            var("VdGlobalXamDevice", 4);
            // An RTL_CRITICAL_SECTION the kernel initializes at boot (free = -1).
            uint32_t hsioLock = var("VdHSIOCalibrationLock", 32);
            mem::Ptr<uint8_t>(hsioLock)[0] = 1; // synchronization event header
            mem::Write32(hsioLock + 0x10, 0xFFFFFFFF);
            var("KeDebugMonitorData", 256);
            var("KeCertMonitorData", 256);
            var("XexExecutableModuleHandle", 4);

            // Object type descriptors (only compared by address).
            for (const char* name : { "ExThreadObjectType", "ExEventObjectType", "ExSemaphoreObjectType",
                     "ExTimerObjectType", "ExMutantObjectType" })
                var(name, 64);
        }

        void ResolveImports(const Module& module)
        {
            const uint8_t* h = module.headerBytes.data();
            const uint32_t count = BE32(h + 0x14);
            uint32_t offset = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                if (BE32(h + 0x18 + i * 8) == 0x000103FF)
                    offset = BE32(h + 0x1C + i * 8);
            }
            if (!offset)
                return;

            const uint32_t stringSize = BE32(h + offset + 4);
            const uint32_t libraryCount = BE32(h + offset + 8);
            std::vector<std::string> names;
            for (const char* p = (const char*)h + offset + 12; p < (const char*)h + offset + 12 + stringSize;)
            {
                if (*p)
                {
                    names.emplace_back(p);
                    p += names.back().size();
                }
                ++p;
            }

            const uint8_t* lib = h + offset + 12 + stringSize;
            for (uint32_t l = 0; l < libraryCount; l++)
            {
                const uint32_t size = BE32(lib);
                const std::string& libName = names[BE16(lib + 0x24) & 0xFF];
                const auto& table = libName == "xam.xex" ? XamExports : XboxKernelExports;
                const uint16_t records = BE16(lib + 0x26);
                for (uint16_t r = 0; r < records; r++)
                {
                    const uint32_t slot = BE32(lib + 0x28 + r * 4);
                    // Image::ParseImage byte-swapped every record in place.
                    const uint32_t value = *mem::Ptr<uint32_t>(slot);
                    if ((value >> 24) != 0)
                        continue; // function thunk: recompiled as __imp__Name
                    auto name = table.find(value & 0xFFFF);
                    if (name == table.end())
                        continue;
                    // XenonUtils' tables name exports "__imp__<Name>".
                    std::string exportName = name->second;
                    if (exportName.rfind("__imp__", 0) == 0)
                        exportName.erase(0, 7);
                    auto variable = g_variables.find(exportName);
                    // Others are IAT slots of function imports, called via thunks.
                    if (variable != g_variables.end())
                        mem::Write32(slot, variable->second);
                }
                lib += size;
            }
        }

        // XEX DLLs keep their exports in an XEX export table pointed to by the
        // security info: base ordinal, count, image base (>> 16) and offsets.
        // Falls back to a regular (little-endian) PE export directory.
        void ParseExports(Module& module)
        {
            const uint8_t* h = module.headerBytes.data();
            const uint32_t security = BE32(h + 0x10);
            const uint32_t table = BE32(h + security + 0x160);
            if (table)
            {
                const uint32_t imageBase = mem::Read32(table + 0x20) << 16;
                const uint32_t count = mem::Read32(table + 0x24);
                const uint32_t ordinalBase = mem::Read32(table + 0x28);
                for (uint32_t i = 0; i < count; i++)
                {
                    if (const uint32_t offset = mem::Read32(table + 0x2C + i * 4))
                        module.exports[ordinalBase + i] = imageBase + offset;
                }
                return;
            }

            const uint8_t* img = mem::Ptr<uint8_t>(module.base);
            const uint32_t pe = *(const uint32_t*)(img + 0x3C);
            const uint32_t exportRva = *(const uint32_t*)(img + pe + 0x78);
            if (!exportRva)
                return;
            const uint8_t* dir = img + exportRva;
            const uint32_t ordinalBase = *(const uint32_t*)(dir + 0x10);
            const uint32_t functionCount = *(const uint32_t*)(dir + 0x14);
            const uint32_t* functions = (const uint32_t*)(img + *(const uint32_t*)(dir + 0x1C));
            for (uint32_t i = 0; i < functionCount; i++)
            {
                if (functions[i])
                    module.exports[ordinalBase + i] = module.base + functions[i];
            }
        }

        bool LoadModule(const std::filesystem::path& gameDir, const char* relPath)
        {
            std::ifstream file(gameDir / relPath, std::ios::binary);
            if (!file)
            {
                Log("missing module %s", relPath);
                return false;
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            Image image = Image::ParseImage(data.data(), data.size());
            if (!image.data)
            {
                Log("failed to parse %s", relPath);
                return false;
            }

            auto module = std::make_unique<Module>();
            module->path = relPath;
            module->name = std::filesystem::path(relPath).filename().string();
            module->base = uint32_t(image.base);
            module->size = image.size;
            module->entry = uint32_t(image.entry_point);
            memcpy(mem::Translate(module->base), image.data.get(), image.size);

            const uint32_t headerSize = BE32(data.data() + 8); // PE data offset == header size
            module->headerBytes.assign(data.begin(), data.begin() + headerSize);
            module->xexHeaderSize = headerSize;
            module->xexHeader = mem::AllocSystem(headerSize);
            memcpy(mem::Translate(module->xexHeader), data.data(), headerSize);
            module->isDll = (BE32(data.data() + 4) & 0x8) != 0;

            uint32_t stackSize = 0;
            module->stackSize = GetXexHeaderField(*module, 0x00020200, stackSize) ? stackSize : 0x40000;

            // Guest LDR_DATA_TABLE_ENTRY; its address is the module handle.
            module->ldrEntry = mem::AllocSystem(0x80);
            const uint32_t e = module->ldrEntry;
            for (uint32_t link = 0; link < 0x18; link += 8)
            {
                mem::Write32(e + link, e + link);
                mem::Write32(e + link + 4, e + link);
            }
            mem::Write32(e + 0x18, module->base);
            mem::Write32(e + 0x1C, module->base);
            mem::Write32(e + 0x20, module->size);
            WriteUnicodeString(e + 0x24, "game:\\" + module->path);
            WriteUnicodeString(e + 0x2C, module->name);
            mem::Write32(e + 0x3C, module->entry);
            mem::Write32(e + 0x58, module->xexHeader);

            ParseExports(*module);
            Log("loaded %-34s 0x%08X-0x%08X entry 0x%08X, %zu exports", module->name.c_str(), module->base,
                module->base + module->size, module->entry, module->exports.size());
            g_modules.push_back(std::move(module));
            return true;
        }
    }

    bool LoadModules(const std::filesystem::path& gameDir)
    {
        CreateKernelVariables();
        for (const char* path : kModulePaths)
        {
            if (!LoadModule(gameDir, path))
                return false;
        }
        for (auto& module : g_modules)
            ResolveImports(*module);

        Module* main = MainModule();
        mem::Write32(g_variables["XexExecutableModuleHandle"], main->ldrEntry);

        // TLS layout of the main executable: slot count, raw data, sizes.
        uint32_t tls = 0;
        if (GetXexHeaderField(*main, 0x00020104, tls))
            SetTlsInfo(mem::Read32(tls), mem::Read32(tls + 4), mem::Read32(tls + 8), mem::Read32(tls + 12));
        return true;
    }

    Module* MainModule() { return g_modules.empty() ? nullptr : g_modules.front().get(); }

    Module* FindModuleByName(const std::string& path)
    {
        std::string name = path;
        const size_t slash = name.find_last_of("\\/:");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        for (auto& module : g_modules)
        {
            if (_stricmp(module->name.c_str(), name.c_str()) == 0)
                return module.get();
        }
        return nullptr;
    }

    Module* FindModuleByHandle(uint32_t handle)
    {
        for (auto& module : g_modules)
        {
            if (module->ldrEntry == handle || module->base == handle)
                return module.get();
        }
        return nullptr;
    }

    Module* FindModuleByXexHeader(uint32_t xexHeader)
    {
        for (auto& module : g_modules)
        {
            if (module->xexHeader == xexHeader)
                return module.get();
        }
        return MainModule();
    }

    bool GetXexHeaderField(const Module& module, uint32_t key, uint32_t& result)
    {
        const uint8_t* h = module.headerBytes.data();
        const uint32_t count = BE32(h + 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t entryKey = BE32(h + 0x18 + i * 8);
            if (entryKey != key)
                continue;
            const uint32_t value = BE32(h + 0x1C + i * 8);
            switch (key & 0xFF)
            {
            case 0x00: result = value; break;                                        // inline value
            case 0x01: result = module.xexHeader + 0x1C + i * 8; break;              // pointer to the value
            default: result = module.xexHeader + value; break;                       // pointer into the header
            }
            return true;
        }
        return false;
    }

    uint32_t KernelVariable(const char* name)
    {
        auto it = g_variables.find(name);
        return it == g_variables.end() ? 0 : it->second;
    }

    void UpdateTimeStampBundle()
    {
        const uint32_t bundle = KernelVariable("KeTimeStampBundle");
        if (!bundle)
            return;
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const uint64_t system = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const uint64_t interrupt = uint64_t(GetTickCount64()) * 10000;
        *mem::Ptr<be<uint64_t>>(bundle) = interrupt;
        *mem::Ptr<be<uint64_t>>(bundle + 8) = system;
        mem::Write32(bundle + 16, uint32_t(GetTickCount64()));
    }
}
