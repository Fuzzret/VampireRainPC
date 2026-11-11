// Maps console paths to host paths.
//   \Device\Cdrom0\..., game:\..., d:\...  -> extracted disc folder (read-only)
//   \Device\Harddisk0\..., cache:\, hdd:\  -> writable data folder
// Symbolic links created with ObCreateSymbolicLink are honoured.
#pragma once

#include <filesystem>
#include <string>

namespace vfs
{
    void Init(const std::string& gameDir, const std::string& dataDir);

    struct Resolved
    {
        std::filesystem::path host;
        bool writable = false;
        bool ok = false;
    };
    Resolved Resolve(const std::string& guestPath);

    void AddSymlink(const std::string& link, const std::string& target);
    // Mounts a host folder as a writable drive, e.g. "save" -> "save:\..."
    // (used for save game containers).
    void Mount(const std::string& root, const std::filesystem::path& hostDir);
    void Unmount(const std::string& root);
    std::filesystem::path DataDir();
    void RemoveSymlink(const std::string& link);
}
