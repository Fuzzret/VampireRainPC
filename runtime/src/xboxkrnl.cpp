// xboxkrnl.exe exports with real implementations. Everything not here is a
// logged stub in stubs.cpp.
#include "kernel.h"
#include "memory.h"
#include "objects.h"
#include "thread.h"
#include "xex_loader.h"

#include <chrono>
#include <ctime>
#include <thread>

// ===========================================================================
// Memory
// ===========================================================================
GUEST_IMPORT(NtAllocateVirtualMemory)
{
    // (base_address*, region_size*, allocation_type, protect, unknown)
    uint32_t basePtr = ARG32(0), sizePtr = ARG32(1), type = ARG32(2);
    uint32_t region = Load32(basePtr);
    uint32_t size = Load32(sizePtr);
    uint32_t page = (type & 0x20000000) ? 0x10000 : 0x1000; // MEM_LARGE_PAGES
    size = (size + page - 1) & ~(page - 1);

    if (region)
    {
        region &= ~(page - 1);
        if (mem::g_virtualHeap.Contains(region))
            mem::g_virtualHeap.AllocAt(region, size); // may already be ours (commit after reserve)
        else
            mem::Commit(region, size);
    }
    else
    {
        region = mem::g_virtualHeap.Alloc(size, std::max<uint32_t>(page, 0x10000));
        if (!region)
            RETURN(STATUS_NO_MEMORY);
    }
    Store32(basePtr, region);
    Store32(sizePtr, size);
    TRACE("[mem] NtAllocateVirtualMemory -> %08X size %X type %X", region, size, type);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtFreeVirtualMemory)
{
    // (base_address*, region_size*, free_type, unknown)
    uint32_t basePtr = ARG32(0), sizePtr = ARG32(1), type = ARG32(2);
    uint32_t region = Load32(basePtr);
    if (type & 0x8000) // MEM_RELEASE
    {
        uint32_t size = mem::g_virtualHeap.SizeOf(region);
        if (!mem::g_virtualHeap.Free(region))
            RETURN(STATUS_INVALID_PARAMETER);
        if (sizePtr)
            Store32(sizePtr, size);
    }
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtQueryVirtualMemory)
{
    // (base_address, memory_basic_information*, ...)
    uint32_t addr = ARG32(0), info = ARG32(1);
    mem::Heap* heap = mem::HeapFor(addr);
    Store32(info + 0x00, addr & ~0xFFFu);   // base_address
    Store32(info + 0x04, addr & ~0xFFFu);   // allocation_base
    Store32(info + 0x08, 4);                // allocation_protect = PAGE_READWRITE
    Store32(info + 0x0C, 0x1000);           // region_size
    Store32(info + 0x10, heap || (addr >= PPC_IMAGE_BASE && addr < PPC_IMAGE_BASE + PPC_IMAGE_SIZE) ? 0x1000 : 0x10000); // MEM_COMMIT / MEM_FREE
    Store32(info + 0x14, 4);                // protect
    Store32(info + 0x18, 0x20000);          // MEM_PRIVATE
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    // (flags, size, protect, min_address, max_address, alignment) -> address
    uint32_t size = ARG32(1), protect = ARG32(2), alignment = ARG32(5);
    uint32_t page = (protect & 0x20000000) ? 0x10000 : (protect & 0x80000000) ? 0x1000000 : 0x1000;
    if (page == 0x1000000)
        page = 0x10000;  // 16 MB pages: 64 KB alignment is enough here
    size = (size + page - 1) & ~(page - 1);
    uint32_t addr = mem::g_physicalHeap.Alloc(size, std::max(alignment, page));
    TRACE("[mem] MmAllocatePhysicalMemoryEx size %X -> %08X", size, addr);
    RETURN(addr);
}

GUEST_IMPORT(MmFreePhysicalMemory)
{
    // (type, address)
    mem::g_physicalHeap.Free(ARG32(1));
    RETURN(0);
}

GUEST_IMPORT(MmGetPhysicalAddress) { RETURN(ARG32(0) & 0x1FFFFFFF); }
GUEST_IMPORT(MmQueryAddressProtect) { RETURN(4); } // PAGE_READWRITE

