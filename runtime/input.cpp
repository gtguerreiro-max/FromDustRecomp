#include "input.h"
#include "settings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>

namespace pcinput {
namespace {
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_blocked{false};
std::array<std::atomic<bool>, 256> g_down{}, g_pressed{};
std::mutex g_pollMutex;
std::array<State, 4> g_previous{};
POINT g_mouse{};
bool g_mouseValid = false;
ULONGLONG g_mouseTick = 0;
bool Focused() {
    HWND window = g_window.load();
    return window && GetForegroundWindow() == window;
}
int16_t Axis(float value) { return int16_t(std::clamp(value, -32767.0f, 32767.0f)); }
void Deadzone(int16_t& x, int16_t& y, int percent) {
    const float distance = std::hypot(float(x), float(y));
    const float threshold = 32767.0f * percent / 100.0f;
    if (distance <= threshold) { x = y = 0; return; }
    const float factor = std::min(32767.0f, (distance - threshold) * 32767.0f /
                                 (32767.0f - threshold)) / distance;
    x = Axis(x * factor); y = Axis(y * factor);
}
// XInputGetState/GetCapabilities on an empty port re-enumerate devices and
// cost milliseconds; the game polls every port each frame. Empty ports are
// only re-checked once per second (hot-plugging still works).
std::array<std::atomic<ULONGLONG>, 4> g_emptyUntil{};
struct ScriptedPress { Gamepad pad; ULONGLONG from, until; };
std::mutex g_scriptMutex;
std::vector<ScriptedPress> g_script;
DWORD CachedXInputGetState(DWORD user, XINPUT_STATE* state) {
    if (GetTickCount64() < g_emptyUntil[user].load()) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = XInputGetState(user, state);
    if (result == ERROR_DEVICE_NOT_CONNECTED) g_emptyUntil[user] = GetTickCount64() + 1000;
    return result;
}
DWORD CachedXInputGetCapabilities(DWORD user, XINPUT_CAPABILITIES* caps) {
    if (GetTickCount64() < g_emptyUntil[user].load()) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = XInputGetCapabilities(user, 0, caps);
    if (result == ERROR_DEVICE_NOT_CONNECTED) g_emptyUntil[user] = GetTickCount64() + 1000;
    return result;
}
}
void SetWindow(void* hwnd) { g_window = static_cast<HWND>(hwnd); }
void HandleKey(uint32_t key, bool down) {
    if (key >= 256) return;
    if (down && !g_down[key].exchange(true)) g_pressed[key] = true;
    if (!down) g_down[key] = false;
}
void ClearKeys() {
    for (size_t i = 0; i < g_down.size(); ++i) { g_down[i] = false; g_pressed[i] = false; }
}
void SetBlocked(bool blocked) {
    if (g_blocked.exchange(blocked) == blocked) return;
    ClearKeys();
    if (blocked) {
        XINPUT_VIBRATION zero{};
        for (DWORD user = 0; user < 4; ++user) XInputSetState(user, &zero);
    }
}
bool Blocked() { return g_blocked.load(); }
Gamepad MapKeyboard(const settings::Data& d, const std::array<bool, 256>& keys) {
    Gamepad pad;
    auto held = [&](settings::Action action) { return keys[d.keys[action]]; };
    constexpr uint16_t buttons[] = {0x10, 0x20, 0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200};
    for (int i = 0; i < 8; ++i) if (held(settings::Action(i))) pad.buttons |= buttons[i];
    pad.leftTrigger = held(settings::LeftTrigger) ? 255 : 0;
    pad.rightTrigger = held(settings::RightTrigger) ? 255 : 0;
    for (int i = 0; i < 4; ++i) if (held(settings::Action(settings::DpadUp + i))) pad.buttons |= uint16_t(1 << i);
    pad.leftX = int16_t((int(held(settings::MoveRight)) - int(held(settings::MoveLeft))) * 32767);
    pad.leftY = int16_t((int(held(settings::MoveUp)) - int(held(settings::MoveDown))) * 32767);
    pad.rightX = int16_t((int(held(settings::LookRight)) - int(held(settings::LookLeft))) * 32767);
    pad.rightY = int16_t((int(held(settings::LookUp)) - int(held(settings::LookDown))) * 32767);
    Deadzone(pad.leftX, pad.leftY, 0);
    Deadzone(pad.rightX, pad.rightY, 0);
    return pad;
}
uint32_t GetCapabilities(uint32_t user, bool& vibration) {
    vibration = false;
    if (user >= 4) return ERROR_DEVICE_NOT_CONNECTED;
    auto d = settings::Get();
    if (d.inputMode == 1) return user == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    XINPUT_CAPABILITIES caps{};
    const DWORD result = CachedXInputGetCapabilities(user, &caps);
    vibration = result == ERROR_SUCCESS && d.vibration;
    return user == 0 && d.inputMode == 0 ? ERROR_SUCCESS : result;
}
uint32_t GetState(uint32_t user, State& state) {
    state = {};
    if (user >= 4) return ERROR_DEVICE_NOT_CONNECTED;
    const auto d = settings::Get();
    XINPUT_STATE native{};
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (d.inputMode != 1) result = CachedXInputGetState(user, &native);
    const bool keyboard = user == 0 && d.inputMode != 2;
    if (!keyboard && result != ERROR_SUCCESS) return result;
    std::lock_guard lock(g_pollMutex);
    if (Focused() && !Blocked()) {
        if (result == ERROR_SUCCESS) {
            auto& p = native.Gamepad;
            state.pad = {p.wButtons, p.bLeftTrigger, p.bRightTrigger, p.sThumbLX, p.sThumbLY, p.sThumbRX, p.sThumbRY};
            Deadzone(state.pad.leftX, state.pad.leftY, d.deadzone);
            Deadzone(state.pad.rightX, state.pad.rightY, d.deadzone);
        }
        if (keyboard) {
            std::array<bool, 256> keys{};
            for (size_t i = 0; i < keys.size(); ++i) {
                const bool latched = g_pressed[i].exchange(false);
                keys[i] = g_down[i].load() || latched;
            }
            const auto mapped = MapKeyboard(d, keys);
            state.pad.buttons |= mapped.buttons;
            state.pad.leftTrigger = std::max(state.pad.leftTrigger, mapped.leftTrigger);
            state.pad.rightTrigger = std::max(state.pad.rightTrigger, mapped.rightTrigger);
            state.pad.leftX = Axis(float(state.pad.leftX) + mapped.leftX);
            state.pad.leftY = Axis(float(state.pad.leftY) + mapped.leftY);
            state.pad.rightX = Axis(float(state.pad.rightX) + mapped.rightX);
            state.pad.rightY = Axis(float(state.pad.rightY) + mapped.rightY);
            POINT mouse{};
            const ULONGLONG now = GetTickCount64();
            if (GetCursorPos(&mouse)) {
                if (d.mouseCamera && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) && g_mouseValid) {
                    const float elapsed = float(std::max<ULONGLONG>(1, now - g_mouseTick));
                    state.pad.rightX = Axis(state.pad.rightX + (mouse.x - g_mouse.x) * 40000.0f / elapsed);
                    state.pad.rightY = Axis(state.pad.rightY - (mouse.y - g_mouse.y) * 40000.0f / elapsed);
                }
                g_mouse = mouse; g_mouseTick = now; g_mouseValid = true;
            }
        }
        state.pad.rightX = Axis(state.pad.rightX * (d.sensitivity / 100.0f));
        state.pad.rightY = Axis(state.pad.rightY * (d.sensitivity / 100.0f) * (d.invertY ? -1.0f : 1.0f));
        Deadzone(state.pad.leftX, state.pad.leftY, 0);
        Deadzone(state.pad.rightX, state.pad.rightY, 0);
    } else {
        ClearKeys();
        g_mouseValid = false;
    }
    // Scripted presses for unattended diagnostic runs (see SchedulePad).
    if (user == 0) {
        std::lock_guard scriptLock(g_scriptMutex);
        const ULONGLONG now = GetTickCount64();
        for (const auto& press : g_script) {
            if (now < press.from || now >= press.until) continue;
            state.pad.buttons |= press.pad.buttons;
            state.pad.leftTrigger = std::max(state.pad.leftTrigger, press.pad.leftTrigger);
            state.pad.rightTrigger = std::max(state.pad.rightTrigger, press.pad.rightTrigger);
            if (press.pad.leftX || press.pad.leftY) { state.pad.leftX = press.pad.leftX; state.pad.leftY = press.pad.leftY; }
            if (press.pad.rightX || press.pad.rightY) { state.pad.rightX = press.pad.rightX; state.pad.rightY = press.pad.rightY; }
        }
    }
    auto& previous = g_previous[user];
    state.packet = previous.packet + (memcmp(&state.pad, &previous.pad, sizeof(Gamepad)) != 0);
    previous = state;
    return ERROR_SUCCESS;
}
void SchedulePad(const Gamepad& pad, uint32_t delayMs, uint32_t durationMs) {
    const ULONGLONG from = GetTickCount64() + delayMs;
    std::lock_guard lock(g_scriptMutex);
    g_script.push_back({pad, from, from + durationMs});
}
uint32_t SetVibration(uint32_t user, uint16_t left, uint16_t right) {
    if (user >= 4) return ERROR_DEVICE_NOT_CONNECTED;
    if (!settings::Get().vibration || Blocked() || !Focused()) left = right = 0;
    XINPUT_VIBRATION vibration{left, right};
    const DWORD result = XInputSetState(user, &vibration);
    return user == 0 && settings::Get().inputMode != 2 ? ERROR_SUCCESS : result;
}
}
