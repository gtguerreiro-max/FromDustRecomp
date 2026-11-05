#ifdef FD_NATIVE_RENDERER
#include "prompt_glyphs.h"
#include "../../input.h"
#include "../../settings.h"
#include "../../kernel/kernel.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gpu::native::prompts
{
namespace
{
struct Rgba { float r, g, b, a; };
// Straight-alpha RGBA8 canvas, top-down.
struct Canvas
{
    int w, h;
    std::vector<Rgba> px;
    Canvas(int width, int height) : w(width), h(height), px(size_t(width) * height, {0, 0, 0, 0}) {}
    void Blend(int x, int y, Rgba c, float coverage)
    {
        if (x < 0 || y < 0 || x >= w || y >= h || coverage <= 0) return;
        auto& d = px[size_t(y) * w + x];
        const float a = c.a * std::min(coverage, 1.0f), out = a + d.a * (1 - a);
        if (out <= 0) return;
        d.r = (c.r * a + d.r * d.a * (1 - a)) / out;
        d.g = (c.g * a + d.g * d.a * (1 - a)) / out;
        d.b = (c.b * a + d.b * d.a * (1 - a)) / out;
        d.a = out;
    }
};
// Signed distance to a rounded rectangle (centre cx, cy; half sizes hx, hy).
float RoundedBox(float x, float y, float cx, float cy, float hx, float hy, float r)
{
    const float qx = std::fabs(x - cx) - hx + r, qy = std::fabs(y - cy) - hy + r;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}
template <typename Shape> void Fill(Canvas& c, Shape shape, auto colour)
{
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x)
        {
            const float px = x + 0.5f, py = y + 0.5f, d = shape(px, py);
            if (d < 0.5f) c.Blend(x, y, colour(px, py), std::clamp(0.5f - d, 0.0f, 1.0f));
        }
}
const Rgba kBorder{0.13f, 0.13f, 0.14f, 1}, kInk{0.10f, 0.10f, 0.11f, 1}, kAccent{0.95f, 0.70f, 0.20f, 1};
Rgba Face(float y, float top, float bottom)
{
    const float t = std::clamp((y - top) / std::max(1.0f, bottom - top), 0.0f, 1.0f);
    const float v = 0.96f - 0.20f * t;
    return {v, v, v * 1.01f, 1};
}
// Anti-aliased text coverage from GDI, fitted into the box.
std::vector<float> TextMask(const std::wstring& text, int w, int h, int boxW, int boxH)
{
    std::vector<float> mask(size_t(w) * h, 0.0f);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits) { if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); return mask; }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    memset(bits, 0, size_t(w) * h * 4);
    HFONT font = nullptr;
    for (int size = boxH; size >= 6; --size)
    {
        if (font) DeleteObject(font);
        const bool symbol = std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch > 0x2000; });
        font = CreateFontW(-size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, symbol ? L"Segoe UI Symbol" : L"Segoe UI");
        SelectObject(dc, font);
        SIZE extent{};
        GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent);
        if (extent.cx <= boxW && extent.cy * 0.8f <= boxH) break;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rect{0, 0, w, h};
    DrawTextW(dc, text.c_str(), int(text.size()), &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    const auto* p = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < mask.size(); ++i) mask[i] = std::max({p[i * 4], p[i * 4 + 1], p[i * 4 + 2]}) / 255.0f;
    SelectObject(dc, oldBitmap);
    if (font) DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return mask;
}
void Keycap(Canvas& c, const std::wstring& label)
{
    const float cx = c.w / 2.0f, cy = c.h / 2.0f, hx = c.w / 2.0f - 1, hy = c.h / 2.0f - 1;
    const float r = std::min(c.w, c.h) * 0.22f, border = std::max(1.5f, std::min(c.w, c.h) * 0.07f);
    Fill(c, [&](float x, float y) { return RoundedBox(x, y, cx, cy, hx, hy, r); }, [&](float, float) { return kBorder; });
    Fill(c, [&](float x, float y) { return RoundedBox(x, y, cx, cy - border * 0.35f, hx - border, hy - border * 1.35f, std::max(1.0f, r - border)); },
        [&](float, float y) { return Face(y, 0, float(c.h)); });
    const int pad = int(border * 2 + 1);
    const auto mask = TextMask(label, c.w, c.h, c.w - pad * 2, int((c.h - pad * 2) * 0.95f));
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) c.Blend(x, y - int(border * 0.35f), kInk, mask[size_t(y) * c.w + x]);
}
// button: 0 left, 1 right, 2 middle, 3 movement (no button highlighted).
void Mouse(Canvas& c, int button)
{
    const float cx = c.w / 2.0f, cy = c.h / 2.0f;
    const float hy = c.h / 2.0f - 1, hx = std::min(c.w / 2.0f - 1, hy * 0.68f), r = hx * 0.95f;
    const float border = std::max(1.5f, c.h * 0.07f), split = cy - hy * 0.12f;
    auto body = [&](float x, float y) { return RoundedBox(x, y, cx, cy, hx, hy, r); };
    auto inner = [&](float x, float y) { return RoundedBox(x, y, cx, cy, hx - border, hy - border, std::max(1.0f, r - border)); };
    Fill(c, body, [&](float, float) { return kBorder; });
    Fill(c, inner, [&](float, float y) { return Face(y, 0, float(c.h)); });
    if (button == 0 || button == 1)
        Fill(c, [&](float x, float y) {
            const float side = button == 0 ? x - (cx - border * 0.5f) : (cx + border * 0.5f) - x;
            return std::max({inner(x, y), y - split, side});
        }, [&](float, float) { return kAccent; });
    // Divider and seam between the buttons.
    Fill(c, [&](float x, float y) { return std::max(inner(x, y), std::fabs(y - split) - border * 0.45f); }, [&](float, float) { return kBorder; });
    Fill(c, [&](float x, float y) { return std::max({inner(x, y), y - split, std::fabs(x - cx) - border * 0.45f}); }, [&](float, float) { return kBorder; });
    if (button == 2 || button == 3)
    {
        const float wy = cy - hy * 0.45f;
        Fill(c, [&](float x, float y) { return RoundedBox(x, y, cx, wy, border * 1.1f, hy * 0.2f, border); },
            [&](float, float) { return button == 2 ? kAccent : kBorder; });
    }
    if (button == 3)
    {
        // Movement: small arrows left and right of the body.
        for (int side : {-1, 1})
        {
            const float tip = cx + side * (hx + (c.w / 2.0f - hx) * 0.85f), base = cx + side * (hx + border);
            Fill(c, [&](float x, float y) {
                const float t = (x - base) / (tip - base);
                if (t < 0 || t > 1) return 1.0f;
                return std::fabs(y - cy) - (1 - t) * hy * 0.25f;
            }, [&](float, float) { return kBorder; });
        }
    }
}
// English key labels; wide keys use their keycap symbols, which stay
// readable at the small size the game draws prompts.
std::wstring KeyName(uint16_t vk)
{
    switch (vk)
    {
    case VK_SPACE: return L"␣";
    case VK_RETURN: return L"↵";
    case VK_TAB: return L"⇥";
    case VK_BACK: return L"⌫";
    case VK_ESCAPE: return L"Esc";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return L"⇧";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return L"Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return L"Alt";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_DELETE: return L"Del";
    case VK_INSERT: return L"Ins";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PgUp";
    case VK_NEXT: return L"PgDn";
    case VK_CAPITAL: return L"Caps";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, wchar_t(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"N" + std::to_wstring(vk - VK_NUMPAD0);
    // Punctuation and the rest: the character the key types on this layout.
    if (const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF; ch > 32) return std::wstring(1, wchar_t(ch));
    return L"?";
}
// Glyph slots in the decoded atlas (top-down rows; the atlas is stored upside
// down, so glyphs are written flipped).
enum class Slot { Accept, Cancel, X, Y, LeftShoulder, RightShoulder, LeftTrigger, RightTrigger, Back, Start, Stick };
struct Glyph { int x, y, w, h; Slot slot; };
// Two 1024x1024 UI atlases carry the controller glyphs: the front end's (menus)
// and the in-game HUD's (tutorials, tool tips). Each is recognised by a 16x16
// sample inside its A glyph.
struct Atlas { int sampleX, sampleY; uint32_t hash; std::vector<Glyph> glyphs; };
const Atlas kAtlases[] = {
    {400, 720, 0x5755A483u, {
        {389, 703, 61, 61, Slot::Accept}, {455, 703, 61, 61, Slot::Cancel}, {521, 703, 61, 61, Slot::X},
        {590, 709, 56, 51, Slot::LeftTrigger}, {656, 709, 56, 51, Slot::RightTrigger},
        {967, 662, 28, 22, Slot::LeftTrigger}, {397, 596, 28, 22, Slot::RightTrigger},
        {993, 867, 28, 20, Slot::LeftShoulder}, {69, 273, 28, 20, Slot::RightShoulder},
        {399, 564, 24, 18, Slot::Back}, {592, 624, 66, 66, Slot::Stick}}},
    {310, 420, 0xDBED4158u, {
        {296, 209, 61, 61, Slot::Y}, {296, 275, 61, 61, Slot::X}, {296, 341, 61, 61, Slot::Cancel}, {296, 407, 61, 61, Slot::Accept},
        {770, 702, 28, 22, Slot::LeftTrigger}, {804, 702, 28, 22, Slot::RightTrigger},
        {778, 734, 58, 24, Slot::LeftShoulder}, {844, 734, 58, 24, Slot::RightShoulder},
        {909, 737, 28, 20, Slot::LeftShoulder}, {943, 737, 28, 20, Slot::RightShoulder},
        {840, 704, 24, 18, Slot::Back}, {874, 704, 24, 18, Slot::Start}, {294, 8, 66, 66, Slot::Stick}}},
};
const Atlas* FindAtlas(const DecodedTexture& image)
{
    if (image.floatPixels || image.width != 1024 || image.height != 1024 || image.pixels.size() != 1024u * 1024 * 4) return nullptr;
    for (const auto& atlas : kAtlases)
    {
        uint32_t hash = 2166136261u;
        for (int y = atlas.sampleY; y < atlas.sampleY + 16; ++y)
            for (int x = atlas.sampleX * 4; x < (atlas.sampleX + 16) * 4; ++x) hash = (hash ^ image.pixels[size_t(y) * 4096 + x]) * 16777619u;
        if (hash == atlas.hash) return &atlas;
    }
    return nullptr;
}
void Draw(DecodedTexture& image, const Glyph& glyph, const settings::Data& d)
{
    Canvas c(glyph.w, glyph.h);
    auto key = [&](settings::Action action) { Keycap(c, KeyName(d.keys[action])); };
    switch (glyph.slot)
    {
    case Slot::Accept: key(settings::Accept); break;
    case Slot::Cancel: key(settings::Cancel); break;
    case Slot::X: key(settings::X); break;
    case Slot::Y: key(settings::Y); break;
    case Slot::Start: key(settings::Start); break;
    case Slot::LeftShoulder: key(settings::LeftShoulder); break;
    case Slot::RightShoulder: key(settings::RightShoulder); break;
    case Slot::Back: key(settings::Back); break;
    case Slot::LeftTrigger: if (d.mouseButtons) Mouse(c, 0); else key(settings::LeftTrigger); break;
    case Slot::RightTrigger: if (d.mouseButtons && d.cameraButton != 1) Mouse(c, 1); else key(settings::RightTrigger); break;
    case Slot::Stick: if (d.mouseBreath) Mouse(c, 3); else Keycap(c, KeyName(d.keys[settings::MoveUp]) + KeyName(d.keys[settings::MoveLeft]) +
                                                               KeyName(d.keys[settings::MoveDown]) + KeyName(d.keys[settings::MoveRight])); break;
    }
    for (int y = 0; y < glyph.h; ++y)
    {
        uint8_t* row = image.pixels.data() + (size_t(glyph.y + glyph.h - 1 - y) * 1024 + glyph.x) * 4;
        for (int x = 0; x < glyph.w; ++x)
        {
            const auto& p = c.px[size_t(y) * glyph.w + x];
            row[x * 4 + 0] = uint8_t(std::lround(std::clamp(p.r, 0.0f, 1.0f) * 255));
            row[x * 4 + 1] = uint8_t(std::lround(std::clamp(p.g, 0.0f, 1.0f) * 255));
            row[x * 4 + 2] = uint8_t(std::lround(std::clamp(p.b, 0.0f, 1.0f) * 255));
            row[x * 4 + 3] = uint8_t(std::lround(std::clamp(p.a, 0.0f, 1.0f) * 255));
        }
    }
}
}
int Apply(TextureResource& entry)
{
    const Atlas* atlas = FindAtlas(entry.image);
    if (!atlas) return -1;
    const uint32_t generation = pcinput::PromptGeneration();
    if (pcinput::KeyboardPrompts())
    {
        const auto d = settings::Get();
        for (const auto& glyph : atlas->glyphs) Draw(entry.image, glyph, d);
        // Mip levels would still show the controller glyphs when minified.
        entry.mips.clear();
        kernel::Log("native prompts: keyboard / mouse glyphs drawn into UI atlas %d", int(atlas - kAtlases));
    }
    return int(generation & 0x7FFFFFFF);
}
}
#endif
