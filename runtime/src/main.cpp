// Vampire Rain recompiled runtime - entry point.
//
// Usage: VampireRain [--game <dir>] [--xex <file>] [--data <dir>] [--verbose] [--console] [--trace]
//   --game   extracted disc folder (default: "Vampire Rain (USA, Europe)")
//   --xex    decrypted executable  (default: "work/default_dec.xex")
//   --data   writable folder for saves/cache (default: "userdata")
//   --verbose  detailed log (per-frame statistics, network, system messages)
//   --console  show the log in a console window (the exe has none by default)
//   --trace  log every call to an implemented kernel function (implies --verbose)
//   --software-gpu  render on the CPU instead of with Direct3D 12 (slow)
//   --launcher / --no-launcher  show / skip the settings window at startup

#include "kernel.h"
#include "memory.h"
#include "objects.h"
#include "thread.h"
#include "xma.h"
#include "settings.h"
#include "vfs.h"
#include "xex_loader.h"
#include "gpu.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <csignal>
#include <ucontext.h>
#endif

void OpenLogFile(const char* path);
void CalibrateTsc();
void StartTimeStampBundleThread();
void InitVideo(const std::string& dataDir);

// Reports where a crash happened. A fault inside the 4 GB guest range is shown
// as a guest address, which is usually the useful clue (an unmapped region the
// game expected to exist, or a kernel structure field it read).
static std::string GuestFunctionAt(uintptr_t hostPc);
static void ReportCrash(const char* what, uintptr_t faultAddr, uintptr_t pc)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
    if (faultAddr >= base && faultAddr < base + PPC_MEMORY_SIZE)
        LOG("CRASH: %s at guest address %08llX (host pc %p, guest thread %u, in %s)", what,
            (unsigned long long)(faultAddr - base), reinterpret_cast<void*>(pc), CurrentThread()->id,
            pc ? GuestFunctionAt(pc).c_str() : "?");
    else
        LOG("CRASH: %s at host address %p (host pc %p)", what, reinterpret_cast<void*>(faultAddr), reinterpret_cast<void*>(pc));
    fflush(stdout);
}

// The game's own assertions are PowerPC trap instructions, which the
// recompiler turns into a breakpoint. Report which game function hit one
// (and from where it was called), then carry on as a retail console without
// a debugger attached would for a non-fatal check.
static std::string GuestFunctionAt(uintptr_t hostPc)
{
    static std::vector<std::pair<uintptr_t, uint32_t>> table;
    static std::once_flag once;
    std::call_once(once, [] {
        for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++)
            table.emplace_back(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest));
        std::sort(table.begin(), table.end());
    });
    auto it = std::upper_bound(table.begin(), table.end(), std::make_pair(hostPc, UINT32_MAX));
    if (it == table.begin())
        return "unknown";
    --it;
    char buf[32];
    snprintf(buf, sizeof(buf), "sub_%08X", it->second);
    return buf;
}

// For other files (camera trace): name of the game function at a host address.
std::string GuestFunctionNameAt(uintptr_t hostPc) { return GuestFunctionAt(hostPc); }

static void ReportTrap(uintptr_t hostPc)
{
    static std::atomic<int> count{ 0 };
    int n = ++count;
    if (n > 20 && (n & (n - 1)) != 0)
        return;
    GuestThread* t = CurrentThread();
    LOG("[trap] game assertion #%d in %s (thread %u '%s', called from %08X, r3=%08X r4=%08X r5=%08X) - continuing",
        n, GuestFunctionAt(hostPc).c_str(), t ? t->id : 0, t ? t->name.c_str() : "?",
        t && t->ctx ? uint32_t(t->ctx->lr) : 0, t && t->ctx ? t->ctx->r3.u32 : 0,
        t && t->ctx ? t->ctx->r4.u32 : 0, t && t->ctx ? t->ctx->r5.u32 : 0);
}

#ifdef _WIN32
static LONG WINAPI TrapHandler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    ReportTrap(uintptr_t(info->ExceptionRecord->ExceptionAddress));
    info->ContextRecord->Rip += 1; // skip the int3
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    auto* rec = info->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        ReportCrash(rec->ExceptionInformation[0] ? "write" : "read", rec->ExceptionInformation[1], (uintptr_t)rec->ExceptionAddress);
    else
        LOG("CRASH: exception %08lX at %p", rec->ExceptionCode, rec->ExceptionAddress);
    fflush(stdout);
    MessageBoxA(nullptr, "Vampire Rain has crashed.\n\nDetails are in runtime.log in the game folder.", "Vampire Rain", MB_ICONERROR);
    std::_Exit(3);
}
#else
static void TrapSignal(int, siginfo_t*, void* uctx)
{
    auto* uc = static_cast<ucontext_t*>(uctx);
    ReportTrap(uintptr_t(uc->uc_mcontext.gregs[REG_RIP]) - 1); // RIP is already past the int3
}

static void CrashHandler(int sig, siginfo_t* info, void* uctx)
{
    ReportCrash(sig == SIGSEGV ? "segfault" : "bus error", reinterpret_cast<uintptr_t>(info->si_addr), 0);
    std::_Exit(3);
}
#endif

static void InstallCrashHandler()
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, TrapHandler);
    SetUnhandledExceptionFilter(CrashHandler);
#else
    struct sigaction ta {};
    ta.sa_sigaction = TrapSignal;
    ta.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &ta, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = CrashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
