// File system kernel functions (Nt*File, Ob*SymbolicLink).
#include "kernel.h"
#include "memory.h"
#include "objects.h"
#include "vfs.h"

#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3,
                       FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5;
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x1;
    constexpr uint32_t FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3;
    constexpr uint32_t ATTR_DIRECTORY = 0x10, ATTR_NORMAL = 0x80;
    constexpr uint64_t FILE_USE_FILE_POINTER_POSITION = 0xFFFFFFFFFFFFFFFEull;

    int64_t Tell(FILE* f)
    {
#ifdef _WIN32
        return _ftelli64(f);
#else
        return ftello(f);
#endif
    }
    int Seek(FILE* f, int64_t off)
    {
#ifdef _WIN32
        return _fseeki64(f, off, SEEK_SET);
#else
        return fseeko(f, off, SEEK_SET);
#endif
    }

    struct KFileObj : KObject
    {
        KFileObj() : KObject(Type::File) {}
        ~KFileObj() override
        {
            if (file)
                fclose(file);
            if (deleteOnClose)
            {
                std::error_code ec;
                fs::remove(hostPath, ec);
            }
        }
        FILE* file = nullptr;
        fs::path hostPath;
        std::string guestPath;
        bool isDirectory = false;
        bool writable = false;
        bool deleteOnClose = false;
        // directory enumeration
        std::vector<fs::directory_entry> listing;
        size_t listingPos = 0;
        bool listed = false;
    };

    void SetIoStatus(uint32_t ioStatus, uint32_t status, uint32_t info)
    {
        if (ioStatus)
        {
            Store32(ioStatus, status);
            Store32(ioStatus + 4, info);
        }
    }

    void SignalEventHandle(uint32_t handle)
    {
        if (!handle)
            return;
        if (auto ev = LookupHandleAs<KEventObj>(handle))
        {
            std::lock_guard lock(g_sync);
            ev->signaled = true;
            ev->MirrorToGuest();
            NotifySyncChanged();
        }
    }

    std::string ObjectAttributesPath(uint32_t objAttr)
    {
        if (!objAttr)
            return {};
        uint32_t root = Load32(objAttr);
        std::string name = ReadAnsiString(Load32(objAttr + 4));
        if (root)
        {
            if (auto dir = LookupHandleAs<KFileObj>(root))
                return dir->guestPath + "\\" + name;
        }
        return name;
    }

    uint64_t FileSize(const KFileObj& f)
    {
        std::error_code ec;
        if (f.isDirectory)
            return 0;
        auto s = fs::file_size(f.hostPath, ec);
        return ec ? 0 : s;
    }

    // X_FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
    void WriteNetworkOpenInfo(uint32_t p, uint64_t size, bool dir)
    {
        uint64_t now = HostFileTimeNow();
        Store64(p + 0x00, now);
        Store64(p + 0x08, now);
        Store64(p + 0x10, now);
        Store64(p + 0x18, now);
        Store64(p + 0x20, (size + 0xFFF) & ~0xFFFull);
        Store64(p + 0x28, size);
        Store32(p + 0x30, dir ? ATTR_DIRECTORY : ATTR_NORMAL);
        Store32(p + 0x34, 0);
    }

    bool MatchPattern(const std::string& name, const std::string& pattern)
    {
        // Supports "*", "*.ext", "prefix*", exact names, '?'. Case-insensitive.
        size_t n = 0, p = 0, star = std::string::npos, mark = 0;
        auto eq = [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); };
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], name[n]))) { n++; p++; }
            else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = n; }
            else if (star != std::string::npos) { p = star + 1; n = ++mark; }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*')
            p++;
        return p == pattern.size();
    }

    uint32_t OpenFile(uint32_t handleOut, uint32_t desiredAccess, uint32_t objAttr, uint32_t ioStatus,
                      uint32_t disposition, uint32_t createOptions, const char* fn)
    {
        std::string guestPath = ObjectAttributesPath(objAttr);
        vfs::Resolved r = vfs::Resolve(guestPath);
        std::error_code ec;
        bool exists = fs::exists(r.host, ec);
        bool isDir = exists && fs::is_directory(r.host, ec);
        bool wantDir = (createOptions & FILE_DIRECTORY_FILE) != 0;
        bool wantWrite = (desiredAccess & (0x40000000 /*GENERIC_WRITE*/ | 0x2 /*FILE_WRITE_DATA*/ | 0x4 | 0x10000000)) != 0;

        auto file = std::make_shared<KFileObj>();
        file->hostPath = r.host;
        file->guestPath = guestPath;
        file->writable = r.writable;
        uint32_t info = FILE_OPENED;

        if (!exists)
        {
            bool create = disposition == FILE_CREATE || disposition == FILE_OPEN_IF ||
                          disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE;
            if (!create || !r.writable)
            {
                TRACE("[file] %s '%s' -> not found (%s)", fn, guestPath.c_str(), r.host.string().c_str());
                uint32_t st = fs::exists(r.host.parent_path(), ec) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
                SetIoStatus(ioStatus, st, 0);
                return st;
            }
            fs::create_directories(r.host.parent_path(), ec);
            if (wantDir)
            {
                fs::create_directory(r.host, ec);
                file->isDirectory = true;
            }
            else
            {
                file->file = fopen(r.host.string().c_str(), "w+b");
            }
            info = FILE_CREATED;
        }
        else if (isDir)
        {
            file->isDirectory = true;
        }
        else
        {
            if (disposition == FILE_CREATE)
            {
                SetIoStatus(ioStatus, STATUS_OBJECT_NAME_COLLISION, 0);
                return STATUS_OBJECT_NAME_COLLISION;
            }
            bool truncate = disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE;
            if (truncate && r.writable)
            {
                file->file = fopen(r.host.string().c_str(), "w+b");
                info = FILE_OVERWRITTEN;
            }
            else
            {
                file->file = fopen(r.host.string().c_str(), (wantWrite && r.writable) ? "r+b" : "rb");
            }
        }

        if (!file->isDirectory && !file->file)
        {
            LOG("[file] %s '%s': cannot open %s", fn, guestPath.c_str(), r.host.string().c_str());
            SetIoStatus(ioStatus, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }

        uint32_t h = CreateHandle(file);
        Store32(handleOut, h);
        SetIoStatus(ioStatus, STATUS_SUCCESS, info);
        TRACE("[file] %s '%s' -> %08X (%s)", fn, guestPath.c_str(), h, file->isDirectory ? "dir" : "file");
        return STATUS_SUCCESS;
    }
}

