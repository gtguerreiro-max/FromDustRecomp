// XAM: license, the local user/profile, notifications, content (saves) and
// system UI. Behaviour follows Xenia's xam_*.cc (BSD): one local profile is
// signed in on slot 0, the title is fully licensed, and system UIs complete
// immediately with their default answer.

#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "kernel.h"
#include "objects.h"
#include "thread.h"
#include "vfs.h"

using namespace kernel;

namespace
{
    constexpr uint32_t X_ERROR_FUNCTION_FAILED = 0x65B;
    constexpr uint32_t X_ERROR_INVALID_NAME = 0x7B;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;
    constexpr uint32_t X_E_NO_SUCH_USER = 0x80070525;

    constexpr uint64_t kXuid = 0xE000000046524F4Dull; // local (offline) XUID
    constexpr const char* kGamertag = "Player";

    uint32_t HResult(uint32_t win32) { return win32 ? (0x80070000u | (win32 & 0xFFFF)) : 0; }

    void SignalEventHandle(uint32_t handle)
    {
        KernelObject* object = LookupHandle(handle);
        if (!object || (object->type != kNotificationEvent && object->type != kSynchronizationEvent))
            return;
        {
            std::lock_guard lock(g_dispatcherLock);
            static_cast<Event*>(object)->signaled = true;
        }
        NotifyObject(object);
    }

    // XAM_OVERLAPPED: result@0, length@4, context@8, event@0xC, routine@0x10,
    // routine context@0x14, extended error@0x18. Completed immediately.
    uint32_t CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t extended, uint32_t length)
    {
        mem::Write32(ov + 0x00, result);
        mem::Write32(ov + 0x04, length);
        mem::Write32(ov + 0x18, extended);
        if (const uint32_t routine = mem::Read32(ov + 0x10))
            Log("XAM overlapped completion routine 0x%08X not supported", routine);
        if (const uint32_t event = mem::Read32(ov + 0x0C))
            SignalEventHandle(event);
        return ERROR_IO_PENDING;
    }

    // Returns `result` synchronously, or completes `ov` with it.
    uint32_t Finish(uint32_t ov, uint32_t result, uint32_t length = 0)
    {
        if (!ov)
            return result;
        return CompleteOverlapped(ov, result ? X_ERROR_FUNCTION_FAILED : 0, HResult(result), length);
    }

    bool SignedIn(uint32_t user) { return user == 0; }

    // --- Notifications ----------------------------------------------------------

    struct Listener : KernelObject
    {
        uint64_t mask;
        std::deque<std::pair<uint32_t, uint32_t>> queue; // dispatcher lock
        explicit Listener(uint64_t m) : KernelObject(kOther), mask(m) {}
        bool IsSignaled() const override { return !queue.empty(); }
    };

    std::mutex g_listenerLock;
    std::vector<Listener*> g_listeners;

    void Push(Listener* listener, uint32_t id, uint32_t param)
    {
        {
            std::lock_guard lock(g_dispatcherLock);
            listener->queue.emplace_back(id, param);
        }
        NotifyObject(listener);
    }

    void Broadcast(uint32_t id, uint32_t param)
    {
        const uint32_t maskIndex = (id >> 25) & 0x3F;
        std::lock_guard lock(g_listenerLock);
        for (Listener* listener : g_listeners)
        {
            if (listener->mask & (1ull << maskIndex))
                Push(listener, id, param);
        }
    }

    constexpr uint32_t XN_SYS_UI = 0x00000009;
    constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;
    constexpr uint32_t XN_SYS_PROFILESETTINGCHANGED = 0x0000000E;
    constexpr uint32_t XN_LIVE_CONNECTIONCHANGED = 0x02000001;
    constexpr uint32_t XN_LIVE_LINKSTATECHANGED = 0x02000003;

    // --- Profile settings (persisted per setting id) ------------------------------

    std::mutex g_settingsLock;
    std::map<uint32_t, std::vector<uint8_t>> g_settings; // id -> X_USER_DATA payload
    bool g_settingsLoaded = false;

    std::filesystem::path SettingsPath() { return vfs::UserDirectory() / "profile_settings.bin"; }

    void LoadSettings()
    {
        if (g_settingsLoaded)
            return;
        g_settingsLoaded = true;
        std::ifstream file(SettingsPath(), std::ios::binary);
        uint32_t id, size;
        while (file.read(reinterpret_cast<char*>(&id), 4) && file.read(reinterpret_cast<char*>(&size), 4))
        {
            std::vector<uint8_t> data(size);
            if (!file.read(reinterpret_cast<char*>(data.data()), size))
                break;
            g_settings[id] = std::move(data);
        }
    }

    void SaveSettings()
    {
        std::filesystem::create_directories(SettingsPath().parent_path());
        std::ofstream file(SettingsPath(), std::ios::binary | std::ios::trunc);
        for (auto& [id, data] : g_settings)
        {
            const uint32_t size = uint32_t(data.size());
            file.write(reinterpret_cast<const char*>(&id), 4);
            file.write(reinterpret_cast<const char*>(&size), 4);
            file.write(reinterpret_cast<const char*>(data.data()), size);
        }
    }

    uint32_t SettingType(uint32_t id) { return id >> 28; }
    uint32_t SettingMaxSize(uint32_t id) { return (id >> 16) & 0xFFF; }
    bool VariableSize(uint32_t id) { return SettingType(id) == 4 || SettingType(id) == 6; } // wstring / binary

    // --- Content (save data) -----------------------------------------------------

    std::mutex g_contentLock;
    std::map<std::string, std::filesystem::path> g_mounted; // root -> folder

    std::filesystem::path ContentFolder(uint32_t contentData)
    {
        // XCONTENT_DATA: device@0, type@4, display name@8 (UTF-16), file name@0x108 (char[42]).
        std::string name(mem::Ptr<char>(contentData + 0x108), strnlen(mem::Ptr<char>(contentData + 0x108), 42));
        for (char& c : name)
        {
            if (strchr("<>:\"/\\|?*", c))
                c = '_';
        }
        char type[16];
        snprintf(type, sizeof(type), "%08X", mem::Read32(contentData + 4));
        return vfs::UserDirectory() / "content" / type / name;
    }
}

