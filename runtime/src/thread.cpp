#include "thread.h"
#include "kernel.h"
#include "memory.h"
#include "objects.h"

#include <mutex>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <vector>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <pthread.h>
#endif

namespace
{
    thread_local GuestThread* t_current = nullptr;
    std::atomic<uint32_t> g_nextThreadId{1};
    std::mutex g_threadsMutex;
    std::unordered_map<uint32_t, GuestThread*> g_threadsByKThread;

    constexpr uint32_t PCR_SIZE = 0x1000;
    constexpr uint32_t KTHREAD_SIZE = 0xB00;
    constexpr uint32_t TLS_BLOCK_SIZE = 0x400;
    constexpr uint32_t MIN_STACK = 256 * 1024;

    // Process info block shared by all threads (KTHREAD+0x84 points to it).
    uint32_t ProcessInfoBlock()
    {
        static uint32_t pib = [] {
            uint32_t p = mem::g_systemHeap.Alloc(0x60, 16);
            Store32(p + 0x04, p + 0x04);    // list head (flink/blink)
            Store32(p + 0x08, p + 0x04);
            Store32(p + 0x0C, 0x7F);
            Store32(p + 0x10, 0x001F0000);
            Store8(p + 0x1B, 6);
            Store32(p + 0x1C, 16 * 1024);   // kernel stack size
            Store16(p + 0x2C, 64 * 4);      // TLS slot area size
            Store8(p + 0x2F, 1);            // process type: title
            Store32(p + 0x54, p + 0x54);
            Store32(p + 0x58, p + 0x54);
            return p;
        }();
        return pib;
    }

    GuestThread* AllocateThread(uint32_t stackSize, const char* name)
    {
        auto* t = new GuestThread();
        t->id = g_nextThreadId++;
        t->name = name ? name : "";
        t->stackSize = std::max(MIN_STACK, (stackSize + 0xFFFF) & ~0xFFFFu);
        t->stackBase = mem::g_systemHeap.Alloc(t->stackSize, 0x10000);
        t->pcr = mem::g_systemHeap.Alloc(PCR_SIZE, 0x1000);
        t->kthread = mem::g_systemHeap.Alloc(KTHREAD_SIZE, 0x10);
        t->tlsBlock = mem::g_systemHeap.Alloc(TLS_BLOCK_SIZE, 0x10);
        if (!t->stackBase || !t->pcr || !t->kthread || !t->tlsBlock)
            FatalExit("out of system memory creating thread '%s'", t->name.c_str());

        // KPCR
        Store32(t->pcr + 0x000, t->tlsBlock);                   // tls_ptr
        Store32(t->pcr + 0x030, t->pcr);                        // pcr_ptr
        Store32(t->pcr + 0x070, t->stackBase + t->stackSize);   // stack_base_ptr
        Store32(t->pcr + 0x074, t->stackBase);                  // stack_end_ptr
        Store32(t->pcr + 0x100, t->kthread);                    // prcb.current_thread
        Store8(t->pcr + 0x10C, uint8_t(t->id % 6));             // prcb.current_cpu

        // KTHREAD (layout as set up by the Xenia emulator)
        uint32_t k = t->kthread;
        Store8(k + 0x000, 6);                                // dispatcher header: thread
        Store8(k + 0x002, uint8_t(KTHREAD_SIZE / 4));
        Store32(k + 0x008, k + 0x008);                       // wait list head
        Store32(k + 0x00C, k + 0x008);
        Store32(k + 0x010, k + 0x010);
        Store32(k + 0x014, k + 0x010);
        Store32(k + 0x05C, t->stackBase + t->stackSize);     // stack base
        Store32(k + 0x060, t->stackBase);                    // stack limit
        Store32(k + 0x068, t->tlsBlock);                     // tls address
        Store32(k + 0x074, k + 0x074);
        Store32(k + 0x078, k + 0x074);
        Store32(k + 0x07C, k + 0x07C);
        Store32(k + 0x080, k + 0x07C);
        Store32(k + 0x084, ProcessInfoBlock());
        Store8(k + 0x08B, 1);
        Store32(k + 0x09C, 0xFDFFD7FF);
        Store32(k + 0x0D0, t->stackBase + t->stackSize);
        Store64(k + 0x130, HostFileTimeNow());               // create time
        Store32(k + 0x144, k + 0x144);
        Store32(k + 0x148, k + 0x144);
        Store32(k + 0x14C, t->id);                           // thread id
        Store32(k + 0x154, k + 0x154);
        Store32(k + 0x158, k + 0x154);
        Store32(k + 0x160, 0);                               // last error
        Store32(k + 0x17C, 1);

        t->object = std::make_shared<KThreadObj>();
        t->object->thread = t;
        RegisterGuestObject(t->kthread, t->object);
        t->handle = CreateHandle(t->object);

        t->ctx = std::make_unique<PPCContext>();
        {
            std::lock_guard lock(g_threadsMutex);
            g_threadsByKThread[t->kthread] = t;
        }
        return t;
    }

