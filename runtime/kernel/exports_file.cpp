// NT file I/O on top of the host file system (see vfs.cpp for path mapping).

#include <filesystem>
#include <string>
#include <vector>

#include "kernel.h"
#include "objects.h"
#include "vfs.h"
#include "../gpu/xenia_bridge.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace kernel;

namespace
{
    struct File : KernelObject
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::filesystem::path path;
        bool directory = false;
        uint64_t position = 0;
        std::vector<std::filesystem::directory_entry> entries; // NtQueryDirectoryFile
        size_t nextEntry = 0;
        bool listed = false;

        File() : KernelObject(kFile) {}
        ~File() override
        {
            if (handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(handle);
        }
        bool IsSignaled() const override { return true; }
    };

    constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4,
                       FILE_OVERWRITE_IF = 5;
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x1;
    // FILE_ATTRIBUTE_DIRECTORY / FILE_ATTRIBUTE_NORMAL come from windows.h.

    // OBJECT_ATTRIBUTES { root, ANSI_STRING* name, attributes }.
    std::string ReadObjectName(uint32_t attributes)
    {
        const uint32_t name = mem::Read32(attributes + 4);
        if (!name)
            return {};
        const uint16_t length = *mem::Ptr<be<uint16_t>>(name);
        const uint32_t buffer = mem::Read32(name + 4);
        return std::string(mem::Ptr<char>(buffer), length);
    }

