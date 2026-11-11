// Camera trace (VR_CAMTRACE=1): finds the game code that turns the right
// stick into camera movement, for direct mouse control.
//
// 1. The game's controller polling function keeps a per-controller block of
//    processed input (sticks as floats). XamInputGetState is called with that
//    block's address in r31, so we know where it is.
// 2. While the player looks around, each float in the block is correlated
//    with the right stick we report; the best matches are the stick X/Y.
// 3. Hardware data breakpoints (debug registers) are put on those two floats
//    in every guest thread; each access traps, and the instruction address is
//    counted. After ~10 seconds the code that read them is logged by function.
#include "kernel.h"
#include "memory.h"
#include "thread.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

std::string GuestFunctionNameAt(uintptr_t hostPc);

namespace
{
    constexpr int kFirst = -64, kCount = 96; // floats around the block pointer
    double g_sumFR[2][kCount], g_sumFF[kCount], g_sumRR[2];
    uint32_t g_samples = 0, g_moving = 0;
    int g_phase = 0; // 0 correlate, 1 watching, 2 done
    uint32_t g_watchCalls = 0;
    uint32_t g_watchAddr[2] = {};

    // Trap records: lock-free open-addressing table of (rip, lr) -> count.
    struct Hit { std::atomic<uint64_t> rip{ 0 }; std::atomic<uint32_t> lr{ 0 }; std::atomic<uint32_t> count{ 0 }; std::atomic<uint32_t> which{ 0 }; };
    constexpr uint32_t kHits = 4096;
    Hit g_hits[kHits];
    std::atomic<bool> g_armed{ false };
    // Step 2: the game copies controller input into the player object
    // (sub_82199018: memcpy(obj + 480, input, 44), returning to 82199098);
    // r31 is the object then. Its right stick floats are at +516 / +520.
    std::atomic<uint32_t> g_playerObj{ 0 };
    int g_round = 0;

    float LoadF(uint32_t a)
    {
        uint32_t v;
        memcpy(&v, g_base + a, 4);
        v = __builtin_bswap32(v);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }

#ifdef _WIN32
    LONG CALLBACK TrapHandler(EXCEPTION_POINTERS* e)
    {
        if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_armed)
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* c = e->ContextRecord;
        uint32_t which = (c->Dr6 & 1) ? 1 : (c->Dr6 & 2) ? 2 : 0;
        if (!which)
            return EXCEPTION_CONTINUE_SEARCH;
        uint64_t rip = c->Rip;
        GuestThread* t = CurrentThread();
        uint32_t lr = t && t->ctx ? uint32_t(t->ctx->lr) : 0;
        if (lr == 0x82199098 && t && t->ctx && !g_playerObj)
            g_playerObj = t->ctx->r31.u32;
        uint32_t h = uint32_t((rip * 0x9E3779B97F4A7C15ull) >> 52) & (kHits - 1);
        for (uint32_t i = 0; i < 64; i++)
        {
            Hit& s = g_hits[(h + i) & (kHits - 1)];
            uint64_t expected = 0;
            if (s.rip.load() == rip || s.rip.compare_exchange_strong(expected, rip) || expected == rip)
            {
                s.lr = lr;
                s.which |= which;
                s.count++;
                break;
            }
        }
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void SetWatch(bool on)
    {
        static bool handlerAdded = false;
        if (!handlerAdded)
        {
            AddVectoredExceptionHandler(1, TrapHandler);
            handlerAdded = true;
        }
        g_armed = on;
        // From a helper thread: a thread can't safely change its own debug registers.
        std::thread([on] {
            ForEachGuestThreadHandle([on](void* hv) {
                HANDLE h = HANDLE(hv);
                if (SuspendThread(h) == DWORD(-1))
                    return;
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &c))
                {
                    if (on)
                    {
                        c.Dr0 = uintptr_t(g_base + g_watchAddr[0]);
                        c.Dr1 = uintptr_t(g_base + g_watchAddr[1]);
                        // L0, L1; read/write (11), 4 bytes (11) for both.
                        c.Dr7 = 1 | 4 | (3u << 16) | (3u << 18) | (3u << 20) | (3u << 22);
                    }
                    else
                        c.Dr7 = 0;
                    c.Dr6 = 0;
                    SetThreadContext(h, &c);
                }
                ResumeThread(h);
            });
        }).join();
    }
#else
    void SetWatch(bool) {}
