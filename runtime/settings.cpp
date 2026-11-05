#include "settings.h"
#include <algorithm>
#include <mutex>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace settings {
namespace {
std::mutex g_mutex;
Data g_data;
std::filesystem::path g_path;
constexpr const wchar_t* kKeyNames[] = {
    L"Start", L"Back", L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT",
    L"MoveUp", L"MoveDown", L"MoveLeft", L"MoveRight", L"LookUp", L"LookDown",
    L"LookLeft", L"LookRight", L"DpadUp", L"DpadDown", L"DpadLeft", L"DpadRight"
};
void Normalize(Data& d) {
    d.windowPreset = std::clamp(d.windowPreset, 0, int(std::size(kWindowWidths))-1);
    d.uiScale = std::clamp(d.uiScale, 75, 150);
    d.hudCorner = std::clamp(d.hudCorner, 0, 3);
    d.scale = std::clamp(d.scale, 1, 3);
    if (d.frameLimit != 0 && d.frameLimit != 30 && d.frameLimit != 60 && d.frameLimit != 120)
        d.frameLimit = 60;
    if (d.anisotropy != -1 && (d.anisotropy < 2 || d.anisotropy > 5)) d.anisotropy = -1;
    d.presentation = std::clamp(d.presentation, 0, 2);
    d.sharpness = std::clamp(d.sharpness, 0, 100);
    d.upscaler = std::clamp(d.upscaler, 0, 1);
    d.frameGeneration = std::clamp(d.frameGeneration, 0, 3);
    d.mouseSpeed = std::clamp(d.mouseSpeed, 25, 300);
    d.cameraButton = std::clamp(d.cameraButton, 0, 2);
    d.wheelZoom = std::clamp(d.wheelZoom, 0, 2);
    d.inputMode = std::clamp(d.inputMode, 0, 2);
    d.sensitivity = std::clamp(d.sensitivity, 25, 300);
    d.deadzone = std::clamp(d.deadzone, 0, 30);
    d.volume = std::clamp(d.volume, 0, 100);
    for (size_t i = 0; i < d.keys.size(); ++i) {
        if (!d.keys[i] || d.keys[i] > 0xFE || d.keys[i] == VK_F1 || d.keys[i] == VK_F11 || d.keys[i] == VK_ESCAPE)
            d.keys[i] = kDefaultKeys[i];
    }
}
}
void Load(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    g_path = std::filesystem::absolute(path);
    Data d;
    auto read = [&](const wchar_t* section, const wchar_t* key, int fallback) {
        return int(GetPrivateProfileIntW(section, key, fallback, g_path.c_str()));
    };
    d.fullscreen = read(L"Display", L"Fullscreen", d.fullscreen) != 0;
    d.windowPreset = read(L"Display", L"WindowPreset", d.windowPreset);
    d.showFps = read(L"Display", L"ShowFPS", d.showFps) != 0;
    d.showFrameTime = read(L"Interface", L"ShowFrameTime", d.showFrameTime) != 0;
    d.uiScale = read(L"Interface", L"Scale", d.uiScale);
    d.hudCorner = read(L"Interface", L"HudCorner", d.hudCorner);
    d.scale = read(L"Graphics", L"Scale", d.scale);
    d.vsync = read(L"Graphics", L"VSync", d.vsync) != 0;
    d.frameLimit = read(L"Graphics", L"FrameLimit", d.frameLimit);
    d.anisotropy = read(L"Graphics", L"Anisotropy", d.anisotropy);
    d.presentation = read(L"Graphics", L"Presentation", d.presentation);
    d.sharpness = read(L"Graphics", L"Sharpness", d.sharpness);
    d.upscaler = read(L"Graphics", L"Upscaler", d.upscaler);
    d.frameGeneration = read(L"Graphics", L"FrameGeneration", d.frameGeneration);
    d.inputMode = read(L"Controls", L"Mode", d.inputMode);
    d.sensitivity = read(L"Controls", L"Sensitivity", d.sensitivity);
    d.deadzone = read(L"Controls", L"Deadzone", d.deadzone);
    d.invertY = read(L"Controls", L"InvertY", d.invertY) != 0;
    d.mouseCamera = read(L"Controls", L"MouseCamera", d.mouseCamera) != 0;
    d.mouseBreath = read(L"Controls", L"MouseBreath", d.mouseBreath) != 0;
    d.mouseSpeed = read(L"Controls", L"MouseSpeed", d.mouseSpeed);
    d.mouseButtons = read(L"Controls", L"MouseButtons", d.mouseButtons) != 0;
    d.cameraButton = read(L"Controls", L"CameraButton", d.cameraButton);
    d.wheelZoom = read(L"Controls", L"WheelZoom", d.wheelZoom);
    d.vibration = read(L"Controls", L"Vibration", d.vibration) != 0;
    d.volume = read(L"Audio", L"Volume", d.volume);
    d.muteUnfocused = read(L"Audio", L"MuteUnfocused", d.muteUnfocused) != 0;
    for (int i = 0; i < ActionCount; ++i) d.keys[i] = uint16_t(read(L"Keyboard", kKeyNames[i], d.keys[i]));
    Normalize(d);
    g_data = d;
}
Data Get() { std::lock_guard lock(g_mutex); return g_data; }
void Set(Data value) { Normalize(value); std::lock_guard lock(g_mutex); g_data = value; }
std::filesystem::path Path() { std::lock_guard lock(g_mutex); return g_path; }
bool Save() {
    std::lock_guard lock(g_mutex);
    std::error_code error;
    std::filesystem::create_directories(g_path.parent_path(), error);
    if (error) return false;
    bool ok = true;
    auto write = [&](const wchar_t* section, const wchar_t* key, int value) {
        const auto text = std::to_wstring(value);
        if (!WritePrivateProfileStringW(section, key, text.c_str(), g_path.c_str())) ok = false;
    };
    const Data& d = g_data;
    write(L"Display", L"Fullscreen", d.fullscreen);
    write(L"Display", L"WindowPreset", d.windowPreset);
    write(L"Display", L"ShowFPS", d.showFps);
    write(L"Interface", L"ShowFrameTime", d.showFrameTime);
    write(L"Interface", L"Scale", d.uiScale);
    write(L"Interface", L"HudCorner", d.hudCorner);
    write(L"Graphics", L"Scale", d.scale);
    write(L"Graphics", L"VSync", d.vsync);
    write(L"Graphics", L"FrameLimit", d.frameLimit);
    write(L"Graphics", L"Anisotropy", d.anisotropy);
    write(L"Graphics", L"Presentation", d.presentation);
    write(L"Graphics", L"Sharpness", d.sharpness);
    write(L"Graphics", L"Upscaler", d.upscaler);
    write(L"Graphics", L"FrameGeneration", d.frameGeneration);
    write(L"Controls", L"Mode", d.inputMode);
    write(L"Controls", L"Sensitivity", d.sensitivity);
    write(L"Controls", L"Deadzone", d.deadzone);
    write(L"Controls", L"InvertY", d.invertY);
    write(L"Controls", L"MouseCamera", d.mouseCamera);
    write(L"Controls", L"MouseBreath", d.mouseBreath);
    write(L"Controls", L"MouseSpeed", d.mouseSpeed);
    write(L"Controls", L"MouseButtons", d.mouseButtons);
    write(L"Controls", L"CameraButton", d.cameraButton);
    write(L"Controls", L"WheelZoom", d.wheelZoom);
    write(L"Controls", L"Vibration", d.vibration);
    write(L"Audio", L"Volume", d.volume);
    write(L"Audio", L"MuteUnfocused", d.muteUnfocused);
    for (int i = 0; i < ActionCount; ++i) write(L"Keyboard", kKeyNames[i], d.keys[i]);
    return ok;
}
bool Bind(Action action, uint16_t key) {
    if (action < 0 || action >= ActionCount || !key || key > 0xFE ||
        key == VK_F1 || key == VK_F11 || key == VK_ESCAPE) return false;
    std::lock_guard lock(g_mutex);
    const auto old = g_data.keys[action];
    for (int i = 0; i < ActionCount; ++i)
        if (i != action && g_data.keys[i] == key) g_data.keys[i] = old;
    g_data.keys[action] = key;
    return true;
}
bool GraphicsChanged(const Data& a, const Data& b) {
    return a.scale != b.scale || a.vsync != b.vsync ||
           a.frameLimit != b.frameLimit || a.anisotropy != b.anisotropy;
}
std::string KeyName(uint16_t key) {
    switch (key) {
    case VK_RETURN: return "Enter";
    case VK_SPACE: return "Espaço";
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_UP: return "Cima";
    case VK_DOWN: return "Baixo";
    case VK_LEFT: return "Esquerda";
    case VK_RIGHT: return "Direita";
    }
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, char(key));
    wchar_t buffer[128]{};
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG flags = LONG((scan & 0xFF) << 16) | ((scan & 0xFF00) ? (1 << 24) : 0);
    if (!GetKeyNameTextW(flags, buffer, 128)) return "Tecla " + std::to_string(key);
    char utf8[512]{};
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return utf8;
}
}
