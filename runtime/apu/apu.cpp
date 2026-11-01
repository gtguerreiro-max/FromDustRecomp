// Audio: the game's XAudio2 mixes into XMA hardware contexts and submits
// 6-channel frames to the kernel's render driver. Both halves follow Xenia's
// AudioSystem / XmaDecoder (BSD), reusing its XmaContextNew (FFmpeg) decoder
// and XAudio2 output driver.
//
// XMA registers live at 0x7FEA0000. The game writes them with stwbrx+eieio,
// which XenonRecomp emits as PPC_MM_STORE_U32 (routed to WriteRegister by
// ppc_overrides.h); the only register it reads is the context array address,
// which is plain memory here.

#include "apu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <bit>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "kernel/kernel.h"
#include "kernel/thread.h"
#include "memory.h"
#include "settings.h"

#if FD_XENIA_GPU
#include "xenia/apu/xaudio2/xaudio2_audio_driver.h"
#include "xenia/apu/xma_context_new.h"
#include "xenia/base/threading.h"
#include "xenia/memory.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace apu
{
namespace
{
constexpr uint32_t kXmaBase = 0x7FEA0000;
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kContextSize = 64;
// Register numbers (xma_register_table.inc).
constexpr uint32_t kRegContextArrayAddress = 0x600;
constexpr uint32_t kRegKick = 0x650, kRegLock = 0x690, kRegClear = 0x6A0;

constexpr size_t kMaxClients = 8;
constexpr uint32_t kMaxQueuedFrames = 64;
constexpr uint64_t kPumpIntervalUs = 5333; // 256 samples at 48 kHz

uint32_t g_contextArray = 0; // guest (virtual) address of the context array
std::mutex g_contextMutex;
bool g_contextUsed[kContextCount] = {};

#if FD_XENIA_GPU
std::unique_ptr<xe::Memory> g_memory;
std::unique_ptr<xe::apu::XmaContext> g_contexts[kContextCount];

struct Client
{
    bool inUse = false;
    uint32_t callback = 0;
    uint32_t wrappedArg = 0; // guest pointer holding the callback argument
    uint64_t nextPumpUs = 0;
    float volume = 1.0f;
    std::unique_ptr<xe::threading::Semaphore> semaphore;
    std::unique_ptr<xe::apu::xaudio2::XAudio2AudioDriver> driver;
};
std::mutex g_clientMutex;
Client g_clients[kMaxClients];
std::once_flag g_workerOnce;
// Diagnostics, logged every 10 s by the worker.
std::atomic<uint64_t> g_kicks{0}, g_frames{0};
std::atomic<float> g_peak{0.0f};

uint64_t NowUs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Calls each client's guest callback every 5.33 ms while the host voice has
// room; the callback mixes and submits one frame (XAudioSubmitRenderDriverFrame).
void WorkerMain()
{
    kernel::AdoptHostThread(0x40000);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint64_t nextReportUs = NowUs() + 10000000;
    for (;;)
    {
        if (NowUs() >= nextReportUs)
        {
            nextReportUs += 10000000;
            kernel::Log("apu: %llu XMA kicks, %llu frames, peak %.3f",
                (unsigned long long)g_kicks.exchange(0), (unsigned long long)g_frames.exchange(0), g_peak.exchange(0.0f));
        }
        uint32_t callback = 0, arg = 0;
        xe::threading::Semaphore* semaphore = nullptr;
        uint64_t deadline = UINT64_MAX;
        {
            std::lock_guard lock(g_clientMutex);
            Client* next = nullptr;
            for (auto& client : g_clients)
                if (client.inUse && client.nextPumpUs < deadline)
                    deadline = client.nextPumpUs, next = &client;
            if (next)
            {
                callback = next->callback;
                arg = next->wrappedArg;
                semaphore = next->semaphore.get();
                next->nextPumpUs = std::max(deadline, NowUs()) + kPumpIntervalUs;
            }
        }
        if (!callback)
        {
            Sleep(5);
            continue;
        }
        const uint64_t now = NowUs();
        if (deadline > now + 1000)
            Sleep(DWORD((deadline - now) / 1000));
        // Skip the pump when the host already holds 64 queued frames.
        if (xe::threading::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
            xe::threading::WaitResult::kSuccess)
            kernel::CallGuestFunction(callback, arg, 0);
    }
}
#endif

int ContextIndex(uint32_t guestPtr)
{
    if (!g_contextArray || guestPtr < g_contextArray || guestPtr >= g_contextArray + kContextCount * kContextSize)
        return -1;
    return int((guestPtr - g_contextArray) / kContextSize);
}
}

void Initialize()
{
    // Skip a page so no context sits at physical address 0.
    g_contextArray = mem::AllocPhysical(kContextCount * kContextSize + 0x1000, 0x1000) + 0x1000;
    // Read with lwbrx: stored little-endian.
    *mem::Ptr<uint32_t>(kXmaBase + kRegContextArrayAddress * 4) = g_contextArray & 0x1FFFFFFF;
#if FD_XENIA_GPU
    g_memory = std::make_unique<xe::Memory>(mem::g_base, mem::g_base + mem::kPhysicalBase);
    for (uint32_t i = 0; i < kContextCount; ++i)
    {
        g_contexts[i] = std::make_unique<xe::apu::XmaContextNew>();
        g_contexts[i]->Setup(i, g_memory.get(), g_contextArray + i * kContextSize);
    }
    for (auto& client : g_clients)
        client.semaphore = xe::threading::Semaphore::Create(0, kMaxQueuedFrames);
    kernel::Log("apu: %u XMA contexts at %08X", kContextCount, g_contextArray);
#endif
}

void WriteRegister(uint32_t address, uint32_t value)
{
    const uint32_t reg = (address & 0xFFFF) / 4;
    auto forEach = [&](uint32_t baseReg, auto&& fn) {
        const uint32_t first = (reg - baseReg) * 32;
        for (uint32_t bits = value; bits; bits &= bits - 1)
        {
            const uint32_t id = first + std::countr_zero(bits);
            if (id < kContextCount) fn(id);
        }
    };
#if FD_XENIA_GPU
    // Decoding runs synchronously on the kicking thread, like Xenia's worker
    // (which the kick waits for), so the game sees updated context data.
    if (reg >= kRegKick && reg < kRegKick + 10)
        forEach(kRegKick, [](uint32_t id) { ++g_kicks; g_contexts[id]->Enable(); g_contexts[id]->Work(); });
    else if (reg >= kRegLock && reg < kRegLock + 10)
        forEach(kRegLock, [](uint32_t id) { g_contexts[id]->Disable(); g_contexts[id]->Block(false); });
    else if (reg >= kRegClear && reg < kRegClear + 10)
        forEach(kRegClear, [](uint32_t id) { g_contexts[id]->Clear(); });
#else
    (void)forEach;
#endif
}
}