// --- License: the full game, not the trial ----------------------------------------

KFUNC(XamContentGetLicenseMask)
{
    // (mask_ptr, overlapped)
    if (!ARG0)
        RETURN(ERROR_INVALID_PARAMETER);
    mem::Write32(ARG0, 0xFFFFFFFF);
    RETURN(Finish(ARG1, ERROR_SUCCESS));
}

// --- Users -----------------------------------------------------------------------------

KFUNC(XamUserGetSigninState) { RETURN(SignedIn(ARG0) ? 1 : 0); } // signed in locally

KFUNC(XamUserGetXUID)
{
    // (user_index, type_mask, xuid_ptr)
    if (!ARG2)
        RETURN(X_E_INVALIDARG);
    *mem::Ptr<be<uint64_t>>(ARG2) = 0;
    if (ARG0 >= 4)
        RETURN(X_E_INVALIDARG);
    if (!SignedIn(ARG0))
        RETURN(X_E_NO_SUCH_USER);
    *mem::Ptr<be<uint64_t>>(ARG2) = kXuid;
    RETURN(0);
}

KFUNC(XamUserGetSigninInfo)
{
    // (user_index, flags, X_USER_SIGNIN_INFO*): xuid@0, flags@8, state@0xC, guest@0x10, sponsor@0x14, name[16]@0x18
    const uint32_t info = ARG2;
    if (!info)
        RETURN(X_E_INVALIDARG);
    memset(mem::Translate(info), 0, 40);
    if (ARG0 >= 4)
        RETURN(X_E_INVALIDARG);
    if (!SignedIn(ARG0))
        RETURN(X_E_NO_SUCH_USER);
    if (ARG1 == 0 || (ARG1 & 2))
        *mem::Ptr<be<uint64_t>>(info) = kXuid;
    mem::Write32(info + 0x0C, 1);
    strncpy(mem::Ptr<char>(info + 0x18), kGamertag, 15);
    RETURN(0);
}

