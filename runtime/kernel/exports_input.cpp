// Xbox gamepad exports backed by Windows XInput and remappable PC keys.
#include "kernel.h"
#include "../input.h"
#include "../game_loop.h"
#include "../latency.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
void WritePad(uint32_t address, const pcinput::Gamepad& pad) {
    *mem::Ptr<be<uint16_t>>(address) = pad.buttons;
    mem::Ptr<uint8_t>(address)[2] = pad.leftTrigger;
    mem::Ptr<uint8_t>(address)[3] = pad.rightTrigger;
    *mem::Ptr<be<int16_t>>(address + 4) = pad.leftX;
    *mem::Ptr<be<int16_t>>(address + 6) = pad.leftY;
    *mem::Ptr<be<int16_t>>(address + 8) = pad.rightX;
    *mem::Ptr<be<int16_t>>(address + 10) = pad.rightY;
}
}
KFUNC(XamInputGetState) {
    if (ARG2) memset(mem::Translate(ARG2), 0, 16);
    pcinput::State state;
    const auto result = pcinput::GetState(ARG0, state);
    // Decoupled simulation: buttons and triggers change for the game only
    // before a world step (the world logic detects presses as edges); the
    // sticks stay live for the per-frame camera.
    // The packet number follows the state the game actually sees (it ignores
    // a state whose packet number did not change).
    static pcinput::Gamepad held[4], shown[4];
    static uint32_t packets[4];
    if (!result && ARG0 < 4) {
        const uint16_t raw = state.pad.buttons;
        const bool mayChange = gameloop::InputMayChange();
        // FD_TRACE_PAD=1: calls while a button is down or the shown buttons change.
        static const bool tracePad = getenv("FD_TRACE_PAD") != nullptr;
        if (tracePad && (raw || held[ARG0].buttons))
            fprintf(stderr, "pad %u: raw %04X may %d held %04X packet %u thread %lu t %llu\n", ARG0, raw, int(mayChange), held[ARG0].buttons, packets[ARG0], GetCurrentThreadId(), GetTickCount64());
        if (mayChange) held[ARG0] = state.pad;
        else {
            state.pad.buttons = held[ARG0].buttons;
            state.pad.leftTrigger = held[ARG0].leftTrigger;
            state.pad.rightTrigger = held[ARG0].rightTrigger;
        }
        if (memcmp(&shown[ARG0], &state.pad, sizeof(state.pad))) { shown[ARG0] = state.pad; ++packets[ARG0]; latency::InputChanged(); }
        state.packet = packets[ARG0];
    }
    if (!result && ARG2) {
        mem::Write32(ARG2, state.packet);
        WritePad(ARG2 + 4, state.pad);
    }
    RETURN(result);
}
KFUNC(XamInputGetCapabilities) {
    if (!ARG2) RETURN(ERROR_INVALID_PARAMETER);
    memset(mem::Translate(ARG2), 0, 20);
    bool vibration;
    const auto result = pcinput::GetCapabilities(ARG0, vibration);
    if (!result) {
        mem::Ptr<uint8_t>(ARG2)[0] = 1; // gamepad
        mem::Ptr<uint8_t>(ARG2)[1] = 1;
        *mem::Ptr<be<uint16_t>>(ARG2 + 2) = vibration ? 1 : 0;
        WritePad(ARG2 + 4, {0xF3FF, 255, 255, 32767, 32767, 32767, 32767});
        *mem::Ptr<be<uint16_t>>(ARG2 + 16) = vibration ? 65535 : 0;
        *mem::Ptr<be<uint16_t>>(ARG2 + 18) = vibration ? 65535 : 0;
    }
    RETURN(result);
}
KFUNC(XamInputGetKeystrokeEx) {
    if (!ARG0 || !ARG2) RETURN(ERROR_INVALID_PARAMETER);
    memset(mem::Translate(ARG2), 0, 8);
    // Gamepad buttons are delivered through GetState. There are no queued
    // text keystrokes, so don't claim success with an uninitialized event.
    const uint32_t user = mem::Read32(ARG0);
    bool vibration;
    const auto result = pcinput::GetCapabilities(user == 0xFF ? 0 : user, vibration);
    RETURN(result ? result : 4306); // ERROR_EMPTY
}
KFUNC(XamInputSetState) {
    if (!ARG2) RETURN(ERROR_INVALID_PARAMETER);
    RETURN(pcinput::SetVibration(ARG0, *mem::Ptr<be<uint16_t>>(ARG2), *mem::Ptr<be<uint16_t>>(ARG2 + 2)));
}
