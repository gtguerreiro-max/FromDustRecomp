#pragma once
// PC mouse pointing for the Breath: the native capture records the scene
// camera (world -> clip) of each frame; the cursor update hook asks for the
// terrain-plane point under the mouse and places the Breath there.
#include <cstdint>

namespace pointer
{
// Scene camera rows (clip = M * [x y z 1]) and the scene viewport size, from
// the first full-screen terrain draw of a frame (game thread).
void SetCamera(const float rows[16], float viewportWidth, float viewportHeight);
// The point of the horizontal plane z = height (the engine is Z-up) under the
// mouse; false when the mouse is not pointing (setting off, controller in use,
// menu open, no camera).
bool Target(float height, float& x, float& y);
// Grabbed mode: moves the world point (x, y, z) on screen by the mouse
// movement since the last call (scaled by the Breath speed setting), back on
// its height plane. False when not pointing or nothing moved.
bool Grab(float x, float y, float z, float& outX, float& outY);
// True while mouse pointing drives the Breath (the stick emulation stays off).
bool Active();
// Diagnostics: the screen position [0, 1] of a world point with the camera.
bool Project(float x, float y, float z, float& sx, float& sy);
// Distance from the camera (its near-plane centre) to a world point.
bool CameraDistance(float x, float y, float z, float& distance);
}
