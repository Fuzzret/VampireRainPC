#include "objects.h"
#include "kernel.h"
#include "memory.h"
#include "thread.h"

#include <chrono>
#include <thread>
#include <unordered_map>

std::mutex g_sync;
std::condition_variable g_syncCv;

// Dispatcher header (16 bytes): u8 type, u8 absolute, u8 size, u8 inserted,
// be32 signal_state, LIST_ENTRY wait_list.
namespace
{
    constexpr uint8_t DISPATCHER_NOTIFICATION_EVENT = 0;
    constexpr uint8_t DISPATCHER_SYNCHRONIZATION_EVENT = 1;
    constexpr uint8_t DISPATCHER_SEMAPHORE = 5;
    constexpr uint8_t DISPATCHER_THREAD = 6;
    constexpr uint8_t DISPATCHER_NOTIFICATION_TIMER = 8;
    constexpr uint8_t DISPATCHER_SYNCHRONIZATION_TIMER = 9;

    std::mutex g_handleMutex;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
    uint32_t g_nextHandle = 0xF8000004;

    std::mutex g_guestObjMutex;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_guestObjects;
}

void KEventObj::MirrorToGuest()
{
    if (guestAddr)
        Store32(guestAddr + 4, signaled ? 1 : 0);
}

void KSemaphoreObj::MirrorToGuest()
{
    if (guestAddr)
        Store32(guestAddr + 4, uint32_t(count));
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
uint32_t CreateHandle(std::shared_ptr<KObject> obj)
{
    std::lock_guard lock(g_handleMutex);
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KObject> LookupHandle(uint32_t handle)
{
    std::lock_guard lock(g_handleMutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool CloseHandle(uint32_t handle)
{
    std::shared_ptr<KObject> obj;
    {
        std::lock_guard lock(g_handleMutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end())
            return false;
        obj = std::move(it->second);
        g_handles.erase(it);
    }
    return true; // obj (and any open file) released here if this was the last reference
}

// ---------------------------------------------------------------------------
// Guest dispatcher objects
// ---------------------------------------------------------------------------
void RegisterGuestObject(uint32_t addr, std::shared_ptr<KObject> obj)
{
    obj->guestAddr = addr;
    std::lock_guard lock(g_guestObjMutex);
    g_guestObjects[addr] = std::move(obj);
}

std::shared_ptr<KObject> LookupGuestObject(uint32_t addr)
{
    if (addr == 0)
        return nullptr;

    std::lock_guard lock(g_guestObjMutex);
    auto it = g_guestObjects.find(addr);
    if (it != g_guestObjects.end())
        return it->second;

    uint8_t type = Load8(addr);
    int32_t signal = int32_t(Load32(addr + 4));
    std::shared_ptr<KObject> obj;
    switch (type)
    {
    case DISPATCHER_NOTIFICATION_EVENT:
        obj = std::make_shared<KEventObj>(true, signal != 0);
        break;
    case DISPATCHER_SYNCHRONIZATION_EVENT:
        obj = std::make_shared<KEventObj>(false, signal != 0);
        break;
    case DISPATCHER_SEMAPHORE:
        obj = std::make_shared<KSemaphoreObj>(signal, int32_t(Load32(addr + 0x10)));
        break;
    case DISPATCHER_NOTIFICATION_TIMER:
    case DISPATCHER_SYNCHRONIZATION_TIMER:
        obj = std::make_shared<KTimerObj>(type == DISPATCHER_NOTIFICATION_TIMER);
        break;
    default:
        LOG("[kernel] unknown dispatcher object type %u at %08X, treating as auto-reset event", type, addr);
        obj = std::make_shared<KEventObj>(false, signal != 0);
        break;
    }
    obj->guestAddr = addr;
    g_guestObjects[addr] = obj;
    return obj;
}

uint32_t EnsureGuestObject(const std::shared_ptr<KObject>& obj)
{
    if (obj->guestAddr)
        return obj->guestAddr;

    uint32_t addr = mem::g_systemHeap.Alloc(0x20, 16);
    uint8_t type = DISPATCHER_SYNCHRONIZATION_EVENT;
    switch (obj->type)
    {
    case KObject::Type::Event: type = static_cast<KEventObj*>(obj.get())->manual ? DISPATCHER_NOTIFICATION_EVENT : DISPATCHER_SYNCHRONIZATION_EVENT; break;
    case KObject::Type::Semaphore: type = DISPATCHER_SEMAPHORE; break;
    case KObject::Type::Thread: type = DISPATCHER_THREAD; break;
    case KObject::Type::Timer: type = DISPATCHER_SYNCHRONIZATION_TIMER; break;
    default: break;
    }
    Store8(addr, type);
    Store8(addr + 2, 4);
    RegisterGuestObject(addr, obj);
    {
        std::lock_guard lock(g_sync);
        obj->MirrorToGuest();
    }
    return addr;
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------
int64_t TimeoutToRelative100ns(uint32_t timeoutPtr)
{
    if (timeoutPtr == 0)
        return -1;
    int64_t t = int64_t(Load64(timeoutPtr));
    if (t <= 0)
        return -t;
    int64_t rel = t - int64_t(HostFileTimeNow()); // absolute FILETIME
    return rel > 0 ? rel : 0;
}

uint32_t WaitForObjects(const std::vector<std::shared_ptr<KObject>>& objects, bool waitAll, uint32_t timeoutPtr)
{
    using clock = std::chrono::steady_clock;
    static const char* const kTypeNames[] = { "event", "semaphore", "thread", "timer", "file", "enumerator" };
    bool reported = false;
    int64_t rel = TimeoutToRelative100ns(timeoutPtr);
    auto deadline = clock::now() + std::chrono::nanoseconds(rel < 0 ? 0 : rel * 100);

    std::unique_lock lock(g_sync);

    // Count ourselves as a waiter while blocked (PulseEvent releases only
    // threads that are already waiting).
    struct WaiterScope
    {
        const std::vector<std::shared_ptr<KObject>>& objs;
        explicit WaiterScope(const std::vector<std::shared_ptr<KObject>>& o) : objs(o) { for (auto& x : objs) if (x) x->waiters++; }
        ~WaiterScope() { for (auto& x : objs) if (x) x->waiters--; }
    } scope(objects);

    for (;;)
    {
        if (waitAll)
        {
            bool all = true;
            for (auto& o : objects)
                all = all && o && o->IsSignaled();
            if (all)
            {
                for (auto& o : objects)
                    o->Acquire();
                return STATUS_SUCCESS;
            }
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (objects[i] && objects[i]->IsSignaled())
                {
                    objects[i]->Acquire();
                    return STATUS_SUCCESS + uint32_t(i);
                }
            }
        }

        if (rel == 0)
            return STATUS_TIMEOUT;
        if (rel < 0)
        {
            // Infinite wait: report once if it lasts long (usual sign of a hang).
            if (g_syncCv.wait_for(lock, std::chrono::seconds(5)) == std::cv_status::timeout && !reported)
            {
                reported = true;
                std::string list;
                for (auto& o : objects)
                {
                    char buf[48];
                    snprintf(buf, sizeof(buf), " %s@%08X", o ? kTypeNames[int(o->type)] : "null", o ? o->guestAddr : 0);
                    list += buf;
                }
                GuestThread* self = CurrentThread();
                LOG("[wait] thread %u ('%s') blocked >5s (wait%s) on%s, called from %08X",
                    self->id, self->name.c_str(), waitAll ? "All" : "Any", list.c_str(), uint32_t(self->ctx->lr));
            }
        }
        else if (g_syncCv.wait_until(lock, deadline) == std::cv_status::timeout)
            rel = 0; // check once more, then time out
    }
}

// ---------------------------------------------------------------------------
// Timers: one scheduler thread fires due timers.
// ---------------------------------------------------------------------------
namespace
{
    std::vector<std::weak_ptr<KTimerObj>> g_timers; // guarded by g_sync
    bool g_timerThreadStarted = false;

    void TimerThread()
    {
        std::unique_lock lock(g_sync);
        for (;;)
        {
            int64_t now = int64_t(InterruptTime100ns());
            int64_t next = INT64_MAX;
            bool fired = false;
            for (auto it = g_timers.begin(); it != g_timers.end();)
            {
                auto t = it->lock();
                if (!t) { it = g_timers.erase(it); continue; }
                if (t->active)
                {
                    if (t->dueTime100ns <= now)
                    {
                        t->signaled = true;
                        t->MirrorToGuest();
                        fired = true;
                        if (t->period100ns > 0)
                            t->dueTime100ns += t->period100ns;
                        else
                            t->active = false;
                    }
                    if (t->active)
                        next = std::min(next, t->dueTime100ns);
                }
                ++it;
            }
            if (fired)
                g_syncCv.notify_all();

            if (next == INT64_MAX)
                g_syncCv.wait_for(lock, std::chrono::milliseconds(50));
            else
                g_syncCv.wait_for(lock, std::chrono::nanoseconds(std::max<int64_t>(next - now, 1000) * 100));
        }
    }
}

void ArmTimer(const std::shared_ptr<KTimerObj>& timer, int64_t relative100ns, int64_t period100ns)
{
    std::lock_guard lock(g_sync);
    timer->dueTime100ns = int64_t(InterruptTime100ns()) + relative100ns;
    timer->period100ns = period100ns;
    timer->active = true;
    timer->signaled = false;
    timer->MirrorToGuest();
    bool known = false;
    for (auto& w : g_timers)
        known = known || w.lock() == timer;
    if (!known)
        g_timers.push_back(timer);
    if (!g_timerThreadStarted)
    {
        g_timerThreadStarted = true;
        std::thread(TimerThread).detach();
    }
    g_syncCv.notify_all();
}

void CancelTimer(const std::shared_ptr<KTimerObj>& timer)
{
    std::lock_guard lock(g_sync);
    timer->active = false;
}
