// Adapts Xenia's GPU backend to the statically recompiled From Dust runtime.
#include "xenia_bridge.h"

#ifdef FD_XENIA_GPU
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "gpu.h"
#include "settings_ui.h"
#include "../settings.h"
#include "../kernel/kernel.h"
#include "../kernel/thread.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context_win.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

namespace {
std::unique_ptr<xe::Memory> g_memory;
std::unique_ptr<xe::cpu::Processor> g_processor;
std::unique_ptr<xe::kernel::KernelState> g_kernel;
std::unique_ptr<xe::ui::Win32WindowedAppContext> g_context;
std::unique_ptr<xe::ui::Window> g_window;
xe::Emulator g_emulator;
std::mutex g_interruptMutex;
thread_local xe::kernel::XThread* g_hostThread = nullptr;
std::chrono::nanoseconds g_frameInterval{};

void PaceFrame() {
  if (g_frameInterval.count() == 0) return;
  static thread_local std::chrono::steady_clock::time_point next;
  const auto now = std::chrono::steady_clock::now();
  if (now < next) std::this_thread::sleep_until(next);
  // Drop missed deadlines rather than issuing a burst of catch-up frames.
  next = std::max(next, now) + g_frameInterval;
}

std::unique_ptr<xe::gpu::d3d12::D3D12GraphicsSystem> g_graphics;

class WindowListener final : public xe::ui::WindowListener {
  void OnClosing(xe::ui::UIEvent&) override { g_context->QuitFromUIThread(); }
} g_windowListener;

LONG WINAPI PhysicalWriteFault(EXCEPTION_POINTERS* info) {
  auto* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      record->NumberParameters < 2 || record->ExceptionInformation[0] != 1 || !g_memory)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t offset = record->ExceptionInformation[1] -
      reinterpret_cast<uintptr_t>(g_memory->physical_membase());
  if (offset >= 0x20000000 || !g_memory->OnPhysicalWrite(uint32_t(offset), 1))
    return EXCEPTION_CONTINUE_SEARCH;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Fallback for write pointer updates outside the hooked D3D kick, and the
// once-per-second FPS log.
void PollWritePointer() {
  uint32_t previous = UINT32_MAX;
  auto last = std::chrono::steady_clock::now();
  uint64_t lastFrames = 0;
  for (;;) {
    const uint32_t value = mem::Read32(gpu::kMmioBase + gpu::reg::CP_RB_WPTR * 4);
    if (value != previous) {
      g_graphics->command_processor()->UpdateWritePointer(value);
      previous = value;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(1)) {
      const auto frames = gpu::g_stats.frames.load();
      kernel::Log("gpu: %llu fps, %llu draws total (Direct3D 12)",
                  frames - lastFrames, gpu::g_stats.draws.load());
      lastFrames = frames;
      last = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
}

namespace xe {
void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* context) {
  auto global = global_critical_region::AcquireDirect();
  std::lock_guard lock(watch_mutex_);
  auto* entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(callback, context);
  invalidation_callbacks_.push_back(entry);
  return entry;
}
void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* handle) {
  auto global = global_critical_region::AcquireDirect();
  std::lock_guard lock(watch_mutex_);
  auto it = std::find(invalidation_callbacks_.begin(), invalidation_callbacks_.end(), handle);
  if (it != invalidation_callbacks_.end()) {
    delete *it;
    invalidation_callbacks_.erase(it);
  }
}
void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t address, uint32_t length,
    bool invalidation, bool dataProviders) {
  if (!invalidation || !length || address >= 0x20000000) return;
  const uint32_t first = address / 0x1000;
  const uint32_t end = uint32_t(std::min(uint64_t(address) + length + 0xFFF,
                                      uint64_t(0x20000000)) / 0x1000);
  auto global = global_critical_region::AcquireDirect();
  std::lock_guard lock(watch_mutex_);
  DWORD old;
  if (!VirtualProtect(physical_membase_ + first * 0x1000,
                      (end - first) * 0x1000, PAGE_READONLY, &old)) {
    ::kernel::Log("gpu: failed to watch physical memory: %lu", GetLastError());
    return;
  }
  std::fill(watched_pages_.begin() + first, watched_pages_.begin() + end, true);
}
bool Memory::OnPhysicalWrite(uint32_t address, uint32_t length) {
  if (!length || address >= 0x20000000) return false;
  // Cache callbacks acquire Xenia's global region. Acquire it before our
  // watch mutex too, matching the GPU's lock order and allowing re-entry.
  auto global = global_critical_region::AcquireDirect();
  std::lock_guard lock(watch_mutex_);
  const uint32_t first = address / 0x1000;
  const uint32_t end = uint32_t(std::min(uint64_t(address) + length + 0xFFF,
                                      uint64_t(0x20000000)) / 0x1000);
  for (uint32_t page = first; page < end; ++page) {
    if (!watched_pages_[page]) continue;
    for (const auto* entry : invalidation_callbacks_)
      entry->first(entry->second, page * 0x1000, 0x1000, true);
    DWORD old;
    if (!VirtualProtect(physical_membase_ + page * 0x1000, 0x1000, PAGE_READWRITE, &old))
      return false;
    watched_pages_[page] = false;
  }
  // Another writing thread may already have serviced the same page fault.
  return true;
}
}

