// LAN play ("System Link"): the Xbox network calls the game makes, on top
// of ordinary Windows UDP sockets.
//
// - Sockets: the game's UDP (and VDP, the Xbox's game-data flavour of UDP -
//   both ends are this runtime, so it's sent as plain UDP) sockets become
//   real sockets. Broadcasts go to the whole LAN.
// - Addresses: our XNADDR carries the PC's LAN IP address; converting a peer's
//   XNADDR back to an IP address just reads it out again.
// - QoS (how a game in the lobby is probed for its details): a small UDP
//   service on port 3076 answers probes with the data the host registered.
// - Same-PC testing: with lan_test_instance = N (1-4) in settings.ini each
//   copy of the game uses its own loopback address 127.0.0.N, and broadcasts
//   go to the other copies.
//
// - Sessions (lobbies): hosted sessions are advertised on the QoS port, and
//   the game's session search asks the LAN (or the other test copies) for
//   them; see "Sessions" below.
#include "kernel.h"
#include "memory.h"
#include "objects.h"
#include "settings.h"
#include "thread.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using HostSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
using HostSocket = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define ioctlsocket ioctl
#endif

namespace
{
    constexpr uint16_t kQosPort = 3076;
    constexpr uint32_t kSocketError = 0xFFFFFFFF;

    std::mutex g_netMutex;
    bool g_netStarted = false;
    uint32_t g_localIp = 0;   // network byte order
    uint32_t g_testInstance = 0;
    std::map<uint32_t, HostSocket> g_sockets;
    uint32_t g_nextSocket = 0x100;
    std::atomic<int> g_logBudget{ 400 };

#define NETLOG(...) do { if (g_logBudget.fetch_sub(1) > 0) LOG(__VA_ARGS__); } while (0)

    uint32_t TestIp(uint32_t n) { return htonl(0x7F000000u | n); } // 127.0.0.n

    void StartNet()
    {
        std::lock_guard lock(g_netMutex);
        if (g_netStarted)
            return;
        g_netStarted = true;
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        g_testInstance = settings::Get().lanTestInstance;
        if (g_testInstance)
        {
            g_localIp = TestIp(g_testInstance);
            LOG("[net] same-PC test mode: this copy is 127.0.0.%u", g_testInstance);
            return;
        }
        // The PC's LAN address: prefer private ranges.
        char host[256] = {};
        gethostname(host, sizeof(host));
        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0)
        {
            uint32_t best = 0;
            for (addrinfo* a = res; a; a = a->ai_next)
            {
                uint32_t ip = reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr;
                uint32_t h = ntohl(ip);
                bool priv = (h >> 24) == 10 || (h >> 16) == 0xC0A8 || ((h >> 20) == 0xAC1);
                if ((h >> 24) == 127)
                    continue;
                if (!best || priv)
                    best = ip;
                if (priv)
                    break;
            }
            freeaddrinfo(res);
            g_localIp = best;
        }
        if (!g_localIp)
            g_localIp = TestIp(1);
        uint32_t h = ntohl(g_localIp);
        LOG("[net] LAN address %u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
    }

    HostSocket Lookup(uint32_t s)
    {
        std::lock_guard lock(g_netMutex);
        auto it = g_sockets.find(s);
        return it == g_sockets.end() ? HostSocket(INVALID_SOCKET) : it->second;
    }

    // Guest sockaddr_in: be16 family, then port and address already in
    // network byte order (same bytes as the host's).
    sockaddr_in FromGuest(uint32_t p)
    {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        memcpy(&a.sin_port, GuestToHost(p + 2), 2);
        memcpy(&a.sin_addr, GuestToHost(p + 4), 4);
        return a;
    }
    void ToGuest(uint32_t p, const sockaddr_in& a)
    {
        Store16(p, 2);
        memcpy(GuestToHost(p + 2), &a.sin_port, 2);
        memcpy(GuestToHost(p + 4), &a.sin_addr, 4);
        memset(GuestToHost(p + 8), 0, 8);
    }

