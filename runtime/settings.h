#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace settings {
enum Action {
    Start, Back, Accept, Cancel, X, Y, LeftShoulder, RightShoulder,
    LeftTrigger, RightTrigger, MoveUp, MoveDown, MoveLeft, MoveRight,
    LookUp, LookDown, LookLeft, LookRight, DpadUp, DpadDown, DpadLeft, DpadRight, ActionCount
};
inline constexpr std::array<uint16_t, ActionCount> kDefaultKeys = {
    0x0D, 0x09, 0x20, 0x08, 'X', 'R', 'Q', 'E', 'Z', 'C',
    'W', 'S', 'A', 'D', 'I', 'K', 'J', 'L', 0x26, 0x28, 0x25, 0x27
};
inline constexpr const char* kActionNames[] = {
    "Start / pausa", "Voltar / seleção", "A / confirmar", "B / cancelar", "X", "Y",
    "LB", "RB", "LT", "RT", "Mover para a frente", "Mover para trás",
    "Mover para a esquerda", "Mover para a direita", "Câmara: cima", "Câmara: baixo",
    "Câmara: esquerda", "Câmara: direita", "Direcional: cima", "Direcional: baixo",
    "Direcional: esquerda", "Direcional: direita"
};
inline constexpr int kWindowWidths[] = {1280, 1600, 1920, 2560, 3840};
inline constexpr int kWindowHeights[] = {720, 900, 1080, 1440, 2160};
struct Data {
    bool fullscreen = false;
    int windowPreset = 0;
    bool showFps = false;
    bool showFrameTime = false;
    int uiScale = 100;
    int hudCorner = 0; // top right, top left, bottom right, bottom left
    int scale = 1;
    bool vsync = true;
    int frameLimit = 60;
    int anisotropy = -1;
    int presentation = 0; // bilinear, FSR, CAS
    int sharpness = 20;
    int upscaler = 0; // native renderer: off, DLAA (live)
    int frameGeneration = 0; // native renderer, DLSS-G: off, 2x, 3x, 4x (load on restart)
    int inputMode = 0; // automatic, keyboard/mouse, controller
    int sensitivity = 100;
    int deadzone = 15;
    bool invertY = false;
    bool mouseCamera = true;
    // PC-style mouse: free movement drives the Breath (left stick), left /
    // right buttons absorb / release (LT / RT), a held button turns the
    // camera (right stick) and the wheel zooms.
    bool mouseBreath = true;
    int mouseSpeed = 100;
    bool mouseButtons = true;
    int cameraButton = 2; // 0 off, 1 right button, 2 middle button
    int wheelZoom = 1;    // 0 off, 1 normal, 2 inverted
    bool vibration = true;
    int volume = 100;
    bool muteUnfocused = false;
    std::array<uint16_t, ActionCount> keys = kDefaultKeys;
    bool operator==(const Data&) const = default;
};
void Load(const std::filesystem::path& path);
Data Get();
void Set(Data value);
bool Save();
std::filesystem::path Path();
std::string KeyName(uint16_t key);
bool Bind(Action action, uint16_t key); // swaps duplicate assignments
bool GraphicsChanged(const Data& a, const Data& b);
}