namespace xe::kernel {
bool XThread::IsInThread(XThread* other) { return g_hostThread == other; }
bool XThread::IsInThread() { return g_hostThread != nullptr; }
X_STATUS XHostThread::Create() {
  threading::Thread::CreationParameters params;
  params.stack_size = std::max(size_t(stack_size_), size_t(4 * 1024 * 1024));
  // Assign thread_ before the new thread starts, so worker initialization can
  // safely access its own Thread object.
  params.create_suspended = true;
  thread_ = threading::Thread::Create(params, [this] {
    g_hostThread = this;
    ::kernel::AdoptHostThread(0x40000);
    host_fn_();
    g_hostThread = nullptr;
  });
  if (!thread_) return X_STATUS_UNSUCCESSFUL;
  thread_->set_name(name_);
  thread_->Resume();
  return X_STATUS_SUCCESS;
}
void KernelState::EmulateCPInterruptDPC(uint32_t callback, uint32_t userData,
                                       uint32_t source, uint32_t cpu) {
  if (!callback) return;
  std::lock_guard lock(g_interruptMutex);
  auto* thread = ::kernel::CurrentThread();
  if (!thread) thread = ::kernel::AdoptHostThread(0x40000);
  mem::Ptr<uint8_t>(thread->pcrAddress)[::kernel::pcr::kCurrentCpu] = uint8_t(cpu);
  if (source == 0)
    mem::Write32(::gpu::kMmioBase + ::gpu::reg::D1MODE_VBLANK_STATUS * 4, 1);
  ::kernel::CallGuestFunction(callback, source, userData);
}
}

