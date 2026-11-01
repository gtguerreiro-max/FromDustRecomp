#include "vfs.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

#include "kernel.h"

namespace kernel::vfs
{
    namespace
    {
        std::mutex g_lock;
        std::filesystem::path g_gameDir, g_userDir;
        std::unordered_map<std::string, std::filesystem::path> g_mounts;

        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        // Case-insensitive lookup of each component (the Xbox FS is
        // case-insensitive; NTFS is too, but this keeps it portable).
        std::filesystem::path Join(const std::filesystem::path& root, const std::string& rel)
        {
            std::string clean = rel;
            std::replace(clean.begin(), clean.end(), '\\', '/');
            while (!clean.empty() && clean.front() == '/')
                clean.erase(clean.begin());
            return root / std::filesystem::path(clean).lexically_normal();
        }
    }

    void SetGameDirectory(const std::filesystem::path& dir)
    {
        std::lock_guard lock(g_lock);
        g_gameDir = dir;
        for (const char* root : { "game", "d", "dvd", "cdrom0", "update", "media" })
            g_mounts[root] = dir;
    }

    void SetUserDirectory(const std::filesystem::path& dir)
    {
        std::lock_guard lock(g_lock);
        g_userDir = dir;
        std::filesystem::create_directories(dir / "cache");
        g_mounts["cache"] = dir / "cache";
    }

    std::filesystem::path UserDirectory()
    {
        std::lock_guard lock(g_lock);
        return g_userDir;
    }

    void Mount(const std::string& root, const std::filesystem::path& dir)
    {
        std::filesystem::create_directories(dir);
        std::lock_guard lock(g_lock);
        g_mounts[Lower(root)] = dir;
        Log("vfs: mounted %s: -> %s", root.c_str(), dir.string().c_str());
    }

    void Unmount(const std::string& root)
    {
        std::lock_guard lock(g_lock);
        g_mounts.erase(Lower(root));
    }

    bool Resolve(const std::string& guestPath, std::filesystem::path& hostPath)
    {
        std::string path = guestPath;
        std::string lower = Lower(path);

        // NT device paths: \Device\Cdrom0\..., \Device\Harddisk0\Partition1\...
        static const char* const kDevices[] = { "\\device\\cdrom0", "\\device\\harddisk0\\partition1", "\\??\\" };
        for (const char* device : kDevices)
        {
            const size_t n = strlen(device);
            if (lower.compare(0, n, device) == 0)
            {
                std::lock_guard lock(g_lock);
                hostPath = Join(g_gameDir, path.substr(n));
                return true;
            }
        }

        const size_t colon = path.find(':');
        std::lock_guard lock(g_lock);
        if (colon == std::string::npos)
        {
            hostPath = Join(g_gameDir, path);
            return true;
        }
        auto it = g_mounts.find(lower.substr(lower.find_first_not_of('\\'), colon - lower.find_first_not_of('\\')));
        if (it == g_mounts.end())
            return false;
        hostPath = Join(it->second, path.substr(colon + 1));
        return true;
    }
}