    void ResetContext(GuestThread* t)
    {
        PPCContext& ctx = *t->ctx;
        ctx = PPCContext{};
        ctx.r1.u64 = t->stackBase + t->stackSize - 0x100;  // leave room for the back chain
        ctx.r13.u64 = t->pcr;
        ctx.fpscr.csr = ctx.fpscr.getcsr();
        ctx.fpscr.disableFlushModeUnconditional();
#ifndef PPC_CONFIG_SKIP_MSR
        ctx.msr = 0x9030;
#endif
    }

    void RunEntry(GuestThread* t, uint32_t entry)
    {
        (PPC_LOOKUP_FUNC(g_base, entry))(*t->ctx, g_base);
    }
}

void StartHostThread(std::function<void()> fn, size_t stackSize)
{
#ifdef _WIN32
    auto* heapFn = new std::function<void()>(std::move(fn));
    HANDLE h = CreateThread(nullptr, stackSize, [](LPVOID p) -> DWORD {
        std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
        (*f)();
        return 0;
    }, heapFn, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h)
        FatalExit("CreateThread failed");
    ::CloseHandle(h);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSize);
    auto* heapFn = new std::function<void()>(std::move(fn));
    pthread_t tid;
    if (pthread_create(&tid, &attr, [](void* p) -> void* {
            std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
            (*f)();
            return nullptr;
        }, heapFn) != 0)
        FatalExit("pthread_create failed");
    pthread_detach(tid);
    pthread_attr_destroy(&attr);
#endif
}

namespace
{
    struct ProfiledThread
    {
        std::string name;
#ifdef _WIN32
        HANDLE handle = nullptr;
#endif
        uint64_t last = 0; // CPU time at the previous report, 100 ns units
        GuestThread* guest = nullptr;
        // Sampled host instruction addresses / guest link registers since the last report.
        std::unordered_map<uintptr_t, uint32_t> pcs;
        std::unordered_map<uint32_t, uint32_t> callers;
        uint32_t samples = 0;
    };
    std::string (*g_symbolize)(uintptr_t) = nullptr;
    std::mutex g_profMutex;
    std::vector<ProfiledThread> g_profiled;
}

void ProfileRegisterCurrentThread(const std::string& name)
{
#ifdef _WIN32
    ProfiledThread p;
    p.name = name;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &p.handle,
                    THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, 0);
    p.guest = name.rfind("guest ", 0) == 0 ? t_current : nullptr;
    std::lock_guard lock(g_profMutex);
    g_profiled.push_back(std::move(p));
#else
    (void)name;
#endif
}

void ProfileReport(uint32_t frames)
{
#ifdef _WIN32
    std::lock_guard lock(g_profMutex);
    std::string line;
    for (auto& p : g_profiled)
    {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(p.handle, &c, &e, &k, &u))
            continue;
        uint64_t total = ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
        double ms = double(total - p.last) / 10000.0 / frames;
        p.last = total;
        if (ms < 0.5)
            continue;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s%s %.1f", line.empty() ? "" : ", ", p.name.c_str(), ms);
        line += buf;
    }
    LOG("[profile] CPU ms per frame by thread: %s", line.c_str());
    // Hottest code of the most-sampled thread.
    ProfiledThread* top = nullptr;
    for (auto& p : g_profiled)
        if (p.samples && (!top || p.samples > top->samples))
            top = &p;
    if (top && g_symbolize)
    {
        std::unordered_map<std::string, uint32_t> byFunc;
        for (auto& [pc, n] : top->pcs)
            byFunc[g_symbolize(pc)] += n;
        std::vector<std::pair<uint32_t, std::string>> sorted;
        for (auto& [f, n] : byFunc)
            sorted.emplace_back(n, f);
        std::sort(sorted.rbegin(), sorted.rend());
        std::string hot;
        for (size_t i = 0; i < sorted.size() && i < 15; i++)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s%s %.1f%%", i ? ", " : "", sorted[i].second.c_str(), 100.0 * sorted[i].first / top->samples);
            hot += buf;
        }
        std::vector<std::pair<uint32_t, uint32_t>> calls;
        for (auto& [lr, n] : top->callers)
            calls.emplace_back(n, lr);
        std::sort(calls.rbegin(), calls.rend());
        std::string cl;
        for (size_t i = 0; i < calls.size() && i < 15; i++)
        {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s%08X %.1f%%", i ? ", " : "", calls[i].second, 100.0 * calls[i].first / top->samples);
            cl += buf;
        }
        LOG("[profile] %s hottest code (%u samples): %s", top->name.c_str(), top->samples, hot.c_str());
        LOG("[profile] %s most common guest return addresses: %s", top->name.c_str(), cl.c_str());
    }
    for (auto& p : g_profiled)
    {
        p.pcs.clear();
        p.callers.clear();
        p.samples = 0;
    }
