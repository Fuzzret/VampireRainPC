#include "kernel.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

bool g_trace = false;
bool g_verbose = false;

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall MessageBoxA(void* hwnd, const char* text, const char* caption, unsigned type);
#endif

namespace
{
    std::mutex g_logMutex;
    FILE* g_logFile = nullptr;
    const auto g_start = std::chrono::steady_clock::now();
    uint64_t g_tscFrequency = 0;
}

void OpenLogFile(const char* path)
{
    // Keep the previous run's log (e.g. after a crash) as <name>.prev.log.
    std::string prev = path;
    size_t dot = prev.rfind('.');
    prev = (dot == std::string::npos ? prev : prev.substr(0, dot)) + ".prev.log";
    remove(prev.c_str());
    rename(path, prev.c_str());
    g_logFile = fopen(path, "w");
}

namespace
{
    // Routine messages only written with --verbose (by their prefix).
    bool IsRoutine(const char* msg)
    {
        static const char* const kRoutine[] = {
            "[gpu] frame ", "[gpu] VdSwap", "[gpu] last 300", "[gpu] ring buffer", "[gpu] read pointer", "[gpu] interrupt",
            "[d3d12] per frame", "[d3d12]   ", "[d3d12] compiled", "[d3d12] compiling", "[profile] CPU ms",
            "[xmsg]", "[net] socket ", "[net] sendto", "[net] recvfrom", "[net] bind", "[net] QoS", "[net] WSAStartup",
            "[net] session search", "[net] search reply",
            "[xam] notification", "[xam] content enumerator", "[xam] XamContentCreateEx", "[xam] sign-in UI",
            "[vfs] mounted", "[vfs] unknown device", "[thread] ", "[stub] ", "[file] symlink",
            "[xma] decoder ready", "[audio] render driver", "[loader] ", "TSC frequency", "main thread",
        };
        if (!strncmp(msg, "[loader] ", 9) && strstr(msg, ": image ") == nullptr)
            return false; // loader problems are always shown
        for (const char* p : kRoutine)
            if (!strncmp(msg, p, strlen(p)))
                return true;
        return false;
    }
}

void Log(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!g_verbose && IsRoutine(buf))
        return;

    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard lock(g_logMutex);
    fprintf(stdout, "[%9.3f] %s\n", t, buf);
    if (g_logFile)
    {
        fprintf(g_logFile, "[%9.3f] %s\n", t, buf);
        fflush(g_logFile);
    }
}

void FatalExit(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("FATAL: %s", buf);
    fflush(stdout);
#ifdef _WIN32
    // No console window: say what went wrong.
    char text[2300];
    snprintf(text, sizeof(text), "%s\n\n(Details are in runtime.log.)", buf);
    MessageBoxA(nullptr, text, "Vampire Rain", 0x10 /* MB_ICONERROR */);
#endif
    std::_Exit(1);
}

uint64_t HostFileTimeNow()
{
    // 100ns intervals between 1601-01-01 and 1970-01-01
    constexpr uint64_t EPOCH_DIFF = 116444736000000000ull;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return EPOCH_DIFF + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
}

uint64_t InterruptTime100ns()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count() / 100);
}

void CalibrateTsc()
{
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint64_t c1 = __rdtsc();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    g_tscFrequency = uint64_t(double(c1 - c0) / secs);
}

uint64_t TscFrequency() { return g_tscFrequency; }

std::string GuestWideToAscii(uint32_t addr, size_t maxChars)
{
    std::string out;
    if (!addr)
        return out;
    for (size_t i = 0; i < maxChars; i++)
    {
        uint16_t c = Load16(addr + uint32_t(i * 2));
        if (!c)
            break;
        out.push_back(c < 0x80 ? char(c) : '?');
    }
    return out;
}

std::string ReadAnsiString(uint32_t strPtr)
{
    if (!strPtr)
        return {};
    uint16_t len = Load16(strPtr);
    uint32_t buf = Load32(strPtr + 4);
    if (!buf)
        return {};
    return std::string(reinterpret_cast<const char*>(GuestToHost(buf)), len);
}