namespace gpu::xenia {
bool Initialize() {
  const char* renderer = getenv("FD_RENDERER");
  if (renderer && !strcmp(renderer, "null")) return true;
  kernel::Log("gpu: initializing logging");
  xe::InitializeLogging("FromDust-GPU");
  kernel::Log("gpu: initializing clock and memory");
  xe::Clock::set_guest_tick_frequency(50000000);
  xe::Clock::set_guest_time_scalar(1.0);
  xe::Clock::set_guest_system_time_base(xe::Clock::QueryHostSystemTime());
  settingsui::ConfigureGraphics();
  g_memory = std::make_unique<xe::Memory>(mem::g_base, mem::g_base + mem::kPhysicalBase);
  g_memory->SetGpuTelemetry(&g_stats.frames, &g_stats.draws);
  const auto graphics = settings::Get();
  const int frameLimit = graphics.frameLimit ? graphics.frameLimit : (graphics.vsync ? 60 : 0);
  g_frameInterval = frameLimit ? std::chrono::nanoseconds(1000000000LL / frameLimit) : std::chrono::nanoseconds{};
  g_memory->SetGpuFramePacer(PaceFrame);
  g_processor = std::make_unique<xe::cpu::Processor>(g_memory.get());
  g_kernel = std::make_unique<xe::kernel::KernelState>(&g_emulator, g_processor.get());
  AddVectoredExceptionHandler(1, PhysicalWriteFault);
  const bool headless = getenv("FD_HEADLESS") && atoi(getenv("FD_HEADLESS"));
  if (!headless) {
    const bool hidden = getenv("FD_HIDE_WINDOW") && atoi(getenv("FD_HIDE_WINDOW"));
    g_context = std::make_unique<xe::ui::Win32WindowedAppContext>(
        GetModuleHandle(nullptr), hidden ? SW_HIDE : SW_SHOWNORMAL);
    if (!g_context->Initialize()) return false;
    const auto display = settings::Get();
    g_window = xe::ui::Window::Create(*g_context, "From Dust",
        settings::kWindowWidths[display.windowPreset], settings::kWindowHeights[display.windowPreset]);
    g_window->AddListener(&g_windowListener);
    if (!g_window->Open()) return false;
  }
  g_graphics = std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>();
  kernel::Log("gpu: creating Direct3D 12 device");
  if (g_graphics->Setup(g_processor.get(), g_kernel.get(), g_context.get(), true) != 0)
    return false;
  if (!g_graphics->provider() || !g_graphics->presenter()) return false;
  if (g_window) {
    g_window->SetPresenter(g_graphics->presenter());
    if (!settingsui::Initialize(g_window.get(), g_graphics.get(), g_context.get())) return false;
  }
  kernel::Log("gpu: Direct3D 12 backend initialized%s", headless ? " (offscreen)" : "");
  std::thread(PollWritePointer).detach();
  // Optional offscreen capture used to validate the rendered guest output.
  if (const char* path = getenv("FD_CAPTURE"); path && *path) {
    std::thread([path = std::string(path)] {
      const char* delay = getenv("FD_CAPTURE_SECONDS");
      std::this_thread::sleep_for(std::chrono::seconds(delay ? atoi(delay) : 10));
      xe::ui::RawImage image;
      if (g_graphics->presenter()->CaptureGuestOutput(image) && !image.data.empty()) {
        bool saved = stbi_write_png(path.c_str(), image.width, image.height, 4,
                                    image.data.data(), int(image.stride)) != 0;
        kernel::Log("gpu: capture %s (%ux%u): %s", path.c_str(), image.width, image.height,
                    saved ? "saved" : "failed");
      } else kernel::Log("gpu: no guest image available for capture");
    }).detach();
  }
  return true;
}
bool Active() { return g_graphics != nullptr; }
void NotifyWritePointer() {
  if (g_graphics)
    g_graphics->command_processor()->UpdateWritePointer(
        mem::Read32(gpu::kMmioBase + gpu::reg::CP_RB_WPTR * 4));
}
bool HasWindow() { return g_window != nullptr; }
int RunMessageLoop() { return g_context->RunMainMessageLoop(); }
void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2) {
  g_graphics->InitializeRingBuffer(address & 0x1FFFFFFF, sizeLog2);
}
void EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2) {
  g_graphics->EnableReadPointerWriteBack(address & 0x1FFFFFFF, blockLog2);
}
void SetInterruptCallback(uint32_t callback, uint32_t userData) {
  g_graphics->SetInterruptCallback(callback, userData);
}
void PrepareHostWrite(uint32_t address, uint32_t length) {
  if (g_memory && address >= mem::kPhysicalBase && address < mem::kPhysicalEnd)
    g_memory->OnPhysicalWrite(address - mem::kPhysicalBase, length);
}
}
#else
namespace gpu::xenia {
bool Initialize() { return true; }
bool Active() { return false; }
bool HasWindow() { return false; }
int RunMessageLoop() { return 0; }
void InitializeRingBuffer(uint32_t, uint32_t) {}
void EnableReadPointerWriteBack(uint32_t, uint32_t) {}
void SetInterruptCallback(uint32_t, uint32_t) {}
void PrepareHostWrite(uint32_t, uint32_t) {}
}
#endif