#else
    (void)frames;
#endif
}

void ForEachGuestThreadHandle(const std::function<void(void*)>& fn)
{
#ifdef _WIN32
    std::lock_guard lock(g_profMutex);
    for (auto& p : g_profiled)
        if (p.guest && p.handle)
            fn(p.handle);
#else
    (void)fn;
#endif
}

void ProfileStartSampler(std::string (*symbolize)(uintptr_t))
{
#ifdef _WIN32
    g_symbolize = symbolize;
    StartHostThread([] {
        timeBeginPeriod(1);
        for (;;)
        {
            Sleep(1);
            std::lock_guard lock(g_profMutex);
            for (auto& p : g_profiled)
            {
                if (!p.guest || SuspendThread(p.handle) == DWORD(-1))
                    continue;
                // Nothing that could take a lock (allocation included) while
                // the thread is suspended - it might be holding it.
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_CONTROL;
                bool ok = GetThreadContext(p.handle, &c) != 0;
                uint32_t lr = (ok && p.guest->ctx) ? uint32_t(p.guest->ctx->lr) : 0;
                ResumeThread(p.handle);
                if (ok)
                {
                    p.pcs[uintptr_t(c.Rip)]++;
                    p.callers[lr]++;
                    p.samples++;
                }
            }
        }
    }, 1 << 20);
#else
    (void)symbolize;
#endif
}

GuestThread* CurrentThread()
{
    if (!t_current)
    {
        t_current = AllocateThread(MIN_STACK, "host");
        ResetContext(t_current);
        ProfileRegisterCurrentThread("host thread " + std::to_string(t_current->id));
    }
    return t_current;
}

GuestThread* FindThreadByKThread(uint32_t kthread)
{
    std::lock_guard lock(g_threadsMutex);
    auto it = g_threadsByKThread.find(kthread);
    return it == g_threadsByKThread.end() ? nullptr : it->second;
}

GuestThread* CreateGuestThread(uint32_t entry, uint32_t arg0, uint32_t arg1,
                               uint32_t stackSize, bool suspended, const char* name)
{
    GuestThread* t = AllocateThread(stackSize, name);
    t->suspendCount = suspended ? 1 : 0;

    StartHostThread([t, entry, arg0, arg1] {
        t_current = t;
        ProfileRegisterCurrentThread("guest " + std::to_string(t->id) + (t->name.empty() ? std::string() : " (" + t->name + ")"));
        {
            std::unique_lock lock(g_sync);
            g_syncCv.wait(lock, [t] { return t->suspendCount.load() <= 0; });
        }

        ResetContext(t);
        t->ctx->r3.u64 = arg0;
        t->ctx->r4.u64 = arg1;

        TRACE("[thread] %u start entry=%08X arg=%08X", t->id, entry, arg0);
        uint32_t exitCode = 0;
        try
        {
            RunEntry(t, entry);
            exitCode = t->ctx->r3.u32;
        }
        catch (const GuestThreadExit& e)
        {
            exitCode = e.code;
        }
        TRACE("[thread] %u exit code=%08X", t->id, exitCode);

        std::lock_guard lock(g_sync);
        t->object->exited = true;
        Store32(t->kthread + 4, 1);
        g_syncCv.notify_all();
    });
    return t;
}

int ResumeGuestThread(GuestThread* t)
{
    std::lock_guard lock(g_sync);
    int prev = t->suspendCount.load();
    if (prev > 0)
        t->suspendCount = prev - 1;
    g_syncCv.notify_all();
    return prev;
}

void ReportBadIndirectCall(uint32_t target, uint32_t returnAddress)
{
    FatalExit("indirect call to %08X (called from %08X, thread %u) has no recompiled function. "
              "If %08X is code, add it to `functions` in vampirerain.toml and re-run XenonRecomp.",
              target, returnAddress - 4, CurrentThread()->id, target);
}

uint64_t CallGuest(uint32_t entry, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6)
{
    GuestThread* t = CurrentThread();
    // Nested calls (guest -> host -> guest) must not clobber the outer context.
    auto saved = std::make_unique<PPCContext>(*t->ctx);
    PPCContext& ctx = *t->ctx;
    ctx.r1.u64 = saved->r1.u64 ? (saved->r1.u64 - 0x200) & ~0xFull : t->stackBase + t->stackSize - 0x100;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    RunEntry(t, entry);
    uint64_t result = ctx.r3.u64;
    *t->ctx = *saved;
    return result;
}