KFUNC(XamUserReadProfileSettings)
{
    // (title_id, user_index, xuid_count, xuids, setting_count, setting_ids, buffer_size_ptr, buffer, overlapped)
    const uint32_t user = ARG1, xuidCount = ARG2, xuids = ARG3, count = ARG4, ids = ARG5, sizePtr = ARG6, buffer = ARG7;
    const uint32_t ov = STACK_ARG(0);
    if (count == 0 || count > 32 || !sizePtr)
        RETURN(ERROR_INVALID_PARAMETER);
    if (mem::Read32(sizePtr) != 0 && !buffer)
        RETURN(ERROR_INVALID_PARAMETER);

    uint32_t header = 40 * count, extra = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = mem::Read32(ids + i * 4);
        if (VariableSize(id))
            extra += SettingMaxSize(id);
    }
    if (xuids)
    {
        header *= xuidCount;
        extra *= xuidCount;
    }
    const uint32_t total = 8 + header + extra;
    if (!buffer || mem::Read32(sizePtr) < total)
    {
        if (mem::Read32(sizePtr) == 0)
            mem::Write32(sizePtr, total);
        RETURN(ERROR_INSUFFICIENT_BUFFER);
    }
    if (!SignedIn(user) && !xuids)
        RETURN(Finish(ov, X_ERROR_FUNCTION_FAILED));

    // X_USER_READ_PROFILE_SETTINGS { count, settings_ptr } followed by
    // X_USER_PROFILE_SETTING[count] (40 bytes: source@0, user/xuid@8,
    // id@0x10, X_USER_DATA@0x18 {type@0x18, value/size@0x20, ptr@0x24}).
    const uint32_t settings = buffer + 8;
    uint32_t extraPtr = settings + 40 * count;
    memset(mem::Translate(settings), 0, 40 * count);
    mem::Write32(buffer, count);
    mem::Write32(buffer + 4, settings);

    std::lock_guard lock(g_settingsLock);
    LoadSettings();
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = mem::Read32(ids + i * 4);
        const uint32_t slot = settings + i * 40;
        auto stored = g_settings.find(id);
        mem::Write32(slot + 0x00, stored != g_settings.end() ? 2 : 1); // TITLE or DEFAULT
        if (xuids)
            *mem::Ptr<be<uint64_t>>(slot + 0x08) = kXuid;
        else
            mem::Write32(slot + 0x08, user);
        mem::Write32(slot + 0x10, id);
        mem::Ptr<uint8_t>(slot + 0x18)[0] = uint8_t(SettingType(id));
        if (VariableSize(id))
        {
            uint32_t size = 0;
            if (stored != g_settings.end())
            {
                size = uint32_t(std::min<size_t>(stored->second.size(), SettingMaxSize(id)));
                memcpy(mem::Translate(extraPtr), stored->second.data(), size);
            }
            mem::Write32(slot + 0x20, size);
            mem::Write32(slot + 0x24, size ? extraPtr : 0);
            extraPtr += SettingMaxSize(id);
        }
        else if (stored != g_settings.end() && stored->second.size() == 8)
        {
            memcpy(mem::Translate(slot + 0x20), stored->second.data(), 8);
        }
    }
    RETURN(Finish(ov, ERROR_SUCCESS));
}

KFUNC(XamUserWriteProfileSettings)
{
    // (title_id, user_index, setting_count, X_USER_PROFILE_SETTING*, overlapped)
    const uint32_t user = ARG1, count = ARG2, settings = ARG3, ov = ARG4;
    if (!count || !settings)
        RETURN(ERROR_INVALID_PARAMETER);
    if (!SignedIn(user))
        RETURN(Finish(ov, ERROR_NO_SUCH_USER));
    {
        std::lock_guard lock(g_settingsLock);
        LoadSettings();
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t slot = settings + i * 40;
            const uint32_t id = mem::Read32(slot + 0x10);
            if (SettingType(id) > 7)
                continue;
            std::vector<uint8_t> data;
            if (VariableSize(id))
            {
                const uint32_t size = mem::Read32(slot + 0x20), ptr = mem::Read32(slot + 0x24);
                if (ptr && size)
                    data.assign(mem::Ptr<uint8_t>(ptr), mem::Ptr<uint8_t>(ptr) + size);
            }
            else
            {
                data.assign(mem::Ptr<uint8_t>(slot + 0x20), mem::Ptr<uint8_t>(slot + 0x20) + 8);
            }
            g_settings[id] = std::move(data);
        }
        SaveSettings();
    }
    Broadcast(XN_SYS_PROFILESETTINGCHANGED, (1u << user) & 0xF);
    RETURN(Finish(ov, ERROR_SUCCESS));
}

