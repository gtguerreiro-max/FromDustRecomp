#include "pointer.h"
#include "input.h"
#include "settings.h"
#include <atomic>
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
    // Experimental until the Breath's own position is confirmed (see
    // docs/NATIVE_RENDERER.md, "Mouse pointing"): FD_MOUSE_POINTING=1.
    static const bool enabled = getenv("FD_MOUSE_POINTING") != nullptr;
    if (!enabled) return false;
    const auto d = settings::Get();
    if (!d.mouseBreath || !pcinput::KeyboardPrompts()) return false;
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
}