// stwbrx stores the byte-reversed register; `value` is the big-endian word.
extern "C" void FdMmioStore32(uint8_t* base, uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
    if ((address & 0xFFFF0000) == apu::kXmaBase)
        apu::WriteRegister(address, __builtin_bswap32(value));
}

using namespace apu;

KFUNC(XMACreateContext)
{
    std::lock_guard lock(g_contextMutex);
    for (uint32_t i = 0; i < kContextCount; ++i)
    {
        if (g_contextUsed[i]) continue;
        g_contextUsed[i] = true;
#if FD_XENIA_GPU
        g_contexts[i]->set_is_allocated(true);
#endif
        mem::Write32(ARG0, g_contextArray + i * kContextSize);
        RETURN(0);
    }
    mem::Write32(ARG0, 0);
    RETURN(0xC0000017); // STATUS_NO_MEMORY
}

KFUNC(XMAReleaseContext)
{
    const int index = ContextIndex(ARG0);
    if (index >= 0)
    {
#if FD_XENIA_GPU
        g_contexts[index]->Release();
#endif
        std::lock_guard lock(g_contextMutex);
        g_contextUsed[index] = false;
    }
    RETURN(0);
}

// The XMA library maps each context's physical address for uncached access.
KFUNC(MmMapIoSpace) { RETURN(mem::kPhysicalBase | (ARG1 & 0x1FFFFFFF)); }

