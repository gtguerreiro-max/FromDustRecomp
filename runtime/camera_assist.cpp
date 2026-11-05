#include "camera_assist.h"
#include "input.h"
#include "settings.h"
#include "kernel/kernel.h"
#include <atomic>
#include <cstdlib>
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
ULONGLONG g_closeSince = 0, g_lastInjected = 0, g_lastLook = 0, g_lastFrame = 0;
uint16_t g_previousButtons = 0;
}

namespace { const bool g_registered = (pcinput::SetButtonObserver(&PlayerButtons), true); }
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
    g_global = distance > kGlobalDistance;
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
}
