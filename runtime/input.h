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
}