GUEST_IMPORT(MmQueryStatistics)
{
    uint32_t stats = ARG32(0);
    uint32_t len = Load32(stats);
    if (len < 0x68)
        RETURN(STATUS_BUFFER_OVERFLOW);
    memset(GuestToHost(stats + 4), 0, len - 4);
    uint32_t used = uint32_t((mem::g_virtualHeap.BytesInUse() + mem::g_physicalHeap.BytesInUse()) / 0x1000);
    Store32(stats + 0x04, 0x20000);                                      // total physical pages (512 MB)
    Store32(stats + 0x08, 0x1000);                                       // kernel pages
    Store32(stats + 0x0C, used < 0x1F000 ? 0x1F000 - used : 0);          // available pages
    Store32(stats + 0x10, used);                                         // title pages
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ExAllocatePool)
{
    RETURN(mem::g_virtualHeap.Alloc(ARG32(0), 16));
}

GUEST_IMPORT(ExFreePool)
{
    mem::g_virtualHeap.Free(ARG32(0));
    RETURN(0);
}

// ===========================================================================
// Threads
// ===========================================================================
GUEST_IMPORT(ExCreateThread)
{
    // (handle*, stack_size, thread_id*, xapi_startup, start_address, start_context, creation_flags)
    uint32_t handlePtr = ARG32(0), stackSize = ARG32(1), idPtr = ARG32(2);
    uint32_t xapiStartup = ARG32(3), start = ARG32(4), context = ARG32(5), flags = ARG32(6);

    // With an XAPI startup routine the thread runs xapi_startup(start, context).
    uint32_t entry = xapiStartup ? xapiStartup : start;
    uint32_t a0 = xapiStartup ? start : context;
    uint32_t a1 = xapiStartup ? context : 0;
    GuestThread* t = CreateGuestThread(entry, a0, a1, stackSize, (flags & 1) != 0, "guest");
    if (handlePtr)
        Store32(handlePtr, t->handle);
    if (idPtr)
        Store32(idPtr, t->id);
    LOG("[thread] ExCreateThread id=%u start=%08X ctx=%08X flags=%X%s", t->id, start, context, flags,
        (flags & 1) ? " (suspended)" : "");
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ExTerminateThread)
{
    throw GuestThreadExit{ ARG32(0) };
}

static GuestThread* ThreadFromHandle(uint32_t handle)
{
    if (handle == CURRENT_THREAD_HANDLE)
        return CurrentThread();
    auto obj = LookupHandleAs<KThreadObj>(handle);
    return obj ? obj->thread : nullptr;
}

GUEST_IMPORT(NtResumeThread)
{
    GuestThread* t = ThreadFromHandle(ARG32(0));
    if (!t)
        RETURN(STATUS_INVALID_HANDLE);
    int prev = ResumeGuestThread(t);
    if (ARG32(1))
        Store32(ARG32(1), uint32_t(prev));
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(KeResumeThread)
{
    GuestThread* t = FindThreadByKThread(ARG32(0));
    RETURN(t ? ResumeGuestThread(t) : 0);
}

GUEST_IMPORT(NtSuspendThread)
{
    GuestThread* t = ThreadFromHandle(ARG32(0));
    if (!t)
        RETURN(STATUS_INVALID_HANDLE);
    int prev;
    {
        std::lock_guard lock(g_sync);
        prev = t->suspendCount.fetch_add(1);
    }
    if (ARG32(1))
        Store32(ARG32(1), uint32_t(prev));

    if (t == CurrentThread())
    {
        // Self-suspension: block until another thread resumes us.
        std::unique_lock lock(g_sync);
        g_syncCv.wait(lock, [t] { return t->suspendCount.load() <= 0; });
    }
    else
    {
        // A running thread can't be stopped safely mid-code; keep the count only.
        static std::atomic<uint32_t> warned{0};
        if (warned++ < 5)
            LOG("[thread] NtSuspendThread(thread %u) from thread %u is not enforced", t->id, CurrentThread()->id);
    }
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(KeSetAffinityThread)
{
    if (ARG32(2))
        Store32(ARG32(2), 0x3F);
    RETURN(0x3F);
}
GUEST_IMPORT(KeSetBasePriorityThread) { RETURN(0); }
GUEST_IMPORT(KeQueryBasePriorityThread) { RETURN(0); }
GUEST_IMPORT(KeGetCurrentProcessType) { RETURN(1); } // title process

GUEST_IMPORT(KeDelayExecutionThread)
{
    // (mode, alertable, interval*)
    int64_t rel = TimeoutToRelative100ns(ARG32(2));
    if (rel <= 0)
        std::this_thread::yield();
    else
        std::this_thread::sleep_for(std::chrono::nanoseconds(rel * 100));
    RETURN(STATUS_SUCCESS);
}

// TLS: 64 slots per thread, stored host-side.
namespace
{
    std::mutex g_tlsMutex;
    uint64_t g_tlsUsed = 0;
}

GUEST_IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t i = 0; i < 64; i++)
    {
        if (!(g_tlsUsed & (1ull << i)))
        {
            g_tlsUsed |= 1ull << i;
            RETURN(i);
        }
    }
    RETURN(0xFFFFFFFF);
}

