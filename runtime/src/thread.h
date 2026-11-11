// Guest threads. Each guest thread runs on its own host thread and has:
//  - a guest stack,
//  - a KPCR (r13 points at it; +0x00 TLS pointer, +0x100 current KTHREAD),
//  - a KTHREAD (dispatcher header + thread id at +0x14C),
//  - its own PPCContext.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct PPCContext;
struct KThreadObj;

struct GuestThread
{
    uint32_t id = 0;
    uint32_t handle = 0;
    uint32_t kthread = 0;
    uint32_t pcr = 0;
    uint32_t stackBase = 0;     // lowest address
    uint32_t stackSize = 0;
    uint32_t tlsBlock = 0;
    uint32_t tlsSlots[64] = {};
    std::atomic<int> suspendCount{0};
    std::shared_ptr<KThreadObj> object;
    std::unique_ptr<PPCContext> ctx;
    std::string name;
};

// The guest thread running on the calling host thread. Host threads that
// never ran guest code (e.g. the audio/vblank threads) get one on first use.
GuestThread* CurrentThread();

// Creates a guest thread that starts at `entry` with r3/r4 = arg0/arg1.
GuestThread* CreateGuestThread(uint32_t entry, uint32_t arg0, uint32_t arg1,
                               uint32_t stackSize, bool suspended, const char* name);

// Resumes a thread created suspended. Returns the previous suspend count.
int ResumeGuestThread(GuestThread* t);

// Runs `entry` on the calling host thread (the main thread, or a helper
// thread calling back into the game) and returns r3.
uint64_t CallGuest(uint32_t entry, uint64_t r3 = 0, uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0);

// Thrown by ExTerminateThread to unwind out of the recompiled code.
struct GuestThreadExit { uint32_t code; };

// Starts a host thread with a large stack (recompiled code uses a lot).
void StartHostThread(std::function<void()> fn, size_t stackSize = 64ull << 20);

GuestThread* FindThreadByKThread(uint32_t kthread);

// CPU-time profiling: threads register themselves; ProfileReport logs how
// much CPU each used per frame since the previous report.
void ProfileRegisterCurrentThread(const std::string& name);
void ProfileReport(uint32_t frames);
// Samples where each guest thread is executing (Windows) and adds the
// busiest thread's hottest functions to ProfileReport. `symbolize` turns a
// host instruction address into a name.
void ProfileStartSampler(std::string (*symbolize)(uintptr_t hostPc));
// Calls fn with the OS handle of every running guest thread (Windows).
void ForEachGuestThreadHandle(const std::function<void(void*)>& fn);
