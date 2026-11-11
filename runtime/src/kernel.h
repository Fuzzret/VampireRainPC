// Core helpers shared by the whole runtime: guest memory access, argument
// access for imported functions, status codes and logging.
#pragma once

#include <ppc_recomp_shared.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// Base of the 4 GB guest address space (set by mem::Init).
extern uint8_t* g_base;

// ---------------------------------------------------------------------------
// Guest memory access (guest memory is big-endian)
// ---------------------------------------------------------------------------
inline uint8_t* GuestToHost(uint32_t addr) { return g_base + addr; }
inline uint32_t HostToGuest(const void* p) { return uint32_t((const uint8_t*)p - g_base); }

inline uint8_t Load8(uint32_t a) { return g_base[a]; }
inline uint16_t Load16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t Load32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t Load64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void Store8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void Store16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void Store32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void Store64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }
inline void StoreF32(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); Store32(a, v); }

// Atomic access to a big-endian 32-bit guest word (must be 4-byte aligned).
// The value seen through it is byte-swapped; compare against __builtin_bswap32(x).
inline std::atomic_ref<uint32_t> GuestAtomic32(uint32_t a)
{
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(g_base + a));
}

inline const char* GuestStr(uint32_t a) { return a ? reinterpret_cast<const char*>(g_base + a) : nullptr; }

// Reads a null-terminated big-endian UTF-16 string (ASCII-folded for logging/paths).
std::string GuestWideToAscii(uint32_t addr, size_t maxChars = 0x10000);

// ANSI_STRING / UNICODE_STRING: be16 Length, be16 MaximumLength, be32 Buffer
std::string ReadAnsiString(uint32_t strPtr);

// ---------------------------------------------------------------------------
// Arguments for imported functions. r3-r10 hold the first 8; the rest are in
// the caller's parameter area (64-bit slots starting at r1 + 0x50).
// ---------------------------------------------------------------------------
inline uint64_t GetArg(PPCContext& ctx, int i)
{
    switch (i)
    {
    case 0: return ctx.r3.u64;
    case 1: return ctx.r4.u64;
    case 2: return ctx.r5.u64;
    case 3: return ctx.r6.u64;
    case 4: return ctx.r7.u64;
    case 5: return ctx.r8.u64;
    case 6: return ctx.r9.u64;
    case 7: return ctx.r10.u64;
    default: return Load64(ctx.r1.u32 + 0x50 + (i - 8) * 8);
    }
}

#define ARG32(i) uint32_t(GetArg(ctx, (i)))
#define ARG64(i) GetArg(ctx, (i))
#define RETURN(v) do { ctx.r3.u64 = uint64_t(v); return; } while (0)

// Defines the host implementation of an imported kernel/XAM function.
// The generated code declares these as __imp__<Name>.
#define GUEST_IMPORT(name) PPC_FUNC(__imp__##name)

// ---------------------------------------------------------------------------
// Status codes
// ---------------------------------------------------------------------------
constexpr uint32_t STATUS_SUCCESS = 0x00000000;
constexpr uint32_t STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t STATUS_PENDING = 0x00000103;
constexpr uint32_t STATUS_BUFFER_OVERFLOW = 0x80000005;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_DLL_NOT_FOUND = 0xC0000135;
constexpr uint32_t STATUS_ENTRYPOINT_NOT_FOUND = 0xC0000139;

constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t ERROR_NO_SUCH_USER = 1317;
constexpr uint32_t ERROR_EMPTY = 4306;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
extern bool g_trace;   // --trace: log every implemented kernel call
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) ::Log(__VA_ARGS__)
// --verbose: also log routine detail (per-frame statistics, network packets,
// system messages, threads...). Without it the log keeps startup, settings,
// warnings and errors.
extern bool g_verbose;
#define TRACE(...) do { if (g_trace) ::Log(__VA_ARGS__); } while (0)

// Time helpers (100ns units)
uint64_t HostFileTimeNow();          // FILETIME (since 1601)
uint64_t InterruptTime100ns();       // since runtime start
uint64_t TscFrequency();             // measured at startup; mftb is recompiled as __rdtsc()

[[noreturn]] void FatalExit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