GUEST_IMPORT(KeTlsFree)
{
    uint32_t i = ARG32(0);
    if (i >= 64)
        RETURN(0);
    std::lock_guard lock(g_tlsMutex);
    g_tlsUsed &= ~(1ull << i);
    RETURN(1);
}

GUEST_IMPORT(KeTlsGetValue)
{
    uint32_t i = ARG32(0);
    RETURN(i < 64 ? CurrentThread()->tlsSlots[i] : 0);
}

GUEST_IMPORT(KeTlsSetValue)
{
    uint32_t i = ARG32(0);
    if (i >= 64)
        RETURN(0);
    CurrentThread()->tlsSlots[i] = ARG32(1);
    RETURN(1);
}

// ===========================================================================
// Objects and handles
// ===========================================================================
GUEST_IMPORT(NtClose)
{
    RETURN(CloseHandle(ARG32(0)) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
}

GUEST_IMPORT(NtDuplicateObject)
{
    // (handle, new_handle*, options)
    uint32_t handle = ARG32(0);
    std::shared_ptr<KObject> obj = handle == CURRENT_THREAD_HANDLE ? CurrentThread()->object : LookupHandle(handle);
    if (!obj)
        RETURN(STATUS_INVALID_HANDLE);
    Store32(ARG32(1), CreateHandle(obj));
    if (ARG32(2) & 1) // DUPLICATE_CLOSE_SOURCE
        CloseHandle(handle);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ObReferenceObjectByHandle)
{
    // (handle, object_type, object*)
    uint32_t handle = ARG32(0), out = ARG32(2);
    std::shared_ptr<KObject> obj = handle == CURRENT_THREAD_HANDLE ? CurrentThread()->object : LookupHandle(handle);
    if (!obj)
        RETURN(STATUS_INVALID_HANDLE);
    if (out)
        Store32(out, EnsureGuestObject(obj));
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ObDereferenceObject) { RETURN(0); }

// Events / semaphores through handles ----------------------------------------
GUEST_IMPORT(NtCreateEvent)
{
    // (handle*, obj_attr, event_type, initial_state); type 0 = manual reset
    auto ev = std::make_shared<KEventObj>(ARG32(2) == 0, ARG32(3) != 0);
    Store32(ARG32(0), CreateHandle(ev));
    RETURN(STATUS_SUCCESS);
}

static uint32_t SetEventState(uint32_t handle, int op, uint32_t prevPtr)
{
    auto ev = LookupHandleAs<KEventObj>(handle);
    if (!ev)
        return STATUS_INVALID_HANDLE;
    std::lock_guard lock(g_sync);
    if (prevPtr)
        Store32(prevPtr, ev->signaled ? 1 : 0);
    if (op == 0)      // set
        ev->signaled = true;
    else if (op == 1) // clear
        ev->signaled = false;
    else              // pulse: release current waiters, leave unsignaled
    {
        ev->signaled = false;
        ev->Pulse();
    }
    ev->MirrorToGuest();
    g_syncCv.notify_all();
    return STATUS_SUCCESS;
}

GUEST_IMPORT(NtSetEvent) { RETURN(SetEventState(ARG32(0), 0, ARG32(1))); }
GUEST_IMPORT(NtClearEvent) { RETURN(SetEventState(ARG32(0), 1, 0)); }
GUEST_IMPORT(NtPulseEvent) { RETURN(SetEventState(ARG32(0), 2, ARG32(1))); }

// Events / semaphores in guest memory ----------------------------------------
GUEST_IMPORT(KeSetEvent)
{
    // (event*, increment, wait) -> previous state
    auto ev = std::dynamic_pointer_cast<KEventObj>(LookupGuestObject(ARG32(0)));
    if (!ev)
        RETURN(0);
    std::lock_guard lock(g_sync);
    uint32_t prev = ev->signaled;
    ev->signaled = true;
    ev->MirrorToGuest();
    g_syncCv.notify_all();
    RETURN(prev);
}

GUEST_IMPORT(KeResetEvent)
{
    auto ev = std::dynamic_pointer_cast<KEventObj>(LookupGuestObject(ARG32(0)));
    if (!ev)
        RETURN(0);
    std::lock_guard lock(g_sync);
    uint32_t prev = ev->signaled;
    ev->signaled = false;
    ev->MirrorToGuest();
    RETURN(prev);
}

GUEST_IMPORT(KeInitializeSemaphore)
{
    // (semaphore*, count, limit)
    uint32_t addr = ARG32(0);
    Store8(addr + 0, 5);
    Store8(addr + 2, 5);
    Store32(addr + 4, ARG32(1));
    Store32(addr + 0x10, ARG32(2));
    RegisterGuestObject(addr, std::make_shared<KSemaphoreObj>(int32_t(ARG32(1)), int32_t(ARG32(2))));
    RETURN(0);
}

GUEST_IMPORT(KeReleaseSemaphore)
{
    // (semaphore*, increment, adjustment, wait) -> previous count
    auto sem = std::dynamic_pointer_cast<KSemaphoreObj>(LookupGuestObject(ARG32(0)));
    if (!sem)
        RETURN(0);
    std::lock_guard lock(g_sync);
    int32_t prev = sem->count;
    sem->count = std::min(sem->count + int32_t(ARG32(2)), sem->limit > 0 ? sem->limit : INT32_MAX);
    sem->MirrorToGuest();
    g_syncCv.notify_all();
    RETURN(uint32_t(prev));
}

// Waiting --------------------------------------------------------------------
GUEST_IMPORT(KeWaitForSingleObject)
{
    // (object*, reason, mode, alertable, timeout*)
    auto obj = LookupGuestObject(ARG32(0));
    RETURN(WaitForObjects({ obj }, false, ARG32(4)));
}

GUEST_IMPORT(KeWaitForMultipleObjects)
{
    // (count, objects*, wait_type, reason, mode, alertable, timeout*, wait_blocks*)
    uint32_t count = ARG32(0), list = ARG32(1);
    std::vector<std::shared_ptr<KObject>> objs;
    for (uint32_t i = 0; i < count; i++)
        objs.push_back(LookupGuestObject(Load32(list + i * 4)));
    RETURN(WaitForObjects(objs, ARG32(2) == 0, ARG32(6)));
}

GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    // (handle, mode, alertable, timeout*)
    uint32_t handle = ARG32(0);
    auto obj = handle == CURRENT_THREAD_HANDLE ? CurrentThread()->object : LookupHandle(handle);
    if (!obj)
        RETURN(STATUS_INVALID_HANDLE);
    RETURN(WaitForObjects({ obj }, false, ARG32(3)));
}

