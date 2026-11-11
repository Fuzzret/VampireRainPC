// xam.xex exports with real implementations.
#include "kernel.h"
#include "input.h"
#include "settings.h"
void CamTraceOnInput(uint32_t block, int16_t rx, int16_t ry);
#include <cstring>
#include "memory.h"
#include "objects.h"
#include "xex_loader.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>
#include <map>
#include <deque>
#include "vfs.h"

namespace
{
    // XOVERLAPPED: be32 InternalLow, InternalHigh, InternalContext, hEvent,
    // pCompletionRoutine, dwCompletionContext, dwExtendedError
    uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0)
    {
        if (!overlapped)
            return result;
        Store32(overlapped + 0x00, result);
        Store32(overlapped + 0x04, length);
        Store32(overlapped + 0x18, result);
        if (uint32_t ev = Load32(overlapped + 0x0C))
        {
            if (auto e = LookupHandleAs<KEventObj>(ev))
            {
                std::lock_guard lock(g_sync);
                e->signaled = true;
                e->MirrorToGuest();
                NotifySyncChanged();
            }
        }
        return ERROR_IO_PENDING;
    }

    struct KEnumeratorObj : KObject
    {
        KEnumeratorObj() : KObject(Type::Enumerator) {}
        std::vector<std::vector<uint8_t>> items; // raw guest structures, in order
        size_t pos = 0;
    };
}

// ===========================================================================
// Input (XInput on Windows; no controller elsewhere)
// ===========================================================================
// XINPUT_GAMEPAD: be16 buttons, u8 lt, u8 rt, be16 lx, ly, rx, ry
[[maybe_unused]] static void WriteGamepad(uint32_t p, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    Store16(p + 0, buttons);
    Store8(p + 2, lt);
    Store8(p + 3, rt);
    Store16(p + 4, uint16_t(lx));
    Store16(p + 6, uint16_t(ly));
    Store16(p + 8, uint16_t(rx));
    Store16(p + 10, uint16_t(ry));
}