KFUNC(XAudioGetSpeakerConfig) { mem::Write32(ARG0, 0x00010000); RETURN(0); } // Dolby Digital (5.1)

KFUNC(XAudioGetVoiceCategoryVolume)
{
    *mem::Ptr<be<float>>(ARG1) = 1.0f;
    RETURN(0);
}

KFUNC(XAudioRegisterRenderDriverClient)
{
    // callback_ptr -> { callback, callback_arg }
    const uint32_t callback = ARG0 ? mem::Read32(ARG0) : 0;
    if (!callback) RETURN(0x80070057); // E_INVALIDARG
    const uint32_t callbackArg = mem::Read32(ARG0 + 4);
#if FD_XENIA_GPU
    std::lock_guard lock(g_clientMutex);
    for (size_t i = 0; i < kMaxClients; ++i)
    {
        auto& client = g_clients[i];
        if (client.inUse) continue;
        client.semaphore->Release(kMaxQueuedFrames, nullptr);
        client.driver = std::make_unique<xe::apu::xaudio2::XAudio2AudioDriver>(client.semaphore.get());
        if (!client.driver->Initialize())
        {
            kernel::Log("apu: XAudio2 initialization failed");
            client.driver.reset();
            RETURN(0x80004005); // E_FAIL
        }
        client.callback = callback;
        client.wrappedArg = mem::AllocSystem(4, 4);
        mem::Write32(client.wrappedArg, callbackArg);
        client.nextPumpUs = 0;
        client.volume = 1.0f;
        client.inUse = true;
        mem::Write32(ARG1, 0x41550000 | uint32_t(i));
        kernel::Log("apu: render client %zu registered (callback %08X)", i, callback);
        std::call_once(g_workerOnce, [] { std::thread(WorkerMain).detach(); });
        RETURN(0);
    }
#endif
    RETURN(0x80004005);
}

KFUNC(XAudioUnregisterRenderDriverClient)
{
#if FD_XENIA_GPU
    const uint32_t index = ARG0 & 0xFFFF;
    std::lock_guard lock(g_clientMutex);
    if (index < kMaxClients && g_clients[index].inUse)
    {
        auto& client = g_clients[index];
        client.driver->Shutdown();
        client.driver.reset();
        client.inUse = false;
        client.callback = 0;
        while (xe::threading::Wait(client.semaphore.get(), false, std::chrono::milliseconds(0)) ==
               xe::threading::WaitResult::kSuccess) {}
    }
#endif
    RETURN(0);
}

KFUNC(XAudioSubmitRenderDriverFrame)
{
#if FD_XENIA_GPU
    const uint32_t index = ARG0 & 0xFFFF;
    std::lock_guard lock(g_clientMutex);
    if (index < kMaxClients && g_clients[index].driver)
    {
        float peak = g_peak.load();
        const be<float>* samples = mem::Ptr<be<float>>(ARG1);
        for (uint32_t i = 0; i < 6 * 256; ++i)
            peak = std::max(peak, std::abs(float(samples[i])));
        g_peak = peak;
        ++g_frames;
        // FD_AUDIO_DUMP=<file>: raw 48 kHz 6-channel float32 (interleaved) for analysis.
        static FILE* dump = [] {
            const char* path = getenv("FD_AUDIO_DUMP");
            return path && *path ? fopen(path, "wb") : nullptr;
        }();
        if (dump)
        {
            float interleaved[6 * 256];
            for (uint32_t c = 0; c < 6; ++c)
                for (uint32_t i = 0; i < 256; ++i)
                    interleaved[i * 6 + c] = samples[c * 256 + i];
            fwrite(interleaved, sizeof(interleaved), 1, dump);
        }
        // Volume from the PC settings (0..100), applied when it changes.
        const auto d = settings::Get();
        const bool focused = GetForegroundWindow() && [] {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            return pid == GetCurrentProcessId();
        }();
        const float volume = d.muteUnfocused && !focused ? 0.0f : d.volume / 100.0f;
        if (volume != g_clients[index].volume)
        {
            g_clients[index].volume = volume;
            g_clients[index].driver->SetVolume(volume);
        }
        g_clients[index].driver->SubmitFrame(mem::Ptr<float>(ARG1));
    }
#endif
    RETURN(0);
}