KFUNC(XamUserCreateStatsEnumerator)
{
    // (title_id, user_index, count, flags, size, stats, buffer_size_ptr, handle_ptr): empty enumerator
    const uint32_t sizePtr = ARG6, handlePtr = ARG7;
    if (!sizePtr || !handlePtr)
        RETURN(ERROR_INVALID_PARAMETER);
    mem::Write32(sizePtr, 0);
    mem::Write32(handlePtr, CreateHandle(new KernelObject(kOther)));
    RETURN(ERROR_SUCCESS);
}

KFUNC(XamEnumerate)
{
    // (handle, flags, buffer, buffer_length, items_returned, overlapped): nothing to enumerate
    if (!LookupHandle(ARG0))
        RETURN(6); // ERROR_INVALID_HANDLE
    if (ARG4)
        mem::Write32(ARG4, 0);
    RETURN(Finish(ARG5, ERROR_NO_MORE_FILES));
}

// --- Notifications -------------------------------------------------------------------------

KFUNC(XamNotifyCreateListener)
{
    // (qword mask, max_version): mask arrives as a 64-bit register
    auto* listener = new Listener(ctx.r3.u64);
    const uint32_t handle = CreateHandle(listener);
    // What a console reports to a title at startup: no UI, a local user
    // signed in, Live disconnected.
    if (listener->mask & 1)
    {
        Push(listener, XN_SYS_UI, 0);
        Push(listener, XN_SYS_SIGNINCHANGED, 1);
    }
    if (listener->mask & 2)
    {
        Push(listener, XN_LIVE_CONNECTIONCHANGED, 0x001510F1);
        Push(listener, XN_LIVE_LINKSTATECHANGED, 0);
    }
    {
        std::lock_guard lock(g_listenerLock);
        g_listeners.push_back(listener);
    }
    RETURN(handle);
}

KFUNC(XNotifyGetNext)
{
    // (handle, match_id, id_ptr, param_ptr) -> BOOL
    if (ARG3)
        mem::Write32(ARG3, 0);
    if (!ARG2)
        RETURN(0);
    mem::Write32(ARG2, 0);
    auto* listener = dynamic_cast<Listener*>(LookupHandle(ARG0));
    if (!listener)
        RETURN(0);
    std::lock_guard lock(g_dispatcherLock);
    for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it)
    {
        if (ARG1 && it->first != ARG1)
            continue;
        mem::Write32(ARG2, it->first);
        if (ARG3)
            mem::Write32(ARG3, it->second);
        listener->queue.erase(it);
        RETURN(1);
    }
    RETURN(0);
}

// --- System UI: answer immediately with the default choice ---------------------------------

KFUNC(XamShowSigninUI)
{
    // (users_needed, flags): the local profile is already signed in.
    if (ARG0 != 1 && ARG0 != 2 && ARG0 != 4)
        RETURN(ERROR_INVALID_PARAMETER);
    Broadcast(XN_SYS_UI, 1);
    Broadcast(XN_SYS_SIGNINCHANGED, 1);
    Broadcast(XN_SYS_UI, 0);
    RETURN(ERROR_SUCCESS);
}

KFUNC(XamShowDeviceSelectorUI)
{
    // (user_index, content_type, content_flags, qword total_requested, device_id_ptr, overlapped)
    const uint32_t devicePtr = ARG4, ov = ARG5;
    if (!ov)
        RETURN(ERROR_INVALID_PARAMETER);
    if (!devicePtr)
    {
        mem::Write32(ov + 0x18, ERROR_INVALID_PARAMETER);
        RETURN(ERROR_INVALID_PARAMETER);
    }
    mem::Write32(devicePtr, 1); // the hard drive
    RETURN(CompleteOverlapped(ov, ERROR_SUCCESS, 0, 0));
}