#endif

    void Report()
    {
        std::vector<std::pair<uint32_t, uint32_t>> hits; // count, index
        for (uint32_t i = 0; i < kHits; i++)
            if (g_hits[i].count)
                hits.push_back({ g_hits[i].count.load(), i });
        std::sort(hits.rbegin(), hits.rend());
        LOG("[camtrace] %zu code locations touched the stick floats:", hits.size());
        for (size_t i = 0; i < hits.size() && i < 40; i++)
        {
            Hit& h = g_hits[hits[i].second];
            LOG("[camtrace]   %6u x  %s (host %p) %s, called from %08X", hits[i].first,
                GuestFunctionNameAt(uintptr_t(h.rip.load())).c_str(), reinterpret_cast<void*>(h.rip.load()),
                h.which == 1 ? "X" : h.which == 2 ? "Y" : "X+Y", h.lr.load());
        }
    }
}

// Called from XamInputGetState for controller 0 (r31 = the input block).
void CamTraceOnInput(uint32_t block, int16_t rx, int16_t ry)
{
    static const bool enabled = getenv("VR_CAMTRACE") != nullptr;
    if (!enabled || g_phase == 2 || block < 0x1000)
        return;
    if (g_phase == 0)
    {
        double r[2] = { rx / 32767.0, ry / 32767.0 };
        for (int i = 0; i < kCount; i++)
        {
            uint32_t a = block + uint32_t(kFirst + i * 4);
            float f = LoadF(a);
            if (!std::isfinite(f) || std::fabs(f) > 1e6f)
                f = 0;
            for (int k = 0; k < 2; k++)
                g_sumFR[k][i] += f * r[k];
            g_sumFF[i] += double(f) * f;
        }
        g_sumRR[0] += r[0] * r[0];
        g_sumRR[1] += r[1] * r[1];
        g_samples++;
        if (std::fabs(r[0]) > 0.3 || std::fabs(r[1]) > 0.3)
            g_moving++;
        if (g_samples % 300 == 0)
            LOG("[camtrace] block %08X: %u samples, %u with the right stick moved", block, g_samples, g_moving);
        if (g_moving >= 150 && g_sumRR[0] > 5 && g_sumRR[1] > 5)
        {
            // Floats are read one input call late (processed after the call
            // returns), so correlation is a little lower than perfect.
            int best[2] = { -1, -1 };
            double bestC[2] = { 0, 0 };
            for (int k = 0; k < 2; k++)
                for (int i = 0; i < kCount; i++)
                {
                    if (g_sumFF[i] <= 0)
                        continue;
                    double c = g_sumFR[k][i] / std::sqrt(g_sumFF[i] * g_sumRR[k]);
                    if (std::fabs(c) > std::fabs(bestC[k]))
                    {
                        bestC[k] = c;
                        best[k] = i;
                    }
                }
            for (int k = 0; k < 2; k++)
                LOG("[camtrace] right stick %c: best match block%+d (correlation %.2f)", k ? 'Y' : 'X',
                    best[k] < 0 ? 0 : kFirst + best[k] * 4, bestC[k]);
            if (best[0] < 0 || best[1] < 0 || std::fabs(bestC[0]) < 0.5 || std::fabs(bestC[1]) < 0.5)
            {
                LOG("[camtrace] no clear match yet - keep looking around");
                memset(g_sumFR, 0, sizeof(g_sumFR));
                memset(g_sumFF, 0, sizeof(g_sumFF));
                g_sumRR[0] = g_sumRR[1] = 0;
                g_moving = 0;
                return;
            }
            g_watchAddr[0] = block + uint32_t(kFirst + best[0] * 4);
            g_watchAddr[1] = block + uint32_t(kFirst + best[1] * 4);
            LOG("[camtrace] watching accesses to %08X (X) and %08X (Y) - keep looking around (and aim/zoom if you can)",
                g_watchAddr[0], g_watchAddr[1]);
            SetWatch(true);
            g_phase = 1;
        }
        return;
    }
    if (g_phase == 1 && ++g_watchCalls >= 450)
    {
        SetWatch(false);
        Report();
        uint32_t obj = g_playerObj;
        if (g_round == 0 && obj)
        {
            // Round 2: who reads the player object's copy of the stick.
            for (auto& h : g_hits)
            {
                h.rip = 0;
                h.count = 0;
                h.which = 0;
            }
            g_round = 1;
            g_watchCalls = 0;
            g_watchAddr[0] = obj + 516;
            g_watchAddr[1] = obj + 520;
            LOG("[camtrace] player object %08X - now watching its stick copy at %08X / %08X; keep looking around and aiming",
                obj, g_watchAddr[0], g_watchAddr[1]);
            SetWatch(true);
            return;
        }
        g_phase = 2;
        LOG("[camtrace] done - you can quit");
    }
}
