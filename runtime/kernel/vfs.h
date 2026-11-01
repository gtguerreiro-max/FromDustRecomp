#pragma once

#include <filesystem>
#include <string>

namespace kernel::vfs
{
    void SetGameDirectory(const std::filesystem::path& dir);
    void SetUserDirectory(const std::filesystem::path& dir); // saves, cache

    // Registers "<root>:" (e.g. from XamContentCreateEx) to a host directory.
    void Mount(const std::string& root, const std::filesystem::path& dir);
    void Unmount(const std::string& root);
    std::filesystem::path UserDirectory();

    // Resolves a guest path (game:\x, d:\x, \Device\Cdrom0\x, save:\x ...).
    // Returns false for paths that don't map anywhere.
    bool Resolve(const std::string& guestPath, std::filesystem::path& hostPath);
}