KFUNC(XamShowMessageBoxUIEx)
{
    // (user, title, text, button_count, buttons, active_button, flags, unused, result_ptr, overlapped)
    auto utf16 = [](uint32_t p) {
        std::string s;
        for (; p && *mem::Ptr<be<uint16_t>>(p); p += 2)
        {
            const uint16_t c = *mem::Ptr<be<uint16_t>>(p);
            s += c < 0x80 ? char(c) : '?';
        }
        return s;
    };
    Log("XamShowMessageBoxUIEx: \"%s\" - \"%s\" (answering button %u)", utf16(ARG1).c_str(), utf16(ARG2).c_str(), ARG5);
    const uint32_t resultPtr = STACK_ARG(0), ov = STACK_ARG(1);
    if (resultPtr)
        mem::Write32(resultPtr, ARG5);
    RETURN(ov ? CompleteOverlapped(ov, ERROR_SUCCESS, 0, 0) : ERROR_SUCCESS);
}

// --- Content: save data lives in user/content/<type>/<file name> ------------------------------

KFUNC(XamContentCreateEx)
{
    // (user_index, root_name, content_data, flags, disposition_ptr, license_mask_ptr, cache_size, qword content_size, overlapped)
    const uint32_t user = ARG0, rootPtr = ARG1, data = ARG2, flags = ARG3, dispositionPtr = ARG4, licensePtr = ARG5;
    const uint32_t ov = STACK_ARG(0);
    if (!data || !mem::Ptr<char>(data + 0x108)[0])
        RETURN(X_ERROR_INVALID_NAME);
    if (user < 4 && !SignedIn(user))
        RETURN(Finish(ov, ERROR_NO_SUCH_USER));
    if (!rootPtr || !mem::Ptr<char>(rootPtr)[0])
        RETURN(Finish(ov, X_ERROR_INVALID_NAME));

    const std::string root = mem::Ptr<char>(rootPtr);
    const std::filesystem::path folder = ContentFolder(data);
    std::error_code ec;
    const bool exists = std::filesystem::exists(folder, ec);
    uint32_t result = ERROR_SUCCESS, disposition = 2; // opened
    switch (flags & 0xF)
    {
    case 1: // create new
        if (exists)
            result = ERROR_ALREADY_EXISTS;
        disposition = 1;
        break;
    case 2: // create always
        std::filesystem::remove_all(folder, ec);
        disposition = 1;
        break;
    case 3: // open existing
        if (!exists)
            result = ERROR_PATH_NOT_FOUND;
        break;
    case 4: // open always
        disposition = exists ? 2 : 1;
        break;
    case 5: // truncate existing
        if (!exists)
            result = ERROR_PATH_NOT_FOUND;
        else
            std::filesystem::remove_all(folder, ec);
        disposition = 1;
        break;
    default:
        result = ERROR_INVALID_PARAMETER;
        break;
    }
    if (result == ERROR_SUCCESS)
    {
        vfs::Mount(root, folder);
        std::lock_guard lock(g_contentLock);
        g_mounted[root] = folder;
        if (dispositionPtr)
            mem::Write32(dispositionPtr, disposition);
        if (licensePtr)
            mem::Write32(licensePtr, 0xFFFFFFFF);
    }
    Log("XamContentCreateEx(%s, %s, flags 0x%X) = %u", root.c_str(), folder.string().c_str(), flags, result);
    if (!ov)
        RETURN(result);
    RETURN(CompleteOverlapped(ov, result ? X_ERROR_FUNCTION_FAILED : 0, HResult(result), result ? 0 : disposition));
}

KFUNC(XamContentClose)
{
    // (root_name, overlapped)
    const std::string root = ARG0 ? mem::Ptr<char>(ARG0) : "";
    {
        std::lock_guard lock(g_contentLock);
        g_mounted.erase(root);
    }
    vfs::Unmount(root);
    RETURN(ARG1 ? CompleteOverlapped(ARG1, ERROR_SUCCESS, 0, 0) : ERROR_SUCCESS);
}

