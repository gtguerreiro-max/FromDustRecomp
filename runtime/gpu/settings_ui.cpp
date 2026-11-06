#include "settings_ui.h"
#ifdef FD_XENIA_GPU
#include "../settings.h"
#include "../input.h"
#include "gpu.h"
#include "frame_timing.h"
#include "native/live_audit.h"
#ifdef FD_NATIVE_RENDERER
#include "native/upscaler.h"
#include "native/frame_generation.h"
#include "../game_loop.h"
#include "../latency.h"
#endif
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/window_win.h"

DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
DECLARE_uint32(font_size);

// ImGui's optional Xenia gamepad navigation isn't used by this runtime. The
// original drawer references these two methods even without an InputSystem.
namespace xe::hid {
X_RESULT InputSystem::GetCapabilities(uint32_t, uint32_t, X_INPUT_CAPABILITIES* caps) {
    if (caps) *caps = {};
    return X_ERROR_DEVICE_NOT_CONNECTED;
}
X_RESULT InputSystem::GetState(uint32_t, uint32_t, X_INPUT_STATE* state) {
    if (state) *state = {};
    return X_ERROR_DEVICE_NOT_CONNECTED;
}
}

namespace settingsui {
namespace {
xe::ui::Window* g_window;
xe::ui::GraphicsProvider* g_provider;
xe::ui::Presenter* g_presenter;
xe::ui::WindowedAppContext* g_context;
std::unique_ptr<xe::ui::ImmediateDrawer> g_immediate;
std::unique_ptr<xe::ui::ImGuiDrawer> g_imgui;
settings::Data g_applied;
bool g_appliedValid = false;
ImGuiStyle g_baseStyle;

void ApplyLive() {
    const auto d = settings::Get();
    const bool resize = !g_appliedValid || d.windowPreset != g_applied.windowPreset ||
                        (g_applied.fullscreen && !d.fullscreen);
    if (g_window->IsFullscreen() != d.fullscreen) g_window->SetFullscreen(d.fullscreen);
    if (!d.fullscreen && resize) {
        HWND hwnd = static_cast<xe::ui::Win32Window*>(g_window)->hwnd();
        const int width = int(g_window->SizeToPhysical(settings::kWindowWidths[d.windowPreset]));
        const int height = int(g_window->SizeToPhysical(settings::kWindowHeights[d.windowPreset]));
        if (g_window->GetActualPhysicalWidth() != width || g_window->GetActualPhysicalHeight() != height) {
            RECT rect{0, 0, width, height};
            AdjustWindowRectExForDpi(&rect, DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                                    GetMenu(hwnd) != nullptr, DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                                    g_window->GetDpi());
            SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    auto config = g_presenter->GetGuestOutputPaintConfigFromUIThread();
    using Effect = xe::ui::Presenter::GuestOutputPaintConfig::Effect;
    config.SetEffect(d.presentation == 1 ? Effect::kFsr : d.presentation == 2 ? Effect::kCas : Effect::kBilinear);
#ifdef FD_NATIVE_RENDERER
    // FSR and CAS work on SDR values; the HDR guest output is linear scRGB.
    if (gpu::native::HdrActive()) config.SetEffect(Effect::kBilinear);
#endif
    config.SetCasAdditionalSharpness(d.sharpness / 100.0f);
    config.SetFsrSharpnessReduction(2.0f * (1.0f - d.sharpness / 100.0f));
    g_presenter->SetGuestOutputPaintConfigFromUIThread(config);
#ifdef FD_NATIVE_RENDERER
    gpu::native::SetUpscalerSetting(gpu::native::UpscalerMode(d.upscaler));
    gpu::native::SetRenderScaleSetting(d.renderScale / 100.0);
    gpu::native::SetAnisotropySetting(d.anisotropy > 0 ? 1u << (d.anisotropy - 1) : 1u);
    gpu::native::SetShadowScaleSetting(uint32_t(d.shadowScale));
    gpu::native::SetHdrLevels(float(d.hdrPaperWhite), float(d.hdrPeak));
    xe::ui::d3d12::fd_present_hooks.hdr_capture_scale = 80.0f / float(d.hdrPaperWhite);
    if (gpu::native::fg::Loaded()) gpu::native::fg::SetFrames(gpu::native::fg::ConfiguredFrames(d.frameGeneration));
    gameloop::SetFrameGenerationActive(gpu::native::fg::Loaded() && gpu::native::fg::ConfiguredFrames(d.frameGeneration) > 0);
    // Fixed-rate world: 30 world steps per second, the game loop up to 120 FPS.
    gameloop::SetSimulationRate(d.decoupledSim ? 30.0 : 0.0);
    gpu::SetVblankRate(d.decoupledSim ? 240 : 60);
#endif
    g_applied = d;
    g_appliedValid = true;
}
// Explanation shown when the previous widget is hovered.
void Help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void Section(const char* title) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(.88f, .76f, .47f, 1), "%s", title);
    ImGui::Separator();
    ImGui::Spacing();
}
bool Restart() {
    std::wstring command = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        ExitProcess(0);
    }
    return false;
}

class Overlay final : public xe::ui::ImGuiDialog, public xe::ui::WindowInputListener,
                      public xe::ui::WindowListener {
public:
    explicit Overlay(xe::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer), startup_(settings::Get()) {
        const char* backend=getenv("FD_RENDERER");
        nativeBackend_=backend&&!strcmp(backend,"native");
        g_window->AddInputListener(this, 2);
        g_window->AddListener(this);
        const char* open = getenv("FD_SETTINGS_OPEN");
        if (open && atoi(open)) Toggle();
        const char* tab=getenv("FD_SETTINGS_TAB");
        selectTab_=tab?tab:"";
        auto* provider=static_cast<xe::ui::d3d12::D3D12Provider*>(g_provider);
        adapterName_=provider->GetAdapterDescription();
        const auto luid=provider->GetDevice()->GetAdapterLuid();
        for(UINT i=0;;++i) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if(FAILED(provider->GetDXGIFactory()->EnumAdapters1(i,&adapter)))break;
            DXGI_ADAPTER_DESC1 desc{};
            if(SUCCEEDED(adapter->GetDesc1(&desc))&&desc.AdapterLuid.HighPart==luid.HighPart&&desc.AdapterLuid.LowPart==luid.LowPart) {
                adapter.As(&adapter_);break;
            }
        }
    }
private:
    bool visible_ = false, saveFailed_ = false, restartFailed_ = false;
public:
    bool Visible() const { return visible_; }
private:
    int capture_ = -1;
    settings::Data startup_;
    uint64_t previousFrames_ = 0;
    float fps_ = 0;
    int appliedUiScale_=0;
    bool memoryAvailable_=false;
    std::string selectTab_;
    bool nativeBackend_=false;
    std::string adapterName_;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory_{};
    gpu::FrameTimingSnapshot timing_;
    gpu::native::audit::Status native_;
    std::chrono::steady_clock::time_point sample_ = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    void Toggle() {
        visible_ = !visible_;
#ifdef FD_NATIVE_RENDERER
        gpu::native::fg::SetMenuOpen(visible_);
#endif
        capture_ = -1;
        pcinput::SetBlocked(visible_);
    }
    void Changed(settings::Data d) {
        settings::Set(d);
        saveFailed_ = !settings::Save();
        pcinput::ClearKeys();
        // Resizing can re-enter presenter code; apply after the current paint.
        g_context->CallInUIThreadDeferred(ApplyLive);
    }
    void OnKeyDown(xe::ui::KeyEvent& e) override {
        const uint32_t key = uint32_t(e.virtual_key());
        if (e.prev_state()) return;
        if (capture_ >= 0) {
            if (key == VK_ESCAPE) capture_ = -1;
            else if (settings::Bind(settings::Action(capture_), uint16_t(key))) {
                capture_ = -1;
                saveFailed_ = !settings::Save();
            }
            e.set_handled(true);
        } else if (key == VK_F1 || (key == VK_ESCAPE && visible_)) {
            Toggle();
            e.set_handled(true);
        } else if (key == VK_F11 || (key == VK_RETURN && e.is_alt_pressed())) {
            auto d = settings::Get(); d.fullscreen = !d.fullscreen; Changed(d);
            e.set_handled(true);
        } else if (!visible_) pcinput::HandleKey(key, true);
    }
    void OnKeyUp(xe::ui::KeyEvent& e) override { pcinput::HandleKey(uint32_t(e.virtual_key()), false); }
    void OnMouseWheel(xe::ui::MouseEvent& e) override {
        if (!visible_) { pcinput::HandleWheel(e.scroll_y()); e.set_handled(true); }
    }
    void OnLostFocus(xe::ui::UISetupEvent&) override {
        pcinput::ClearKeys();
        for (uint32_t user = 0; user < 4; ++user) pcinput::SetVibration(user, 0, 0);
    }
    void OnDraw(ImGuiIO& io) override {
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - sample_).count();
        if (elapsed >= 1.0f) {
            const auto frames = gpu::g_stats.frames.load();
            fps_ = float(frames - previousFrames_) / elapsed;
            previousFrames_ = frames; sample_ = now;
            timing_=gpu::FrameTimings().Snapshot();
            memoryAvailable_=adapter_&&SUCCEEDED(adapter_->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory_));
            const auto status=gpu::native::audit::GetStatus();
            if(!status.busy)native_=status;
        }
        auto d = settings::Get();
        {
            using Visibility = xe::ui::Window::CursorVisibility;
            const auto wanted = !visible_ && pcinput::MouseCaptured() ? Visibility::kHidden : Visibility::kVisible;
            if (g_window->GetCursorVisibility() != wanted) g_window->SetCursorVisibility(wanted);
        }
        const float uiScale=d.uiScale/100.0f;
        io.FontGlobalScale=uiScale;
        if(appliedUiScale_!=d.uiScale){ImGui::GetStyle()=g_baseStyle;ImGui::GetStyle().ScaleAllSizes(uiScale);appliedUiScale_=d.uiScale;}
        const bool left=d.hudCorner==1||d.hudCorner==3,bottom=d.hudCorner>=2;
        ImGui::SetNextWindowPos(ImVec2(left?18:io.DisplaySize.x-18,bottom?io.DisplaySize.y-18:18),ImGuiCond_Always,ImVec2(left?0:1,bottom?1:0));
        ImGui::SetNextWindowBgAlpha(0.88f);
        constexpr auto hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        // A short hint at start-up; afterwards only the enabled counters.
        const bool hint = now - started_ < std::chrono::seconds(15) && !visible_;
        if (hint || d.showFps || d.showFrameTime) {
        ImGui::Begin("##pc_hud", nullptr, hudFlags);
        bool first = true;
        auto item = [&] { if (!first) ImGui::SameLine(); first = false; };
        if (hint) { item(); ImGui::TextDisabled("F1  Settings"); }
        if (d.showFps) {
            item();
#ifdef FD_NATIVE_RENDERER
            const auto fgs = gpu::native::fg::GetStatus();
            if (fgs.active && fgs.presentedPerFrame > 1) ImGui::Text("%.0f FPS  (DLSS FG %u×: %.0f)", fps_, fgs.presentedPerFrame, fps_ * fgs.presentedPerFrame);
            else
#endif
            ImGui::Text("%.0f FPS", fps_);
        }
        if(d.showFrameTime&&timing_.count){item();ImGui::Text("%.1f ms",timing_.meanMs);}
        if(d.showFrameTime){const auto lat=latency::Get();if(lat.samples){item();ImGui::Text("| latency %.0f ms",lat.averageMs);}}
        ImGui::End();
        }
        if (!visible_) return;

        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
        const ImVec2 size(std::min(820.0f*uiScale, io.DisplaySize.x - 36), std::min(680.0f*uiScale, io.DisplaySize.y - 88));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f + 12), ImGuiCond_Always, ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        constexpr auto panelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("##pc_settings", nullptr, panelFlags);
        ImGui::TextColored(ImVec4(.88f, .76f, .47f, 1), "FROM DUST  /  PC SETTINGS");
        ImGui::TextDisabled("F1 opens or closes this menu. Choices are saved on this PC.");
        ImGui::Spacing();
        const auto before = d;
        if (ImGui::BeginTabBar("settings_tabs")) {
            if (ImGui::BeginTabItem("Display",nullptr,selectTab_=="screen"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("screen_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                int mode = d.fullscreen ? 1 : 0;
                Section("WINDOW");
                if (ImGui::Combo("Display mode", &mode, "Windowed\0Borderless fullscreen\0")) d.fullscreen = mode == 1;
                Help("Borderless fullscreen covers the monitor without changing its resolution. Shortcuts: F11 or Alt+Enter.");
                ImGui::BeginDisabled(d.fullscreen);
                ImGui::Combo("Window size", &d.windowPreset, "1280 × 720\0 1600 × 900\0 1920 × 1080\0 2560 × 1440\0 3840 × 2160\0");
                ImGui::EndDisabled();
                Section("INTERFACE");
                ImGui::Checkbox("Show FPS counter", &d.showFps);
                ImGui::Checkbox("Show frame time and latency",&d.showFrameTime);
                ImGui::Combo("Counter position",&d.hudCorner,"Top right\0Top left\0Bottom right\0Bottom left\0");
                ImGui::SliderInt("Menu size",&d.uiScale,75,150,"%d %%");
                Section("PRESENTATION");
                ImGui::Combo("Image scaling", &d.presentation, "Smooth (bilinear)\0FSR (upscale and sharpen)\0CAS (sharpen detail)\0");
                Help("How the game image is scaled to the window. FSR and CAS sharpen detail; Sharpness sets the strength.");
                ImGui::BeginDisabled(d.presentation == 0);
                ImGui::SliderInt("Sharpness", &d.sharpness, 0, 100, "%d %%");
                ImGui::EndDisabled();
                ImGui::TextWrapped("These options apply immediately.");
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graphics",nullptr,selectTab_=="graphics"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("graphics_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                Section("ANTI-ALIASING AND UPSCALING");
                {
#ifdef FD_NATIVE_RENDERER
                    const auto status = gpu::native::GetUpscalerStatus();
#endif
                    ImGui::BeginDisabled(!nativeBackend_);
                    constexpr int scales[] = {100, 150, 200, 250, 300};
                    int scaleIndex = int(std::find(std::begin(scales), std::end(scales), d.renderScale) - std::begin(scales));
                    if (scaleIndex >= 5) scaleIndex = 0;
                    if (ImGui::Combo("Internal resolution", &scaleIndex,
                            "Original (1024×600)\0" "1.5× (1536×900)\0" "2× (2048×1200)\0" "2.5× (2560×1500)\0" "3× (3072×1800)\0"))
                        d.renderScale = scales[scaleIndex];
                    Help("Renders the game at a higher resolution (sharper image, more GPU work). "
                         "The terrain and water simulation keeps its original resolution. Applies immediately.");
                    ImGui::Combo("Anti-aliasing / DLSS", &d.upscaler,
                                 "Game default\0" "NVIDIA DLAA\0" "DLSS Quality\0" "DLSS Balanced\0" "DLSS Performance\0" "DLSS Ultra Performance\0");
                    ImGui::EndDisabled();
                    Help("DLAA uses NVIDIA DLSS at the internal resolution to smooth edges and stabilise the image. "
                         "The DLSS modes render the scene below the internal resolution (67%, 58%, 50% or 33%) and reconstruct it: "
                         "with a 2× internal resolution and DLSS Performance, the scene is rendered at 1024×600 and shown at 2048×1200. "
                         "Needs an NVIDIA RTX GPU and the native renderer. Applies immediately.");
                    if (!nativeBackend_) ImGui::TextDisabled("Available with the native renderer (Jogar-nativo.cmd).");
#ifdef FD_NATIVE_RENDERER
                    else if (d.upscaler == 0) ImGui::TextDisabled("Off: the game's own image.");
                    else if (status.available == 0) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "DLSS unavailable: needs an NVIDIA RTX GPU, a recent driver and nvngx_dlss.dll.");
                    else if (status.available < 0 || !status.sceneFound) ImGui::TextDisabled("Active once a 3D scene is shown (menus and videos are unchanged).");
                    else ImGui::TextColored(ImVec4(.55f, .85f, .55f, 1), "%s active  |  %u×%u → %u×%u  |  %llu frames  |  camera %s",
                                            gpu::native::UpscalerModeName(status.active), status.renderWidth, status.renderHeight,
                                            status.outputWidth, status.outputHeight, (unsigned long long)status.evaluatedFrames,
                                            status.cameraFound ? "found" : "not found (no motion vectors)");
#endif
                }
                Section("IMAGE QUALITY");
                if (nativeBackend_) {
                    constexpr int filters[] = {-1, 2, 3, 4, 5};
                    int filter = int(std::find(std::begin(filters), std::end(filters), d.anisotropy) - std::begin(filters));
                    if (filter >= 5) filter = 0;
                    if (ImGui::Combo("Texture filtering", &filter, "Game default\0Anisotropic 2×\0Anisotropic 4×\0Anisotropic 8×\0Anisotropic 16×\0"))
                        d.anisotropy = filters[filter];
                    Help("Anisotropic filtering keeps the ground sharp where it is seen at a low angle (the far camera looks "
                         "across the terrain). Applies to the game's mipmapped textures; very little GPU cost. Applies immediately.");
                    constexpr int shadows[] = {1, 2, 4};
                    int shadow = int(std::find(std::begin(shadows), std::end(shadows), d.shadowScale) - std::begin(shadows));
                    if (shadow >= 3) shadow = 0;
                    if (ImGui::Combo("Shadow resolution", &shadow, "Original (512×512)\0" "2× (1024×1024)\0" "4× (2048×2048)\0"))
                        d.shadowScale = shadows[shadow];
                    Help("The sun's shadows use four 512×512 shadow maps, blocky next to a high internal resolution. "
                         "2× and 4× render them at 1024 or 2048 pixels: sharper shadow edges and fewer jagged steps. "
                         "Applies immediately.");
                } else ImGui::TextDisabled("Available with the native renderer.");
                Section("HDR");
                if (nativeBackend_) {
#ifdef FD_NATIVE_RENDERER
                    int display = -1; float displayNits = 0;
                    gpu::native::HdrDisplay(display, displayNits);
                    ImGui::Checkbox("HDR output", &d.hdr);
                    Help("Shows the game in HDR on an HDR display (Windows HDR on). The image keeps its look at the "
                         "'Paper white' brightness; the bright parts of the scene (sky, sun on the water, lava) rise towards "
                         "'Peak brightness'. The HUD, subtitles and menus stay at paper white. Applies on restart.");
                    if (gpu::native::HdrActive())
                        ImGui::TextColored(ImVec4(.55f, .85f, .55f, 1), "HDR active (scRGB)  |  display peak %s",
                                           displayNits > 0 ? (std::to_string(int(displayNits)) + " nits").c_str() : "unknown");
                    else if (d.hdr && display == 0)
                        ImGui::TextColored(ImVec4(1, .8f, .4f, 1), "Windows HDR is off for this display: turn on 'Use HDR' in Windows Settings > Display, then restart.");
                    else if (d.hdr) ImGui::TextColored(ImVec4(1, .8f, .4f, 1), "Restart the game to turn HDR on.");
                    ImGui::BeginDisabled(!d.hdr);
                    ImGui::SliderInt("Paper white", &d.hdrPaperWhite, 80, 400, "%d nits");
                    Help("Brightness of white in the game's normal image (mid tones, HUD, menus). About 200 nits suits a lit "
                         "room; lower it in a dark room. Applies immediately.");
                    ImGui::SliderInt("Peak brightness", &d.hdrPeak, 400, 2000, "%d nits");
                    Help("Brightness of the strongest highlights. Set it to your display's peak brightness (Windows reports it "
                         "next to 'HDR active'). Applies immediately.");
                    ImGui::EndDisabled();
#endif
                } else ImGui::TextDisabled("Available with the native renderer.");
                Section("SMOOTHNESS");
                {
                    ImGui::BeginDisabled(!nativeBackend_);
                    ImGui::Checkbox("Fixed-rate world simulation (experimental)", &d.decoupledSim);
                    ImGui::EndDisabled();
                    Help("The original game advances the world (water, terrain, people) one step per frame, so it is locked to 30 FPS. "
                         "With this option the world always advances 30 times per second and the rest of the game (camera, image) runs at "
                         "the 'Frame limit' (60, 120 or none; applies on restart). Water and people still update at 30 Hz. "
                         "Applies immediately.");
                    if (nativeBackend_ && d.decoupledSim)
                    {
                        ImGui::TextColored(ImVec4(.55f, .85f, .55f, 1), "World at 30 Hz  |  image up to %s FPS",
                            d.frameLimit ? std::to_string((std::min)(d.frameLimit, 120)).c_str() : "120");
                        ImGui::Checkbox("Even frame pacing", &d.evenPacing);
                        Help("Holds 120, 60 or 30 FPS, the fastest of them the PC keeps up with, so each world step stays on screen "
                             "for the same time. Rates in between (say 45 or 55 FPS) show some steps for one frame and others for two: "
                             "water and people move in small jerks while the camera is smooth. Off: the plain frame limit. "
                             "Not used with DLSS Frame Generation, which paces the frames itself.");
                        if (const int even = gameloop::EvenPaceRate(); even > 0)
                            ImGui::TextDisabled("Now paced at %d FPS (frames take %.1f ms). Rates between 30, 60 and 120 would "
                                                "show the world's steps unevenly.", even, gameloop::EvenPaceWorkMs());
                    }
                    else if (!nativeBackend_) ImGui::TextDisabled("Available with the native renderer.");
                }
                Section("FRAME GENERATION");
                {
                    ImGui::BeginDisabled(!nativeBackend_);
                    ImGui::Combo("DLSS Frame Generation", &d.frameGeneration,
                                 "Off\0" "2× (DLSS FG)\0" "3× (DLSS Multi Frame)\0" "4× (DLSS Multi Frame)\0");
                    ImGui::EndDisabled();
                    Help("NVIDIA frame generation adds 1 to 3 generated images between real frames "
                         "(2×, 3× or 4× the game's frame rate on screen). Latency stays that of the real frames. "
                         "2× needs an RTX 40 or newer; 3× and 4× an RTX 50. It pauses while this menu is open. "
                         "Turning it on or off applies on restart; the multiplier changes immediately.");
#ifdef FD_NATIVE_RENDERER
                    if (nativeBackend_) {
                        const auto fgs = gpu::native::fg::GetStatus();
                        if (!gpu::native::fg::Loaded()) {
                            if (d.frameGeneration > 0) ImGui::TextColored(ImVec4(1, .8f, .4f, 1), "%s",
                                fgs.reason[0] ? fgs.reason : "Restart the game to enable it.");
                            else ImGui::TextDisabled("Off.");
                        } else if (fgs.available == 0) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "Unavailable: %s", fgs.reason);
                        else if (d.frameGeneration == 0) ImGui::TextDisabled("Off (restart to unload Streamline).");
                        else if (fgs.maxFrames && d.frameGeneration > fgs.maxFrames)
                            ImGui::TextColored(ImVec4(1, .8f, .4f, 1), "This GPU generates at most %d frame(s): %d×.", fgs.maxFrames, fgs.maxFrames + 1);
                        else if (fgs.taggedFrames) ImGui::TextColored(ImVec4(.55f, .85f, .55f, 1), "Active in game (%d×)  |  %llu frames  |  paused while this menu is open",
                                                                      (std::min)(d.frameGeneration, fgs.maxFrames ? fgs.maxFrames : 3) + 1, (unsigned long long)fgs.taggedFrames);
                        else ImGui::TextDisabled("Waiting for a 3D scene (menus and videos stay at the game's rate).");
                    }
#endif
                }
                Section("RENDERING (APPLIES ON RESTART)");
                if (!nativeBackend_) {
                    int scale = d.scale - 1;
                    if (ImGui::Combo("Internal resolution (Xenia)", &scale, "Native (1×)\0High (2×)\0Very high (3×)\0")) d.scale = scale + 1;
                    ImGui::TextDisabled("2× and 3× add detail and GPU work.");
                }
                ImGui::Checkbox("Vertical sync (VSync)", &d.vsync);
                Help("Syncs with the monitor and avoids tearing.");
                constexpr int limits[] = {30, 60, 120, 0};
                int limit = int(std::find(std::begin(limits), std::end(limits), d.frameLimit) - std::begin(limits));
                if (ImGui::Combo("Frame limit", &limit, "30\0 60\0 120\0None\0")) d.frameLimit = limits[limit];
                ImGui::TextDisabled("With VSync, 'None' follows the monitor's refresh rate.");
                if (!nativeBackend_) {
                    constexpr int filters[] = {-1, 2, 3, 4, 5};
                    int filter = int(std::find(std::begin(filters), std::end(filters), d.anisotropy) - std::begin(filters));
                    if (filter >= 5) filter = 0;
                    if (ImGui::Combo("Texture filtering", &filter, "Game default\0Anisotropic 2×\0Anisotropic 4×\0Anisotropic 8×\0Anisotropic 16×\0")) d.anisotropy = filters[filter];
                }
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls",nullptr,selectTab_=="controls"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("controls_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                ImGui::Combo("Device", &d.inputMode, "Automatic\0Keyboard and mouse\0XInput controller\0");
                Help("Automatic accepts keyboard, mouse and controller together; on-screen prompts follow the last device used.");
                Section("MOUSE");
                ImGui::Checkbox("Move the Breath with the mouse", &d.mouseBreath);
                Help("The cursor is hidden and mouse movement guides the Breath over the terrain. F1 shows the cursor.");
                ImGui::BeginDisabled(!d.mouseBreath);
                ImGui::Combo("Breath mouse control", &d.mouseMode, "Direct (follows the mouse)\0Classic (mouse as stick)\0");
                Help("Direct moves the Breath over the ground with the mouse, at the same rate in every direction (at 100 %, a sideways "
                     "movement matches the screen at the Breath), and keeps it where you leave it "
                     "(no snapping to villagers). It needs the native renderer; otherwise, or with the controller, the classic "
                     "mode is used. Classic turns mouse speed into the left stick, as the original PC port did.");
                ImGui::SliderInt(d.mouseMode == 0 ? "Breath sensitivity" : "Breath speed", &d.mouseSpeed, 25, 300, "%d %%");
                ImGui::BeginDisabled(d.mouseMode != 0);
                ImGui::Checkbox("Move the camera only at the screen edges", &d.edgeScroll);
                Help("Direct mode: the camera stays still while the Breath moves over the screen, and scrolls when the Breath "
                     "reaches an edge (faster the closer it gets). The Breath stays on screen. Off: the camera follows the "
                     "Breath, as in the original game.");
                ImGui::BeginDisabled(!d.edgeScroll);
                ImGui::SliderInt("Edge scroll speed", &d.edgeScrollSpeed, 25, 300, "%d %%");
                Help("How fast the camera scrolls with the Breath at the very edge of the screen.");
                ImGui::SliderInt("Edge zone", &d.edgeScrollBand, 3, 15, "%d %% of the screen");
                Help("How close to the edge the Breath must be for the camera to start scrolling.");
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                ImGui::Checkbox("Left button absorbs, right button releases", &d.mouseButtons);
                Help("Left = LT (absorb matter), right = RT (release). Hold them like the triggers.");
                ImGui::Combo("Rotate camera", &d.cameraButton, "Off\0Drag with the right button\0Drag with the middle button\0");
                Help("Hold the button and move the mouse to rotate the camera.");
                ImGui::BeginDisabled(d.cameraButton == 0);
                ImGui::SliderInt("Rotation speed", &d.mouseRotateSpeed, 25, 300, "%d %%");
                Help("Camera rotation by mouse drag (Camera sensitivity below also applies).");
                ImGui::Checkbox("Invert rotation left / right", &d.mouseRotateInvertX);
                ImGui::SameLine();
                ImGui::Checkbox("Invert up / down", &d.mouseRotateInvertY);
                ImGui::EndDisabled();
                ImGui::Combo("Mouse wheel", &d.wheelZoom, "Off\0Zoom\0Inverted zoom\0");
                Help("In the global view the wheel zooms the view straight in and out (it stays in the global view; "
                     "the camera keeps its angle). In the close camera it zooms like the right stick.");
                ImGui::BeginDisabled(d.wheelZoom == 0);
                ImGui::SliderInt("Zoom speed", &d.wheelZoomSpeed, 25, 300, "%d %%");
                Help("Zoom per wheel notch in the global view (100 %: 10 % closer or farther per notch).");
                ImGui::Checkbox("Remember the zoom", &d.rememberZoom);
                Help("Keeps the global view's zoom between sessions.");
                ImGui::EndDisabled();
                Section("CAMERA AND CONTROLLER");
                ImGui::Checkbox("Prefer the global view (RB camera)", &d.globalView);
                Help("Keeps the far camera, an overview that suits a PC screen and the mouse. When the game returns to the close "
                     "camera by itself (after cut scenes and tutorials), the global view comes back after two seconds. Press RB "
                     "to choose the close camera yourself; it then stays until you press RB again.");
                ImGui::SliderInt("Camera sensitivity", &d.sensitivity, 25, 300, "%d %%");
                ImGui::Checkbox("Invert camera Y axis", &d.invertY);
                ImGui::SliderInt("Stick dead zone", &d.deadzone, 0, 30, "%d %%");
                ImGui::Checkbox("Controller vibration", &d.vibration);
                Section("KEYBOARD");
                ImGui::TextDisabled("Click a key to change it. Esc cancels.");
                if (capture_ >= 0) ImGui::TextColored(ImVec4(.88f, .76f, .47f, 1), "Press a key for: %s", settings::kActionNames[capture_]);
                if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    for (int i = 0; i < settings::ActionCount; ++i) {
                        ImGui::PushID(i);
                        ImGui::TableNextRow(); ImGui::TableNextColumn();
                        ImGui::TextUnformatted(settings::kActionNames[i]);
                        ImGui::TableNextColumn();
                        const auto text = capture_ == i ? std::string("Waiting…") : settings::KeyName(d.keys[i]);
                        if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) capture_ = i;
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio",nullptr,selectTab_=="audio"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("audio_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                ImGui::SliderInt("Volume", &d.volume, 0, 100, "%d %%");
                ImGui::Checkbox("Mute when the window is not active", &d.muteUnfocused);
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Renderer and statistics",nullptr,selectTab_=="renderer"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("renderer_information",ImVec2(0,-116*uiScale));
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(.88f,.76f,.47f,1),"ACTIVE RENDERER");
                ImGui::TextUnformatted(nativeBackend_ ? "Native / Direct3D 12" : "Xenia / Direct3D 12");
                ImGui::TextWrapped("GPU: %s",adapterName_.empty()?"Unavailable":adapterName_.c_str());
                ImGui::Text("Window: %u × %u px  |  Internal scale: %d×",g_window->GetActualPhysicalWidth(),g_window->GetActualPhysicalHeight(),nativeBackend_?1:startup_.scale);
                ImGui::Text("VSync: %s  |  Limit: %s",startup_.vsync?"on":"off",startup_.frameLimit?std::to_string(startup_.frameLimit).c_str():"none");
                if(memoryAvailable_)ImGui::Text("Local GPU memory: %.0f / %.0f MiB (usage / budget)",memory_.CurrentUsage/1048576.0,memory_.Budget/1048576.0);
                else ImGui::TextDisabled("Local GPU memory: unavailable");
                ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
                ImGui::TextUnformatted("PERFORMANCE");
                if(timing_.count) {
                    ImGui::Text("%.1f FPS  |  Mean %.2f ms  |  P95 %.2f ms",timing_.fps,timing_.meanMs,timing_.p95Ms);
                    ImGui::Text("Longest interval: %.2f ms  |  Draw calls/frame: %.0f",timing_.maxMs,timing_.drawsPerFrame);
                    ImGui::PlotLines("##frame_intervals",timing_.milliseconds.data(),int(timing_.count),0,"Frame intervals (ms)",0,(std::max)(40.0f,float(timing_.maxMs)*1.1f),ImVec2(-1,90*uiScale));
                } else ImGui::TextDisabled("Collecting frame intervals…");
                ImGui::TextDisabled("Up to 240 game frames; these values do not measure GPU execution time.");
                ImGui::Text("Frames processed: %llu  |  Draw calls: %llu",gpu::g_stats.frames.load(),gpu::g_stats.draws.load());
                if(ImGui::Button("Clear history")){gpu::FrameTimings().Reset();timing_={};}
                ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
                ImGui::TextUnformatted("NATIVE INTEGRATION");
#ifdef FD_NATIVE_RENDERER
                ImGui::TextWrapped(nativeBackend_ ? "This session uses the native renderer." : "The native renderer is available for testing. This session uses Xenia.");
#else
                ImGui::TextWrapped("The native renderer is not included in this build.");
#endif
                if(native_.enabled) {
                    ImGui::Text("Catalog shaders: %llu  |  Recognised initializations: %llu / %llu",native_.catalogShaders,native_.matchedShaders,native_.shaderInitializations);
                    ImGui::Text("Draws without an identified shader: %llu",native_.unresolvedDraws);
                    if(native_.captureEnabled)ImGui::Text("Capture: %s  |  Geometry %llu  |  Clears %llu  |  Resolves %llu",native_.captureComplete?"complete":"waiting / running",native_.geometryPackets,native_.clearPackets,native_.resolvePackets);
                    if(native_.captureFailures)ImGui::TextColored(ImVec4(1,.5f,.4f,1),"Capture failures: %llu",native_.captureFailures);
                } else ImGui::TextDisabled("Native audit: off in this session.");
                ImGui::EndChild();ImGui::EndTabItem();
            }
            selectTab_.clear();
            ImGui::EndTabBar();
        }
        if (d != before) Changed(d);
        ImGui::Separator(); ImGui::Spacing();
        // The native renderer applies texture filtering immediately.
        settings::Data compared = d;
        if (nativeBackend_) compared.anisotropy = startup_.anisotropy;
        const bool restart = settings::GraphicsChanged(startup_, compared);
        if (saveFailed_) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "Could not save the settings.");
        else if (restartFailed_) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "Could not restart. Close and reopen the game.");
        else if (restart) ImGui::TextColored(ImVec4(.88f, .76f, .47f, 1), "Graphics saved; restart to apply.");
        else ImGui::TextDisabled("Saved automatically.");
        if (ImGui::Button("Restore defaults")) { capture_ = -1; Changed(settings::Data{}); }
        ImGui::SameLine();
        if (restart && ImGui::Button("Restart game")) {
            saveFailed_ = !settings::Save();
            if (!saveFailed_) g_context->CallInUIThreadDeferred([this] { restartFailed_ = !Restart(); });
        }
        if (restart) ImGui::SameLine();
        if (ImGui::Button("Close")) Toggle();
        ImGui::SameLine();
        ImGui::TextDisabled("F1 menu  ·  F11 fullscreen  ·  Esc close");
        ImGui::End();
    }
};
std::unique_ptr<Overlay> g_overlay;
}

