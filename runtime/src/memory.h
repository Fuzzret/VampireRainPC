// Guest address space: a 4 GB host reservation that guest addresses index
// directly (host = g_base + guest). Pages are committed on demand, so a stray
// access to unallocated guest memory faults instead of silently reading zeros.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>

namespace mem
{
    // Guest address layout used by this runtime.
    constexpr uint32_t VIRTUAL_BASE  = 0x40000000; // NtAllocateVirtualMemory, ExAllocatePool, XamAlloc
    constexpr uint32_t VIRTUAL_END   = 0x7F000000;
    constexpr uint32_t GPU_MMIO_BASE = 0x7FC80000; // Xenos GPU registers (the game writes these directly)
    constexpr uint32_t GPU_MMIO_SIZE = 0x00080000;
    constexpr uint32_t XMA_MMIO_BASE = 0x7FEA0000; // audio (XMA decoder) registers
    constexpr uint32_t XMA_MMIO_SIZE = 0x00010000;
    // 0x82000000 - image, followed by the recompiler's function lookup table
    constexpr uint32_t SYSTEM_BASE   = 0x90000000; // runtime-owned: stacks, PCRs, kernel objects
    constexpr uint32_t SYSTEM_END    = 0xA0000000;
    constexpr uint32_t PHYSICAL_BASE = 0xA0000000; // MmAllocatePhysicalMemoryEx
    constexpr uint32_t PHYSICAL_END  = 0xC0000000;

    bool Init();                                  // reserves the 4 GB range and sets g_base
    bool Commit(uint32_t addr, uint32_t size);    // make [addr, addr+size) readable/writable
    void Decommit(uint32_t addr, uint32_t size);

    // Thread-safe first-fit allocator over a guest address range. Allocations
    // are committed and zero-filled.
    class Heap
    {
    public:
        void Init(uint32_t start, uint32_t end, const char* name);
        uint32_t Alloc(uint32_t size, uint32_t align = 16);
        bool AllocAt(uint32_t addr, uint32_t size);   // claim a fixed range (best effort)
        bool Free(uint32_t addr);
        uint32_t SizeOf(uint32_t addr);
        bool Contains(uint32_t addr) const { return addr >= m_start && addr < m_end; }
        uint64_t BytesInUse() const { return m_inUse; }

    private:
        std::mutex m_mutex;
        std::map<uint32_t, uint32_t> m_free;            // addr -> size
        std::unordered_map<uint32_t, uint32_t> m_used;  // addr -> size
        uint32_t m_start = 0, m_end = 0;
        uint64_t m_inUse = 0;
        const char* m_name = "";
    };

    extern Heap g_virtualHeap;
    extern Heap g_systemHeap;
    extern Heap g_physicalHeap;

    // Picks the heap that owns an address (or nullptr).
    Heap* HeapFor(uint32_t addr);
}
