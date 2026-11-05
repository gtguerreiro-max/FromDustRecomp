#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#include "apu/apu.h"
#include "gpu/gpu.h"
#include "gpu/xenia_bridge.h"
#include "gpu/native/live_audit.h"
#include "kernel/debug.h"
#include "kernel/kernel.h"
#include "kernel/loader.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "input.h"
#include "memory.h"
#include "settings.h"
#ifdef FD_NATIVE_RENDERER
#include "gpu/native/renderer.h"
#include "gpu/native/agility_exports.h"
#include "gpu/native/shader_capabilities.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace kernel;

namespace kernel::selftest { bool RunSave(); }

namespace
{
    // Like Xenia's main_win.cc: without this every short Sleep (GPU
    // WAIT_REG_MEM, guest KeDelayExecutionThread) lasts a 15.6 ms tick.
    void RaiseTimerResolution()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        using Query = LONG(NTAPI*)(PULONG, PULONG, PULONG);
        using Set = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
        auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryTimerResolution"));
        auto set = reinterpret_cast<Set>(GetProcAddress(ntdll, "NtSetTimerResolution"));
        if (!query || !set)
            return;
        ULONG minimum, maximum, current;
        query(&minimum, &maximum, &current);
        set(maximum, TRUE, &current);
        kernel::Log("timer resolution: %.2f ms", current / 10000.0);
    }
}

int main(int argc, char** argv)
{
#ifdef FD_NATIVE_RENDERER
#ifdef FD_XENIA_GPU
    if (argc > 1 && !strcmp(argv[1], "--native-shader-capabilities"))
        return argc == 4 ? gpu::native::SurveyShaderCapabilities(argv[2], argv[3]) : 2;
#endif
    // Runs the shared native core inside this executable before game workers
    // start. Live guest draw hooks are connected in a subsequent integration.
    if (argc > 1 && !strcmp(argv[1], "--native-replay"))
    {
        if (argc < 3) return 2;
        gpu::native::Renderer renderer;
        if (const char* catalog = getenv("FD_NATIVE_SHADER_CATALOG"); catalog && *catalog)
            if (!renderer.LoadShaderCatalog(catalog)) return 1;
        for (int index = 2; index < argc; ++index)
            if (const int result = renderer.RenderReplay(argv[index])) return result;
        printf("FromDust native diagnostic: %llu frames, %llu game draws, one device\n",
            static_cast<unsigned long long>(renderer.CompletedFrames()),
            static_cast<unsigned long long>(renderer.CompletedDraws()));
        return 0;
    }
#endif
    // Game files: first argument, or "game" next to the executable.
    std::filesystem::path exeDir = std::filesystem::absolute(argv[0]).parent_path();
    std::filesystem::path gameDir = argc > 1 ? std::filesystem::path(argv[1]) : exeDir / "game";
    if (argc == 1 && !std::filesystem::exists(gameDir / "Default.xex"))
        gameDir = exeDir.parent_path() / "game";
    const char* settingsFile = getenv("FD_SETTINGS_FILE");
    settings::Load(settingsFile && *settingsFile ? std::filesystem::path(settingsFile) : exeDir / "user" / "settings.ini");
    if (!std::filesystem::exists(gameDir / "Default.xex"))
    {
        fprintf(stderr, "Default.xex not found in %s\nUsage: FromDust.exe <game folder>\n", gameDir.string().c_str());
        return 1;
    }

    RaiseTimerResolution();
    if (!mem::Init() || !InitFunctionTable())
        return 1;
    debug::InstallCrashHandler();

    vfs::SetGameDirectory(gameDir);
    vfs::SetUserDirectory(exeDir / "user");
    if (const char* test = getenv("FD_SELFTEST"); test && !strcmp(test, "save"))
        return selftest::RunSave() ? 0 : 1;
    if (!gpu::native::audit::Initialize() || !LoadModules(gameDir))
        return 1;

    // KeTimeStampBundle is read directly by guest code.
    std::thread([] {
        for (;;)
        {
            UpdateTimeStampBundle();
            Sleep(1);
        }
    }).detach();

    if (!gpu::xenia::Initialize()) {
        Log("gpu: Direct3D 12 initialization failed; see FromDust-GPU.log");
        // GPU workers may have started already. Don't destroy their shared
        // state before the process tears the workers down.
        ExitProcess(1);
    }
    gpu::Start();
    apu::Initialize();

    if (const char* watchdog = getenv("FD_WATCHDOG"); watchdog && atoi(watchdog) > 0)
        debug::StartWatchdog(unsigned(atoi(watchdog)));
    // Bounded diagnostic runs launched by capture tools, without an external
    // process killer. Leave enough time after FD_FRAME_CAPTURE for readbacks.
    if (const char* timeout = getenv("FD_EXIT_SECONDS"); timeout && atoi(timeout) > 0)
        std::thread([seconds = unsigned(atoi(timeout))] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            ExitProcess(gpu::native::audit::Finish() ? 0 : 1);
        }).detach();
    // FD_AUTOPRESS=<seconds>:<hex buttons>[:<ms>[:<LT>:<RT>[:<LX>:<LY>[:<RX>:<RY>]]]][,...] drives pad 0,
    // e.g. "40:0010" = Start, "160:0:4000:255:0" = hold LT for 4 s, "170:0:2000:0:0:20000:0" = left stick right.
    if (const char* press = getenv("FD_AUTOPRESS"); press && *press)
    {
        for (const char* p = press; p && *p; p = strchr(p, ','), p = p ? p + 1 : nullptr)
        {
            unsigned seconds = 0, buttons = 0, ms = 300, lt = 0, rt = 0;
            int lx = 0, ly = 0, rx = 0, ry = 0;
            if (sscanf(p, "%u:%x:%u:%u:%u:%d:%d:%d:%d", &seconds, &buttons, &ms, &lt, &rt, &lx, &ly, &rx, &ry) >= 2)
            {
                pcinput::Gamepad pad;
                pad.buttons = uint16_t(buttons);
                pad.leftTrigger = uint8_t(std::min(lt, 255u));
                pad.rightTrigger = uint8_t(std::min(rt, 255u));
                pad.leftX = int16_t(lx);
                pad.leftY = int16_t(ly);
                pad.rightX = int16_t(rx);
                pad.rightY = int16_t(ry);
                pcinput::SchedulePad(pad, seconds * 1000, ms);
            }
        }
    }
    if (const char* profile = getenv("FD_PROFILE"); profile && atoi(profile) > 0)
        debug::StartProfiler(unsigned(atoi(profile)));

    Module* main = MainModule();
    if (gpu::xenia::HasWindow()) {
        std::thread([main] { RunMainThread(main->entry, main->stackSize); }).detach();
        const int result = gpu::xenia::RunMessageLoop();
        ExitProcess(gpu::native::audit::Finish() ? result : 1);
    }
    RunMainThread(main->entry, main->stackSize);
    return gpu::native::audit::Finish() ? 0 : 1;
}
