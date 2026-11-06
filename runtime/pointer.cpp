#include "pointer.h"
#include "input.h"
#include "settings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <mutex>

namespace pointer
{
namespace
{
std::mutex g_mutex;
float g_rows[16]{};
bool g_camera = false;
bool Invert(const float* m, double* out)
{
    double inv[16], a[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    inv[0] = a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    const double det = a[0]*inv[0]+a[1]*inv[4]+a[2]*inv[8]+a[3]*inv[12];
    if (std::fabs(det) < 1e-12) return false;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}
}
namespace { const bool g_registered = (pcinput::SetPointerQuery(&Active), true); }
void SetCamera(const float rows[16], float, float)
{
    std::lock_guard lock(g_mutex);
    memcpy(g_rows, rows, sizeof(g_rows));
    g_camera = true;
}
bool Active()
{
    // Direct mode (F1 > Controls), while the keyboard / mouse is the last
    // device used. FD_MOUSE_CLASSIC=1 forces the classic stick emulation.
    static const bool classic = getenv("FD_MOUSE_CLASSIC") != nullptr;
    const auto d = settings::Get();
    if (classic || !d.mouseBreath || d.mouseMode != 0 || !pcinput::KeyboardPrompts()) return false;
    std::lock_guard lock(g_mutex);
    return g_camera;
}
bool Target(float height, float& x, float& y)
{
    float nx, ny;
    if (!Active() || !pcinput::MousePosition(nx, ny)) return false;
    double inv[16];
    {
        std::lock_guard lock(g_mutex);
        if (!Invert(g_rows, inv)) return false;
    }
    // Screen -> clip: x right, y up; two depths along the ray.
    const double cx = nx * 2 - 1, cy = 1 - ny * 2;
    double p[2][3];
    for (int k = 0; k < 2; ++k)
    {
        const double cz = k ? 1.0 : 0.0;
        double v[4];
        for (int r = 0; r < 4; ++r) v[r] = inv[r * 4 + 0] * cx + inv[r * 4 + 1] * cy + inv[r * 4 + 2] * cz + inv[r * 4 + 3];
        if (std::fabs(v[3]) < 1e-9) return false;
        for (int c = 0; c < 3; ++c) p[k][c] = v[c] / v[3];
    }
    const double dz = p[1][2] - p[0][2];
    if (std::fabs(dz) < 1e-9) return false;
    const double t = (height - p[0][2]) / dz;
    if (!std::isfinite(t)) return false;
    x = float(p[0][0] + (p[1][0] - p[0][0]) * t);
    y = float(p[0][1] + (p[1][1] - p[0][1]) * t);
    return std::isfinite(x) && std::isfinite(y);
}
namespace
{
bool Unproject(float sx, float sy, float height, float& x, float& y)
{
    double inv[16];
    {
        std::lock_guard lock(g_mutex);
        if (!g_camera || !Invert(g_rows, inv)) return false;
    }
    const double cx = sx * 2 - 1, cy = 1 - sy * 2;
    double p[2][3];
    for (int k = 0; k < 2; ++k)
    {
        double v[4];
        for (int r = 0; r < 4; ++r) v[r] = inv[r * 4] * cx + inv[r * 4 + 1] * cy + inv[r * 4 + 2] * k + inv[r * 4 + 3];
        if (std::fabs(v[3]) < 1e-9) return false;
        for (int c = 0; c < 3; ++c) p[k][c] = v[c] / v[3];
    }
    const double dz = p[1][2] - p[0][2];
    if (std::fabs(dz) < 1e-9) return false;
    const double t = (height - p[0][2]) / dz;
    // Behind the camera or beyond the far plane: no ground point there.
    if (!std::isfinite(t) || t < 0 || t > 1) return false;
    x = float(p[0][0] + (p[1][0] - p[0][0]) * t);
    y = float(p[0][1] + (p[1][1] - p[0][1]) * t);
    return std::isfinite(x) && std::isfinite(y);
}
}
bool Grab(float x, float y, float z, float& outX, float& outY)
{
    if (!Active() || pcinput::MouseAbsolute()) return false;
    float dx, dy;
    if (!pcinput::TakeMouseDelta(dx, dy) || (dx == 0 && dy == 0)) return false;
    const float speed = settings::Get().mouseSpeed / 100.0f;
    float sx, sy;
    if (!Project(x, y, z, sx, sy)) return false;
    // Planar mapping: the mouse moves the Breath over the ground along the
    // camera's right and forward directions at one rate, scaled so that a
    // sideways movement matches the screen at the Breath's distance. Mapping
    // the screen one to one instead made vertical movement depend on the
    // camera's low pitch (far too fast towards the horizon).
    constexpr float step = 0.01f;
    float rx, ry;
    if (!Unproject(sx + step, sy, z, rx, ry)) return false;
    const float rightX = rx - x, rightY = ry - y, length = std::sqrt(rightX * rightX + rightY * rightY);
    if (!(length > 1e-5f)) return false;
    const float perWidth = length / step; // world units per window width
    const float ux = rightX / length, uy = rightY / length;
    // Forward on the ground: perpendicular to right, pointing away from the
    // camera (towards the top of the screen).
    float fx = -uy, fy = ux, upX, upY;
    if (Unproject(sx, sy - step, z, upX, upY) && (upX - x) * fx + (upY - y) * fy < 0) { fx = -fx; fy = -fy; }
    float width = 16, height = 9;
    pcinput::WindowAspect(width, height);
    const float sideways = dx * perWidth * speed, forward = -dy * (height / width) * perWidth * speed;
    outX = x + ux * sideways + fx * forward;
    outY = y + uy * sideways + fy * forward;
    return std::isfinite(outX) && std::isfinite(outY);
}
namespace
{
struct FocusState
{
    bool valid = false;
    float x = 0, y = 0, z = 0;
    float lastX = 0, lastY = 0; // the Breath as edge scrolling left it
    unsigned long long tick = 0;
};
std::mutex g_focusMutex;
FocusState g_focus;
unsigned long long Now()
{
    using namespace std::chrono;
    return (unsigned long long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}
bool EdgeScroll(float liveX, float liveY, float& x, float& y, float z)
{
    const auto d = settings::Get();
    std::lock_guard lock(g_focusMutex);
    const unsigned long long now = Now();
    if (!d.edgeScroll || !Active()) { g_focus.valid = false; return false; }
    // Starts over after a gap (menus, cut scenes, loading) or when the game
    // moved the cursor itself (tutorials, looking at a villager): the camera
    // then goes where the game put the cursor.
    const float jumpX = liveX - g_focus.lastX, jumpY = liveY - g_focus.lastY;
    float sx, sy;
    if (!Project(x, y, z, sx, sy)) { g_focus.valid = false; return false; }
    float rx, ry;
    constexpr float step = 0.01f;
    // World units per window width at the screen centre (scroll speed and
    // jump size do not depend on where the Breath is in the perspective).
    float perWidth = 0, ox, oy;
    if (Unproject(0.5f, 0.5f, z, ox, oy) && Unproject(0.5f + step, 0.5f, z, rx, ry))
        perWidth = std::sqrt((rx - ox) * (rx - ox) + (ry - oy) * (ry - oy)) / step;
    if (!g_focus.valid || now - g_focus.tick > 500 ||
        (perWidth > 0 && std::sqrt(jumpX * jumpX + jumpY * jumpY) > 0.2f * perWidth))
    {
        g_focus.valid = true;
        g_focus.x = liveX; g_focus.y = liveY; g_focus.z = z;
        g_focus.tick = now;
    }
    else
    {
        // Small corrections by the game (the map's limits stop the Breath
        // that the scroll carried): the focus takes them too, so the camera
        // never runs on past the Breath.
        g_focus.x += jumpX; g_focus.y += jumpY;
    }
    const float dt = (std::min)(0.1f, (now - g_focus.tick) / 1000.0f);
    g_focus.tick = now;
    // Keep the Breath on screen (a little inside the edges).
    constexpr float inside = 0.01f;
    const float cx = std::clamp(sx, inside, 1 - inside), cy = std::clamp(sy, inside, 1 - inside);
    if ((cx != sx || cy != sy) && Unproject(cx, cy, z, rx, ry)) { x = rx; y = ry; sx = cx; sy = cy; }
    // Inside the edge band, scroll towards it: up to half a view width per
    // second at the edge (x Edge scroll speed), carrying the Breath.
    constexpr float band = 0.08f;
    auto push = [](float s) { return s < band ? -(band - s) / band : s > 1 - band ? (s - (1 - band)) / band : 0.0f; };
    const float px = push(sx), py = push(sy);
    if ((px != 0 || py != 0) && perWidth > 0 && dt > 0)
    {
        float ux = 1, uy = 0, fx = 0, fy = 1, upX, upY;
        if (Unproject(sx + step, sy, z, rx, ry))
        {
            const float length = std::sqrt((rx - x) * (rx - x) + (ry - y) * (ry - y));
            if (length > 1e-5f) { ux = (rx - x) / length; uy = (ry - y) / length; }
        }
        fx = -uy; fy = ux;
        if (Unproject(sx, sy - step, z, upX, upY) && (upX - x) * fx + (upY - y) * fy < 0) { fx = -fx; fy = -fy; }
        const float speed = 0.5f * perWidth * (d.edgeScrollSpeed / 100.0f) * dt;
        // Smooth start inside the band: quadratic in the depth.
        const float ex = px * std::fabs(px), ey = py * std::fabs(py);
        const float mx = (ux * ex - fx * ey) * speed, my = (uy * ex - fy * ey) * speed;
        g_focus.x += mx; g_focus.y += my;
        x += mx; y += my;
    }
    // The focus height follows the Breath slowly (no bobbing over terrain
    // while the Breath alone moves, no lag across a long scroll).
    g_focus.z += (z - g_focus.z) * (std::min)(1.0f, dt * 1.5f);
    g_focus.lastX = x; g_focus.lastY = y;
    return true;
}
bool Focus(float& x, float& y, float& z)
{
    std::lock_guard lock(g_focusMutex);
    if (!g_focus.valid || Now() - g_focus.tick > 250) return false;
    x = g_focus.x; y = g_focus.y; z = g_focus.z;
    return true;
}
bool Project(float x, float y, float z, float& sx, float& sy)
{
    std::lock_guard lock(g_mutex);
    if (!g_camera) return false;
    double v[4];
    for (int r = 0; r < 4; ++r) v[r] = g_rows[r * 4] * x + g_rows[r * 4 + 1] * y + g_rows[r * 4 + 2] * z + g_rows[r * 4 + 3];
    if (std::fabs(v[3]) < 1e-9) return false;
    sx = float((v[0] / v[3] + 1) * 0.5); sy = float((1 - v[1] / v[3]) * 0.5);
    return true;
}
bool CameraDistance(float x, float y, float z, float& distance)
{
    double inv[16];
    {
        std::lock_guard lock(g_mutex);
        if (!g_camera || !Invert(g_rows, inv)) return false;
    }
    double v[4];
    for (int r = 0; r < 4; ++r) v[r] = inv[r * 4 + 3]; // clip (0, 0, 0, 1): near-plane centre
    if (std::fabs(v[3]) < 1e-9) return false;
    const double cx = v[0] / v[3] - x, cy = v[1] / v[3] - y, cz = v[2] / v[3] - z;
    distance = float(std::sqrt(cx * cx + cy * cy + cz * cz));
    return std::isfinite(distance);
}
}
