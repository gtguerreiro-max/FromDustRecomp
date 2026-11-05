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
    config.SetCasAdditionalSharpness(d.sharpness / 100.0f);
    config.SetFsrSharpnessReduction(2.0f * (1.0f - d.sharpness / 100.0f));
    g_presenter->SetGuestOutputPaintConfigFromUIThread(config);
#ifdef FD_NATIVE_RENDERER
    gpu::native::SetUpscalerSetting(d.upscaler == 1 ? gpu::native::UpscalerMode::Dlaa : gpu::native::UpscalerMode::Off);
    if (gpu::native::fg::Loaded()) gpu::native::fg::SetFrames(gpu::native::fg::ConfiguredFrames(d.frameGeneration));
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
        ImGui::Begin("##pc_hud", nullptr, hudFlags);
        if (ImGui::Button(visible_ ? "Fechar  [F1]" : "Definições  [F1]")) Toggle();
        if (d.showFps) {
            ImGui::SameLine();
#ifdef FD_NATIVE_RENDERER
            const auto fgs = gpu::native::fg::GetStatus();
            if (fgs.active && fgs.presentedPerFrame > 1) ImGui::Text("%.0f FPS  (DLSS FG %u×: %.0f)", fps_, fgs.presentedPerFrame, fps_ * fgs.presentedPerFrame);
            else
#endif
            ImGui::Text("%.0f FPS", fps_);
        }
        if(d.showFrameTime&&timing_.count){ImGui::SameLine();ImGui::Text("%.1f ms",timing_.meanMs);}
        ImGui::End();
        if (!visible_) return;

        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
        const ImVec2 size(std::min(820.0f*uiScale, io.DisplaySize.x - 36), std::min(680.0f*uiScale, io.DisplaySize.y - 88));
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
            if (ImGui::BeginTabItem("Ecrã",nullptr,selectTab_=="screen"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("screen_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                int mode = d.fullscreen ? 1 : 0;
                Section("JANELA");
                if (ImGui::Combo("Modo de ecrã", &mode, "Janela\0Ecrã inteiro sem margens\0")) d.fullscreen = mode == 1;
                Help("Ecrã inteiro sem margens ocupa o monitor sem mudar a sua resolução. Atalhos: F11 ou Alt+Enter.");
                ImGui::BeginDisabled(d.fullscreen);
                ImGui::Combo("Tamanho da janela", &d.windowPreset, "1280 × 720\0 1600 × 900\0 1920 × 1080\0 2560 × 1440\0 3840 × 2160\0");
                ImGui::EndDisabled();
                Section("INTERFACE");
                ImGui::Checkbox("Mostrar contador de FPS", &d.showFps);
                ImGui::Checkbox("Mostrar tempo entre frames",&d.showFrameTime);
                ImGui::Combo("Posição do HUD",&d.hudCorner,"Canto superior direito\0Canto superior esquerdo\0Canto inferior direito\0Canto inferior esquerdo\0");
                ImGui::SliderInt("Tamanho da interface",&d.uiScale,75,150,"%d %%");
                Section("APRESENTAÇÃO");
                ImGui::Combo("Apresentação da imagem", &d.presentation, "Suave (bilinear)\0FSR — ampliar e definir\0CAS — realçar detalhes\0");
                Help("Como a imagem do jogo é ampliada para o tamanho da janela. FSR e CAS realçam o detalhe; a nitidez ajusta a intensidade.");
                ImGui::BeginDisabled(d.presentation == 0);
                ImGui::SliderInt("Nitidez", &d.sharpness, 0, 100, "%d %%");
                ImGui::EndDisabled();
                ImGui::TextWrapped("Estas opções aplicam-se de imediato.");
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gráficos",nullptr,selectTab_=="graphics"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("graphics_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                Section("ANTI-ALIASING E UPSCALING");
                {
#ifdef FD_NATIVE_RENDERER
                    const auto status = gpu::native::GetUpscalerStatus();
#endif
                    ImGui::BeginDisabled(!nativeBackend_);
                    ImGui::Combo("Anti-aliasing", &d.upscaler, "Original do jogo\0NVIDIA DLAA\0");
                    ImGui::EndDisabled();
                    Help("DLAA usa o DLSS da NVIDIA à resolução nativa para suavizar arestas e estabilizar a imagem. "
                         "Precisa de uma GPU NVIDIA RTX e do renderer nativo. Aplica-se de imediato.");
                    if (!nativeBackend_) ImGui::TextDisabled("Disponível com o renderer nativo (Jogar-nativo.cmd).");
#ifdef FD_NATIVE_RENDERER
                    else if (d.upscaler == 0) ImGui::TextDisabled("Desligado: imagem original do jogo.");
                    else if (status.available == 0) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "DLSS indisponível: requer GPU NVIDIA RTX, driver recente e nvngx_dlss.dll.");
                    else if (status.available < 0 || !status.sceneFound) ImGui::TextDisabled("Ativo quando houver uma cena 3D (menus e vídeos ficam iguais).");
                    else ImGui::TextColored(ImVec4(.55f, .85f, .55f, 1), "DLAA ativo  |  %llu frames  |  câmara %s", (unsigned long long)status.evaluatedFrames,
                                            status.cameraFound ? "detetada" : "não detetada (sem vetores)");
#endif
                }
                Section("GERAÇÃO DE FRAMES");
                {
                    ImGui::BeginDisabled(!nativeBackend_);
                    ImGui::Combo("DLSS Frame Generation", &d.frameGeneration,
                                 "Desligada " "2× (DLSS FG) " "3× (DLSS Multi Frame) " "4× (DLSS Multi Frame) ");
                    ImGui::EndDisabled();
                    Help("O jogo corre a 30 FPS; a geração de frames da NVIDIA cria 1 a 3 imagens intermédias entre cada "
                         "frame real, para 60, 90 ou 120 FPS no ecrã. A latência mantém-se a dos 30 FPS. "
                         "2× precisa de uma RTX 40 ou mais recente; 3× e 4× de uma RTX 50. Desliga-se sozinha com este menu aberto. "
                         "Ligar ou desligar aplica ao reiniciar; o multiplicador muda de imediato.");
#ifdef FD_NATIVE_RENDERER
                    if (nativeBackend_) {
                        const auto fgs = gpu::native::fg::GetStatus();
                        if (!gpu::native::fg::Loaded()) {
                            if (d.frameGeneration > 0) ImGui::TextColored(ImVec4(1, .8f, .4f, 1), "%s",
                                fgs.reason[0] ? fgs.reason : "Reinicia o jogo para ativar.");
                            else ImGui::TextDisabled("Desligada.");
                        } else if (fgs.available == 0) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "Indisponível: %s", fgs.reason);
                        else if (d.frameGeneration == 0) ImGui::TextDisabled("Desligada (reinicia para descarregar o Streamline).");
                        else if (fgs.maxFrames && d.frameGeneration > fgs.maxFrames)
                            ImGui::TextColored(ImVec4(1, .8f, .4f, 1), "Esta GPU gera no máximo %d frame(s): %d×.", fgs.maxFrames, fgs.maxFrames + 1);
                        else if (fgs.taggedFrames) ImGui::TextColored(ImVec4(.55f, .85f, .55f, 1), "Ativa no jogo (%d×)  |  %llu frames  |  em pausa com este menu aberto",
                                                                      (std::min)(d.frameGeneration, fgs.maxFrames ? fgs.maxFrames : 3) + 1, (unsigned long long)fgs.taggedFrames);
                        else ImGui::TextDisabled("À espera de uma cena 3D (menus e vídeos ficam a 30 FPS).");
                    }
#endif
                }
                Section("RENDERIZAÇÃO (APLICA AO REINICIAR)");
                int scale = nativeBackend_ ? 0 : d.scale - 1;
                ImGui::BeginDisabled(nativeBackend_);
                if (ImGui::Combo("Resolução interna", &scale, "Nativa (1×)\0Alta (2×)\0Muito alta (3×)\0")) d.scale = scale + 1;
                ImGui::EndDisabled();
                ImGui::TextDisabled(nativeBackend_ ? "Nesta versão do renderer nativo, a resolução interna é 1×." : "2× e 3× aumentam o detalhe e o trabalho da GPU.");
                ImGui::Checkbox("Sincronização vertical (VSync)", &d.vsync);
                Help("Sincroniza com o monitor e evita cortes na imagem. O jogo foi feito para 30 FPS; a física da água depende do ritmo dos frames.");
                constexpr int limits[] = {30, 60, 120, 0};
                int limit = int(std::find(std::begin(limits), std::end(limits), d.frameLimit) - std::begin(limits));
                if (ImGui::Combo("Limite de FPS", &limit, "30\0 60\0 120\0Sem limite\0")) d.frameLimit = limits[limit];
                ImGui::TextDisabled("Com VSync, 'Sem limite' mantém o ritmo de 60 Hz.");
                constexpr int filters[] = {-1, 2, 3, 4, 5};
                int filter = nativeBackend_ ? 0 : int(std::find(std::begin(filters), std::end(filters), d.anisotropy) - std::begin(filters));
                ImGui::BeginDisabled(nativeBackend_);
                if (ImGui::Combo("Filtragem de texturas", &filter, "Original do jogo\0Anisotrópica 2×\0Anisotrópica 4×\0Anisotrópica 8×\0Anisotrópica 16×\0")) d.anisotropy = filters[filter];
                ImGui::EndDisabled();
                if(nativeBackend_)ImGui::TextDisabled("Nesta versão do renderer nativo, é usada a filtragem do jogo.");
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controlos",nullptr,selectTab_=="controls"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("controls_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                ImGui::Combo("Dispositivo", &d.inputMode, "Automático\0Teclado e rato\0Comando XInput\0");
                Help("Automático aceita teclado, rato e comando ao mesmo tempo.");
                Section("RATO (ESTILO PC)");
                ImGui::Checkbox("Mover o Breath com o rato", &d.mouseBreath);
                Help("O cursor fica escondido e o movimento do rato guia o Breath pelo terreno. F1 ou Esc mostram o cursor.");
                ImGui::BeginDisabled(!d.mouseBreath);
                ImGui::SliderInt("Velocidade do Breath", &d.mouseSpeed, 25, 300, "%d %%");
                ImGui::EndDisabled();
                ImGui::Checkbox("Botão esquerdo aspira, direito larga", &d.mouseButtons);
                Help("Esquerdo = LT (aspirar matéria), direito = RT (largar). Mantém premido como nos gatilhos.");
                ImGui::Combo("Rodar a câmara", &d.cameraButton, "Desligado\0Arrastar com o botão direito\0Arrastar com o botão do meio\0");
                Help("Mantém o botão premido e move o rato para rodar a câmara.");
                ImGui::Combo("Roda do rato", &d.wheelZoom, "Desligada\0Zoom\0Zoom invertido\0");
                Section("CÂMARA E COMANDO");
                ImGui::SliderInt("Sensibilidade da câmara", &d.sensitivity, 25, 300, "%d %%");
                ImGui::Checkbox("Inverter eixo vertical da câmara", &d.invertY);
                ImGui::SliderInt("Zona morta dos analógicos", &d.deadzone, 0, 30, "%d %%");
                ImGui::Checkbox("Vibração do comando", &d.vibration);
                Section("TECLADO");
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
            if (ImGui::BeginTabItem("Som",nullptr,selectTab_=="audio"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("audio_options", ImVec2(0, -116*uiScale));
                ImGui::Spacing();
                ImGui::SliderInt("Volume", &d.volume, 0, 100, "%d %%");
                ImGui::Checkbox("Silenciar quando a janela não está ativa", &d.muteUnfocused);
                ImGui::EndChild(); ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Renderer e estatísticas",nullptr,selectTab_=="renderer"?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::BeginChild("renderer_information",ImVec2(0,-116*uiScale));
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(.88f,.76f,.47f,1),"RENDERER ATIVO");
                ImGui::TextUnformatted(nativeBackend_ ? "Nativo / Direct3D 12" : "Xenia / Direct3D 12");
                ImGui::TextWrapped("GPU: %s",adapterName_.empty()?"Indisponível":adapterName_.c_str());
                ImGui::Text("Janela: %u × %u px  |  Escala interna: %d×",g_window->GetActualPhysicalWidth(),g_window->GetActualPhysicalHeight(),nativeBackend_?1:startup_.scale);
                ImGui::Text("VSync: %s  |  Limite: %s",startup_.vsync?"ligado":"desligado",startup_.frameLimit?std::to_string(startup_.frameLimit).c_str():"sem limite");
                if(memoryAvailable_)ImGui::Text("Memória GPU local: %.0f / %.0f MiB (uso / orçamento)",memory_.CurrentUsage/1048576.0,memory_.Budget/1048576.0);
                else ImGui::TextDisabled("Memória GPU local: indisponível");
                ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
                ImGui::TextUnformatted("DESEMPENHO");
                if(timing_.count) {
                    ImGui::Text("%.1f FPS  |  Média %.2f ms  |  P95 %.2f ms",timing_.fps,timing_.meanMs,timing_.p95Ms);
                    ImGui::Text("Maior intervalo: %.2f ms  |  Pedidos de desenho/frame: %.0f",timing_.maxMs,timing_.drawsPerFrame);
                    ImGui::PlotLines("##frame_intervals",timing_.milliseconds.data(),int(timing_.count),0,"Intervalos entre frames (ms)",0,(std::max)(40.0f,float(timing_.maxMs)*1.1f),ImVec2(-1,90*uiScale));
                } else ImGui::TextDisabled("A recolher tempos entre frames…");
                ImGui::TextDisabled("Até 240 frames do jogo; estes valores não medem o tempo de execução da GPU.");
                ImGui::Text("Frames processados: %llu  |  Pedidos de desenho: %llu",gpu::g_stats.frames.load(),gpu::g_stats.draws.load());
                if(ImGui::Button("Limpar histórico")){gpu::FrameTimings().Reset();timing_={};}
                ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
                ImGui::TextUnformatted("INTEGRAÇÃO NATIVA");
#ifdef FD_NATIVE_RENDERER
                ImGui::TextWrapped(nativeBackend_ ? "Esta sessão usa o renderer nativo, em integração." : "O renderer nativo está disponível para testes. Esta sessão usa Xenia.");
#else
                ImGui::TextWrapped("O motor nativo não está incluído nesta compilação.");
#endif
                if(native_.enabled) {
                    ImGui::Text("Shaders no catálogo: %llu  |  Inicializações reconhecidas: %llu / %llu",native_.catalogShaders,native_.matchedShaders,native_.shaderInitializations);
                    ImGui::Text("Desenhos sem shader identificado: %llu",native_.unresolvedDraws);
                    if(native_.captureEnabled)ImGui::Text("Captura: %s  |  Geometria %llu  |  Limpezas %llu  |  Cópias %llu",native_.captureComplete?"concluída":"a aguardar / em curso",native_.geometryPackets,native_.clearPackets,native_.resolvePackets);
                    if(native_.captureFailures)ImGui::TextColored(ImVec4(1,.5f,.4f,1),"Falhas na captura: %llu",native_.captureFailures);
                } else ImGui::TextDisabled("Auditoria nativa: desligada nesta sessão.");
                ImGui::EndChild();ImGui::EndTabItem();
            }
            selectTab_.clear();
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
        ImGui::SameLine();
        ImGui::TextDisabled("F1 menu  ·  F11 ecrã inteiro  ·  Esc fechar");
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
    ApplyLive();
    return true;
}
}
#endif
