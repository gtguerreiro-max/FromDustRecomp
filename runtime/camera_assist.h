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
// True while the camera is in the global view (measured by Update).
bool InGlobalView();
// Mouse wheel zoom of the global view: a factor on the global camera's height
// and distance to the cursor (DustAi::CameraManager m_globalCameraHeight /
// m_globalDistanceToCursor), so the view zooms straight in and out and never
// falls back to the close camera. Notches > 0 zoom in.
void ZoomNotches(float notches);
// The current factor (smoothed towards the wheel's target; 1 = the game's).
float GlobalZoom();
}