KFUNC(XamContentFlush)
{
    RETURN(ARG1 ? CompleteOverlapped(ARG1, ERROR_SUCCESS, 0, 0) : ERROR_SUCCESS);
}

KFUNC(XamContentGetCreator)
{
    // (user_index, content_data, is_creator_ptr, creator_xuid_ptr, overlapped)
    if (!ARG2)
        RETURN(ERROR_INVALID_PARAMETER);
    if (!SignedIn(ARG0))
        RETURN(Finish(ARG4, ERROR_NO_SUCH_USER));
    std::error_code ec;
    if (!std::filesystem::exists(ContentFolder(ARG1), ec))
        RETURN(Finish(ARG4, ERROR_PATH_NOT_FOUND));
    const bool saved = mem::Read32(ARG1 + 4) == 1; // XContentType::SavedGame
    mem::Write32(ARG2, saved ? 1 : 0);
    if (ARG3)
        *mem::Ptr<be<uint64_t>>(ARG3) = saved ? kXuid : 0;
    RETURN(Finish(ARG4, ERROR_SUCCESS));
}

KFUNC(XamContentSetThumbnail)
{
    // (user_index, content_data, png, size, overlapped): thumbnails aren't shown anywhere.
    RETURN(Finish(ARG4, ERROR_SUCCESS));
}

KFUNC(XamContentGetDeviceData)
{
    // (device_id, X_CONTENT_DEVICE_DATA*): id@0, type@4, total@8, free@0x10, name[28]@0x18
    if (ARG0 != 1)
        RETURN(ERROR_DEVICE_NOT_CONNECTED);
    const uint32_t info = ARG1;
    memset(mem::Translate(info), 0, 0x50);
    mem::Write32(info, 1);
    mem::Write32(info + 4, 1); // hard drive
    *mem::Ptr<be<uint64_t>>(info + 0x08) = 20ull << 30;
    *mem::Ptr<be<uint64_t>>(info + 0x10) = 10ull << 30;
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i]; i++)
        *mem::Ptr<be<uint16_t>>(info + 0x18 + i * 2) = uint16_t(name[i]);
    RETURN(ERROR_SUCCESS);
}

// --- Tasks ------------------------------------------------------------------------------------------

KFUNC(XamTaskSchedule)
{
    // (callback, message, X_TASK_ARGS*, handle_ptr): run callback(message) on a new thread
    GuestThread* thread = CreateGuestThread(0x10000, ARG0, ARG1, 0, false);
    if (ARG3)
        mem::Write32(ARG3, CreateHandle(thread));
    RETURN(STATUS_SUCCESS);
}

KFUNC(XamTaskShouldExit) { RETURN(0); }
KFUNC(XamTaskCloseHandle) { RETURN(CloseHandle(ARG0) ? 1 : 0); }

// --- XMsg ----------------------------------------------------------------------
// Requests to XAM's system apps. The game sends XMPSetPlaybackController (app
// 0xFA, music player) and XGI user context / avatar awards (app 0xFB); Xenia's
// apps accept these without side effects, so every request succeeds.

namespace
{
    uint32_t StartIORequest(PPCContext& ctx)
    {
        static std::atomic<int> logged{0};
        if (logged++ < 8)
            Log("XMsgStartIORequest(app 0x%X, msg 0x%08X, len %u)", ARG0, ARG1, ARG4);
        if (const uint32_t ov = ARG2)
            return CompleteOverlapped(ov, ERROR_SUCCESS, 0, 0);
        return ERROR_SUCCESS;
    }
}

KFUNC(XMsgStartIORequest) { RETURN(StartIORequest(ctx)); }
KFUNC(XMsgStartIORequestEx) { RETURN(StartIORequest(ctx)); }

// No network: an empty XNADDR with XNET_GET_XNADDR_NONE makes callers give up
// gracefully (as in Xenia).
KFUNC(NetDll_XNetGetDebugXnAddr)
{
    if (ARG1)
        memset(mem::Translate(ARG1), 0, 36);
    RETURN(1);
}