void ConfigureGraphics() {
    const auto d = settings::Get();
    // Called before GPU / UI workers start; the values stay fixed until the
    // next process, avoiding cross-thread writes to Xenia's plain CVar fields.
    cvars::draw_resolution_scale_x = d.scale;
    cvars::draw_resolution_scale_y = d.scale;
    cvars::vsync = d.vsync;
    cvars::framerate_limit = uint64_t(d.frameLimit);
    cvars::anisotropic_override = d.anisotropy;
    cvars::font_size = 18;
    // Diagnostic overrides are read once, before any shaders are translated.
    if (const char* pointSnap = getenv("FD_GPU_POINT_SNAP"); pointSnap && *pointSnap)
        cvars::texture_point_snap = atoi(pointSnap) != 0;
    if (const char* halfPixel = getenv("FD_GPU_HALF_PIXEL"); halfPixel && *halfPixel)
        cvars::half_pixel_offset = atoi(halfPixel) != 0;
}
bool Initialize(xe::ui::Window* window, xe::ui::GraphicsProvider* provider,xe::ui::Presenter* presenter,
                xe::ui::WindowedAppContext* context) {
    g_window = window; g_provider=provider;g_presenter=presenter;g_context = context;
    pcinput::SetWindow(static_cast<xe::ui::Win32Window*>(window)->hwnd());
    g_immediate = provider->CreateImmediateDrawer();
    if (!g_immediate) return false;
    g_immediate->SetPresenter(presenter);
    g_imgui = std::make_unique<xe::ui::ImGuiDrawer>(window, 1);
    g_imgui->SetPresenterAndImmediateDrawer(presenter, g_immediate.get());
    auto& io = g_imgui->GetIO();
    io.IniFilename = nullptr;
    auto& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowPadding = ImVec2(20, 18);
    style.ItemSpacing = ImVec2(12, 12);
    style.FramePadding = ImVec2(12, 7);
    style.WindowRounding = 12;
    style.FrameRounding = 6;
    style.GrabRounding = 5;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.045f, .06f, .075f, .98f);
    style.Colors[ImGuiCol_Button] = ImVec4(.16f, .22f, .25f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.25f, .33f, .36f, 1);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(.88f, .76f, .47f, 1);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(.88f, .76f, .47f, 1);
    g_baseStyle=style;
    g_overlay = std::make_unique<Overlay>(g_imgui.get());
    // Only the open F1 menu needs continuous repaints (hover, animation); the
    // FPS counter repaints with each game frame. FD_UI_CONTINUOUS=1: always.
    if (!getenv("FD_UI_CONTINUOUS")) xe::ui::fd_imgui_continuous_repaint = [] { return g_overlay && g_overlay->Visible(); };
    ApplyLive();
    return true;
}
}
#endif