    std::string AddrStr(const sockaddr_in& a)
    {
        char b[48];
        uint32_t h = ntohl(a.sin_addr.s_addr);
        snprintf(b, sizeof(b), "%u.%u.%u.%u:%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255, ntohs(a.sin_port));
        return b;
    }

    bool IsBroadcast(uint32_t ipNet)
    {
        uint32_t h = ntohl(ipNet);
        return h == 0xFFFFFFFFu || (h & 0xFF) == 0xFF;
    }

    void FillXnAddr(uint32_t p)
    {
        // XNADDR: ina, inaOnline, be16 wPortOnline, abEnet[6], abOnline[20]
        memset(GuestToHost(p), 0, 36);
        memcpy(GuestToHost(p), &g_localIp, 4);
        memcpy(GuestToHost(p + 4), &g_localIp, 4);
        Store16(p + 8, 3074);
        uint8_t* enet = GuestToHost(p + 10);
        enet[0] = 0x00; enet[1] = 0x22; enet[2] = 0x48; // made-up MAC from the IP
        memcpy(enet + 3, reinterpret_cast<uint8_t*>(&g_localIp) + 1, 3);
        memcpy(GuestToHost(p + 16), &g_localIp, 4);
    }

    // ---- QoS: hosts answer probes with the data they registered ----
    struct QosListen { std::vector<uint8_t> data; bool enabled = false; };
    std::map<uint64_t, QosListen> g_qosListen;
    std::mutex g_qosMutex;
    HostSocket g_qosSocket = INVALID_SOCKET;

    uint64_t Kid(uint32_t p)
    {
        uint64_t k;
        memcpy(&k, GuestToHost(p), 8);
        return k;
    }

    std::vector<uint8_t> BuildAdverts(); // sessions this copy hosts (below)

    void QosServer()
    {
        ProfileRegisterCurrentThread("lan qos");
        uint8_t buf[2048];
        for (;;)
        {
            sockaddr_in from = {};
#ifdef _WIN32
            int fl = sizeof(from);
#else
            socklen_t fl = sizeof(from);
#endif
            int n = recvfrom(g_qosSocket, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n >= 4 && memcmp(buf, "VRS1", 4) == 0) // session search
            {
                if (g_testInstance && from.sin_addr.s_addr == g_localIp)
                    continue;
                auto reply = BuildAdverts();
                sendto(g_qosSocket, reinterpret_cast<const char*>(reply.data()), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
                uint32_t count;
                memcpy(&count, reply.data() + 4, 4);
                if (count)
                    NETLOG("[net] session search from %s: answered with %u session(s)", AddrStr(from).c_str(), count);
                continue;
            }
            if (n < 16 || memcmp(buf, "VRQ1", 4) != 0)
                continue;
            uint64_t kid;
            memcpy(&kid, buf + 4, 8);
            uint8_t reply[1500];
            memcpy(reply, "VRQ2", 4);
            memcpy(reply + 4, buf + 12, 4); // nonce
            uint16_t len = 0;
            uint8_t enabled = 0;
            {
                std::lock_guard lock(g_qosMutex);
                auto it = g_qosListen.find(kid);
                if (it != g_qosListen.end())
                {
                    enabled = it->second.enabled ? 1 : 0;
                    len = uint16_t(std::min<size_t>(it->second.data.size(), sizeof(reply) - 11));
                    memcpy(reply + 11, it->second.data.data(), len);
                }
            }
            reply[8] = enabled;
            memcpy(reply + 9, &len, 2);
            sendto(g_qosSocket, reinterpret_cast<const char*>(reply), 11 + len, 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
            NETLOG("[net] QoS probe from %s answered (%s, %u bytes)", AddrStr(from).c_str(), enabled ? "listening" : "not listening", len);
        }
    }

    void StartQosServer()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            g_qosSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_port = htons(kQosPort);
            a.sin_addr.s_addr = g_testInstance ? g_localIp : INADDR_ANY;
            if (g_qosSocket == HostSocket(INVALID_SOCKET) || bind(g_qosSocket, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
            {
                LOG("[net] couldn't open the QoS port %u - other players won't see this game's details", kQosPort);
                return;
            }
            StartHostThread(QosServer, 1 << 20);
        });
    }

    void SignalGuestEvent(uint32_t handle)
    {
        if (!handle)
            return;
        if (auto e = LookupHandleAs<KEventObj>(handle))
        {
            std::lock_guard lock(g_sync);
            e->signaled = true;
            e->MirrorToGuest();
            NotifySyncChanged();
        }
    }
}

// ===========================================================================
// Winsock
// ===========================================================================
GUEST_IMPORT(NetDll_WSAStartup)
{
    // (caller, version, wsadata*)
    StartNet();
    // WSADATA: be16 wVersion, be16 wHighVersion, ... The game checks that the
    // version it asked for (2.2) is what it got.
    if (ARG32(2))
    {
        memset(GuestToHost(ARG32(2)), 0, 0x190);
        Store16(ARG32(2), 0x0202);
        Store16(ARG32(2) + 2, 0x0202);
    }
    NETLOG("[net] WSAStartup(version %04X)", ARG32(1));
    RETURN(0);
}
GUEST_IMPORT(NetDll_WSACleanup) { RETURN(0); }
GUEST_IMPORT(NetDll_XNetStartup) { StartNet(); RETURN(0); }
GUEST_IMPORT(NetDll_XNetCleanup) { RETURN(0); }

GUEST_IMPORT(NetDll_socket)
{
    // (caller, af, type, protocol). VDP (protocol 254) is sent as UDP.
    StartNet();
    uint32_t type = ARG32(2), proto = ARG32(3);
    if (type != 2) // only datagram sockets (SOCK_DGRAM) are used for System Link
    {
        NETLOG("[net] socket(type %u, protocol %u) not supported", type, proto);
        RETURN(kSocketError);
    }
    HostSocket h = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h == HostSocket(INVALID_SOCKET))
        RETURN(kSocketError);
    int on = 1;
    setsockopt(h, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    std::lock_guard lock(g_netMutex);
    uint32_t id = g_nextSocket++;
    g_sockets[id] = h;
    NETLOG("[net] socket %u (%s)", id, proto == 254 ? "VDP" : "UDP");
    RETURN(id);
}

GUEST_IMPORT(NetDll_closesocket)
{
    HostSocket h = INVALID_SOCKET;
    {
        std::lock_guard lock(g_netMutex);
        auto it = g_sockets.find(ARG32(1));
        if (it != g_sockets.end())
        {
            h = it->second;
            g_sockets.erase(it);
        }
    }
    if (h != HostSocket(INVALID_SOCKET))
        closesocket(h);
    RETURN(0);
}

GUEST_IMPORT(NetDll_bind)
{
    // (caller, s, sockaddr*, namelen)
    HostSocket h = Lookup(ARG32(1));
    if (h == HostSocket(INVALID_SOCKET) || !ARG32(2))
        RETURN(kSocketError);
    sockaddr_in a = FromGuest(ARG32(2));
    if (g_testInstance)
        a.sin_addr.s_addr = g_localIp; // each test copy on its own loopback address
    int r = bind(h, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    NETLOG("[net] bind socket %u to %s: %s", ARG32(1), AddrStr(a).c_str(), r == 0 ? "ok" : "failed");
    RETURN(r == 0 ? 0 : kSocketError);
}

GUEST_IMPORT(NetDll_ioctlsocket)
{
    // (caller, s, cmd, argp*)
    HostSocket h = Lookup(ARG32(1));
    if (h == HostSocket(INVALID_SOCKET))
        RETURN(kSocketError);
    uint32_t cmd = ARG32(2);
    if (cmd == 0x8004667E && ARG32(3)) // FIONBIO
    {
#ifdef _WIN32
        u_long v = Load32(ARG32(3));
        RETURN(ioctlsocket(h, FIONBIO, &v) == 0 ? 0 : kSocketError);
#else
        int fl = fcntl(h, F_GETFL, 0);
        fcntl(h, F_SETFL, Load32(ARG32(3)) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
        RETURN(0);
#endif
    }
    if (cmd == 0x4004667F && ARG32(3)) // FIONREAD
    {
#ifdef _WIN32
        u_long v = 0;
        ioctlsocket(h, FIONREAD, &v);
        Store32(ARG32(3), uint32_t(v));
#endif
        RETURN(0);
    }
    NETLOG("[net] ioctlsocket(%u, %08X) not supported", ARG32(1), cmd);
    RETURN(0);
}

GUEST_IMPORT(NetDll_sendto)
{
    // (caller, s, buf, len, flags, to*, tolen)
    HostSocket h = Lookup(ARG32(1));
    uint32_t len = ARG32(3);
    if (h == HostSocket(INVALID_SOCKET) || !ARG32(5))
        RETURN(kSocketError);
    sockaddr_in to = FromGuest(ARG32(5));
    const char* data = reinterpret_cast<const char*>(GuestToHost(ARG32(2)));
    int r = -1;
    if (IsBroadcast(to.sin_addr.s_addr) && g_testInstance)
    {
        // Same-PC test: "broadcast" to the other copies' loopback addresses.
        for (uint32_t n = 1; n <= 4; n++)
            if (n != g_testInstance)
            {
                sockaddr_in t = to;
                t.sin_addr.s_addr = TestIp(n);
                r = sendto(h, data, int(len), 0, reinterpret_cast<sockaddr*>(&t), sizeof(t));
            }
        r = int(len);
    }
    else
    {
        if (IsBroadcast(to.sin_addr.s_addr))
            to.sin_addr.s_addr = INADDR_BROADCAST;
        r = sendto(h, data, int(len), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    static std::atomic<uint32_t> count{ 0 };
    uint32_t c = ++count;
    if (c <= 20 || (c & (c - 1)) == 0)
        NETLOG("[net] sendto socket %u -> %s, %u bytes (#%u)%s", ARG32(1), AddrStr(to).c_str(), len, c, r < 0 ? " FAILED" : "");
    RETURN(r < 0 ? kSocketError : uint32_t(r));
}

GUEST_IMPORT(NetDll_recvfrom)
{
    // (caller, s, buf, len, flags, from*, fromlen*)
    HostSocket h = Lookup(ARG32(1));
    if (h == HostSocket(INVALID_SOCKET))
        RETURN(kSocketError);
    sockaddr_in from = {};
#ifdef _WIN32
    int fl = sizeof(from);
#else
    socklen_t fl = sizeof(from);
#endif
    int r = recvfrom(h, reinterpret_cast<char*>(GuestToHost(ARG32(2))), int(ARG32(3)), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (r < 0)
        RETURN(kSocketError); // usually "would block" on a non-blocking socket
    if (g_testInstance && from.sin_addr.s_addr == g_localIp)
        RETURN(kSocketError); // our own broadcast
    if (ARG32(5))
        ToGuest(ARG32(5), from);
    if (ARG32(6))
        Store32(ARG32(6), 16);
    static std::atomic<uint32_t> count{ 0 };
    uint32_t c = ++count;
    if (c <= 20 || (c & (c - 1)) == 0)
        NETLOG("[net] recvfrom socket %u <- %s, %d bytes (#%u)", ARG32(1), AddrStr(from).c_str(), r, c);
    RETURN(uint32_t(r));
}

// ===========================================================================
// XNet
// ===========================================================================
GUEST_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    // (caller, xnaddr*) -> flags: 2 ethernet, 4 static address
    StartNet();
    if (ARG32(1))
        FillXnAddr(ARG32(1));
    RETURN(0x00000006);
}

GUEST_IMPORT(NetDll_XNetXnAddrToInAddr)
{
    // (caller, xnaddr*, xnkid*, in_addr*)
    if (!ARG32(1) || !ARG32(3))
        RETURN(0x80070057);
    memcpy(GuestToHost(ARG32(3)), GuestToHost(ARG32(1)), 4);
    RETURN(0);
}

GUEST_IMPORT(NetDll_XNetQosListen)
{
    // (caller, xnkid*, data*, size, bits_per_sec, flags)
    // flags: 1 enable, 2 disable, 4 set data, 8 set bits/sec, 16 release
    StartNet();
    StartQosServer();
    uint64_t kid = ARG32(1) ? Kid(ARG32(1)) : 0;
    uint32_t flags = ARG32(5);
    {
        std::lock_guard lock(g_qosMutex);
        QosListen& l = g_qosListen[kid];
        if (flags & 4)
            l.data.assign(GuestToHost(ARG32(2)), GuestToHost(ARG32(2)) + std::min<uint32_t>(ARG32(3), 1400));
        if (flags & 1)
            l.enabled = true;
        if (flags & 2)
            l.enabled = false;
        if (flags & 16)
            g_qosListen.erase(kid);
    }
    NETLOG("[net] QoS listen key %016llX flags %X data %u bytes", (unsigned long long)kid, flags, ARG32(3));
    RETURN(0);
}

GUEST_IMPORT(NetDll_XNetQosLookup)
{
    // (caller, cxna, apxna*, apxnkid*, apxnkey*, cina, aina*, adwServiceId*,
    //  cProbes, dwBitsPerSec, dwFlags, hEvent, ppxnqos*) - 13 arguments; the
    // ones past r10 are on the stack in 8-byte slots (low word at r1 + 0x54).
    StartNet();
    StartQosServer();
    uint32_t cxna = ARG32(1), apxna = ARG32(2), apxnkid = ARG32(3);
    uint32_t sp = ctx.r1.u32;
    uint32_t hEvent = Load32(sp + 0x54 + 8 * 3), ppxnqos = Load32(sp + 0x54 + 8 * 4);
    uint32_t n = std::min<uint32_t>(cxna, 64);
    uint32_t size = 8 + 24 * std::max<uint32_t>(n, 1);
    uint32_t q = mem::g_virtualHeap.Alloc(size + n * 1400, 16);
    if (!q)
        RETURN(0x8007000E);
    memset(GuestToHost(q), 0, size);
    Store32(q, n);
    Store32(q + 4, n);
    if (ppxnqos)
        Store32(ppxnqos, q);
    struct Target { sockaddr_in addr; uint64_t kid; };
    std::vector<Target> targets;
    for (uint32_t i = 0; i < n; i++)
    {
        Target t = {};
        t.addr.sin_family = AF_INET;
        t.addr.sin_port = htons(kQosPort);
        uint32_t xna = Load32(apxna + i * 4);
        if (xna)
            memcpy(&t.addr.sin_addr, GuestToHost(xna), 4);
        uint32_t kidp = apxnkid ? Load32(apxnkid + i * 4) : 0;
        t.kid = kidp ? Kid(kidp) : 0;
        targets.push_back(t);
        NETLOG("[net] QoS lookup %u/%u: %s key %016llX", i + 1, n, AddrStr(t.addr).c_str(), (unsigned long long)t.kid);
    }
    std::thread([q, n, size, targets, hEvent] {
        HostSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_testInstance)
        {
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = g_localIp;
            bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        }
#ifdef _WIN32
        DWORD timeout = 200;
#else
        timeval timeout = { 0, 200000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t info = q + 8 + i * 24;
            uint8_t probe[16];
            memcpy(probe, "VRQ1", 4);
            memcpy(probe + 4, &targets[i].kid, 8);
            uint32_t nonce = 0x5652u ^ (i * 2654435761u) ^ uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
            memcpy(probe + 12, &nonce, 4);
            bool got = false;
            uint32_t xmit = 0;
            auto t0 = std::chrono::steady_clock::now();
            uint8_t reply[1500];
            for (int attempt = 0; attempt < 3 && !got; attempt++)
            {
                sendto(s, reinterpret_cast<const char*>(probe), sizeof(probe), 0, reinterpret_cast<const sockaddr*>(&targets[i].addr), sizeof(sockaddr_in));
                xmit++;
                int r = recv(s, reinterpret_cast<char*>(reply), sizeof(reply), 0);
                if (r >= 11 && memcmp(reply, "VRQ2", 4) == 0 && memcmp(reply + 4, &nonce, 4) == 0)
                {
                    got = true;
                    auto rtt = uint16_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
                    uint16_t len;
                    memcpy(&len, reply + 9, 2);
                    len = std::min<uint16_t>(len, uint16_t(r - 11));
                    uint32_t dataAddr = q + size + i * 1400;
                    memcpy(GuestToHost(dataAddr), reply + 11, len);
                    Store16(info + 4, 1);       // probes received
                    Store16(info + 6, len);
                    Store32(info + 8, len ? dataAddr : 0);
                    Store16(info + 12, rtt);
                    Store16(info + 14, rtt);
                    Store32(info + 16, 100000000); // 100 Mbit/s up/down: it's a LAN
                    Store32(info + 20, 100000000);
                    // complete | target contacted | (disabled) | data received
                    Store8(info, uint8_t(0x01 | 0x02 | (reply[8] ? 0 : 0x04) | (len ? 0x08 : 0)));
                    NETLOG("[net] QoS reply from %s: %u bytes, %u ms", AddrStr(targets[i].addr).c_str(), len, rtt);
                }
            }
            Store16(info + 2, uint16_t(xmit));
            if (!got)
            {
                Store8(info, 0x01); // complete, no contact
                NETLOG("[net] QoS: no reply from %s", AddrStr(targets[i].addr).c_str());
            }
            Store32(q + 4, n - i - 1);
        }
        closesocket(s);
        SignalGuestEvent(hEvent);
    }).detach();
    RETURN(0);
}

GUEST_IMPORT(NetDll_XNetQosRelease)
{
    // (caller, xnqos*): the lookup thread may still be writing; leak if pending.
    if (ARG32(1) && Load32(ARG32(1) + 4) == 0)
        mem::g_virtualHeap.Free(ARG32(1));
    RETURN(0);
}

// ===========================================================================
// Sessions (XSession*): the game's lobbies.
//
// A session is a kernel object the game gets a handle to (XamSessionCreateHandle)
// and passes, as a pointer, in every XGI session message. Sessions this copy
// hosts are advertised on the QoS port: a search ("VRS1", sent to the LAN's
// broadcast address, or to the other same-PC test copies) is answered with
// each open session's details ("VRS2"), which become the game's search
// results. The game then probes the host with QoS and joins it directly.
// ===========================================================================
namespace
{
    struct KSessionObj : KObject
    {
        KSessionObj() : KObject(Type::Session) {}
    };

    struct Session
    {
        bool host = false;
        bool started = false;
        uint32_t flags = 0;
        uint32_t publicSlots = 0, privateSlots = 0;
        uint32_t filledPublic = 0, filledPrivate = 0;
        uint8_t info[60] = {}; // XSESSION_INFO: id, host XNADDR, key
    };
    std::map<uint32_t, Session> g_sessions; // by guest object address
    std::mutex g_sessionMutex;

    // The local players' contexts and properties (XUserSetContext /
    // XUserSetProperty): advertised with hosted sessions, since the game's
    // search filters (game mode, map...) are made of them.
    std::map<uint32_t, uint32_t> g_contexts;
    std::map<uint32_t, std::vector<uint8_t>> g_properties; // raw guest bytes

    void Put32(std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); }
    uint32_t Get32(const uint8_t*& p, const uint8_t* end)
    {
        uint32_t x = 0;
        if (end - p >= 4) { memcpy(&x, p, 4); p += 4; }
        else p = end;
        return x;
    }

    // One advertised session as sent on the wire (host byte order; both ends
    // are this runtime).
    struct Advert
    {
        uint8_t info[60];
        uint32_t openPublic, openPrivate, filledPublic, filledPrivate;
        std::vector<std::pair<uint32_t, uint32_t>> contexts;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> properties;
    };

    std::vector<uint8_t> BuildAdverts()
    {
        std::vector<uint8_t> out;
        std::lock_guard lock(g_sessionMutex);
        uint32_t count = 0;
        std::vector<uint8_t> body;
        for (auto& [obj, ss] : g_sessions)
        {
            if (!ss.host)
                continue;
            if (ss.started && (ss.flags & 0x400)) // join in progress disabled
                continue;
            uint32_t openPub = ss.publicSlots > ss.filledPublic ? ss.publicSlots - ss.filledPublic : 0;
            uint32_t openPriv = ss.privateSlots > ss.filledPrivate ? ss.privateSlots - ss.filledPrivate : 0;
            if (!openPub && !openPriv)
                continue;
            body.insert(body.end(), ss.info, ss.info + 60);
            Put32(body, openPub);
            Put32(body, openPriv);
            Put32(body, ss.filledPublic);
            Put32(body, ss.filledPrivate);
            Put32(body, uint32_t(g_contexts.size()));
            for (auto& [id, v] : g_contexts) { Put32(body, id); Put32(body, v); }
            Put32(body, uint32_t(g_properties.size()));
            for (auto& [id, d] : g_properties)
            {
                Put32(body, id);
                Put32(body, uint32_t(d.size()));
                body.insert(body.end(), d.begin(), d.end());
            }
            count++;
        }
        out.insert(out.end(), { 'V', 'R', 'S', '2' });
        Put32(out, count);
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    bool ParseAdverts(const uint8_t* p, size_t n, std::vector<Advert>& out)
    {
        const uint8_t* end = p + n;
        if (n < 8 || memcmp(p, "VRS2", 4) != 0)
            return false;
        p += 4;
        uint32_t count = Get32(p, end);
        for (uint32_t i = 0; i < count && p < end; i++)
        {
            Advert a = {};
            if (end - p < 60)
                return false;
            memcpy(a.info, p, 60);
            p += 60;
            a.openPublic = Get32(p, end);
            a.openPrivate = Get32(p, end);
            a.filledPublic = Get32(p, end);
            a.filledPrivate = Get32(p, end);
            uint32_t nc = std::min<uint32_t>(Get32(p, end), 64);
            for (uint32_t c = 0; c < nc; c++)
            {
                uint32_t id = Get32(p, end);
                a.contexts.push_back({ id, Get32(p, end) });
            }
            uint32_t np = std::min<uint32_t>(Get32(p, end), 64);
            for (uint32_t k = 0; k < np && p < end; k++)
            {
                uint32_t id = Get32(p, end), len = Get32(p, end);
                len = std::min<uint32_t>(len, uint32_t(end - p));
                a.properties.push_back({ id, std::vector<uint8_t>(p, p + len) });
                p += len;
            }
            out.push_back(std::move(a));
        }
        return true;
    }

    // Asks the network for open sessions (about a quarter of a second).
    std::vector<Advert> SearchLan()
    {
        StartNet();
        StartQosServer(); // so this copy answers searches too
        std::vector<Advert> found;
        HostSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == HostSocket(INVALID_SOCKET))
            return found;
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        if (g_testInstance)
        {
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = g_localIp;
            bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        }
#ifdef _WIN32
        DWORD timeout = 60;
#else
        timeval timeout = { 0, 60000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        auto send = [&] {
            sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_port = htons(kQosPort);
            if (g_testInstance)
            {
                for (uint32_t n = 1; n <= 4; n++)
                    if (n != g_testInstance)
                    {
                        to.sin_addr.s_addr = TestIp(n);
                        sendto(s, "VRS1", 4, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
                    }
            }
            else
            {
                to.sin_addr.s_addr = INADDR_BROADCAST;
                sendto(s, "VRS1", 4, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            }
        };
        send();
        auto t0 = std::chrono::steady_clock::now();
        int resent = 0;
        uint8_t buf[8192];
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300))
        {
            sockaddr_in from = {};
#ifdef _WIN32
            int fl = sizeof(from);
#else
            socklen_t fl = sizeof(from);
#endif
            int r = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (r <= 0)
            {
                if (!resent++)
                    send(); // once more, in case the first was lost
                continue;
            }
            std::vector<Advert> got;
            if (!ParseAdverts(buf, size_t(r), got))
                continue;
            for (auto& a : got)
            {
                bool dup = false, mine = false;
                for (auto& f : found)
                    dup |= memcmp(f.info, a.info, 8) == 0;
                {
                    std::lock_guard lock(g_sessionMutex);
                    for (auto& [o, ss] : g_sessions)
                        mine |= ss.host && memcmp(ss.info, a.info, 8) == 0;
                }
                if (!dup && !mine)
                    found.push_back(std::move(a));
            }
            NETLOG("[net] search reply from %s: %zu session(s)", AddrStr(from).c_str(), got.size());
        }
        closesocket(s);
        return found;
    }

    // Writes search results: header { count, results* }, then XSESSION_SEARCHRESULT
    // (92 bytes each), then their contexts and properties.
    void WriteResults(uint32_t buf, uint32_t size, const std::vector<Advert>& ads, uint32_t maxResults)
    {
        uint32_t n = std::min<uint32_t>(uint32_t(ads.size()), maxResults);
        while (n && 8 + n * 92 > size)
            n--;
        Store32(buf, n);
        Store32(buf + 4, n ? buf + 8 : 0);
        uint32_t extra = buf + 8 + n * 92, end = buf + size;
        for (uint32_t i = 0; i < n; i++)
        {
            const Advert& a = ads[i];
            uint32_t r = buf + 8 + i * 92;
            memcpy(GuestToHost(r), a.info, 60);
            Store32(r + 60, a.openPublic);
            Store32(r + 64, a.openPrivate);
            Store32(r + 68, a.filledPublic);
            Store32(r + 72, a.filledPrivate);
            // XUSER_PROPERTY: id, (pad), XUSER_DATA { type, (pad), value } = 24 bytes
            uint32_t props = 0, pbase = (extra + 7) & ~7u;
            uint32_t pcount = 0;
            for (auto& [id, d] : a.properties)
            {
                (void)d;
                if (pbase + (pcount + 1) * 24 > end) break;
                pcount++;
            }
            uint32_t data = pbase + pcount * 24;
            for (uint32_t k = 0; k < pcount; k++)
            {
                uint32_t id = a.properties[k].first;
                const auto& d = a.properties[k].second;
                uint32_t pr = pbase + k * 24;
                memset(GuestToHost(pr), 0, 24);
                uint8_t type = uint8_t(id >> 28);
                Store32(pr, id);
                Store8(pr + 8, type);
                if (type == 4 || type == 6) // string / binary: size + pointer
                {
                    uint32_t len = uint32_t(d.size());
                    if (data + len > end)
                        len = 0;
                    if (len)
                        memcpy(GuestToHost(data), d.data(), len);
                    Store32(pr + 16, len);
                    Store32(pr + 20, len ? data : 0);
                    data = (data + len + 3) & ~3u;
                }
                else
                    memcpy(GuestToHost(pr + 16), d.data(), std::min<size_t>(d.size(), 8));
            }
            if (pcount)
                props = pbase;
            uint32_t cbase = (data + 3) & ~3u, ccount = 0;
            for (auto& [id, v] : a.contexts)
            {
                if (cbase + (ccount + 1) * 8 > end) break;
                Store32(cbase + ccount * 8, id);
                Store32(cbase + ccount * 8 + 4, v);
                ccount++;
            }
            Store32(r + 76, pcount);
            Store32(r + 80, ccount);
            Store32(r + 84, props);
            Store32(r + 88, ccount ? cbase : 0);
            extra = cbase + ccount * 8;
        }
    }
}

GUEST_IMPORT(XamSessionCreateHandle)
{
    // (handle*)
    auto obj = std::make_shared<KSessionObj>();
    uint32_t h = CreateHandle(obj);
    uint32_t addr = EnsureGuestObject(obj);
    {
        std::lock_guard lock(g_sessionMutex);
        g_sessions[addr] = Session{};
    }
    if (ARG32(0))
        Store32(ARG32(0), h);
    NETLOG("[xmsg] session handle %08X (object %08X)", h, addr);
    RETURN(0);
}

GUEST_IMPORT(XamSessionRefObjByHandle)
{
    // (handle, object*)
    auto obj = LookupHandleAs<KSessionObj>(ARG32(0));
    if (!obj)
    {
        NETLOG("[xmsg] XamSessionRefObjByHandle: %08X is not a session", ARG32(0));
        RETURN(6); // ERROR_INVALID_HANDLE
    }
    if (ARG32(1))
        Store32(ARG32(1), EnsureGuestObject(obj));
    RETURN(0);
}

// ===========================================================================
// XMsg: messages to system "apps" (0xFA media player, 0xFB XGI = sessions,
// achievements, presence, 0xFC XLiveBase)
// ===========================================================================
namespace
{
    void DumpBuffer(const char* what, uint32_t app, uint32_t msg, uint32_t buf, uint32_t size)
    {
        char hex[200] = {};
        uint32_t n = std::min<uint32_t>(size ? size : 32, 48);
        for (uint32_t i = 0; i < n && buf && i * 3 + 3 < sizeof(hex); i++)
            snprintf(hex + i * 3, 4, "%02X ", Load8(buf + i));
        NETLOG("[xmsg] %s app %02X message %08X buffer %08X (%u bytes): %s", what, app, msg, buf, size, hex);
    }

    bool Plausible(uint32_t p) { return p >= 0x40000000u && p < 0xC0000000u; }

    // XSessionCreate (0x000B0010): { session, flags, public slots, private
    // slots, user, session info*, nonce* }. A host gets a new session id,
    // its own address and a key; a joining player passes the host's info.
    void SessionCreate(uint32_t buf)
    {
        uint32_t obj = Load32(buf), flags = Load32(buf + 4), info = Load32(buf + 20), nonce = Load32(buf + 24);
        NETLOG("[xmsg] XSessionCreate session %08X flags %08X slots %u+%u info %08X nonce %08X", obj, flags, Load32(buf + 8), Load32(buf + 12), info, nonce);
        static std::mt19937_64 rng(std::random_device{}());
        bool host = (flags & 1) != 0;
        if (host && Plausible(info))
        {
            uint64_t kid = rng() & ~0xF0ull; // byte 0 top nibble 0
            memcpy(GuestToHost(info), &kid, 8);
            FillXnAddr(info + 8);
            for (int i = 0; i < 16; i++)
                Store8(info + 44 + i, uint8_t(rng()));
            NETLOG("[xmsg]   hosting session %016llX", (unsigned long long)kid);
        }
        if (Plausible(nonce))
            Store64(nonce, rng());
        std::lock_guard lock(g_sessionMutex);
        Session& ss = g_sessions[obj];
        ss = Session{};
        ss.host = host;
        ss.flags = flags;
        ss.publicSlots = Load32(buf + 8);
        ss.privateSlots = Load32(buf + 12);
        if (Plausible(info))
            memcpy(ss.info, GuestToHost(info), 60);
        if (host)
            StartQosServer();
    }

    // Join (0x12) / leave (0x13): { session, count, xuids* or session,
    // user indices* or session, private slots* }. Only the counts matter.
    void SessionMembers(uint32_t buf, bool join)
    {
        uint32_t obj = Load32(buf), count = Load32(buf + 4), priv = join ? Load32(buf + 16) : 0;
        std::lock_guard lock(g_sessionMutex);
        auto it = g_sessions.find(obj);
        if (it == g_sessions.end())
            return;
        Session& ss = it->second;
        for (uint32_t i = 0; i < std::min<uint32_t>(count, 16); i++)
        {
            bool p = join && ss.privateSlots && Plausible(priv) && Load32(priv + i * 4) != 0;
            uint32_t& slot = p ? ss.filledPrivate : ss.filledPublic;
            if (join)
                slot++;
            else if (ss.filledPublic)
                ss.filledPublic--;
            else if (ss.filledPrivate)
                ss.filledPrivate--;
        }
        NETLOG("[xmsg]   session %08X: %u public + %u private members", obj, ss.filledPublic, ss.filledPrivate);
    }

    // Handles an XGI message; returns its Win32 result.
    uint32_t XgiMessage(uint32_t msg, uint32_t buf, uint32_t size)
    {
        if (!buf)
            return 0;
        switch (msg)
        {
        case 0x000B0006: // XUserSetContext { user, ?, xuid, id, value }
        {
            std::lock_guard lock(g_sessionMutex);
            g_contexts[Load32(buf + 16)] = Load32(buf + 20);
            return 0;
        }
        case 0x000B0007: // XUserSetProperty { user, ?, xuid, id, size, value* }
        {
            uint32_t id = Load32(buf + 16), len = std::min<uint32_t>(Load32(buf + 20), 512), p = Load32(buf + 24);
            std::lock_guard lock(g_sessionMutex);
            if (Plausible(p))
                g_properties[id].assign(GuestToHost(p), GuestToHost(p) + len);
            return 0;
        }
        case 0x000B0010:
            SessionCreate(buf);
            return 0;
        case 0x000B0011: // XSessionDelete { session }
        {
            std::lock_guard lock(g_sessionMutex);
            g_sessions.erase(Load32(buf));
            NETLOG("[xmsg]   session %08X deleted", Load32(buf));
            return 0;
        }
        case 0x000B0012:
            SessionMembers(buf, true);
            return 0;
        case 0x000B0013:
            SessionMembers(buf, false);
            return 0;
        case 0x000B0014: // XSessionStart
        case 0x000B0015: // XSessionEnd
        {
            std::lock_guard lock(g_sessionMutex);
            auto it = g_sessions.find(Load32(buf));
            if (it != g_sessions.end())
                it->second.started = msg == 0x000B0014;
            return 0;
        }
        case 0x000B001C: // XSessionSearch { procedure, user, max results, u16 props, u16 contexts, props*, contexts*, size, results*, users }
        {
            uint32_t maxResults = Load32(buf + 8), rsize = Load32(buf + 24), results = Load32(buf + 28);
            auto ads = SearchLan();
            NETLOG("[xmsg] XSessionSearch (procedure %u): %zu session(s) found", Load32(buf), ads.size());
            if (Plausible(results))
                WriteResults(results, rsize, ads, maxResults);
            return 0;
        }
        case 0x000B001B: // XSessionSearchByID { user, id high, id low, size, results* }
        {
            uint32_t rsize = Load32(buf + 12), results = Load32(buf + 16);
            uint8_t id[8];
            memcpy(id, GuestToHost(buf + 4), 8);
            auto ads = SearchLan();
            std::vector<Advert> match;
            for (auto& a : ads)
                if (memcmp(a.info, id, 8) == 0)
                    match.push_back(a);
            NETLOG("[xmsg] XSessionSearchByID: %s", match.empty() ? "not found" : "found");
            if (Plausible(results))
                WriteResults(results, rsize, match, 1);
            return match.empty() ? 1168 : 0; // ERROR_NOT_FOUND
        }
        default:
            return 0;
        }
    }
}

GUEST_IMPORT(XMsgStartIORequest)
{
    // (app, message, overlapped*, buffer*, buffer_size)
    uint32_t app = ARG32(0), msg = ARG32(1), ov = ARG32(2), buf = ARG32(3), size = ARG32(4);
    DumpBuffer("StartIORequest", app, msg, buf, size);
    uint32_t result = app == 0xFB ? XgiMessage(msg, buf, size) : 0;
    if (ov)
    {
        Store32(ov + 0x00, result);
        Store32(ov + 0x04, 0);
        Store32(ov + 0x18, result);
        SignalGuestEvent(Load32(ov + 0x0C));
        RETURN(0x3E5); // ERROR_IO_PENDING
    }
    RETURN(result);
}

GUEST_IMPORT(XMsgInProcessCall)
{
    // (app, message, param1, param2)
    uint32_t app = ARG32(0), msg = ARG32(1), p1 = ARG32(2);
    if (app == 0xFA && msg == 0x00070009)
    {
        // Media player status: { client, state* } -> 0 = not playing music.
        if (Plausible(p1) && Plausible(Load32(p1 + 4)))
            Store32(Load32(p1 + 4), 0);
        RETURN(0);
    }
    DumpBuffer("InProcessCall", app, msg, p1, 0);
    if (app == 0xFC) // XLiveBase (replies as Xenia gives them)
    {
        const uint32_t kFail = 0x80004005;
        switch (msg)
        {
        case 0x00058004: // logon id / Live base ready: a 32-bit value, 1
        case 0x00058006: // NAT type: 1 = open
            if (Plausible(p1))
                Store32(p1, 1);
            RETURN(0);
        case 0x00058046:
            RETURN(0);
        case 0x00058023: // accepted game invite info: there is no invite
            NETLOG("[xmsg]   no accepted game invite");
            RETURN(kFail);
        default:         // friends list (0x58020) and others: nothing
            NETLOG("[xmsg]   XLiveBase message not supported, reporting failure");
            RETURN(kFail);
        }
    }
    RETURN(0);
}

GUEST_IMPORT(XMsgCancelIORequest) { RETURN(0); }
