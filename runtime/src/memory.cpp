#include "memory.h"
#include "kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>

uint8_t* g_base = nullptr;

namespace mem
{
    Heap g_virtualHeap;
    Heap g_systemHeap;
    Heap g_physicalHeap;

    static constexpr uint64_t PAGE = 0x1000;

    // Physical memory (512 MB) is one shared memory object mapped three times,
    // at 0xA0000000, 0xC0000000 and 0xE0000000, so those views alias the same
    // RAM like on the console. The game converts GPU physical addresses back
    // to virtual ones using any of these views.
    static constexpr uint64_t PHYS_SIZE = 0x20000000;
    static constexpr uint32_t kPhysicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

    bool Init()
    {
#ifdef _WIN32
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                            DWORD(PHYS_SIZE >> 32), DWORD(PHYS_SIZE), nullptr);
        if (!section)
            return false;
        // Find a free 4 GB range, then rebuild it as [reservation | 3 views].
        for (int attempt = 0; attempt < 16 && !g_base; attempt++)
        {
            void* probe = VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS);
            if (!probe)
                return false;
            VirtualFree(probe, 0, MEM_RELEASE);
            uint8_t* b = static_cast<uint8_t*>(probe);

            if (!VirtualAlloc(b, kPhysicalViews[0], MEM_RESERVE, PAGE_NOACCESS))
                continue;
            void* views[3] = {};
            bool ok = true;
            for (int i = 0; i < 3 && ok; i++)
            {
                views[i] = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(PHYS_SIZE), b + kPhysicalViews[i]);
                ok = views[i] != nullptr;
            }
            if (ok)
            {
                g_base = b;
                break;
            }
            for (void* v : views)
                if (v)
                    UnmapViewOfFile(v);
            VirtualFree(b, 0, MEM_RELEASE);
        }
        if (!g_base)
            return false;
#else
        void* p = mmap(nullptr, PPC_MEMORY_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            return false;
        g_base = static_cast<uint8_t*>(p);
        int fd = memfd_create("vr_physical", 0);
        if (fd < 0 || ftruncate(fd, PHYS_SIZE) != 0)
            return false;
        for (uint32_t view : kPhysicalViews)
            if (mmap(g_base + view, PHYS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
                return false;
#endif

        g_virtualHeap.Init(VIRTUAL_BASE, VIRTUAL_END, "virtual");
        g_systemHeap.Init(SYSTEM_BASE, SYSTEM_END, "system");
        g_physicalHeap.Init(PHYSICAL_BASE, PHYSICAL_END, "physical");
        return true;
    }

    bool Commit(uint32_t addr, uint32_t size)
    {
        if (size == 0)
            return true;
        if (addr < 0x10000)
        {
            // Never map the null page: on the console a null dereference faults.
            LOG("[mem] refusing to commit %08X+%X (null page)", addr, size);
            return false;
        }
        if (addr >= kPhysicalViews[0])
            return true; // physical views are always mapped
        uint64_t start = addr & ~(PAGE - 1);
        uint64_t end = (uint64_t(addr) + size + PAGE - 1) & ~(PAGE - 1);
#ifdef _WIN32
        return VirtualAlloc(g_base + start, end - start, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
        return mprotect(g_base + start, end - start, PROT_READ | PROT_WRITE) == 0;
#endif
    }

    void Decommit(uint32_t addr, uint32_t size)
    {
        uint64_t start = (uint64_t(addr) + PAGE - 1) & ~(PAGE - 1);
        uint64_t end = (uint64_t(addr) + size) & ~(PAGE - 1);
        if (end <= start || addr >= kPhysicalViews[0])
            return;
#ifdef _WIN32
        VirtualFree(g_base + start, end - start, MEM_DECOMMIT);
#else
        madvise(g_base + start, end - start, MADV_DONTNEED);
#endif
    }

    // -----------------------------------------------------------------------

    void Heap::Init(uint32_t start, uint32_t end, const char* name)
    {
        std::lock_guard lock(m_mutex);
        m_start = start;
        m_end = end;
        m_name = name;
        m_free.clear();
        m_used.clear();
        m_free[start] = end - start;
    }

    uint32_t Heap::Alloc(uint32_t size, uint32_t align)
    {
        if (size == 0)
            size = 1;
        align = std::max<uint32_t>(align, 16);
        size = (size + 15) & ~15u;

        uint32_t result = 0;
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_free.begin(); it != m_free.end(); ++it)
            {
                uint64_t blockStart = it->first;
                uint64_t blockEnd = blockStart + it->second;
                uint64_t aligned = (blockStart + align - 1) & ~uint64_t(align - 1);
                if (aligned + size > blockEnd)
                    continue;

                m_free.erase(it);
                if (aligned > blockStart)
                    m_free[uint32_t(blockStart)] = uint32_t(aligned - blockStart);
                if (aligned + size < blockEnd)
                    m_free[uint32_t(aligned + size)] = uint32_t(blockEnd - (aligned + size));

                result = uint32_t(aligned);
                m_used[result] = size;
                m_inUse += size;
                break;
            }
        }

        if (result == 0)
        {
            LOG("[mem] %s heap exhausted (request 0x%X, in use 0x%llX)", m_name, size, (unsigned long long)m_inUse);
            return 0;
        }

        Commit(result, size);
        memset(g_base + result, 0, size);
        return result;
    }

    bool Heap::AllocAt(uint32_t addr, uint32_t size)
    {
        size = (size + 15) & ~15u;
        std::lock_guard lock(m_mutex);
        auto it = m_free.upper_bound(addr);
        if (it == m_free.begin())
            return false;
        --it;
        uint64_t blockStart = it->first;
        uint64_t blockEnd = blockStart + it->second;
        if (addr < blockStart || uint64_t(addr) + size > blockEnd)
            return false;

        m_free.erase(it);
        if (addr > blockStart)
            m_free[uint32_t(blockStart)] = uint32_t(addr - blockStart);
        if (uint64_t(addr) + size < blockEnd)
            m_free[addr + size] = uint32_t(blockEnd - (uint64_t(addr) + size));
        m_used[addr] = size;
        m_inUse += size;
        Commit(addr, size);
        return true;
    }

    bool Heap::Free(uint32_t addr)
    {
        std::lock_guard lock(m_mutex);
        auto used = m_used.find(addr);
        if (used == m_used.end())
            return false;

        uint32_t start = addr;
        uint32_t size = used->second;
        m_used.erase(used);
        m_inUse -= size;

        // Coalesce with neighbours.
        auto next = m_free.lower_bound(start);
        if (next != m_free.end() && next->first == start + size)
        {
            size += next->second;
            next = m_free.erase(next);
        }
        if (next != m_free.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start)
            {
                start = prev->first;
                size += prev->second;
                m_free.erase(prev);
            }
        }
        m_free[start] = size;
        return true;
    }

    uint32_t Heap::SizeOf(uint32_t addr)
    {
        std::lock_guard lock(m_mutex);
        auto it = m_used.find(addr);
        return it == m_used.end() ? 0 : it->second;
    }

    Heap* HeapFor(uint32_t addr)
    {
        if (g_virtualHeap.Contains(addr)) return &g_virtualHeap;
        if (g_systemHeap.Contains(addr)) return &g_systemHeap;
        if (g_physicalHeap.Contains(addr)) return &g_physicalHeap;
        return nullptr;
    }
}
