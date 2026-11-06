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
    "Start / pause", "Back / select", "A / confirm", "B / cancel", "X", "Y",
    "LB", "RB", "LT", "RT", "Move forward", "Move back",
    "Move left", "Move right", "Camera up", "Camera down",
    "Camera left", "Camera right", "D-pad up", "D-pad down",
    "D-pad left", "D-pad right"
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
    int anisotropy = -1;    // -1 game default, 2..5: anisotropic 2x / 4x / 8x / 16x
    int shadowScale = 1;    // native renderer: shadow maps 1x / 2x / 4x the game's 512x512 (live)
    bool hdr = false;       // native renderer: HDR output (scRGB; on restart)
    int hdrPaperWhite = 200; // nits of SDR white in HDR (live)
    int hdrPeak = 800;       // nits of the brightest highlights in HDR (live)
    int presentation = 0; // bilinear, FSR, CAS
    int sharpness = 20;
    int upscaler = 0; // native renderer: off, DLAA, DLSS quality / balanced / performance / ultra (live)
    int renderScale = 100; // native renderer internal resolution, percent of 1024x600 (live)
    bool decoupledSim = false; // world at a fixed 30 Hz, render up to the FPS limit (live)
    bool evenPacing = true;    // with it: only 120 / 60 / 30 FPS, so world steps stay on screen evenly
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
    bool globalView = true; // keep the far (RB) camera unless the player leaves it
    int mouseMode = 0; // 0 direct (the Breath moves with the mouse on screen), 1 classic (mouse as stick)
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