// Timers ---------------------------------------------------------------------
GUEST_IMPORT(NtCreateTimer)
{
    // (handle*, obj_attr, timer_type); type 0 = notification (manual reset)
    auto t = std::make_shared<KTimerObj>(ARG32(2) == 0);
    Store32(ARG32(0), CreateHandle(t));
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtSetTimerEx)
{
    // (handle, due_time*, apc_routine, apc_mode, apc_context, resume, period_ms, previous_state*)
    auto t = LookupHandleAs<KTimerObj>(ARG32(0));
    if (!t)
        RETURN(STATUS_INVALID_HANDLE);
    if (ARG32(2))
        LOG("[timer] NtSetTimerEx APC routine %08X not supported", ARG32(2));
    int64_t rel = TimeoutToRelative100ns(ARG32(1));
    ArmTimer(t, rel < 0 ? 0 : rel, int64_t(ARG32(6)) * 10000);
    if (ARG32(7))
        Store32(ARG32(7), 0);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtCancelTimer)
{
    auto t = LookupHandleAs<KTimerObj>(ARG32(0));
    if (!t)
        RETURN(STATUS_INVALID_HANDLE);
    CancelTimer(t);
    if (ARG32(1))
        Store32(ARG32(1), 0);
    RETURN(STATUS_SUCCESS);
}

