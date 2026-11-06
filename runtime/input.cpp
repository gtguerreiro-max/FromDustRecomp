#include "input.h"
#include "settings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
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
// PC-style mouse: the cursor is captured (hidden, kept at the window centre)
// while the game has focus and the settings are closed; its movement drives
// the Breath, a held button turns the camera and the wheel zooms.
std::atomic<int> g_wheel{0};          // pending wheel notches (WHEEL_DELTA units)
float g_breathX = 0, g_breathY = 0;   // smoothed mouse stick
ULONGLONG g_wheelUntil = 0;
int g_wheelDirection = 0;
std::atomic<bool> g_captured{false}; // the settings UI hides the cursor while set
void Capture(HWND, bool capture) {
    if (capture == g_captured.exchange(capture)) return;
    if (!capture) { ClipCursor(nullptr); g_mouseValid = false; }
}
bool WindowCentre(HWND window, POINT& centre, RECT& screen) {
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top) return false;
    POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
    ClientToScreen(window, &topLeft); ClientToScreen(window, &bottomRight);
    screen = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    centre = {(topLeft.x + bottomRight.x) / 2, (topLeft.y + bottomRight.y) / 2};
    return true;
}
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
const ULONGLONG g_inputStart = GetTickCount64();
std::atomic<bool> g_lastKeyboard{true};
std::atomic<bool (*)()> g_pointerActive{nullptr};
std::atomic<void (*)(uint16_t)> g_buttonObserver{nullptr};
std::atomic<bool (*)()> g_wheelZoomActive{nullptr};
std::atomic<void (*)(float)> g_wheelZoom{nullptr};
float g_grabX = 0, g_grabY = 0;      // pending grabbed mouse movement (pixels)
float g_grabWidth = 1, g_grabHeight = 1;
ULONGLONG g_grabTick = 0;
bool PointerActive() { auto query = g_pointerActive.load(); return query && query(); }
std::atomic<uint32_t> g_promptGeneration{1};
uint64_t g_promptSignature = 0;
void UpdatePrompts(const settings::Data& d) {
    const bool keyboard = d.inputMode == 1 || (d.inputMode == 0 && g_lastKeyboard.load());
    uint64_t signature = 1469598103934665603ull;
    auto mix = [&](uint64_t value) { signature = (signature ^ value) * 1099511628211ull; };
    mix(keyboard); mix(d.mouseButtons); mix(d.cameraButton); mix(d.mouseBreath);
    for (auto key : d.keys) mix(key);
    if (signature != g_promptSignature) { g_promptSignature = signature; ++g_promptGeneration; }
}
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
void HandleWheel(int delta) { g_wheel += delta; }
bool MouseCaptured() { return g_captured.load(); }
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
            // A pressed button, a pulled trigger or a moved stick makes the controller the prompt device.
            if (state.pad.buttons || state.pad.leftTrigger > 40 || state.pad.rightTrigger > 40 ||
                state.pad.leftX || state.pad.leftY || state.pad.rightX || state.pad.rightY) g_lastKeyboard = false;
        }
        if (keyboard) {
            std::array<bool, 256> keys{};
            for (size_t i = 0; i < keys.size(); ++i) {
                const bool latched = g_pressed[i].exchange(false);
                keys[i] = g_down[i].load() || latched;
                if (keys[i]) g_lastKeyboard = true;
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
            HWND window = g_window.load();
            const bool pcMouse = d.mouseBreath || d.mouseButtons || d.cameraButton || d.wheelZoom;
            const int cameraKey = d.cameraButton == 1 ? VK_RBUTTON : d.cameraButton == 2 ? VK_MBUTTON : 0;
            const bool turning = cameraKey && (GetAsyncKeyState(cameraKey) & 0x8000);
            const bool pointing = d.mouseBreath && PointerActive();
            const bool absolute = pointing && MouseAbsolute();
            if (pcMouse && d.mouseBreath && !absolute) Capture(window, true);
            if (absolute) {
                // Visible cursor kept inside the window; the Breath follows it.
                if (g_captured) Capture(window, false);
                POINT centre{}; RECT screen{}; RECT current{};
                if (WindowCentre(window, centre, screen) && (!GetClipCursor(&current) || current.left != screen.left ||
                    current.top != screen.top || current.right != screen.right || current.bottom != screen.bottom))
                    ClipCursor(&screen);
            }
            if (GetCursorPos(&mouse)) {
                const float elapsed = float(std::max<ULONGLONG>(1, now - g_mouseTick));
                float dx = g_mouseValid ? float(mouse.x - g_mouse.x) : 0.0f;
                float dy = g_mouseValid ? float(mouse.y - g_mouse.y) : 0.0f;
                // A jump of more than a third of the screen in one poll is the
                // cursor being clipped / warped, not a hand movement.
                if (std::abs(dx) > 600 || std::abs(dy) > 400) dx = dy = 0;
                if (std::abs(dx) >= 3 || std::abs(dy) >= 3) g_lastKeyboard = true;
                // Mouse rotation: F1 > Controls > Rotation speed / invert.
                const float rotate = 40000.0f * (d.mouseRotateSpeed / 100.0f) / elapsed;
                const float rotateX = dx * rotate * (d.mouseRotateInvertX ? -1.0f : 1.0f);
                const float rotateY = -dy * rotate * (d.mouseRotateInvertY ? -1.0f : 1.0f);
                if (turning) {
                    state.pad.rightX = Axis(state.pad.rightX + rotateX);
                    state.pad.rightY = Axis(state.pad.rightY + rotateY);
                } else if (!d.mouseBreath && d.mouseCamera && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
                    // Legacy mode: right-button drag turns the camera.
                    state.pad.rightX = Axis(state.pad.rightX + rotateX);
                    state.pad.rightY = Axis(state.pad.rightY + rotateY);
                }
                if (pointing && !absolute && !turning) {
                    // Grabbed: the cursor update hook moves the Breath by this much on screen.
                    g_grabX += dx; g_grabY += dy;
                    RECT client{};
                    if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
                        g_grabWidth = float(client.right); g_grabHeight = float(client.bottom);
                    }
                }
                if (d.mouseBreath && !turning && !pointing) {
                    // Mouse speed (px/ms) -> left stick; ~0.8 px/ms is full deflection at 100 %.
                    const float gain = 32767.0f / 0.8f * (d.mouseSpeed / 100.0f) / elapsed;
                    g_breathX = g_breathX * 0.35f + dx * gain * 0.65f;
                    g_breathY = g_breathY * 0.35f - dy * gain * 0.65f;
                    if (std::fabs(dx) < 0.5f && std::fabs(dy) < 0.5f) { g_breathX *= 0.5f; g_breathY *= 0.5f; }
                    state.pad.leftX = Axis(state.pad.leftX + g_breathX);
                    state.pad.leftY = Axis(state.pad.leftY + g_breathY);
                } else { g_breathX = g_breathY = 0; }
                g_mouse = mouse; g_mouseTick = now; g_mouseValid = true;
                // Keep the hidden cursor inside the window so movement never stops at an edge.
                POINT centre{}; RECT screen{};
                if (g_captured && WindowCentre(window, centre, screen)) {
                    RECT current{};
                    if (!GetClipCursor(&current) || current.left != screen.left || current.top != screen.top ||
                        current.right != screen.right || current.bottom != screen.bottom) {
                        // Clipping (re)starts: the OS moves the cursor inside;
                        // the next poll measures from the new position.
                        ClipCursor(&screen);
                        SetCursorPos(centre.x, centre.y);
                        g_mouse = centre;
                    }
                    if (std::abs(mouse.x - centre.x) > (screen.right - screen.left) / 4 ||
                        std::abs(mouse.y - centre.y) > (screen.bottom - screen.top) / 4) {
                        SetCursorPos(centre.x, centre.y);
                        g_mouse = centre;
                    }
                }
            }
            if (d.mouseButtons) {
                if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) { state.pad.leftTrigger = 255; g_lastKeyboard = true; }
                if (cameraKey != VK_RBUTTON && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) state.pad.rightTrigger = 255;
            }
            // Each wheel notch holds the camera's vertical axis (zoom) briefly.
            // In the global view the wheel zooms it straight in and out
            // (camera_assist.cpp) instead of the stick's zoom, which falls back
            // to the close camera.
            const auto zoomActive = g_wheelZoomActive.load();
            const auto zoom = g_wheelZoom.load();
            if (d.wheelZoom && zoomActive && zoom && zoomActive()) {
                // Fractions of a notch too (high-resolution wheels).
                if (const int wheel = g_wheel.exchange(0))
                    zoom(float(wheel) / WHEEL_DELTA * (d.wheelZoom == 2 ? -1.0f : 1.0f));
            }
            if (const int wheel = g_wheel.exchange(0); wheel && d.wheelZoom) {
                const int direction = (wheel > 0 ? 1 : -1) * (d.wheelZoom == 2 ? -1 : 1);
                // 220 ms of full deflection per notch; notches in the same
                // direction accumulate (up to ~1.3 s), a reversal restarts.
                const ULONGLONG start = direction == g_wheelDirection && g_wheelUntil > now ? g_wheelUntil : now;
                g_wheelDirection = direction;
                g_wheelUntil = std::min(now + 1300, start + ULONGLONG(220 * std::min(6, std::max(1, std::abs(wheel) / WHEEL_DELTA))));
            }
            if (now < g_wheelUntil) state.pad.rightY = Axis(state.pad.rightY + g_wheelDirection * 32767.0f);
        }
        state.pad.rightX = Axis(state.pad.rightX * (d.sensitivity / 100.0f));
        state.pad.rightY = Axis(state.pad.rightY * (d.sensitivity / 100.0f) * (d.invertY ? -1.0f : 1.0f));
        Deadzone(state.pad.leftX, state.pad.leftY, 0);
        Deadzone(state.pad.rightX, state.pad.rightY, 0);
    } else {
        ClearKeys();
        g_mouseValid = false;
        g_breathX = g_breathY = 0;
        g_wheel = 0;
        if (g_captured) Capture(g_window.load(), false);
    }
    if (user == 0) UpdatePrompts(d);
    if (user == 0) if (auto observer = g_buttonObserver.load()) observer(state.pad.buttons);
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
void SetPointerQuery(bool (*active)()) { g_pointerActive = active; }
void SetButtonObserver(void (*observer)(uint16_t buttons)) { g_buttonObserver = observer; }
void SetWheelZoom(bool (*active)(), void (*zoom)(float notches)) { g_wheelZoomActive = active; g_wheelZoom = zoom; }
void WindowAspect(float& width, float& height) {
    std::lock_guard lock(g_pollMutex);
    if (g_grabWidth > 1 && g_grabHeight > 1) { width = g_grabWidth; height = g_grabHeight; }
}
bool MouseAbsolute() { static const bool absolute = getenv("FD_MOUSE_ABSOLUTE") != nullptr; return absolute; }
bool TakeMouseDelta(float& dx, float& dy) {
    std::lock_guard lock(g_pollMutex);
    dx = g_grabX / g_grabWidth; dy = g_grabY / g_grabHeight;
    g_grabX = g_grabY = 0;
    // Movement collected while the cursor update did not run (menus, cut
    // scenes) is dropped rather than applied as one jump.
    if (g_grabTick && GetTickCount64() - g_grabTick > 250) dx = dy = 0;
    // FD_MOUSE_MOVES=<s>:<vx>,<vy>;...: diagnostic scripted grabbed movement
    // (window fractions per second) from those times after start.
    static const std::vector<std::array<float, 3>> scripted = [] {
        std::vector<std::array<float, 3>> moves;
        if (const char* text = getenv("FD_MOUSE_MOVES"))
            for (const char* p = text; *p;) {
                float t, vx, vy;
                if (sscanf(p, "%f:%f,%f", &t, &vx, &vy) == 3) moves.push_back({t, vx, vy});
                const char* next = strchr(p, ';');
                if (!next) break;
                p = next + 1;
            }
        return moves;
    }();
    const ULONGLONG now = GetTickCount64();
    if (!scripted.empty() && g_grabTick) {
        const float t = (now - g_inputStart) / 1000.0f, step = (now - g_grabTick) / 1000.0f;
        const std::array<float, 3>* current = nullptr;
        for (const auto& move : scripted) if (t >= move[0]) current = &move;
        if (current) { dx += (*current)[1] * step; dy += (*current)[2] * step; }
    }
    g_grabTick = now;
    return true;
}
bool MousePosition(float& x, float& y) {
    // FD_MOUSE_POINTS=<s>:<x>,<y>;...: diagnostic scripted mouse positions
    // (fractions of the window) from those times after start.
    static const std::vector<std::array<float, 3>> scripted = [] {
        std::vector<std::array<float, 3>> points;
        if (const char* text = getenv("FD_MOUSE_POINTS"))
            for (const char* p = text; *p;) {
                float t, px, py;
                if (sscanf(p, "%f:%f,%f", &t, &px, &py) == 3) points.push_back({t, px, py});
                const char* next = strchr(p, ';');
                if (!next) break;
                p = next + 1;
            }
        return points;
    }();
    if (!scripted.empty()) {
        const float now = (GetTickCount64() - g_inputStart) / 1000.0f;
        const std::array<float, 3>* current = nullptr;
        for (const auto& point : scripted) if (now >= point[0]) current = &point;
        if (current) { x = (*current)[1]; y = (*current)[2]; return true; }
    }
    HWND window = g_window.load();
    if (!window || GetForegroundWindow() != window || Blocked()) return false;
    POINT mouse{}; RECT client{};
    if (!GetCursorPos(&mouse) || !ScreenToClient(window, &mouse) || !GetClientRect(window, &client)) return false;
    if (client.right <= 0 || client.bottom <= 0 || mouse.x < 0 || mouse.y < 0 || mouse.x >= client.right || mouse.y >= client.bottom) return false;
    x = (mouse.x + 0.5f) / client.right; y = (mouse.y + 0.5f) / client.bottom;
    return true;
}
bool KeyboardPrompts() {
    const int mode = settings::Get().inputMode;
    return mode == 1 || (mode == 0 && g_lastKeyboard.load());
}
uint32_t PromptGeneration() { return g_promptGeneration.load(); }
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
