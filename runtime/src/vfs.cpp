#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "objects.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path g_gameDir;
    fs::path g_dataDir;
    std::mutex g_linkMutex;
    std::map<std::string, std::string> g_symlinks; // lower-case link -> target
    std::map<std::string, fs::path> g_mounts;      // lower-case "root:" -> host folder

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    std::string Normalize(std::string p)
    {
        std::replace(p.begin(), p.end(), '/', '\\');
        if (p.rfind("\\??\\", 0) == 0)
            p = p.substr(4);
        return p;
    }

    // Case-insensitive path join (needed on case-sensitive host file systems;
    // the disc's names are mixed case while the game asks for lower case).
    fs::path JoinCaseInsensitive(const fs::path& root, const std::string& rel)
    {
        fs::path cur = root;
        size_t start = 0;
        while (start <= rel.size())
        {
            size_t end = rel.find('\\', start);
            if (end == std::string::npos)
                end = rel.size();
            std::string part = rel.substr(start, end - start);
            start = end + 1;
            if (part.empty() || part == ".")
                continue;
            if (part == "..")
            {
                cur = cur.parent_path();
                continue;
            }
            fs::path exact = cur / part;
            std::error_code ec;
            if (!fs::exists(exact, ec) && fs::is_directory(cur, ec))
            {
                std::string want = Lower(part);
                for (auto& e : fs::directory_iterator(cur, ec))
                {
                    if (Lower(e.path().filename().string()) == want)
                    {
                        exact = e.path();
                        break;
                    }
                }
            }
            cur = exact;
        }
        return cur;
    }

    struct Device { const char* prefix; bool game; const char* dataSub; };
    constexpr Device kDevices[] = {
        { "\\device\\cdrom0",                 true,  nullptr },
        { "game:",                            true,  nullptr },
        { "d:",                               true,  nullptr },
        { "\\device\\harddisk0\\partition1",  false, "hdd" },
        { "\\device\\harddisk0\\cache0",      false, "cache" },
        { "\\device\\harddisk0\\cache1",      false, "cache" },
        { "\\device\\harddisk0\\systempartition", false, "system" },
        { "hdd:",                             false, "hdd" },
        { "cache:",                           false, "cache" },
        { "e:",                               false, "hdd" },
        { "\\device\\mu0",                    false, "mu0" },
        { "\\device\\mu1",                    false, "mu1" },
    };
}

namespace vfs
{
    void Init(const std::string& gameDir, const std::string& dataDir)
    {
        g_gameDir = fs::absolute(gameDir);
        g_dataDir = fs::absolute(dataDir);
        std::error_code ec;
        fs::create_directories(g_dataDir, ec);
        LOG("[vfs] game: %s", g_gameDir.string().c_str());
        LOG("[vfs] data: %s", g_dataDir.string().c_str());
    }

    void Mount(const std::string& root, const fs::path& hostDir)
    {
        std::error_code ec;
        fs::create_directories(hostDir, ec);
        std::lock_guard lock(g_linkMutex);
        g_mounts[Lower(root) + ":"] = hostDir;
        LOG("[vfs] mounted %s: -> %s", root.c_str(), hostDir.string().c_str());
    }

    void Unmount(const std::string& root)
    {
        std::lock_guard lock(g_linkMutex);
        g_mounts.erase(Lower(root) + ":");
    }

    fs::path DataDir() { return g_dataDir; }

    void AddSymlink(const std::string& link, const std::string& target)
    {
        std::lock_guard lock(g_linkMutex);
        g_symlinks[Lower(Normalize(link))] = Normalize(target);
    }

    void RemoveSymlink(const std::string& link)
    {
        std::lock_guard lock(g_linkMutex);
        g_symlinks.erase(Lower(Normalize(link)));
    }

    Resolved Resolve(const std::string& guestPath)
    {
        std::string p = Normalize(guestPath);

        // Apply symbolic links (longest match first, a few levels deep).
        for (int depth = 0; depth < 4; depth++)
        {
            std::lock_guard lock(g_linkMutex);
            std::string lp = Lower(p);
            const std::pair<const std::string, std::string>* best = nullptr;
            for (auto& kv : g_symlinks)
            {
                if (lp.rfind(kv.first, 0) == 0 && (lp.size() == kv.first.size() || lp[kv.first.size()] == '\\') &&
                    (!best || kv.first.size() > best->first.size()))
                    best = &kv;
            }
            if (!best)
                break;
            p = best->second + p.substr(best->first.size());
        }

        std::string lp = Lower(p);
        {
            std::lock_guard lock(g_linkMutex);
            for (auto& kv : g_mounts)
            {
                size_t n = kv.first.size();
                if (lp.rfind(kv.first, 0) == 0 && (lp.size() == n || lp[n] == '\\'))
                {
                    Resolved r;
                    r.ok = true;
                    r.writable = true;
                    r.host = JoinCaseInsensitive(kv.second, p.substr(n));
                    return r;
                }
            }
        }
        for (auto& d : kDevices)
        {
            size_t n = strlen(d.prefix);
            if (lp.rfind(d.prefix, 0) != 0 || (lp.size() > n && lp[n] != '\\'))
                continue;
            std::string rel = p.substr(n);
            Resolved r;
            r.ok = true;
            r.writable = !d.game;
            fs::path root = d.game ? g_gameDir : g_dataDir / d.dataSub;
            if (!d.game)
            {
                std::error_code ec;
                fs::create_directories(root, ec);
            }
            r.host = JoinCaseInsensitive(root, rel);
            return r;
        }

        // Unknown device: try it relative to the disc so the log shows what was asked for.
        LOG("[vfs] unknown device in path '%s', looking on the disc", guestPath.c_str());
        Resolved r;
        size_t colon = p.find(':');
        r.host = JoinCaseInsensitive(g_gameDir, colon == std::string::npos ? p : p.substr(colon + 1));
        r.ok = true;
        return r;
    }
}
