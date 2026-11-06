#include "camera_assist.h"
#include "input.h"
#include "settings.h"
#include "kernel/kernel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace camera
{
namespace
{
// Measured on the first levels: close camera ~25 units from the cursor,
// global view ~113.
constexpr float kGlobalDistance = 60.0f;
constexpr ULONGLONG kStableMs = 2000, kRetryMs = 4000, kAfterLookMs = 20000;
constexpr uint16_t kRb = 0x0200, kLb = 0x0100;

std::mutex g_mutex;
bool g_playerWantsClose = false;
bool g_global = false;
// Global view zoom: target set by the wheel, current eased towards it.
constexpr float kZoomMin = 0.4f, kZoomMax = 1.6f, kZoomStep = 0.9f;
float g_zoomTarget = 1.0f, g_zoom = 1.0f;
ULONGLONG g_zoomTick = 0;
ULONGLONG g_closeSince = 0, g_lastInjected = 0, g_lastLook = 0, g_lastFrame = 0;
uint16_t g_previousButtons = 0;
}

namespace { const bool g_registered = (pcinput::SetButtonObserver(&PlayerButtons), pcinput::SetWheelZoom(&InGlobalView, &ZoomNotches), true); }
void PlayerButtons(uint16_t buttons)
{
    std::lock_guard lock(g_mutex);
    const uint16_t pressed = buttons & ~g_previousButtons;
    g_previousButtons = buttons;
    const ULONGLONG now = GetTickCount64();
    // LB looks at a villager up close: leave that alone for a while.
    if (pressed & kLb) g_lastLook = now;
    // The player's RB: from the global view it means "close"; from the close
    // view it means "global" again.
    if (pressed & kRb) g_playerWantsClose = g_global;
}

void Update(float distance)
{
    static const bool forcedOff = getenv("FD_NO_GLOBAL_VIEW") != nullptr;
    const auto d = settings::Get();
    std::lock_guard lock(g_mutex);
    const ULONGLONG now = GetTickCount64();
    // A gap in gameplay frames (menus, loading): start the timers again.
    if (now - g_lastFrame > 500) g_closeSince = 0;
    g_lastFrame = now;
    // The global view's distance scales with the zoom (close ~25, global
    // ~113 x zoom): the threshold follows it, never below 35.
    g_global = distance > (std::max)(35.0f, kGlobalDistance * g_zoom);
    if (g_global) { g_closeSince = 0; return; }
    if (!g_closeSince) g_closeSince = now;
    if (forcedOff || !d.globalView || g_playerWantsClose || pcinput::Blocked()) return;
    if (now - g_closeSince < kStableMs || now - g_lastInjected < kRetryMs || now - g_lastLook < kAfterLookMs) return;
    g_lastInjected = now;
    pcinput::Gamepad tap{};
    tap.buttons = kRb;
    pcinput::SchedulePad(tap, 0, 150);
    static const bool trace = getenv("FD_TRACE_CAMERA_DISTANCE") != nullptr;
    if (trace) kernel::Log("camera assist: close view (%.1f) for %llu ms, switching to the global view", distance, now - g_closeSince);
}
bool InGlobalView()
{
    std::lock_guard lock(g_mutex);
    return g_global && GetTickCount64() - g_lastFrame < 500;
}
void ZoomNotches(float notches)
{
    std::lock_guard lock(g_mutex);
    g_zoomTarget = std::clamp(g_zoomTarget * std::pow(kZoomStep, notches), kZoomMin, kZoomMax);
}
float GlobalZoom()
{
    std::lock_guard lock(g_mutex);
    const ULONGLONG now = GetTickCount64();
    // FD_CAMERA_ZOOM_TEST=<s>:<factor>;...: diagnostic zoom targets over time.
    static const char* test = getenv("FD_CAMERA_ZOOM_TEST");
    static const ULONGLONG start = now;
    if (test)
        for (const char* p = test; p && *p; p = strchr(p, ';'), p = p ? p + 1 : nullptr)
        {
            float t, factor;
            if (sscanf(p, "%f:%f", &t, &factor) == 2 && (now - start) / 1000.0f >= t) g_zoomTarget = std::clamp(factor, kZoomMin, kZoomMax);
        }
    // Eased: about 90% of the way in a quarter of a second.
    const float dt = g_zoomTick ? (std::min)(0.1f, (now - g_zoomTick) / 1000.0f) : 0.0f;
    g_zoomTick = now;
    g_zoom += (g_zoomTarget - g_zoom) * (std::min)(1.0f, dt * 9.0f);
    if (std::fabs(g_zoom - g_zoomTarget) < 1e-4f) g_zoom = g_zoomTarget;
    return g_zoom;
}
}
