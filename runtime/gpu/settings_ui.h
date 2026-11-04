#pragma once
namespace xe::ui { class Window; class WindowedAppContext; class GraphicsProvider; class Presenter; }
namespace xe::gpu { class GraphicsSystem; }
namespace settingsui {
void ConfigureGraphics();
bool Initialize(xe::ui::Window* window, xe::ui::GraphicsProvider* provider,xe::ui::Presenter* presenter,
                xe::ui::WindowedAppContext* context);
}