GUEST_IMPORT(XamInputGetState)
{
    // (user_index, flags, state*)
    uint32_t user = ARG32(0) & 0xFF, state = ARG32(2);
    if (user == 0xFF)
        user = 0;
#ifdef _WIN32
    XINPUT_STATE s{};
    int padIndex = input::PadForPlayer(user);
    bool pad = padIndex >= 0 && XInputGetState(DWORD(padIndex), &s) == ERROR_SUCCESS;
    // Player 1 also gets the keyboard and mouse, so a controller is always
    // there for them.
    if (pad || (user == 0 && input::Enabled()))
    {
        auto g = s.Gamepad;
        if (user == 0)
        {
            input::Poll(g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
            // The game's polling function keeps its processed input at r31.
            CamTraceOnInput(ctx.r31.u32, g.sThumbRX, g.sThumbRY);
        }
        // The packet number tells the game whether anything changed.
        static uint32_t packet[4] = {};
        static XINPUT_GAMEPAD last[4] = {};
        if (memcmp(&g, &last[user], sizeof(g)) != 0)
        {
            last[user] = g;
            packet[user]++;
        }
        Store32(state, packet[user]);
        WriteGamepad(state + 4, g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
        RETURN(ERROR_SUCCESS);
    }
#endif
    (void)state;
    RETURN(ERROR_DEVICE_NOT_CONNECTED);
}

GUEST_IMPORT(XamInputGetCapabilities)
{
    // (user_index, flags, caps*)  XINPUT_CAPABILITIES: u8 type, u8 subtype, be16 flags, gamepad, vibration
    uint32_t user = ARG32(0) & 0xFF, caps = ARG32(2);
    if (user == 0xFF)
        user = 0;
#ifdef _WIN32
    XINPUT_CAPABILITIES c{};
    int padIndex = input::PadForPlayer(user);
    if ((padIndex >= 0 && XInputGetCapabilities(DWORD(padIndex), 0, &c) == ERROR_SUCCESS) || (user == 0 && input::Enabled()))
    {
        Store8(caps + 0, 1);   // XINPUT_DEVTYPE_GAMEPAD
        Store8(caps + 1, 1);   // XINPUT_DEVSUBTYPE_GAMEPAD
        Store16(caps + 2, 0);
        WriteGamepad(caps + 4, 0xF3FF, 0xFF, 0xFF, 0x7FC0, 0x7FC0, 0x7FC0, 0x7FC0);
        Store16(caps + 16, 0xFFFF);
        Store16(caps + 18, 0xFFFF);
        RETURN(ERROR_SUCCESS);
    }
#endif
    (void)caps;
    RETURN(ERROR_DEVICE_NOT_CONNECTED);
}

GUEST_IMPORT(XamInputSetState)
{
    // (user_index, unknown, vibration*)  XINPUT_VIBRATION: be16 left, be16 right
    uint32_t user = ARG32(0) & 0xFF;
    if (user == 0xFF)
        user = 0;
    if (input::PadForPlayer(user) < 0)
        RETURN(user == 0 && input::Enabled() ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED); // keyboard: nothing to shake
    if (ARG32(2))
        input::SetVibration(user, Load16(ARG32(2)), Load16(ARG32(2) + 2));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamInputGetKeystrokeEx) { RETURN(ERROR_EMPTY); }

// ===========================================================================
// Users: one local, signed-in (offline) profile in slot 0
// ===========================================================================
// The profile reports being signed in to Xbox Live (state 2) so the game's
// online menus are available; its "online" play is served over the LAN
// (net.cpp). Each copy/PC needs a different XUID and name to tell players
// apart: derived from the player name and the LAN test copy number.
static uint64_t Xuid()
{
    static const uint64_t xuid = [] {
        const auto& st = settings::Get();
        uint64_t h = 0xCBF29CE484222325ull;
        for (char c : st.playerName)
            h = (h ^ uint8_t(c)) * 0x100000001B3ull;
        h ^= uint64_t(st.lanTestInstance) * 0x9E3779B97F4A7C15ull;
        return 0x0009000000000000ull | (h & 0x0000FFFFFFFFFFFFull); // online XUID
    }();
    return xuid;
}

GUEST_IMPORT(XamUserGetSigninState) { RETURN(ARG32(0) == 0 ? (settings::Get().liveSignIn ? 2 : 1) : 0); }

GUEST_IMPORT(XamUserGetXUID)
{
    // (user_index, type_mask, xuid*)
    if (ARG32(0) != 0)
        RETURN(ERROR_NO_SUCH_USER);
    if (ARG32(2))
        Store64(ARG32(2), Xuid());
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserGetName)
{
    // (user_index, buffer, buffer_size)
    if (ARG32(0) != 0)
        RETURN(ERROR_NO_SUCH_USER);
    const std::string& name = settings::Get().playerName;
    uint32_t size = ARG32(2);
    if (size)
    {
        uint32_t n = std::min<uint32_t>(size - 1, std::min<uint32_t>(uint32_t(name.size()), 15));
        memcpy(GuestToHost(ARG32(1)), name.data(), n);
        Store8(ARG32(1) + n, 0);
    }
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserCheckPrivilege)
{
    // (user_index, privilege, result*)
    if (ARG32(2))
        Store32(ARG32(2), 1);
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserAreUsersFriends)
{
    // (user_index, xuids*, count, results*, overlapped)
    uint32_t count = ARG32(2), results = ARG32(3);
    for (uint32_t i = 0; results && i < count; i++)
        Store32(results + i * 4, 0);
    RETURN(CompleteOverlapped(ARG32(4), ERROR_SUCCESS));
}

// ===========================================================================
// Notifications
// ===========================================================================
// Notification listeners get the system's state at creation, like a console
// right after boot: user 0 signed in, and (when reporting a Live sign-in)
// connected to Xbox Live with the network link up. Games wait for these
// before enabling their online menus.
namespace
{
    struct Listener { uint64_t areas; std::deque<std::pair<uint32_t, uint32_t>> queue; std::shared_ptr<KEventObj> ev; };
    std::mutex g_notifyMutex;
    std::map<uint32_t, Listener> g_listeners;

    // Queues a notification for every listener of its area (area = id >> 25:
    // 0 system, 1 Live, 2 friends, ...).
    void PostNotification(uint32_t id, uint32_t param)
    {
        uint64_t area = 1ull << (id >> 25);
        std::lock_guard lock(g_notifyMutex);
        for (auto& [h, l] : g_listeners)
        {
            if (!(l.areas & area))
                continue;
            l.queue.push_back({ id, param });
            std::lock_guard lock2(g_sync);
            l.ev->signaled = true;
            l.ev->MirrorToGuest();
            NotifySyncChanged();
        }
    }

    // A system UI (guide, sign-in, message box...) opened and closed at once:
    // XN_SYS_UI shown, then hidden. Games pause their menus while it is shown.
    void SystemUiFlash()
    {
        PostNotification(0x00000009, 1);
        PostNotification(0x00000009, 0);
    }
}

GUEST_IMPORT(XamNotifyCreateListener)
{
    // (areas mask (64-bit: bit 0 system, 1 Live, 2 friends, ...), version)
    uint64_t areas = ctx.r3.u64;
    auto ev = std::make_shared<KEventObj>(true, false);
    uint32_t h = CreateHandle(ev);
    Listener l;
    l.areas = areas;
    l.ev = ev;
    if (areas & 1)
    {
        // XN_SYS_UI: no system UI showing. Vampire Rain starts out assuming
        // one is (and its Xbox Live menu waits for it to close).
        l.queue.push_back({ 0x00000009, 0 });
        l.queue.push_back({ 0x0000000A, 1 }); // XN_SYS_SIGNINCHANGED: user 0
    }
    if ((areas & 2) && settings::Get().liveSignIn)
    {
        l.queue.push_back({ 0x02000002, 1 });          // XN_LIVE_LINK_STATE_CHANGED: link up
        l.queue.push_back({ 0x02000001, 0x00151000 }); // XN_LIVE_CONNECTIONCHANGED: connection established
    }
    if (!l.queue.empty())
    {
        std::lock_guard lock(g_sync);
        ev->signaled = true;
        ev->MirrorToGuest();
        NotifySyncChanged();
    }
    LOG("[xam] notification listener %08X for areas %llX (%zu initial notifications)", h, (unsigned long long)areas, l.queue.size());
    std::lock_guard lock(g_notifyMutex);
    g_listeners[h] = std::move(l);
    RETURN(h);
}

GUEST_IMPORT(XNotifyGetNext)
{
    // (listener, id filter (0 = any), id*, param*) -> TRUE if one was taken
    uint32_t h = ARG32(0), filter = ARG32(1);
    std::lock_guard lock(g_notifyMutex);
    auto it = g_listeners.find(h);
    if (it == g_listeners.end())
        RETURN(0);
    auto& q = it->second.queue;
    for (auto n = q.begin(); n != q.end(); ++n)
    {
        if (filter && n->first != filter)
            continue;
        if (ARG32(2))
            Store32(ARG32(2), n->first);
        if (ARG32(3))
            Store32(ARG32(3), n->second);
        LOG("[xam] notification %08X (param %08X) delivered", n->first, n->second);
        q.erase(n);
        if (q.empty())
        {
            std::lock_guard lock2(g_sync);
            it->second.ev->signaled = false;
            it->second.ev->MirrorToGuest();
        }
        RETURN(1);
    }
    RETURN(0);
}
GUEST_IMPORT(XNotifyPositionUI) { RETURN(0); }

// ===========================================================================
// Content (save games)
//
// A save is a folder: <data>/content/<content type>/<file name>/, with the
// title shown in the dashboard kept in "<file name>.title" beside it. Opening
// one with XamContentCreateEx mounts it as a drive ("save:" etc.); the game
// then uses normal file I/O on it.
//
// XCONTENT_DATA (0x134 bytes): be32 device_id, be32 content_type,
// be16 display_name[128], char file_name[42], pad[2]
// ===========================================================================
namespace
{
    constexpr uint32_t kHddDeviceId = 1;
    constexpr uint32_t kContentDataSize = 0x134;
    constexpr uint32_t ERROR_FILE_NOT_FOUND_ = 2;
    constexpr uint32_t ERROR_PATH_NOT_FOUND_ = 3;
    constexpr uint32_t ERROR_ALREADY_EXISTS_ = 183;
    constexpr uint32_t ERROR_INSUFFICIENT_BUFFER_ = 122;

    std::filesystem::path ContentRoot(uint32_t contentType)
    {
        char sub[16];
        snprintf(sub, sizeof(sub), "%08X", contentType);
        return vfs::DataDir() / "content" / sub;
    }

    std::string ContentFileName(uint32_t data)
    {
        std::string name;
        for (uint32_t i = 0; i < 42; i++)
        {
            char c = char(Load8(data + 0x108 + i));
            if (!c) break;
            // Keep it a safe single path component.
            name += (c == '/' || c == '\\' || c == ':') ? '_' : c;
        }
        return name.empty() ? std::string("default") : name;
    }

    std::string RootName(uint32_t addr)
    {
        std::string root;
        for (uint32_t i = 0; i < 16; i++)
        {
            char c = char(Load8(addr + i));
            if (!c || c == ':') break;
            root += c;
        }
        return root;
    }
}

GUEST_IMPORT(XamContentCreateEx)
{
    // (user_index, root_name, content_data, flags, disposition*, license_mask*,
    //  cache_size, content_size, overlapped)
    std::string root = RootName(ARG32(1));
    uint32_t data = ARG32(2);
    uint32_t flags = ARG32(3);
    uint32_t contentType = Load32(data + 4);
    std::string fileName = ContentFileName(data);
    std::filesystem::path dir = ContentRoot(contentType) / fileName;
    std::error_code ec;
    bool exists = std::filesystem::is_directory(dir, ec);

    uint32_t result = ERROR_SUCCESS;
    uint32_t disposition = 0; // 1 = created, 2 = opened
    switch (flags & 0xF)
    {
    case 1: // create new
        if (exists) result = ERROR_ALREADY_EXISTS_;
        else disposition = 1;
        break;
    case 2: // create always (existing contents are removed)
        if (exists) std::filesystem::remove_all(dir, ec);
        disposition = 1;
        break;
    case 3: // open existing
        if (!exists) result = ERROR_PATH_NOT_FOUND_;
        else disposition = 2;
        break;
    case 4: // open always
        disposition = exists ? 2 : 1;
        break;
    case 5: // truncate existing
        if (!exists) result = ERROR_PATH_NOT_FOUND_;
        else { std::filesystem::remove_all(dir, ec); disposition = 2; }
        break;
    default:
        disposition = exists ? 2 : 1;
        break;
    }
    if (result == ERROR_SUCCESS)
    {
        vfs::Mount(root, dir);
        if (disposition == 1)
        {
            // Remember the title the game gave the save, for enumeration.
            std::ofstream t(ContentRoot(contentType) / (fileName + ".title"), std::ios::binary);
            for (uint32_t i = 0; i < 128; i++)
            {
                uint16_t ch = Load16(data + 8 + i * 2);
                t.put(char(ch >> 8));
                t.put(char(ch & 0xFF));
                if (!ch) break;
            }
        }
    }
    LOG("[xam] XamContentCreateEx %s: %s type %08X flags %X -> %s", root.c_str(), fileName.c_str(), contentType, flags,
        result ? "error" : (disposition == 1 ? "created" : "opened"));
    if (ARG32(4))
        Store32(ARG32(4), disposition);
    if (ARG32(5))
        Store32(ARG32(5), 0xFFFFFFFF);
    RETURN(CompleteOverlapped(ARG32(8), result));
}

GUEST_IMPORT(XamContentClose)
{
    // (root_name, overlapped)
    vfs::Unmount(RootName(ARG32(0)));
    RETURN(CompleteOverlapped(ARG32(1), ERROR_SUCCESS));
}

GUEST_IMPORT(XamContentGetCreator)
{
    // (user_index, content_data, is_creator*, xuid*, overlapped)
    if (ARG32(2))
        Store32(ARG32(2), 1);
    if (ARG32(3))
        Store64(ARG32(3), Xuid());
    RETURN(CompleteOverlapped(ARG32(4), ERROR_SUCCESS));
}

GUEST_IMPORT(XamContentGetDeviceData)
{
    // (device_id, X_DEVICE_DATA*): be32 id, be32 type, be64 total, be64 free, be16 name[28]
    if (ARG32(0) != kHddDeviceId)
        RETURN(ERROR_DEVICE_NOT_CONNECTED);
    uint32_t d = ARG32(1);
    for (uint32_t i = 0; i < 0x50; i += 4)
        Store32(d + i, 0);
    Store32(d + 0x00, kHddDeviceId);
    Store32(d + 0x04, 1); // hard drive
    Store64(d + 0x08, 20ull << 30);
    Store64(d + 0x10, 16ull << 30);
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i]; i++)
        Store16(d + 0x18 + i * 2, uint16_t(name[i]));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamContentCreateEnumerator)
{
    // (user, device_id, content_type, flags, items_per_enum, buffer_size*, handle*)
    uint32_t device = ARG32(1), contentType = ARG32(2);
    auto e = std::make_shared<KEnumeratorObj>();
    if (device == 0 || device == kHddDeviceId)
    {
        std::error_code ec;
        std::filesystem::path root = ContentRoot(contentType);
        for (auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            if (!entry.is_directory(ec))
                continue;
            std::string fileName = entry.path().filename().string();
            if (fileName.size() > 41)
                continue;
            std::vector<uint8_t> item(kContentDataSize, 0);
            auto put32 = [&](size_t off, uint32_t v) { item[off] = uint8_t(v >> 24); item[off + 1] = uint8_t(v >> 16); item[off + 2] = uint8_t(v >> 8); item[off + 3] = uint8_t(v); };
            put32(0, kHddDeviceId);
            put32(4, contentType);
            std::ifstream t(root / (fileName + ".title"), std::ios::binary);
            std::vector<char> title((std::istreambuf_iterator<char>(t)), {});
            memcpy(&item[8], title.data(), std::min<size_t>(title.size(), 254)); // already big-endian UTF-16
            memcpy(&item[0x108], fileName.data(), fileName.size());
            e->items.push_back(std::move(item));
        }
    }
    LOG("[xam] content enumerator for type %08X: %zu save(s)", contentType, e->items.size());
    if (ARG32(5))
        Store32(ARG32(5), std::max<uint32_t>(ARG32(4), 1) * kContentDataSize);
    Store32(ARG32(6), CreateHandle(e));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserCreateStatsEnumerator)
{
    // (title_id, xuid, unk, start, count, view_count, view_ids, buffer_size*, handle*)
    if (ARG32(7))
        Store32(ARG32(7), 0x100);
    Store32(ARG32(8), CreateHandle(std::make_shared<KEnumeratorObj>()));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamEnumerate)
{
    // (handle, flags, buffer, buffer_size, items_returned*, overlapped)
    auto obj = LookupHandle(ARG32(0));
    KEnumeratorObj* e = (obj && obj->type == KObject::Type::Enumerator) ? static_cast<KEnumeratorObj*>(obj.get()) : nullptr;
    uint32_t buffer = ARG32(2), size = ARG32(3), count = 0;
    if (e)
    {
        uint32_t used = 0;
        while (e->pos < e->items.size() && used + e->items[e->pos].size() <= size)
        {
            const auto& item = e->items[e->pos++];
            for (size_t i = 0; i < item.size(); i++)
                Store8(buffer + used + uint32_t(i), item[i]);
            used += uint32_t(item.size());
            count++;
        }
    }
    if (ARG32(4))
        Store32(ARG32(4), count);
    uint32_t result = count ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
    if (ARG32(5))
    {
        CompleteOverlapped(ARG32(5), result, count);
        RETURN(ERROR_IO_PENDING);
    }
    RETURN(result);
}

