// Kernel objects (events, semaphores, timers, threads, files) and waiting.
//
// Objects are reachable two ways, like on the console:
//  - through handles (Nt* functions), and
//  - through guest memory addresses of dispatcher objects (Ke* functions).
//    The game initializes KEVENTs inline, so a guest address seen for the
//    first time is turned into a host object by reading its dispatcher header.
//
// All signal state is guarded by one global mutex (g_sync); waiters sleep on
// one condition variable. Coarse, but simple and correct.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct GuestThread;

struct KObject : std::enable_shared_from_this<KObject>
{
    enum class Type { Event, Semaphore, Thread, Timer, File, Enumerator, Session };

    explicit KObject(Type t) : type(t) {}
    virtual ~KObject() = default;

    // Called with g_sync held.
    virtual bool IsSignaled() const { return false; }
    virtual void Acquire() {}           // consume after a satisfied wait
    virtual void MirrorToGuest() {}     // copy signal state into guest dispatcher header

    Type type;
    uint32_t guestAddr = 0;             // dispatcher header in guest memory (0 = none yet)
    int waiters = 0;                    // threads currently blocked on this object (g_sync)
};

struct KEventObj : KObject
{
    KEventObj(bool manualReset, bool initial) : KObject(Type::Event), manual(manualReset), signaled(initial) {}
    bool IsSignaled() const override { return signaled || pulseRelease > 0; }
    void Acquire() override
    {
        if (pulseRelease > 0)
            pulseRelease--;
        else if (!manual)
        {
            signaled = false;
            MirrorToGuest();
        }
    }
    // PulseEvent: release the threads waiting right now (all for manual
    // reset, one for auto reset) without leaving the event signaled.
    void Pulse() { pulseRelease = manual ? waiters : std::min(waiters, 1); }
    void MirrorToGuest() override;
    bool manual;
    bool signaled;
    int pulseRelease = 0;
};

struct KSemaphoreObj : KObject
{
    KSemaphoreObj(int32_t initial, int32_t max) : KObject(Type::Semaphore), count(initial), limit(max) {}
    bool IsSignaled() const override { return count > 0; }
    void Acquire() override { --count; MirrorToGuest(); }
    void MirrorToGuest() override;
    int32_t count;
    int32_t limit;
};

struct KTimerObj : KEventObj
{
    explicit KTimerObj(bool manualReset) : KEventObj(manualReset, false) { type = Type::Timer; }
    int64_t dueTime100ns = 0;     // on the InterruptTime100ns() clock
    int64_t period100ns = 0;
    bool active = false;
};

struct KThreadObj : KObject
{
    KThreadObj() : KObject(Type::Thread) {}
    bool IsSignaled() const override { return exited; }
    GuestThread* thread = nullptr;
    bool exited = false;
};

extern std::mutex g_sync;
extern std::condition_variable g_syncCv;

// Wake every waiter after changing signal state (call with or without g_sync).
inline void NotifySyncChanged() { g_syncCv.notify_all(); }

// Handles ---------------------------------------------------------------------
constexpr uint32_t CURRENT_THREAD_HANDLE = 0xFFFFFFFE;
constexpr uint32_t CURRENT_PROCESS_HANDLE = 0xFFFFFFFF;

uint32_t CreateHandle(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> LookupHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);

template<typename T>
std::shared_ptr<T> LookupHandleAs(uint32_t handle)
{
    return std::dynamic_pointer_cast<T>(LookupHandle(handle));
}

// Guest dispatcher objects -----------------------------------------------------
void RegisterGuestObject(uint32_t addr, std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> LookupGuestObject(uint32_t addr);   // creates on first use
uint32_t EnsureGuestObject(const std::shared_ptr<KObject>& obj); // allocates a header if needed

// Waiting ------------------------------------------------------------------------
// timeoutPtr: guest pointer to a LARGE_INTEGER (negative = relative, 100ns units;
// positive = absolute FILETIME), or 0 for an infinite wait.
// Returns STATUS_SUCCESS + index of the satisfied object, or STATUS_TIMEOUT.
uint32_t WaitForObjects(const std::vector<std::shared_ptr<KObject>>& objects, bool waitAll, uint32_t timeoutPtr);

// Converts a guest LARGE_INTEGER timeout into a relative duration in 100ns units.
// Returns -1 for "infinite".
int64_t TimeoutToRelative100ns(uint32_t timeoutPtr);

// Timers -----------------------------------------------------------------------
void ArmTimer(const std::shared_ptr<KTimerObj>& timer, int64_t relative100ns, int64_t period100ns);
void CancelTimer(const std::shared_ptr<KTimerObj>& timer);