    void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t information)
    {
        if (!iosb)
            return;
        mem::Write32(iosb, status);
        mem::Write32(iosb + 4, information);
    }

    uint64_t FileTime(const std::filesystem::file_time_type& t)
    {
        // file_time_type on Windows is FILETIME-based (100 ns since 1601).
        return uint64_t(t.time_since_epoch().count());
    }

    // FILE_NETWORK_OPEN_INFORMATION (56 bytes).
    void WriteNetworkOpenInfo(uint32_t info, const std::filesystem::path& path)
    {
        std::error_code ec;
        const bool dir = std::filesystem::is_directory(path, ec);
        const uint64_t size = dir ? 0 : std::filesystem::file_size(path, ec);
        const uint64_t time = FileTime(std::filesystem::last_write_time(path, ec));
        for (uint32_t i = 0; i < 4; i++)
            *mem::Ptr<be<uint64_t>>(info + i * 8) = time;
        *mem::Ptr<be<uint64_t>>(info + 0x20) = (size + 0xFFF) & ~0xFFFull;
        *mem::Ptr<be<uint64_t>>(info + 0x28) = size;
        mem::Write32(info + 0x30, dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        mem::Write32(info + 0x34, 0);
    }

    uint32_t OpenFile(uint32_t handlePtr, uint32_t access, uint32_t attributes, uint32_t iosb, uint32_t disposition,
        uint32_t options)
    {
        const std::string guestPath = ReadObjectName(attributes);
        std::filesystem::path hostPath;
        if (!vfs::Resolve(guestPath, hostPath))
        {
            Log("open %s: unknown device", guestPath.c_str());
            SetIoStatus(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        std::error_code ec;
        const bool exists = std::filesystem::exists(hostPath, ec);
        const bool isDir = exists ? std::filesystem::is_directory(hostPath, ec) : (options & FILE_DIRECTORY_FILE) != 0;

        if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
        {
            Log("open %s: not found", guestPath.c_str());
            SetIoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (exists && disposition == FILE_CREATE)
        {
            SetIoStatus(iosb, STATUS_OBJECT_NAME_COLLISION, 0);
            return STATUS_OBJECT_NAME_COLLISION;
        }

        auto* file = new File();
        file->path = hostPath;
        file->directory = isDir;
        if (isDir)
        {
            std::filesystem::create_directories(hostPath, ec);
        }
        else
        {
            DWORD creation = OPEN_EXISTING;
            switch (disposition)
            {
            case FILE_SUPERSEDE:
            case FILE_OVERWRITE_IF: creation = CREATE_ALWAYS; break;
            case FILE_CREATE: creation = CREATE_NEW; break;
            case FILE_OPEN_IF: creation = OPEN_ALWAYS; break;
            case FILE_OVERWRITE: creation = TRUNCATE_EXISTING; break;
            }
            // GENERIC_WRITE / FILE_WRITE_DATA / FILE_APPEND_DATA / DELETE
            const bool write = (access & (0x40000000 | 0x2 | 0x4 | 0x10000)) != 0 || creation != OPEN_EXISTING;
            if (write)
                std::filesystem::create_directories(hostPath.parent_path(), ec);
            file->handle = CreateFileW(hostPath.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file->handle == INVALID_HANDLE_VALUE)
            {
                Log("open %s: host error %lu", guestPath.c_str(), GetLastError());
                delete file;
                SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
                return STATUS_ACCESS_DENIED;
            }
        }

        mem::Write32(handlePtr, CreateHandle(file));
        // Information: FILE_OPENED (1) / FILE_CREATED (2)
        SetIoStatus(iosb, STATUS_SUCCESS, exists ? 1 : 2);
        return STATUS_SUCCESS;
    }

    void SignalCompletion(uint32_t eventHandle)
    {
        if (!eventHandle)
            return;
        KernelObject* event = LookupHandle(eventHandle);
        if (event && (event->type == kNotificationEvent || event->type == kSynchronizationEvent))
        {
            {
                std::lock_guard lock(g_dispatcherLock);
                static_cast<Event*>(event)->signaled = true;
            }
            NotifyObject(event);
        }
    }
}

KFUNC(NtCreateFile)
{
    // (handle_ptr, access, attributes, iosb, allocation_size, file_attributes, share, disposition, options)
    RETURN(OpenFile(ARG0, ARG1, ARG2, ARG3, ARG7, STACK_ARG(0)));
}

KFUNC(NtOpenFile)
{
    // (handle_ptr, access, attributes, iosb, share, options)
    RETURN(OpenFile(ARG0, ARG1, ARG2, ARG3, FILE_OPEN, ARG5));
}

KFUNC(NtReadFile)
{
    // (handle, event, apc_routine, apc_context, iosb, buffer, length, byte_offset_ptr)
    auto* file = LookupHandle<File>(ARG0, kFile);
    if (!file || file->directory)
        RETURN(STATUS_INVALID_HANDLE);
    const uint32_t iosb = ARG4, buffer = ARG5, length = ARG6, offsetPtr = ARG7;
    uint64_t offset = file->position;
    if (offsetPtr)
    {
        const int64_t requested = int64_t(*mem::Ptr<be<uint64_t>>(offsetPtr));
        if (requested >= 0)
            offset = uint64_t(requested);
    }

    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD read = 0;
    gpu::xenia::PrepareHostWrite(buffer, length);
    if (!ReadFile(file->handle, mem::Translate(buffer), length, &read, &ov) && GetLastError() != ERROR_HANDLE_EOF)
    {
        SetIoStatus(iosb, STATUS_UNSUCCESSFUL, 0);
        RETURN(STATUS_UNSUCCESSFUL);
    }
    file->position = offset + read;
    const uint32_t status = (read == 0 && length != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    SetIoStatus(iosb, status, read);
    SignalCompletion(ARG1);
    RETURN(status);
}

KFUNC(NtWriteFile)
{
    // (handle, event, apc_routine, apc_context, iosb, buffer, length, byte_offset_ptr)
    auto* file = LookupHandle<File>(ARG0, kFile);
    if (!file || file->directory)
        RETURN(STATUS_INVALID_HANDLE);
    const uint32_t iosb = ARG4, buffer = ARG5, length = ARG6, offsetPtr = ARG7;
    uint64_t offset = file->position;
    if (offsetPtr)
    {
        const int64_t requested = int64_t(*mem::Ptr<be<uint64_t>>(offsetPtr));
        if (requested >= 0)
            offset = uint64_t(requested);
    }
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD written = 0;
    if (!WriteFile(file->handle, mem::Translate(buffer), length, &written, &ov))
    {
        SetIoStatus(iosb, STATUS_UNSUCCESSFUL, 0);
        RETURN(STATUS_UNSUCCESSFUL);
    }
    file->position = offset + written;
    SetIoStatus(iosb, STATUS_SUCCESS, written);
    SignalCompletion(ARG1);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtFlushBuffersFile)
{
    auto* file = LookupHandle<File>(ARG0, kFile);
    if (file && file->handle != INVALID_HANDLE_VALUE)
        FlushFileBuffers(file->handle);
    SetIoStatus(ARG1, STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtQueryInformationFile)
{
    // (handle, iosb, info, length, class)
    auto* file = LookupHandle<File>(ARG0, kFile);
    if (!file)
        RETURN(STATUS_INVALID_HANDLE);
    const uint32_t iosb = ARG1, info = ARG2, length = ARG3, infoClass = ARG4;
    std::error_code ec;
    const uint64_t size = file->directory ? 0 : std::filesystem::file_size(file->path, ec);
    memset(mem::Translate(info), 0, length);
    switch (infoClass)
    {
    case 5: // FileStandardInformation
        *mem::Ptr<be<uint64_t>>(info) = (size + 0xFFF) & ~0xFFFull;
        *mem::Ptr<be<uint64_t>>(info + 8) = size;
        mem::Write32(info + 16, 1);
        mem::Ptr<uint8_t>(info)[21] = file->directory;
        SetIoStatus(iosb, STATUS_SUCCESS, 24);
        break;
    case 14: // FilePositionInformation
        *mem::Ptr<be<uint64_t>>(info) = file->position;
        SetIoStatus(iosb, STATUS_SUCCESS, 8);
        break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(info, file->path);
        SetIoStatus(iosb, STATUS_SUCCESS, 56);
        break;
    case 6: // FileInternalInformation
        *mem::Ptr<be<uint64_t>>(info) = std::hash<std::wstring>{}(file->path.wstring());
        SetIoStatus(iosb, STATUS_SUCCESS, 8);
        break;
    default:
        Log("NtQueryInformationFile: unsupported class %u", infoClass);
        RETURN(STATUS_NOT_IMPLEMENTED);
    }
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtSetInformationFile)
{
    // (handle, iosb, info, length, class)
    auto* file = LookupHandle<File>(ARG0, kFile);
    if (!file)
        RETURN(STATUS_INVALID_HANDLE);
    const uint32_t iosb = ARG1, info = ARG2, infoClass = ARG4;
    switch (infoClass)
    {
    case 14: // FilePositionInformation
        file->position = *mem::Ptr<be<uint64_t>>(info);
        break;
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
    {
        LARGE_INTEGER end;
        end.QuadPart = int64_t(uint64_t(*mem::Ptr<be<uint64_t>>(info)));
        if (infoClass == 20 && file->handle != INVALID_HANDLE_VALUE)
        {
            SetFilePointerEx(file->handle, end, nullptr, FILE_BEGIN);
            SetEndOfFile(file->handle);
        }
        break;
    }
    case 13: // FileDispositionInformation (delete on close)
        if (mem::Ptr<uint8_t>(info)[0])
        {
            std::error_code ec;
            if (file->handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(file->handle);
                file->handle = INVALID_HANDLE_VALUE;
            }
            std::filesystem::remove(file->path, ec);
        }
        break;
    default:
        Log("NtSetInformationFile: unsupported class %u", infoClass);
        RETURN(STATUS_NOT_IMPLEMENTED);
    }
    SetIoStatus(iosb, STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtQueryFullAttributesFile)
{
    // (attributes, FILE_NETWORK_OPEN_INFORMATION*)
    const std::string guestPath = ReadObjectName(ARG0);
    std::filesystem::path hostPath;
    std::error_code ec;
    if (!vfs::Resolve(guestPath, hostPath) || !std::filesystem::exists(hostPath, ec))
        RETURN(STATUS_OBJECT_NAME_NOT_FOUND);
    WriteNetworkOpenInfo(ARG1, hostPath);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtQueryVolumeInformationFile)
{
    // (handle, iosb, info, length, class)
    const uint32_t iosb = ARG1, info = ARG2, length = ARG3, infoClass = ARG4;
    memset(mem::Translate(info), 0, length);
    switch (infoClass)
    {
    case 3: // FileFsSizeInformation: total, available (u64), sectors/unit, bytes/sector
        *mem::Ptr<be<uint64_t>>(info) = 0x100000;
        *mem::Ptr<be<uint64_t>>(info + 8) = 0x80000;
        mem::Write32(info + 16, 8);
        mem::Write32(info + 20, 512);
        SetIoStatus(iosb, STATUS_SUCCESS, 24);
        break;
    case 1: // FileFsVolumeInformation
    case 5: // FileFsAttributeInformation
        SetIoStatus(iosb, STATUS_SUCCESS, length);
        break;
    default:
        Log("NtQueryVolumeInformationFile: unsupported class %u", infoClass);
        RETURN(STATUS_NOT_IMPLEMENTED);
    }
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtQueryDirectoryFile)
{
    // (handle, event, apc, apc_context, iosb, info, length, file_name_mask, restart_scan)
    auto* file = LookupHandle<File>(ARG0, kFile);
    if (!file || !file->directory)
        RETURN(STATUS_INVALID_HANDLE);
    const uint32_t iosb = ARG4, info = ARG5, length = ARG6, mask = ARG7;
    const bool restart = STACK_ARG(0) != 0;

    if (!file->listed || restart)
    {
        std::string pattern = "*";
        if (mask)
        {
            const uint16_t n = *mem::Ptr<be<uint16_t>>(mask);
            pattern.assign(mem::Ptr<char>(mem::Read32(mask + 4)), n);
        }
        file->entries.clear();
        file->nextEntry = 0;
        file->listed = true;
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(file->path, ec))
        {
            const std::string name = entry.path().filename().string();
            // Only "*" and exact names are used by the game's loaders.
            if (pattern == "*" || pattern == "*.*" || _stricmp(pattern.c_str(), name.c_str()) == 0)
                file->entries.push_back(entry);
        }
    }

    if (file->nextEntry >= file->entries.size())
    {
        SetIoStatus(iosb, STATUS_NO_MORE_FILES, 0);
        RETURN(STATUS_NO_MORE_FILES);
    }

    // FILE_DIRECTORY_INFORMATION with an ANSI name, one entry per call.
    const auto& entry = file->entries[file->nextEntry++];
    const std::string name = entry.path().filename().string();
    if (length < 0x40 + name.size())
        RETURN(STATUS_INVALID_PARAMETER);
    memset(mem::Translate(info), 0, 0x40);
    std::error_code ec;
    const bool dir = entry.is_directory(ec);
    const uint64_t size = dir ? 0 : entry.file_size(ec);
    const uint64_t time = FileTime(entry.last_write_time(ec));
    for (uint32_t i = 0; i < 4; i++)
        *mem::Ptr<be<uint64_t>>(info + 8 + i * 8) = time;
    *mem::Ptr<be<uint64_t>>(info + 0x28) = size;
    *mem::Ptr<be<uint64_t>>(info + 0x30) = (size + 0xFFF) & ~0xFFFull;
    mem::Write32(info + 0x38, dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    mem::Write32(info + 0x3C, uint32_t(name.size()));
    memcpy(mem::Ptr<char>(info + 0x40), name.data(), name.size());
    SetIoStatus(iosb, STATUS_SUCCESS, uint32_t(0x40 + name.size()));
    RETURN(STATUS_SUCCESS);
}

// Symbolic links (e.g. "game:" -> "\Device\Cdrom0") are handled by vfs mounts.
KFUNC(ObCreateSymbolicLink) { RETURN(STATUS_SUCCESS); }
KFUNC(ObDeleteSymbolicLink) { RETURN(STATUS_SUCCESS); }
