#pragma once
namespace xe::ui { class Window; class WindowedAppContext; }
namespace xe::gpu { class GraphicsSystem; }
namespace settingsui {
void ConfigureGraphics();
bool Initialize(xe::ui::Window* window, xe::gpu::GraphicsSystem* graphics,
                xe::ui::WindowedAppContext* context);
}