// ===========================================================================
// Critical sections and spin locks (state lives in guest memory)
// ===========================================================================
// RTL_CRITICAL_SECTION: dispatcher header (0x10), be32 lock_count (0x10),
// be32 recursion_count (0x14), be32 owning_thread (0x18)
GUEST_IMPORT(RtlInitializeCriticalSection)
{
    uint32_t cs = ARG32(0);
    Store8(cs + 0, 1);
    Store8(cs + 1, 0);
    Store8(cs + 2, 4);
    Store32(cs + 4, 0);
    Store32(cs + 0x10, uint32_t(-1));
    Store32(cs + 0x14, 0);
    Store32(cs + 0x18, 0);
    RETURN(0);
}

static bool TryEnterCriticalSection(uint32_t cs, uint32_t self)
{
    uint32_t owner = Load32(cs + 0x18);
    if (owner == self)
    {
        Store32(cs + 0x14, Load32(cs + 0x14) + 1);
        Store32(cs + 0x10, Load32(cs + 0x10) + 1);
        return true;
    }
    uint32_t expected = 0;
    if (GuestAtomic32(cs + 0x18).compare_exchange_strong(expected, __builtin_bswap32(self)))
    {
        Store32(cs + 0x14, 1);
        Store32(cs + 0x10, Load32(cs + 0x10) + 1);
        return true;
    }
    return false;
}

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ARG32(0), self = CurrentThread()->kthread;
    auto start = std::chrono::steady_clock::now();
    bool reported = false;
    for (uint32_t spins = 0; !TryEnterCriticalSection(cs, self); spins++)
    {
        if (spins < 64)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (!reported && (spins & 0xFFF) == 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(10))
        {
            GuestThread* owner = FindThreadByKThread(Load32(cs + 0x18));
            LOG("[kernel] thread %u has waited >10s for critical section %08X held by thread %u (possible deadlock)",
                CurrentThread()->id, cs, owner ? owner->id : 0);
            reported = true;
        }
    }
    RETURN(0);
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    RETURN(TryEnterCriticalSection(ARG32(0), CurrentThread()->kthread) ? 1 : 0);
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ARG32(0);
    uint32_t rec = Load32(cs + 0x14);
    Store32(cs + 0x10, Load32(cs + 0x10) - 1);
    if (rec <= 1)
    {
        Store32(cs + 0x14, 0);
        GuestAtomic32(cs + 0x18).store(0);
    }
    else
        Store32(cs + 0x14, rec - 1);
    RETURN(0);
}

static void SpinAcquire(uint32_t lock)
{
    uint32_t self = __builtin_bswap32(CurrentThread()->kthread);
    for (uint32_t spins = 0;; spins++)
    {
        uint32_t expected = 0;
        if (GuestAtomic32(lock).compare_exchange_weak(expected, self))
            return;
        if (spins > 64)
            std::this_thread::yield();
    }
}

GUEST_IMPORT(KfAcquireSpinLock) { SpinAcquire(ARG32(0)); RETURN(0); }
GUEST_IMPORT(KfReleaseSpinLock) { GuestAtomic32(ARG32(0)).store(0); RETURN(0); }
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ARG32(0)); RETURN(0); }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { GuestAtomic32(ARG32(0)).store(0); RETURN(0); }
GUEST_IMPORT(KeRaiseIrqlToDpcLevel) { RETURN(0); }
GUEST_IMPORT(KfLowerIrql) { RETURN(0); }
GUEST_IMPORT(KeEnterCriticalRegion) { RETURN(0); }
GUEST_IMPORT(KeLeaveCriticalRegion) { RETURN(0); }
GUEST_IMPORT(KeEnableFpuExceptions) { RETURN(0); }
GUEST_IMPORT(KeLockL2) { RETURN(0); }
GUEST_IMPORT(KeUnlockL2) { RETURN(0); }

