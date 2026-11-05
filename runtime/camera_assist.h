#pragma once
// "Global view by default": From Dust's far camera (RB) suits a PC screen and
// mouse, like an RTS view. The game falls back to its close camera after cut
// scenes and tutorials; when it does so on its own, an RB tap is injected.
// The player's own RB choice wins: after an RB press that leaves the global
// view, nothing is injected until the player returns to it.
#include <cstdint>

namespace camera
{
// Per gameplay frame, with the camera's distance to the Breath's cursor.
void Update(float distance);
// From the input poll, with the player's buttons (XInput mask) before any
// injected presses.
void PlayerButtons(uint16_t buttons);
}