GUEST_IMPORT(XamContentGetDeviceState)
{
    // (device_id, overlapped)
    RETURN(CompleteOverlapped(ARG32(1), ERROR_SUCCESS));
}

// ===========================================================================
// Profile settings: none are stored; every requested setting comes back as
// "no value" with the right type, so the game uses its defaults.
// X_USER_PROFILE_SETTING (0x28): be32 source, pad, be32 user_index (at 8),
// pad, be32 setting_id (at 0x10), pad, X_USER_DATA (at 0x18: u8 type, value at +8)
// ===========================================================================
GUEST_IMPORT(XamUserReadProfileSettings)
{
    // (title_id, user_index, xuid_count, xuids, setting_count, setting_ids,
    //  buffer_size*, buffer, overlapped)
    uint32_t userIndex = ARG32(1), count = ARG32(4), ids = ARG32(5);
    uint32_t sizePtr = ARG32(6), buffer = ARG32(7), overlapped = ARG32(8);
    uint32_t needed = 8 + count * 0x28;
    if (!sizePtr)
        RETURN(CompleteOverlapped(overlapped, 87)); // ERROR_INVALID_PARAMETER
    if (!buffer || Load32(sizePtr) < needed)
    {
        Store32(sizePtr, needed);
        RETURN(CompleteOverlapped(overlapped, ERROR_INSUFFICIENT_BUFFER_));
    }
    for (uint32_t i = 0; i < needed; i += 4)
        Store32(buffer + i, 0);
    Store32(buffer + 0, count);
    Store32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = Load32(ids + i * 4);
        uint32_t s = buffer + 8 + i * 0x28;
        Store32(s + 0x00, 0);          // source: none (not set) - the game uses its own default
        Store32(s + 0x08, userIndex);
        Store32(s + 0x10, id);
        Store8(s + 0x18, uint8_t((id >> 28) & 0xF)); // data type from the id
        if (id == 0x10040003)          // XPROFILE_OPTION_CONTROLLER_VIBRATION: on (as a console's default profile)
        {
            Store32(s + 0x00, 1);      // source: default
            Store32(s + 0x20, 3);
        }
    }
    RETURN(CompleteOverlapped(overlapped, ERROR_SUCCESS));
}