// ===========================================================================
// Time
// ===========================================================================
GUEST_IMPORT(KeQueryPerformanceFrequency) { RETURN(TscFrequency()); }

GUEST_IMPORT(KeQuerySystemTime)
{
    if (ARG32(0))
        Store64(ARG32(0), HostFileTimeNow());
    RETURN(0);
}

// TIME_FIELDS: be16 year, month, day, hour, minute, second, milliseconds, weekday
GUEST_IMPORT(RtlTimeToTimeFields)
{
    uint64_t ft = Load64(ARG32(0));
    uint32_t tf = ARG32(1);
    int64_t ms = int64_t(ft / 10000);
    int64_t daysSince1601 = ms / 86400000;
    int64_t msOfDay = ms % 86400000;
    // civil-from-days, shifted to the 1970 epoch
    int64_t z = daysSince1601 - 134774 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1;
    int64_t month = mp < 10 ? mp + 3 : mp - 9;
    int64_t year = yoe + era * 400 + (month <= 2);
    Store16(tf + 0, uint16_t(year));
    Store16(tf + 2, uint16_t(month));
    Store16(tf + 4, uint16_t(day));
    Store16(tf + 6, uint16_t(msOfDay / 3600000));
    Store16(tf + 8, uint16_t(msOfDay / 60000 % 60));
    Store16(tf + 10, uint16_t(msOfDay / 1000 % 60));
    Store16(tf + 12, uint16_t(msOfDay % 1000));
    Store16(tf + 14, uint16_t((daysSince1601 + 1) % 7)); // 1601-01-01 was a Monday
    RETURN(0);
}

GUEST_IMPORT(RtlTimeFieldsToTime)
{
    uint32_t tf = ARG32(0);
    int64_t y = int16_t(Load16(tf)), m = Load16(tf + 2), d = Load16(tf + 4);
    if (m < 1 || m > 12 || d < 1 || d > 31)
        RETURN(0);
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days1970 = era * 146097 + doe - 719468;
    int64_t ms = (days1970 + 134774) * 86400000 + Load16(tf + 6) * 3600000ll + Load16(tf + 8) * 60000ll +
                 Load16(tf + 10) * 1000ll + Load16(tf + 12);
    Store64(ARG32(1), uint64_t(ms) * 10000);
    RETURN(1);
}