#endif
}

int main(int argc, char** argv)
{
    std::string gameDir = "Vampire Rain (USA, Europe)";
    std::string xexPath = "work/default_dec.xex";
    std::string dataDir = "userdata";
    bool forceLauncher = false, skipLauncher = false;

    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) gameDir = argv[++i];
        else if (a == "--xex" && i + 1 < argc) xexPath = argv[++i];
        else if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--trace") g_trace = g_verbose = true;
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--console")
        {
#ifdef _WIN32
            // Built as a window program: open a console for the log.
            if (AllocConsole())
            {
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);
            }
#endif
        }
        else if (a == "--software-gpu") gpu::g_softwareGpu = true;
        else if (a == "--launcher") forceLauncher = true;
        else if (a == "--no-launcher") skipLauncher = true;
        else
        {
            const char* usage = "Options:\n"
                                "  --launcher / --no-launcher   show / skip the settings window\n"
                                "  --data <dir>     saves and settings folder (default: userdata)\n"
                                "  --game <dir>     extracted game files\n"
                                "  --xex <file>     decrypted executable (default: work\\default_dec.xex)\n"
                                "  --verbose        detailed log\n"
                                "  --console        show the log in a console window\n"
                                "  --trace          log every system call (very large log)\n"
                                "  --software-gpu   render on the CPU (slow)";
#ifdef _WIN32
            MessageBoxA(nullptr, (std::string("Unknown option: ") + a + "\n\n" + usage).c_str(), "Vampire Rain", MB_ICONINFORMATION);
#else
            fprintf(stderr, "unknown option %s\n%s\n", a.c_str(), usage);
#endif
            return 1;
        }
    }

    // Relative paths are tried against the current folder first, then the
    // folders above the executable (runtime\build\VampireRain.exe sits two
    // levels below the project root).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path exeDir = fs::absolute(fs::path(argv[0]), ec).parent_path();
        std::vector<fs::path> roots = { fs::current_path(ec), exeDir, exeDir.parent_path(), exeDir.parent_path().parent_path() };
        for (auto& root : roots)
        {
            if (fs::is_directory(root / gameDir, ec) && fs::is_regular_file(root / xexPath, ec))
            {
                if (fs::path(gameDir).is_relative()) gameDir = (root / gameDir).string();
                if (fs::path(xexPath).is_relative()) xexPath = (root / xexPath).string();
                if (fs::path(dataDir).is_relative()) dataDir = (root / dataDir).string();
                fs::current_path(root, ec); // runtime.log goes next to the game files
                break;
            }
        }
    }

    // A second copy (e.g. for LAN testing, --data userdata2) logs separately.
    {
        std::string folder = std::filesystem::path(dataDir).filename().string();
        OpenLogFile(folder.empty() || folder == "userdata" ? "runtime.log" : ("runtime-" + folder + ".log").c_str());
    }
    LOG("Vampire Rain runtime starting");
    InstallCrashHandler();

    settings::Load(dataDir);
    if (!skipLauncher && (forceLauncher || settings::Get().showLauncher) && !settings::RunLauncher())
    {
        LOG("launcher closed - exiting");
        return 0;
    }

    if (!std::filesystem::is_directory(gameDir))
        FatalExit("game folder not found: %s (run build.ps1 to set up the game files, or use --game)", gameDir.c_str());

    CalibrateTsc();
    LOG("TSC frequency: %.3f MHz", TscFrequency() / 1e6);

    if (!mem::Init())
        FatalExit("could not reserve 4 GB of address space for guest memory");
    // The game writes GPU registers directly; give those writes somewhere to land.
    mem::Commit(mem::GPU_MMIO_BASE, mem::GPU_MMIO_SIZE);
    // Same for the XMA audio decoder's registers.
    mem::Commit(mem::XMA_MMIO_BASE, mem::XMA_MMIO_SIZE);

    if (!LoadXex(xexPath))
        FatalExit("failed to load %s (use --xex)", xexPath.c_str());
    xma::Init();

    vfs::Init(gameDir, dataDir);
    StartTimeStampBundleThread();
#ifdef _WIN32
    // 1 ms timer resolution: the game's short sleeps and timed waits are
    // otherwise rounded up to ~15.6 ms, which held it at 15-20 fps.
    timeBeginPeriod(1);
#endif
    // Sampling profiler (VR_PROFILE=1): logs the main thread's hottest code.
    if (getenv("VR_PROFILE"))
    ProfileStartSampler([](uintptr_t pc) -> std::string {
#ifdef _WIN32
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(pc), &mod) && mod != GetModuleHandleW(nullptr))
        {
            char name[MAX_PATH] = "?";
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            return std::string("[") + (base ? base + 1 : name) + "]";
        }
#endif
        return GuestFunctionAt(pc);
    });
    InitVideo(dataDir);

    GuestThread* mainThread = CreateGuestThread(g_module.entryPoint, 0, 0,
                                                std::max<uint32_t>(g_module.stackSize, 1u << 20), false, "main");
    LOG("main thread %u started at %08X", mainThread->id, g_module.entryPoint);

    {
        std::unique_lock lock(g_sync);
        g_syncCv.wait(lock, [&] { return mainThread->object->exited; });
    }
    LOG("main thread exited");
    fflush(stdout);
    std::_Exit(0);
}
