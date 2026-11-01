// Exercises persisted preferences, remapping and the Xbox input ABI without
// booting the game or needing a connected controller.
#include "settings.h"
#include "input.h"
#include "kernel/kernel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mem { uint8_t* g_base = nullptr; }
namespace kernel {
bool g_traceImports = false;
void TraceEnter(const char*, const PPCContext&) {}
void TraceLeave(const char*, const PPCContext&) {}
}
PPC_FUNC(__imp__XamInputGetState);
PPC_FUNC(__imp__XamInputGetCapabilities);
PPC_FUNC(__imp__XamInputGetKeystrokeEx);

void Check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const auto path = std::filesystem::absolute(argv[1]);
        // A private test file. The caller uses a dedicated build-directory path.
        std::filesystem::remove(path);
        settings::Load(path);
        Check(settings::Get() == settings::Data{}, "missing file must use defaults");
        auto d = settings::Get();
        d.fullscreen = true; d.scale = 2; d.frameLimit = 30;
        d.presentation = 1; d.sensitivity = 175; d.showFps = true;
        settings::Set(d);
        Check(settings::Bind(settings::Start, 'X'), "valid key bind rejected");
        Check(settings::Get().keys[settings::X] == VK_RETURN, "duplicate bind must swap old assignment");
        Check(!settings::Bind(settings::Start, VK_F1), "settings shortcut must be reserved");
        const auto saved = settings::Get();
        Check(settings::Save(), "save failed");
        settings::Set(settings::Data{});
        settings::Load(path);
        Check(settings::Get() == saved, "preferences or key bindings were lost on reload");
        Check(WritePrivateProfileStringW(L"Graphics", L"Scale", L"99", path.c_str()), "test INI write failed");
        Check(WritePrivateProfileStringW(L"Graphics", L"FrameLimit", L"999", path.c_str()), "test INI write failed");
        Check(WritePrivateProfileStringW(L"Controls", L"Sensitivity", L"0", path.c_str()), "test INI write failed");
        settings::Load(path);
        Check(settings::Get().scale == 3 && settings::Get().frameLimit == 60 &&
              settings::Get().sensitivity == 25, "invalid persisted values weren't normalized");

        d = settings::Data{}; d.keys[settings::Start] = 'P';
        std::array<bool, 256> keys{};
        keys['P'] = true; keys[VK_SPACE] = true; keys['W'] = keys['D'] = true;
        keys['Z'] = true; keys[VK_LEFT] = true;
        const auto pad = pcinput::MapKeyboard(d, keys);
        Check((pad.buttons & 0x1014) == 0x1014, "remapped Start, A or D-pad wasn't delivered");
        Check(pad.leftTrigger == 255, "keyboard trigger wasn't delivered");
        Check(pad.leftX > 0 && pad.leftY > 0 && std::hypot(double(pad.leftX), double(pad.leftY)) <= 32768,
              "diagonal keyboard movement exceeded the analogue range");
        keys = {}; keys['A'] = keys['D'] = true;
        Check(pcinput::MapKeyboard(d, keys).leftX == 0, "opposing directions must cancel");

        settings::Set(settings::Data{});
        pcinput::SetBlocked(true);
        pcinput::HandleKey(VK_RETURN, true);
        pcinput::State state;
        Check(pcinput::GetState(0, state) == 0 && state.pad.buttons == 0,
              "the settings panel must block game input");
        Check(pcinput::GetState(4, state) == 1167, "invalid controller index must be disconnected");

        auto memory = std::make_unique<uint8_t[]>(0x40000);
        mem::g_base = memory.get();
        constexpr uint32_t output = 0x10000, user = 0x20000;
        PPCContext ctx{};
        ctx.r3.u32 = 0; ctx.r4.u32 = 0; ctx.r5.u32 = output;
        __imp__XamInputGetCapabilities(ctx, mem::g_base);
        Check(ctx.r3.u32 == 0 && mem::g_base[output] == 1, "keyboard controller capabilities missing");
        Check(mem::g_base[output + 4] == 0xF3 && mem::g_base[output + 5] == 0xFF &&
              mem::g_base[output + 8] == 0x7F && mem::g_base[output + 9] == 0xFF,
              "controller capabilities must use Xbox big-endian layout");
        std::fill_n(mem::g_base + output, 16, uint8_t(0xCC));
        ctx.r3.u32 = 4; ctx.r4.u32 = 0; ctx.r5.u32 = output;
        __imp__XamInputGetState(ctx, mem::g_base);
        Check(ctx.r3.u32 == 1167 && std::all_of(mem::g_base + output, mem::g_base + output + 16,
                                              [](uint8_t v) { return v == 0; }),
              "failed state queries must clear their output");
        mem::Write32(user, 0xFF);
        ctx.r3.u32 = user; ctx.r4.u32 = 1; ctx.r5.u32 = output;
        __imp__XamInputGetKeystrokeEx(ctx, mem::g_base);
        Check(ctx.r3.u32 == 4306, "empty event queue must return ERROR_EMPTY");
        std::filesystem::remove(path);
        puts("Preferences, remapping, blocked input and Xbox input ABI: passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