// KeTimeStampBundle: be64 interrupt_time, be64 system_time, be32 tick_count
void StartTimeStampBundleThread()
{
    uint32_t bundle = KernelVariable("KeTimeStampBundle");
    if (!bundle)
        return;
    std::thread([bundle] {
        for (;;)
        {
            uint64_t it = InterruptTime100ns();
            Store64(bundle + 0, it);
            Store64(bundle + 8, HostFileTimeNow());
            Store32(bundle + 16, uint32_t(it / 10000));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

// ===========================================================================
// Strings and memory helpers
// ===========================================================================
GUEST_IMPORT(RtlInitAnsiString)
{
    uint32_t dst = ARG32(0), src = ARG32(1);
    uint16_t len = src ? uint16_t(strlen(GuestStr(src))) : 0;
    Store16(dst, len);
    Store16(dst + 2, src ? len + 1 : 0);
    Store32(dst + 4, src);
    RETURN(0);
}

GUEST_IMPORT(RtlInitUnicodeString)
{
    uint32_t dst = ARG32(0), src = ARG32(1);
    uint16_t len = 0;
    if (src)
        while (Load16(src + len * 2))
            len++;
    Store16(dst, uint16_t(len * 2));
    Store16(dst + 2, src ? uint16_t(len * 2 + 2) : 0);
    Store32(dst + 4, src);
    RETURN(0);
}

GUEST_IMPORT(RtlFreeAnsiString)
{
    uint32_t str = ARG32(0);
    uint32_t buf = Load32(str + 4);
    if (buf)
        if (mem::Heap* h = mem::HeapFor(buf))
            h->Free(buf);
    Store16(str, 0);
    Store16(str + 2, 0);
    Store32(str + 4, 0);
    RETURN(0);
}

GUEST_IMPORT(RtlUnicodeStringToAnsiString)
{
    // (dest, source, allocate)
    uint32_t dst = ARG32(0), src = ARG32(1);
    bool alloc = ARG32(2) != 0;
    uint16_t chars = Load16(src) / 2;
    uint32_t srcBuf = Load32(src + 4);
    uint32_t dstBuf;
    if (alloc)
    {
        dstBuf = mem::g_virtualHeap.Alloc(chars + 1);
        Store16(dst + 2, chars + 1);
        Store32(dst + 4, dstBuf);
    }
    else
    {
        dstBuf = Load32(dst + 4);
        if (Load16(dst + 2) < chars + 1)
            RETURN(STATUS_BUFFER_OVERFLOW);
    }
    for (uint16_t i = 0; i < chars; i++)
    {
        uint16_t c = Load16(srcBuf + i * 2);
        Store8(dstBuf + i, uint8_t(c < 0x100 ? c : '?'));
    }
    Store8(dstBuf + chars, 0);
    Store16(dst, chars);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    // (unicode, max_bytes, out_bytes*, multibyte, multibyte_bytes)
    uint32_t uni = ARG32(0), maxBytes = ARG32(1), outPtr = ARG32(2), mb = ARG32(3), mbBytes = ARG32(4);
    uint32_t n = std::min(maxBytes / 2, mbBytes);
    for (uint32_t i = 0; i < n; i++)
        Store16(uni + i * 2, Load8(mb + i));
    if (outPtr)
        Store32(outPtr, n * 2);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    // (multibyte, max_bytes, out_bytes*, unicode, unicode_bytes)
    uint32_t mb = ARG32(0), maxBytes = ARG32(1), outPtr = ARG32(2), uni = ARG32(3), uniBytes = ARG32(4);
    uint32_t n = std::min(maxBytes, uniBytes / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = Load16(uni + i * 2);
        Store8(mb + i, uint8_t(c < 0x100 ? c : '?'));
    }
    if (outPtr)
        Store32(outPtr, n);
    RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(RtlCompareMemoryUlong)
{
    // (source, length, pattern) -> bytes that matched
    uint32_t src = ARG32(0), len = ARG32(1), pattern = ARG32(2);
    uint32_t i = 0;
    while (i + 4 <= len && Load32(src + i) == pattern)
        i += 4;
    RETURN(i);
}

GUEST_IMPORT(RtlFillMemoryUlong)
{
    uint32_t dst = ARG32(0), len = ARG32(1), pattern = ARG32(2);
    for (uint32_t i = 0; i + 4 <= len; i += 4)
        Store32(dst + i, pattern);
    RETURN(0);
}

GUEST_IMPORT(RtlNtStatusToDosError)
{
    uint32_t s = ARG32(0);
    switch (s)
    {
    case STATUS_SUCCESS: RETURN(0);
    case STATUS_TIMEOUT: RETURN(1460);
    case STATUS_PENDING: RETURN(997);
    case STATUS_NO_MORE_FILES: RETURN(18);
    case STATUS_BUFFER_OVERFLOW: RETURN(234);
    case STATUS_INVALID_HANDLE: RETURN(6);
    case STATUS_INVALID_PARAMETER: RETURN(87);
    case STATUS_NO_SUCH_FILE:
    case STATUS_OBJECT_NAME_NOT_FOUND: RETURN(2);
    case STATUS_OBJECT_PATH_NOT_FOUND: RETURN(3);
    case STATUS_END_OF_FILE: RETURN(38);
    case STATUS_NO_MEMORY: RETURN(8);
    case STATUS_ACCESS_DENIED: RETURN(5);
    case STATUS_OBJECT_NAME_COLLISION: RETURN(183);
    default: RETURN((s & 0xC0000000) ? 317 : s);
    }
}

GUEST_IMPORT(RtlRaiseException)
{
    // EXCEPTION_RECORD: code, flags, record, address, num_params, info[]
    uint32_t rec = ARG32(0);
    uint32_t code = Load32(rec);
    if (code == 0x406D1388 && Load32(rec + 0x14) == 0x1000) // SetThreadName(type, name, thread_id)
    {
        const char* name = GuestStr(Load32(rec + 0x18));
        uint32_t id = Load32(rec + 0x1C);
        if (id == 0xFFFFFFFF)
            id = CurrentThread()->id;
        LOG("[thread] thread %u named '%s'", id, name ? name : "");
        if (name && id == CurrentThread()->id)
            CurrentThread()->name = name;
        RETURN(0);
    }
    LOG("[kernel] RtlRaiseException code %08X at %08X (continuing)", code, Load32(rec + 0x0C));
    RETURN(0);
}

// ===========================================================================
// Modules / XEX
// ===========================================================================
GUEST_IMPORT(RtlImageXexHeaderField)
{
    // (xex_header_base, key). Only one module exists, so the base is ignored.
    RETURN(XexHeaderField(ARG32(1)));
}

GUEST_IMPORT(XexCheckExecutablePrivilege)
{
    uint32_t flags = XexHeaderField(0x00030000);
    RETURN((flags >> (ARG32(0) & 31)) & 1);
}

GUEST_IMPORT(XexGetModuleHandle)
{
    // (name, handle*)
    const char* name = GuestStr(ARG32(0));
    if (!name || !strcasecmp(name, "default.xex"))
    {
        Store32(ARG32(1), g_module.moduleHandle);
        RETURN(STATUS_SUCCESS);
    }
    LOG("[kernel] XexGetModuleHandle('%s') -> not found", name);
    Store32(ARG32(1), 0);
    RETURN(STATUS_DLL_NOT_FOUND);
}

GUEST_IMPORT(XexGetProcedureAddress)
{
    LOG("[kernel] XexGetProcedureAddress(%08X, %u) not supported", ARG32(0), ARG32(1));
    if (ARG32(2))
        Store32(ARG32(2), 0);
    RETURN(STATUS_ENTRYPOINT_NOT_FOUND);
}

GUEST_IMPORT(ExGetXConfigSetting)
{
    // (category, setting, buffer, buffer_size, required_size*)
    uint16_t category = uint16_t(ARG32(0)), setting = uint16_t(ARG32(1));
    uint32_t buffer = ARG32(2), size = ARG32(3) & 0xFFFF, required = ARG32(4);
    uint32_t value = 0, valueSize = 4;
    bool known = true;
    if (category == 3 && setting == 9)          value = 1;          // XCONFIG_USER_LANGUAGE: English
    else if (category == 3 && setting == 10)    value = 0x00040000; // XCONFIG_USER_VIDEO_FLAGS: widescreen
    else if (category == 3 && setting == 11)    value = 0;          // XCONFIG_USER_AUDIO_FLAGS
    else if (category == 3 && setting == 1)     value = 0;          // time zone bias
    else if (category == 3 && setting == 14)    value = 0x00010000; // retail flags / AV region
    else if (category == 2 && setting == 3)     value = 0x000000FF; // game region
    else
        known = false;

    if (!known)
        LOG("[kernel] ExGetXConfigSetting(%u, %u) unknown, returning zeros", category, setting);
    if (buffer && size)
    {
        memset(GuestToHost(buffer), 0, size);
        if (size >= 4)
            Store32(buffer, value);
        else if (size >= 2)
            Store16(buffer, uint16_t(value));
    }
    if (required)
        Store16(required, uint16_t(valueSize));
    RETURN(STATUS_SUCCESS);
}

// ===========================================================================
// Fatal / debug
// ===========================================================================
GUEST_IMPORT(KeBugCheck) { FatalExit("KeBugCheck(%08X)", ARG32(0)); }
GUEST_IMPORT(KeBugCheckEx) { FatalExit("KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)", ARG32(0), ARG32(1), ARG32(2), ARG32(3), ARG32(4)); }
GUEST_IMPORT(HalReturnToFirmware) { LOG("HalReturnToFirmware(%u)", ARG32(0)); fflush(stdout); std::_Exit(0); }
GUEST_IMPORT(DbgBreakPoint) { LOG("[kernel] DbgBreakPoint hit (ignored)"); RETURN(0); }
GUEST_IMPORT(ExRegisterTitleTerminateNotification) { RETURN(0); }
GUEST_IMPORT(KiApcNormalRoutineNop) { RETURN(0); }