GUEST_IMPORT(NtCreateFile)
{
    // (handle*, access, obj_attr, io_status, alloc_size*, attributes, share, disposition, create_options)
    RETURN(OpenFile(ARG32(0), ARG32(1), ARG32(2), ARG32(3), ARG32(7), ARG32(8), "NtCreateFile"));
}

GUEST_IMPORT(NtOpenFile)
{
    // (handle*, access, obj_attr, io_status, share, open_options)
    RETURN(OpenFile(ARG32(0), ARG32(1), ARG32(2), ARG32(3), FILE_OPEN, ARG32(5), "NtOpenFile"));
}

GUEST_IMPORT(NtReadFile)
{
    // (handle, event, apc_routine, apc_context, io_status, buffer, length, byte_offset*)
    uint32_t handle = ARG32(0), event = ARG32(1), apc = ARG32(2), ioStatus = ARG32(4);
    uint32_t buffer = ARG32(5), length = ARG32(6), offsetPtr = ARG32(7);
    auto f = LookupHandleAs<KFileObj>(handle);
    if (!f || !f->file)
        RETURN(STATUS_INVALID_HANDLE);
    if (apc)
        LOG("[file] NtReadFile with APC routine %08X ignored", apc);

    if (offsetPtr)
    {
        uint64_t off = Load64(offsetPtr);
        if (off != FILE_USE_FILE_POINTER_POSITION)
            Seek(f->file, int64_t(off));
    }
    if (length)
        mem::Commit(buffer, length);
    size_t n = fread(GuestToHost(buffer), 1, length, f->file);
    uint32_t status = (n == 0 && length > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    SetIoStatus(ioStatus, status, uint32_t(n));
    SignalEventHandle(event);
    TRACE("[file] NtReadFile %08X off=%llX len=%X -> %zX into %08X", handle,
          offsetPtr ? (unsigned long long)Load64(offsetPtr) : ~0ull, length, n, buffer);
    RETURN(status);
}

GUEST_IMPORT(NtWriteFile)
{
    uint32_t handle = ARG32(0), event = ARG32(1), ioStatus = ARG32(4);
    uint32_t buffer = ARG32(5), length = ARG32(6), offsetPtr = ARG32(7);
    auto f = LookupHandleAs<KFileObj>(handle);
    if (!f || !f->file)
        RETURN(STATUS_INVALID_HANDLE);
    if (!f->writable)
    {
        LOG("[file] write to read-only file %s refused", f->guestPath.c_str());
        RETURN(STATUS_ACCESS_DENIED);
    }
    if (offsetPtr)
    {
        uint64_t off = Load64(offsetPtr);
        if (off != FILE_USE_FILE_POINTER_POSITION)
            Seek(f->file, int64_t(off));
    }
    size_t n = fwrite(GuestToHost(buffer), 1, length, f->file);
    SetIoStatus(ioStatus, STATUS_SUCCESS, uint32_t(n));
    SignalEventHandle(event);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtQueryInformationFile)
{
    // (handle, io_status, info, length, class)
    uint32_t handle = ARG32(0), ioStatus = ARG32(1), info = ARG32(2), length = ARG32(3), cls = ARG32(4);
    auto f = LookupHandleAs<KFileObj>(handle);
    if (!f)
        RETURN(STATUS_INVALID_HANDLE);
    uint64_t size = FileSize(*f);
    switch (cls)
    {
    case 5: // FileStandardInformation
        Store64(info + 0, (size + 0xFFF) & ~0xFFFull);
        Store64(info + 8, size);
        Store32(info + 16, 1);
        Store8(info + 20, 0);
        Store8(info + 21, f->isDirectory ? 1 : 0);
        SetIoStatus(ioStatus, STATUS_SUCCESS, 24);
        RETURN(STATUS_SUCCESS);
    case 14: // FilePositionInformation
        Store64(info, f->file ? uint64_t(Tell(f->file)) : 0);
        SetIoStatus(ioStatus, STATUS_SUCCESS, 8);
        RETURN(STATUS_SUCCESS);
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(info, size, f->isDirectory);
        SetIoStatus(ioStatus, STATUS_SUCCESS, 0x38);
        RETURN(STATUS_SUCCESS);
    case 4: // FileBasicInformation
    {
        uint64_t now = HostFileTimeNow();
        for (int i = 0; i < 4; i++)
            Store64(info + i * 8, now);
        Store32(info + 32, f->isDirectory ? ATTR_DIRECTORY : ATTR_NORMAL);
        SetIoStatus(ioStatus, STATUS_SUCCESS, 40);
        RETURN(STATUS_SUCCESS);
    }
    default:
        LOG("[file] NtQueryInformationFile class %u not implemented (len %u)", cls, length);
        SetIoStatus(ioStatus, STATUS_INVALID_PARAMETER, 0);
        RETURN(STATUS_INVALID_PARAMETER);
    }
}

GUEST_IMPORT(NtSetInformationFile)
{
    // (handle, io_status, info, length, class)
    uint32_t handle = ARG32(0), ioStatus = ARG32(1), info = ARG32(2), cls = ARG32(4);
    auto f = LookupHandleAs<KFileObj>(handle);
    if (!f)
        RETURN(STATUS_INVALID_HANDLE);
    switch (cls)
    {
    case 14: // FilePositionInformation
        if (f->file)
            Seek(f->file, int64_t(Load64(info)));
        break;
    case 13: // FileDispositionInformation
        f->deleteOnClose = Load8(info) != 0 && f->writable;
        break;
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
        if (f->file && f->writable)
        {
            fflush(f->file);
            std::error_code ec;
            fs::resize_file(f->hostPath, Load64(info), ec);
        }
        break;
    case 4: // FileBasicInformation (timestamps) - ignored
        break;
    default:
        LOG("[file] NtSetInformationFile class %u ignored", cls);
        break;
    }
    SetIoStatus(ioStatus, STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtQueryFullAttributesFile)
{
    // (obj_attr, info)
    std::string guestPath = ObjectAttributesPath(ARG32(0));
    vfs::Resolved r = vfs::Resolve(guestPath);
    std::error_code ec;
    if (!fs::exists(r.host, ec))
    {
        TRACE("[file] NtQueryFullAttributesFile '%s' -> not found", guestPath.c_str());
        RETURN(STATUS_OBJECT_NAME_NOT_FOUND);
    }
    bool dir = fs::is_directory(r.host, ec);
    uint64_t size = dir ? 0 : fs::file_size(r.host, ec);
    TRACE("[file] NtQueryFullAttributesFile '%s' -> found, %llu bytes", guestPath.c_str(), (unsigned long long)size);
    WriteNetworkOpenInfo(ARG32(1), size, dir);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    // (handle, io_status, info, length, class)
    uint32_t ioStatus = ARG32(1), info = ARG32(2), length = ARG32(3), cls = ARG32(4);
    switch (cls)
    {
    case 1: // FileFsVolumeInformation
    {
        const char label[] = "VAMPIRERAIN";
        Store64(info + 0, HostFileTimeNow());
        Store32(info + 8, 0x12345678);
        Store32(info + 12, sizeof(label) - 1);
        Store8(info + 16, 0);
        if (length > 17)
            memcpy(GuestToHost(info + 17), label, std::min<size_t>(sizeof(label) - 1, length - 17));
        SetIoStatus(ioStatus, STATUS_SUCCESS, 17 + sizeof(label) - 1);
        RETURN(STATUS_SUCCESS);
    }
    case 3: // FileFsSizeInformation
        Store64(info + 0, 0x100000);   // total allocation units
        Store64(info + 8, 0x80000);    // available
        Store32(info + 16, 32);        // sectors per unit
        Store32(info + 20, 512);       // bytes per sector
        SetIoStatus(ioStatus, STATUS_SUCCESS, 24);
        RETURN(STATUS_SUCCESS);
    case 5: // FileFsAttributeInformation
        Store32(info + 0, 0);
        Store32(info + 4, 255);
        Store32(info + 8, 4);
        if (length >= 16)
            memcpy(GuestToHost(info + 12), "FATX", 4);
        SetIoStatus(ioStatus, STATUS_SUCCESS, 16);
        RETURN(STATUS_SUCCESS);
    default:
        LOG("[file] NtQueryVolumeInformationFile class %u not implemented", cls);
        SetIoStatus(ioStatus, STATUS_INVALID_PARAMETER, 0);
        RETURN(STATUS_INVALID_PARAMETER);
    }
}

GUEST_IMPORT(NtQueryDirectoryFile)
{
    // (handle, event, apc, apc_ctx, io_status, info, length, file_name*, restart_scan)
    uint32_t handle = ARG32(0), event = ARG32(1), ioStatus = ARG32(4), info = ARG32(5), length = ARG32(6);
    uint32_t namePtr = ARG32(7);
    bool restart = ARG32(8) != 0;
    auto f = LookupHandleAs<KFileObj>(handle);
    if (!f || !f->isDirectory)
        RETURN(STATUS_INVALID_HANDLE);

    if (!f->listed || restart)
    {
        std::string pattern = namePtr ? ReadAnsiString(namePtr) : "*";
        if (pattern.empty())
            pattern = "*";
        f->listing.clear();
        std::error_code ec;
        for (auto& e : fs::directory_iterator(f->hostPath, ec))
            if (MatchPattern(e.path().filename().string(), pattern))
                f->listing.push_back(e);
        f->listingPos = 0;
        f->listed = true;
        if (f->listing.empty())
        {
            SetIoStatus(ioStatus, STATUS_NO_SUCH_FILE, 0);
            RETURN(STATUS_NO_SUCH_FILE);
        }
    }

    if (f->listingPos >= f->listing.size())
    {
        SetIoStatus(ioStatus, STATUS_NO_MORE_FILES, 0);
        RETURN(STATUS_NO_MORE_FILES);
    }

    // X_FILE_DIRECTORY_INFORMATION: 0x40-byte header + ANSI name
    auto& e = f->listing[f->listingPos];
    std::string name = e.path().filename().string();
    if (0x40 + name.size() > length)
    {
        SetIoStatus(ioStatus, STATUS_BUFFER_OVERFLOW, 0);
        RETURN(STATUS_BUFFER_OVERFLOW);
    }
    f->listingPos++;
    std::error_code ec;
    bool dir = e.is_directory(ec);
    uint64_t size = dir ? 0 : e.file_size(ec);
    uint64_t now = HostFileTimeNow();
    Store32(info + 0x00, 0);                 // next_entry_offset
    Store32(info + 0x04, uint32_t(f->listingPos));
    for (int i = 0; i < 4; i++)
        Store64(info + 0x08 + i * 8, now);
    Store64(info + 0x28, size);
    Store64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
    Store32(info + 0x38, dir ? ATTR_DIRECTORY : ATTR_NORMAL);
    Store32(info + 0x3C, uint32_t(name.size()));
    memcpy(GuestToHost(info + 0x40), name.data(), name.size());
    SetIoStatus(ioStatus, STATUS_SUCCESS, uint32_t(0x40 + name.size()));
    SignalEventHandle(event);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtFlushBuffersFile)
{
    if (auto f = LookupHandleAs<KFileObj>(ARG32(0)); f && f->file)
        fflush(f->file);
    SetIoStatus(ARG32(1), STATUS_SUCCESS, 0);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ObCreateSymbolicLink)
{
    std::string link = ReadAnsiString(ARG32(0));
    std::string target = ReadAnsiString(ARG32(1));
    LOG("[file] symlink %s -> %s", link.c_str(), target.c_str());
    vfs::AddSymlink(link, target);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ObDeleteSymbolicLink)
{
    vfs::RemoveSymlink(ReadAnsiString(ARG32(0)));
    RETURN(STATUS_SUCCESS);
}
