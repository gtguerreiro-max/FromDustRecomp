#include "settings_ui.h"
#ifdef FD_XENIA_GPU
#include "../settings.h"
#include "../input.h"
#include "gpu.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/ui/graphics_provider.h"
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
xe::gpu::GraphicsSystem* g_graphics;
xe::ui::WindowedAppContext* g_context;
std::unique_ptr<xe::ui::ImmediateDrawer> g_immediate;
std::unique_ptr<xe::ui::ImGuiDrawer> g_imgui;
settings::Data g_applied;
bool g_appliedValid = false;

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
    auto config = g_graphics->presenter()->GetGuestOutputPaintConfigFromUIThread();
    using Effect = xe::ui::Presenter::GuestOutputPaintConfig::Effect;
    config.SetEffect(d.presentation == 1 ? Effect::kFsr : d.presentation == 2 ? Effect::kCas : Effect::kBilinear);
    config.SetCasAdditionalSharpness(d.sharpness / 100.0f);
    config.SetFsrSharpnessReduction(2.0f * (1.0f - d.sharpness / 100.0f));
    g_graphics->presenter()->SetGuestOutputPaintConfigFromUIThread(config);
    g_applied = d;
    g_appliedValid = true;
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
        g_window->AddInputListener(this, 2);
        g_window->AddListener(this);
        const char* open = getenv("FD_SETTINGS_OPEN");
        if (open && atoi(open)) Toggle();
    }