// ===========================================================================
// System UI
// ===========================================================================
GUEST_IMPORT(XamShowSigninUI)
{
    LOG("[xam] sign-in UI requested (already signed in)");
    SystemUiFlash();
    PostNotification(0x0000000A, 1); // sign-in "changed": user 0 still signed in
    RETURN(ERROR_SUCCESS);
}
GUEST_IMPORT(XamShowPlayerReviewUI) { SystemUiFlash(); RETURN(ERROR_SUCCESS); }
GUEST_IMPORT(XamShowGamerCardUIForXUID) { SystemUiFlash(); RETURN(ERROR_SUCCESS); }

static void LogMessageBox(uint32_t title, uint32_t text)
{
    LOG("[xam] message box: '%s' - '%s'", GuestWideToAscii(title).c_str(), GuestWideToAscii(text).c_str());
}

GUEST_IMPORT(XamShowMessageBoxUI)
{
    // (user, title, text, button_count, buttons, active_button, flags, result*, overlapped)
    LogMessageBox(ARG32(1), ARG32(2));
    SystemUiFlash();
    if (ARG32(7))
        Store32(ARG32(7), ARG32(5));
    RETURN(CompleteOverlapped(ARG32(8), ERROR_SUCCESS));
}

GUEST_IMPORT(XamShowMessageBoxUIEx)
{
    // (user, title, text, button_count, buttons, active_button, flags, unknown, result*, overlapped)
    LogMessageBox(ARG32(1), ARG32(2));
    SystemUiFlash();
    if (ARG32(8))
        Store32(ARG32(8), ARG32(5));
    RETURN(CompleteOverlapped(ARG32(9), ERROR_SUCCESS));
}

GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    // (user, content_type, content_flags, total_requested, device_id*, overlapped)
    if (ARG32(4))
        Store32(ARG32(4), 1); // hard drive
    SystemUiFlash();
    RETURN(CompleteOverlapped(ARG32(5), ERROR_SUCCESS));
}

GUEST_IMPORT(XamShowDirtyDiscErrorUI)
{
    FatalExit("the game reported a disc read error (XamShowDirtyDiscErrorUI) - a file it needs is probably missing");
}

// ===========================================================================
// System information
// ===========================================================================
GUEST_IMPORT(XGetAVPack) { RETURN(6); }          // HDMI
GUEST_IMPORT(XGetGameRegion) { RETURN(0xFFFF); }
GUEST_IMPORT(XGetLanguage) { RETURN(1); }        // English
GUEST_IMPORT(XamGetSystemVersion) { RETURN(0x20000000); }

// X_VIDEO_MODE: width, height, interlaced, widescreen, hi_def, refresh (float), standard, ...
void WriteVideoMode(uint32_t p)
{
    memset(GuestToHost(p), 0, 0x30);
    Store32(p + 0x00, 1280);
    Store32(p + 0x04, 720);
    Store32(p + 0x08, 0);
    Store32(p + 0x0C, 1);
    Store32(p + 0x10, 1);
    StoreF32(p + 0x14, 60.0f);
    Store32(p + 0x18, 1);      // NTSC-M
    Store32(p + 0x1C, 0x4A);
    Store32(p + 0x20, 0x01);
}

GUEST_IMPORT(XGetVideoMode) { WriteVideoMode(ARG32(0)); RETURN(0); }

GUEST_IMPORT(XamGetExecutionId)
{
    if (ARG32(0))
        Store32(ARG32(0), XexHeaderField(0x00040006));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamAlloc)
{
    // (flags, size, out*)
    uint32_t p = mem::g_virtualHeap.Alloc(ARG32(1), 16);
    if (ARG32(2))
        Store32(ARG32(2), p);
    RETURN(p ? ERROR_SUCCESS : 8 /*ERROR_NOT_ENOUGH_MEMORY*/);
}

GUEST_IMPORT(XamFree)
{
    mem::g_virtualHeap.Free(ARG32(0));
    RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamLoaderTerminateTitle) { LOG("[xam] title requested exit"); fflush(stdout); std::_Exit(0); }
GUEST_IMPORT(XamLoaderLaunchTitle) { LOG("[xam] title tried to launch '%s'; exiting", GuestStr(ARG32(0)) ? GuestStr(ARG32(0)) : ""); fflush(stdout); std::_Exit(0); }

// Networking: see net.cpp
