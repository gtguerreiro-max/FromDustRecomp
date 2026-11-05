#pragma once
#include <cstdint>
#include <array>
namespace settings { struct Data; }
namespace pcinput {
struct Gamepad {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
};
struct State { uint32_t packet = 0; Gamepad pad; };
static_assert(sizeof(Gamepad) == 12 && sizeof(State) == 16);
void SetWindow(void* hwnd);
void HandleKey(uint32_t key, bool down);
void HandleWheel(int delta); // WHEEL_DELTA units, positive = away from the user
bool MouseCaptured();         // PC mouse active: hide the cursor
void ClearKeys();
void SetBlocked(bool blocked);
bool Blocked();
Gamepad MapKeyboard(const settings::Data& config, const std::array<bool, 256>& keys);
uint32_t GetState(uint32_t user, State& state);
uint32_t GetCapabilities(uint32_t user, bool& vibration);
uint32_t SetVibration(uint32_t user, uint16_t left, uint16_t right);
// Holds `pad` (buttons, triggers, sticks) on pad 0 for `durationMs` after
// `delayMs` (diagnostic runs).
void SchedulePad(const Gamepad& pad, uint32_t delayMs, uint32_t durationMs);
// On-screen prompts: keyboard / mouse glyphs while the keyboard or mouse was
// the last device used (or always / never, per the input mode setting).
bool KeyboardPrompts();
// The mouse in the game window, normalized to [0, 1] (false when the window is
// not focused, input is blocked by the menu, or the mouse is outside).
bool MousePosition(float& x, float& y);
// Mouse pointing (pointer.cpp) registers whether it drives the Breath; the
// relative mouse-as-stick emulation and cursor capture are off meanwhile.
void SetPointerQuery(bool (*active)());
// Observer of the player's buttons (XInput mask) each poll, before injected
// presses (camera_assist.cpp).
void SetButtonObserver(void (*observer)(uint16_t buttons));
// Mouse movement since the last call while the Breath is grabbed by the mouse
// (hidden cursor), as fractions of the window size (+x right, +y down).
bool TakeMouseDelta(float& dx, float& dy);
// FD_MOUSE_ABSOLUTE=1: the Breath goes to the visible cursor instead.
bool MouseAbsolute();
// The game window's client size (for aspect-correct mouse mapping).
void WindowAspect(float& width, float& height);
// Changes whenever the prompt style or a binding shown in prompts changes.
uint32_t PromptGeneration();
}