private:
    bool visible_ = false, saveFailed_ = false, restartFailed_ = false;
    int capture_ = -1;
    settings::Data startup_;
    uint64_t previousFrames_ = 0;
    float fps_ = 0;
    std::chrono::steady_clock::time_point sample_ = std::chrono::steady_clock::now();
    void Toggle() {
        visible_ = !visible_;
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
        }
        auto d = settings::Get();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 18, 18), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.88f);
        constexpr auto hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        ImGui::Begin("##pc_hud", nullptr, hudFlags);
        if (ImGui::Button(visible_ ? "Fechar  [F1]" : "Definições  [F1]")) Toggle();
        if (d.showFps) { ImGui::SameLine(); ImGui::Text("%.0f FPS", fps_); }
        ImGui::End();
        if (!visible_) return;

        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
        const ImVec2 size(std::min(780.0f, io.DisplaySize.x - 36), std::min(650.0f, io.DisplaySize.y - 88));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f + 12), ImGuiCond_Always, ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        constexpr auto panelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("##pc_settings", nullptr, panelFlags);
        ImGui::TextColored(ImVec4(.88f, .76f, .47f, 1), "FROM DUST  /  DEFINIÇÕES DE PC");
        ImGui::TextDisabled("F1 abre ou fecha. As escolhas ficam guardadas neste PC.");
        ImGui::Spacing();
        const auto before = d;
        if (ImGui::BeginTabBar("settings_tabs")) {
            if (ImGui::BeginTabItem("Ecrã")) {
                ImGui::BeginChild("screen_options", ImVec2(0, -116));
                ImGui::Spacing();
                int mode = d.fullscreen ? 1 : 0;
                if (ImGui::Combo("Modo de ecrã", &mode, "Janela\0Ecrã inteiro sem margens\0")) d.fullscreen = mode == 1;
                ImGui::BeginDisabled(d.fullscreen);
                ImGui::Combo("Tamanho da janela", &d.windowPreset, "1280 × 720\0 1600 × 900\0 1920 × 1080\0");
                ImGui::EndDisabled();
                ImGui::TextDisabled("Ecrã inteiro usa a resolução do monitor. Atalho: F11.");
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::Checkbox("Mostrar contador de FPS", &d.showFps);
                ImGui::Combo("Apresentação da imagem", &d.presentation, "Suave (bilinear)\0FSR — ampliar e definir\0CAS — realçar detalhes\0");
                ImGui::BeginDisabled(d.presentation == 0);
                ImGui::SliderInt("Nitidez", &d.sharpness, 0, 100, "%d %%");
                ImGui::EndDisabled();
                ImGui::TextWrapped("Estas opções aplicam-se de imediato.");
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gráficos")) {
                ImGui::BeginChild("graphics_options", ImVec2(0, -116));
                ImGui::Spacing();
                ImGui::TextWrapped("Estas opções são aplicadas ao reiniciar o jogo.");
                ImGui::Spacing();
                int scale = d.scale - 1;
                if (ImGui::Combo("Resolução interna", &scale, "Nativa (1×)\0Alta (2×)\0Muito alta (3×)\0")) d.scale = scale + 1;
                ImGui::TextDisabled("2× e 3× aumentam o detalhe e o trabalho da GPU.");
                ImGui::Checkbox("Sincronização vertical (VSync)", &d.vsync);
                constexpr int limits[] = {30, 60, 120, 0};
                int limit = int(std::find(std::begin(limits), std::end(limits), d.frameLimit) - std::begin(limits));
                if (ImGui::Combo("Limite de FPS", &limit, "30\0 60\0 120\0Sem limite\0")) d.frameLimit = limits[limit];
                ImGui::TextDisabled("Com VSync, 'Sem limite' mantém o ritmo de 60 Hz.");
                constexpr int filters[] = {-1, 2, 3, 4, 5};
                int filter = int(std::find(std::begin(filters), std::end(filters), d.anisotropy) - std::begin(filters));
                if (ImGui::Combo("Filtragem de texturas", &filter, "Original do jogo\0Anisotrópica 2×\0Anisotrópica 4×\0Anisotrópica 8×\0Anisotrópica 16×\0")) d.anisotropy = filters[filter];
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controlos")) {
                ImGui::BeginChild("controls_options", ImVec2(0, -116));
                ImGui::Spacing();
                ImGui::Combo("Dispositivo", &d.inputMode, "Automático\0Teclado e rato\0Comando XInput\0");
                ImGui::SliderInt("Sensibilidade da câmara", &d.sensitivity, 25, 300, "%d %%");
                ImGui::SliderInt("Zona morta dos analógicos", &d.deadzone, 0, 30, "%d %%");
                ImGui::Checkbox("Inverter eixo vertical da câmara", &d.invertY);
                ImGui::Checkbox("Mover câmara com botão direito do rato premido", &d.mouseCamera);
                ImGui::Checkbox("Vibração do comando", &d.vibration);
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::TextUnformatted("TECLADO");
                ImGui::TextDisabled("Clica numa tecla para a alterar. Esc cancela a atribuição.");
                if (capture_ >= 0) ImGui::TextColored(ImVec4(.88f, .76f, .47f, 1), "Prime uma tecla para: %s", settings::kActionNames[capture_]);
                if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Ação", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableSetupColumn("Tecla", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    for (int i = 0; i < settings::ActionCount; ++i) {
                        ImGui::PushID(i);
                        ImGui::TableNextRow(); ImGui::TableNextColumn();
                        ImGui::TextUnformatted(settings::kActionNames[i]);
                        ImGui::TableNextColumn();
                        const auto text = capture_ == i ? std::string("A aguardar…") : settings::KeyName(d.keys[i]);
                        if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) capture_ = i;
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Som")) {
                ImGui::BeginChild("audio_options", ImVec2(0, -116));
                ImGui::Spacing();
                ImGui::SliderInt("Volume", &d.volume, 0, 100, "%d %%");
                ImGui::Checkbox("Silenciar quando a janela não está ativa", &d.muteUnfocused);
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (d != before) Changed(d);
        ImGui::Separator(); ImGui::Spacing();
        const bool restart = settings::GraphicsChanged(startup_, d);
        if (saveFailed_) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "Não foi possível guardar as definições.");
        else if (restartFailed_) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "Não foi possível reiniciar. Fecha e volta a abrir o jogo.");
        else if (restart) ImGui::TextColored(ImVec4(.88f, .76f, .47f, 1), "Gráficos guardados — reinicia para aplicar.");
        else ImGui::TextDisabled("Guardado automaticamente.");
        if (ImGui::Button("Repor predefinições")) { capture_ = -1; Changed(settings::Data{}); }
        ImGui::SameLine();
        if (restart && ImGui::Button("Reiniciar o jogo")) {
            saveFailed_ = !settings::Save();
            if (!saveFailed_) g_context->CallInUIThreadDeferred([this] { restartFailed_ = !Restart(); });
        }
        if (restart) ImGui::SameLine();
        if (ImGui::Button("Fechar")) Toggle();
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
bool Initialize(xe::ui::Window* window, xe::gpu::GraphicsSystem* graphics,
                xe::ui::WindowedAppContext* context) {
    g_window = window; g_graphics = graphics; g_context = context;
    pcinput::SetWindow(static_cast<xe::ui::Win32Window*>(window)->hwnd());
    g_immediate = graphics->provider()->CreateImmediateDrawer();
    if (!g_immediate) return false;
    g_immediate->SetPresenter(graphics->presenter());
    g_imgui = std::make_unique<xe::ui::ImGuiDrawer>(window, 1);
    g_imgui->SetPresenterAndImmediateDrawer(graphics->presenter(), g_immediate.get());
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
    g_overlay = std::make_unique<Overlay>(g_imgui.get());
    ApplyLive();
    return true;
}
}
#endif
