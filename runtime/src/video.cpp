// Vd* (video driver) exports and a minimal GPU command processor.
//
// There is no renderer yet. The command processor walks the PM4 command
// stream the game writes to the ring buffer and executes the packets that
// talk back to the CPU, which Direct3D waits on:
//   - type-0 register writes (incl. scratch-register write-back to memory),
//   - MEM_WRITE, EVENT_WRITE_SHD/EXT (fences), COND_WRITE, REG_RMW,
//   - INTERRUPT (calls the game's graphics interrupt callback),
//   - WAIT_REG_MEM, and INDIRECT_BUFFER (nested command buffers).
// Drawing and state packets are skipped. A renderer would handle those here.
// Packet formats follow the Xenia emulator's command processor.
#include "kernel.h"
#include "memory.h"
#include "thread.h"
#include "settings.h"
#include "gpu.h"
#include "d3d12/d3d12_backend.h"

#include "xenia/gpu/register_file.h"

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <thread>

void WriteVideoMode(uint32_t p);

namespace
{
    // Registers (dword index into the 0x7FC80000 block)
    constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
    constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
    constexpr uint32_t REG_COUNT = xe::gpu::RegisterFile::kRegisterCount;

    // PM4 type-3 opcodes
    enum : uint32_t
    {
        PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_WAIT_REG_MEM = 0x3C,
        PM4_MEM_WRITE = 0x3D, PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46,
        PM4_INTERRUPT = 0x54, PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_CFL = 0x59, PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_SET_BIN_MASK = 0x50, PM4_SET_BIN_SELECT = 0x51,
        PM4_DRAW_INDX = 0x22, PM4_VIZ_QUERY = 0x23, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B,
        PM4_SET_CONSTANT = 0x2D, PM4_LOAD_ALU_CONSTANT = 0x2F, PM4_DRAW_INDX_2 = 0x36, PM4_SET_CONSTANT2 = 0x55,
        PM4_SET_SHADER_CONSTANTS = 0x56, PM4_XE_SWAP = 0x64,
        PM4_SET_BIN_MASK_LO = 0x60, PM4_SET_BIN_MASK_HI = 0x61, PM4_SET_BIN_SELECT_LO = 0x62, PM4_SET_BIN_SELECT_HI = 0x63,
    };

    // Tiled rendering: Direct3D replays the same command buffer once per
    // screen tile ("bin"). Packets with the predicate bit set only run when
    // (bin_select & bin_mask) != 0 - e.g. the end-of-frame fence runs once.
    uint64_t g_binMask = ~0ull;
    uint64_t g_binSelect = ~0ull;

    std::atomic<uint32_t> g_ringBase{0}, g_ringSizeBytes{0};
    std::atomic<uint32_t> g_rptrWriteback{0};
    std::atomic<uint32_t> g_interruptCallback{0}, g_interruptData{0};
    std::atomic<uint64_t> g_swapCount{0}, g_vblankCount{0};
    uint32_t* const g_regs = gpu::Regs().values;  // GPU-side register file (CP thread only)

    uint32_t PhysicalToGuest(uint32_t phys) { return mem::PHYSICAL_BASE | (phys & 0x1FFFFFFF); }
    uint32_t ReadMmioRegister(uint32_t index) { return Load32(mem::GPU_MMIO_BASE + index * 4); }

    // Xenos addresses carry the endian mode in their low 2 bits.
    uint32_t GpuSwap(uint32_t v, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);  // 8-in-16
        case 2: return __builtin_bswap32(v);                               // 8-in-32
        case 3: return (v << 16) | (v >> 16);                              // 16-in-32
        default: return v;
        }
    }

    // Writes like the GPU does: swap by endian mode, then store in host order.
    void GpuWrite32(uint32_t addressWithEndian, uint32_t value)
    {
        uint32_t guest = PhysicalToGuest(addressWithEndian & ~3u);
        uint32_t v = GpuSwap(value, addressWithEndian & 3);
        memcpy(GuestToHost(guest), &v, 4);
    }

    uint32_t GpuRead32(uint32_t addressWithEndian)
    {
        uint32_t v;
        memcpy(&v, GuestToHost(PhysicalToGuest(addressWithEndian & ~3u)), 4);
        return GpuSwap(v, addressWithEndian & 3);
    }

    // source 0 = vblank, 1 = command processor. The handler runs "on" the
    // target CPU: like Xenia, set the current CPU number in the KPCR first
    // (the game's handler checks it).
    void DispatchInterrupt(uint32_t source, uint32_t cpu)
    {
        // Interrupts are serialized on the hardware; the vblank and command
        // processor threads must not run the game's handler concurrently.
        static std::mutex interruptMutex;
        if (uint32_t cb = g_interruptCallback.load())
        {
            std::lock_guard lock(interruptMutex);
            Store8(CurrentThread()->pcr + 0x10C, uint8_t(cpu));
            CallGuest(cb, source, g_interruptData.load());
        }
    }

    uint32_t g_lastMmioScratch[8];

    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index >= REG_COUNT)
            return;
        g_regs[index] = value;
        if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
        {
            uint32_t n = index - REG_SCRATCH_REG0;
            if (g_regs[REG_SCRATCH_UMSK] & (1u << n))
                Store32(PhysicalToGuest(g_regs[REG_SCRATCH_ADDR] + n * 4), value);
            // Keep the CPU-visible register in sync.
            Store32(mem::GPU_MMIO_BASE + index * 4, value);
            g_lastMmioScratch[n] = value;
        }
    }

    // The CPU writes scratch registers through the register page (the game's
    // vblank handler clears a flag the GPU waits on this way). Real hardware
    // reacts to those writes immediately; here the command processor polls.
    void SyncCpuScratchWrites()
    {
        for (uint32_t n = 0; n < 8; n++)
        {
            uint32_t v = ReadMmioRegister(REG_SCRATCH_REG0 + n);
            if (v != g_lastMmioScratch[n])
                WriteRegister(REG_SCRATCH_REG0 + n, v);
        }
    }

    bool CompareFunc(uint32_t func, uint32_t value, uint32_t ref)
    {
        switch (func & 7)
        {
        case 0: return false;
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;
        }
    }

    // Reads dwords from a ring (wrapping) or a linear indirect buffer.
    struct Reader
    {
        uint32_t base;      // guest address
        uint32_t sizeDw;    // ring size in dwords (0 = linear)
        uint32_t pos;       // dword index
        uint32_t Read()
        {
            uint32_t v = Load32(base + (sizeDw ? pos % sizeDw : pos) * 4);
            pos++;
            return v;
        }
    };

    void ExecutePackets(Reader& r, uint32_t endPos, int depth);

    // Command processor timing, reported every 300 swaps.
    double g_cpBusyUs = 0, g_cpWaitMemUs = 0;
    std::chrono::steady_clock::time_point g_cpReportStart = std::chrono::steady_clock::now();

    // SET_CONSTANT / LOAD_ALU_CONSTANT target: type selects the register block.
    bool ConstantIndex(uint32_t offsetType, uint32_t& index)
    {
        static const uint32_t kBlock[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        uint32_t type = (offsetType >> 16) & 0xFF;
        if (type > 4)
            return false;
        index = kBlock[type] + (offsetType & 0x7FF);
        return true;
    }

    void ExecutePacket(Reader& r, int depth)
    {
        uint32_t header = r.Read();
        switch (header >> 30)
        {
        case 0: // register writes
        {
            uint32_t index = header & 0x7FFF;
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            bool oneReg = (header >> 15) & 1;
            for (uint32_t i = 0; i < count; i++)
                WriteRegister(oneReg ? index : index + i, r.Read());
            return;
        }
        case 1: // two register writes
        {
            uint32_t a = r.Read(), b = r.Read();
            WriteRegister(header & 0x7FF, a);
            WriteRegister((header >> 11) & 0x7FF, b);
            return;
        }
        case 2: // filler
            return;
        }

        uint32_t opcode = (header >> 8) & 0x7F;
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        uint32_t end = r.pos + count;
        // Experiment switch: VR_DRAW_ALL_TILES=1 runs predicated draws in every tile.
        static const bool drawAllTiles = getenv("VR_DRAW_ALL_TILES") != nullptr;
        if ((header & 1) && (g_binSelect & g_binMask) == 0 &&
            !(drawAllTiles && (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2)))
        {
            if (d3d12::Capturing())
                LOG("[capture] predicated off: opcode %02X (bin select %016llX mask %016llX)", opcode,
                    (unsigned long long)g_binSelect, (unsigned long long)g_binMask);
            r.pos = end; // predicated off for this tile
            return;
        }
        if (d3d12::Capturing() && ((opcode >= 0x50 && opcode <= 0x51) || (opcode >= 0x60 && opcode <= 0x63) || opcode == PM4_EVENT_WRITE_EXT ||
                                   opcode == PM4_VIZ_QUERY || opcode == PM4_EVENT_WRITE_ZPD || opcode == PM4_COND_WRITE || opcode == PM4_DRAW_INDX))
        {
            uint32_t d0 = Load32(r.base + (r.sizeDw ? r.pos % r.sizeDw : r.pos) * 4);
            uint32_t d1 = Load32(r.base + (r.sizeDw ? (r.pos + 1) % r.sizeDw : r.pos + 1) * 4);
            LOG("[capture] packet %02X%s count %u: %08X %08X", opcode, (header & 1) ? " (predicated)" : "", count, d0, d1);
        }
        switch (opcode)
        {
        case PM4_SET_BIN_MASK_LO: g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | r.Read(); break;
        case PM4_SET_BIN_MASK_HI: g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
        case PM4_SET_BIN_SELECT_LO: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | r.Read(); break;
        case PM4_SET_BIN_SELECT_HI: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
        case PM4_SET_BIN_MASK:
        {
            uint64_t hi = r.Read(), lo = r.Read();
            g_binMask = (hi << 32) | lo;
            break;
        }
        case PM4_SET_BIN_SELECT:
        {
            uint64_t hi = r.Read(), lo = r.Read();
            g_binSelect = (hi << 32) | lo;
            break;
        }
        case PM4_MEM_WRITE:
        {
            uint32_t addr = r.Read();
            for (uint32_t i = 1; i < count; i++, addr += 4)
                GpuWrite32(addr, r.Read());
            break;
        }
        case PM4_EVENT_WRITE_SHD:
        {
            uint32_t initiator = r.Read(), addr = r.Read(), value = r.Read();
            GpuWrite32(addr, (initiator >> 31) ? uint32_t(g_vblankCount.load()) : value);
            break;
        }
        case PM4_EVENT_WRITE_EXT:
        {
            r.Read(); // initiator
            uint32_t addr = r.Read();
            // Screen extents of the previous draw (6 x u16), which Direct3D
            // uses to pick the tiles an object is drawn in. Report what Xenia
            // does (an extent covering every tile) so nothing gets skipped.
            // Big-endian u16s (the packet asks for 8-in-16 swapping).
            const uint16_t ext[6] = { 0, 8192 / 8, 0, 8192 / 8, 0, 1 };
            uint32_t guest = PhysicalToGuest(addr & ~3u);
            for (int i = 0; i < 6; i++)
                Store16(guest + i * 2, ext[i]);
            break;
        }
        case PM4_COND_WRITE:
        {
            uint32_t info = r.Read(), pollAddr = r.Read(), ref = r.Read(), mask = r.Read();
            uint32_t writeAddr = r.Read(), writeData = r.Read();
            uint32_t value = (info & 0x10) ? GpuRead32(pollAddr) : g_regs[pollAddr & (REG_COUNT - 1)];
            if (CompareFunc(info, value & mask, ref))
            {
                if (info & 0x100)
                    GpuWrite32(writeAddr, writeData);
                else
                    WriteRegister(writeAddr, writeData);
            }
            break;
        }
        case PM4_WAIT_REG_MEM:
        {
            uint32_t info = r.Read(), pollAddr = r.Read(), ref = r.Read(), mask = r.Read();
            r.Read(); // wait interval
            if (info & 0x10) // memory: wait (like the hardware) until the CPU updates it
            {
                auto start = std::chrono::steady_clock::now();
                struct WaitTimer { std::chrono::steady_clock::time_point t; ~WaitTimer() { g_cpWaitMemUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count(); } } wt{ start };
                bool reported = false;
                while (!CompareFunc(info, GpuRead32(pollAddr) & mask, ref))
                {
                    SyncCpuScratchWrites();
                    std::this_thread::yield();
                    if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
                    {
                        reported = true;
                        LOG("[gpu] command processor waiting >5s: [%08X] & %08X func %u ref %08X (value %08X)",
                            pollAddr, mask, info & 7, ref, GpuRead32(pollAddr));
                    }
                }
                d3d12::NoteCpuSync(); // the CPU may have filled buffers before signalling
            }
            // register waits (vblank/idle status) are treated as satisfied
            break;
        }
        case PM4_REG_RMW:
        {
            uint32_t info = r.Read(), andMask = r.Read(), orMask = r.Read();
            uint32_t index = info & 0x1FFF;
            uint32_t v = g_regs[index];
            v &= (info & 0x80000000) ? g_regs[andMask & 0x1FFF] : andMask;
            v |= (info & 0x40000000) ? g_regs[orMask & 0x1FFF] : orMask;
            WriteRegister(index, v);
            break;
        }
        case PM4_INTERRUPT:
        {
            uint32_t cpuMask = r.Read();
            for (uint32_t cpu = 0; cpu < 6; cpu++)
                if (cpuMask & (1u << cpu))
                    DispatchInterrupt(1, cpu);
            break;
        }
        case PM4_SET_CONSTANT:
        {
            uint32_t index;
            if (ConstantIndex(r.Read(), index))
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(index++, r.Read());
            break;
        }
        case PM4_SET_CONSTANT2:
        case PM4_SET_SHADER_CONSTANTS:
        {
            uint32_t index = r.Read() & 0xFFFF;
            for (uint32_t i = 1; i < count; i++)
                WriteRegister(index++, r.Read());
            break;
        }
        case PM4_LOAD_ALU_CONSTANT:
        {
            uint32_t addr = r.Read() & 0x3FFFFFFF, offsetType = r.Read(), size = r.Read() & 0xFFF;
            uint32_t index;
            if (ConstantIndex(offsetType, index))
                for (uint32_t i = 0; i < size; i++)
                    WriteRegister(index++, Load32(PhysicalToGuest(addr + i * 4)));
            break;
        }
        case PM4_IM_LOAD:
        {
            uint32_t addrType = r.Read(), startSize = r.Read();
            gpu::OnShaderLoad(addrType & 3, PhysicalToGuest(addrType & ~3u), startSize & 0xFFFF);
            break;
        }
        case PM4_IM_LOAD_IMMEDIATE:
        {
            uint32_t type = r.Read(), startSize = r.Read();
            // The code follows in the packet. Ring packets may wrap; copy to a
            // linear scratch buffer when they do.
            uint32_t size = startSize & 0xFFFF;
            uint32_t at = r.base + (r.sizeDw ? r.pos % r.sizeDw : r.pos) * 4;
            if (r.sizeDw && (r.pos % r.sizeDw) + size > r.sizeDw)
            {
                static uint32_t scratch = mem::g_systemHeap.Alloc(0x10000, 16);
                for (uint32_t i = 0; i < size && i < 0x4000; i++)
                    Store32(scratch + i * 4, Load32(r.base + ((r.pos + i) % r.sizeDw) * 4));
                at = scratch;
            }
            gpu::OnShaderLoad(type & 3, at, size);
            break;
        }
        case PM4_DRAW_INDX:
        case PM4_DRAW_INDX_2:
        {
            if (opcode == PM4_DRAW_INDX)
                r.Read(); // viz query condition
            uint32_t initiator = r.Read();
            uint32_t dmaBase = 0, dmaSize = 0;
            if (((initiator >> 6) & 3) == 0) // source select: DMA (index buffer)
            {
                dmaBase = r.Read();
                dmaSize = r.Read();
            }
            gpu::OnDraw(initiator, dmaBase, dmaSize, nullptr, 0);
            break;
        }
        case PM4_EVENT_WRITE_ZPD:
        {
            // Occlusion query sample counts (8 little-endian u32s at
            // RB_SAMPLE_COUNT_ADDR: total, z fail, z pass, stencil fail - A/B
            // halves summed by Direct3D). On "end" Direct3D first writes
            // 0xFFFFFEED to the pass (or fail) pair and waits for it to change.
            // Report every object as visible, like Xenia.
            r.Read(); // initiator
            uint32_t* c = reinterpret_cast<uint32_t*>(GuestToHost(PhysicalToGuest(g_regs[0x2325] & ~3u)));
            // Direct3D writes the marker big-endian; the counts are little-endian.
            constexpr uint32_t kFinished = __builtin_bswap32(0xFFFFFEEDu);
            bool end = (c[4] == kFinished && c[5] == kFinished) || (c[2] == kFinished && c[3] == kFinished);
            memset(c, 0, 32);
            if (end)
                c[0] = c[4] = 1000;
            break;
        }
        case PM4_VIZ_QUERY:
        {
            uint32_t d = r.Read(), id = d & 0x3F;
            if (d & 0x100) // end: report "visible"
                g_regs[id < 32 ? 0x0C44 : 0x0C45] |= 1u << (id & 31);
            break;
        }
        case PM4_XE_SWAP:
        {
            r.Read(); // signature
            uint32_t front = r.Read(), w = r.Read(), h = r.Read();
            gpu::OnSwap(front, w, h);
            static uint64_t cpSwaps = 0;
            if (++cpSwaps % 300 == 0)
            {
                auto now = std::chrono::steady_clock::now();
                double total = std::chrono::duration<double, std::micro>(now - g_cpReportStart).count();
                LOG("[gpu] last 300 frames: %.1f fps; command processor busy %.1f ms/frame (of which waiting on the CPU %.1f), idle (waiting for the game) %.1f ms/frame",
                    300.0 / (total / 1e6), g_cpBusyUs / 300 / 1000, g_cpWaitMemUs / 300 / 1000, (total - g_cpBusyUs) / 300 / 1000);
                ProfileReport(300);
                g_cpBusyUs = g_cpWaitMemUs = 0;
                g_cpReportStart = now;
            }
            break;
        }
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            uint32_t addr = r.Read(), sizeDw = r.Read() & 0xFFFFF;
            if (depth < 4)
            {
                Reader ib{ PhysicalToGuest(addr & ~3u), 0, 0 };
                ExecutePackets(ib, sizeDw, depth + 1);
            }
            break;
        }
        default:
            break; // drawing / state packets: skipped (no renderer yet)
        }
        r.pos = end;
    }

    void ExecutePackets(Reader& r, uint32_t endPos, int depth)
    {
        while (r.pos < endPos)
            ExecutePacket(r, depth);
    }

    void CommandProcessorThread()
    {
        ProfileRegisterCurrentThread("gpu command processor");
        uint32_t rptr = 0;
        for (;;)
        {
            uint32_t base = g_ringBase.load(), sizeBytes = g_ringSizeBytes.load();
            if (!base || !sizeBytes)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            uint32_t sizeDw = sizeBytes / 4;
            SyncCpuScratchWrites();
            uint32_t wptr = ReadMmioRegister(REG_CP_RB_WPTR) % sizeDw;
            if (wptr != rptr)
            {
                uint32_t pending = (wptr + sizeDw - rptr) % sizeDw;
                Reader r{ PhysicalToGuest(base), sizeDw, rptr };
                // The read pointer reports how far the GPU has *fetched*, not
                // executed (completion is tracked with fences). Publish it
                // before executing: Direct3D treats a read pointer that stops
                // moving as a GPU hang.
                if (uint32_t wb = g_rptrWriteback.load())
                    Store32(PhysicalToGuest(wb), wptr);
                auto t0 = std::chrono::steady_clock::now();
                d3d12::NoteCpuSync();
                ExecutePackets(r, rptr + pending, 0);
                g_cpBusyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                rptr = wptr;
            }
            else
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    void VblankThread()
    {
        ProfileRegisterCurrentThread("vblank");
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        // The game waits for two display refreshes per frame (30 fps at
        // 60 Hz). Higher frame rate settings don't change this: the extra
        // frames are drawn by the renderer (d3d12_backend.cpp).
        const auto period = std::chrono::microseconds(16667);
        for (;;)
        {
            next += period;
            std::this_thread::sleep_until(next);
            g_vblankCount++;
            DispatchInterrupt(0, 2);
        }
    }
}

void InitVideo(const std::string& g_dataDir)
{
    // Display registers the driver reads, with the values Xenia reports.
    auto setReg = [](uint32_t index, uint32_t value) { Store32(mem::GPU_MMIO_BASE + index * 4, value); };
    setReg(0x1951, 1);           // display interrupt status: vblank (the vblank handler checks bit 0)
    setReg(0x1961, 0x050002D0);  // D1MODE viewport size: 1280x720
    setReg(0x0F00, 0x08100748);  // RB_EDRAM_TIMING
    setReg(0x0F01, 0x0000200E);  // RB_BC_CONTROL

    gpu::Init(g_dataDir.c_str());
    StartHostThread(CommandProcessorThread);
    StartHostThread(VblankThread);
}

GUEST_IMPORT(VdInitializeEngines) { RETURN(1); }
GUEST_IMPORT(VdShutdownEngines) { RETURN(0); }

GUEST_IMPORT(VdInitializeRingBuffer)
{
    // (physical_address, size_log2)
    g_ringSizeBytes = 1u << (ARG32(1) + 3);
    g_ringBase = ARG32(0);
    LOG("[gpu] ring buffer at %08X, %u bytes", ARG32(0), g_ringSizeBytes.load());
    RETURN(0);
}

GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    // (physical_address, block_size_log2)
    g_rptrWriteback = ARG32(0);
    LOG("[gpu] read pointer write-back at %08X", ARG32(0));
    RETURN(0);
}

GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    // (callback, user_data)
    g_interruptCallback = ARG32(0);
    g_interruptData = ARG32(1);
    LOG("[gpu] interrupt callback %08X (data %08X)", ARG32(0), ARG32(1));
    RETURN(0);
}

GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    if (ARG32(0)) Store32(ARG32(0), 0xBEEF0000);
    if (ARG32(1)) Store32(ARG32(1), 0xBEEF0001);
    RETURN(0);
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { RETURN(0); }

GUEST_IMPORT(VdQueryVideoMode) { WriteVideoMode(ARG32(0)); RETURN(0); }
GUEST_IMPORT(VdQueryVideoFlags) { RETURN(3); } // widescreen | >= 1280 wide

GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    uint32_t p = ARG32(0);
    memset(GuestToHost(p), 0, 0x58);
    Store16(p + 0x00, 1280);  // front buffer width
    Store16(p + 0x02, 720);   // front buffer height
    Store16(p + 0x10, 1280);  // display width
    Store16(p + 0x12, 720);   // display height
    RETURN(0);
}

GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    if (ARG32(0)) Store32(ARG32(0), 1);
    if (ARG32(1)) StoreF32(ARG32(1), 2.22222233f);
    RETURN(0);
}

GUEST_IMPORT(VdSetDisplayMode) { RETURN(0); }
GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { RETURN(1); }
GUEST_IMPORT(VdRetrainEDRAM) { RETURN(0); }
GUEST_IMPORT(VdRetrainEDRAMWorker) { RETURN(0); }
GUEST_IMPORT(VdEnableDisableClockGating) { RETURN(0); }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { RETURN(0); }

GUEST_IMPORT(VdPersistDisplay)
{
    if (ARG32(1)) Store32(ARG32(1), 0);
    RETURN(1);
}

GUEST_IMPORT(VdSwap)
{
    // (buffer, fetch_constant, ..., frontbuffer*, format*, color_space*,
    // width*, height*). The game reserved 64 dwords in the ring for the swap.
    // Like Xenia, write the front buffer texture's fetch constant (converted to
    // a physical address) to fetch slot 0, then a swap packet for the command
    // processor, so the swap happens in order with the frame's rendering.
    uint32_t buffer = ARG32(0), fetchPtr = ARG32(1);
    uint32_t widthPtr = ARG32(8);
    uint32_t heightPtr = ARG32(9);
    uint32_t width = widthPtr ? Load32(widthPtr) : 1280;
    uint32_t height = heightPtr ? Load32(heightPtr) : 720;
    if (buffer)
    {
        uint32_t fetch[6];
        for (int i = 0; i < 6; i++)
            fetch[i] = fetchPtr ? Load32(fetchPtr + i * 4) : 0;
        uint32_t frontVirtual = fetch[1] & 0xFFFFF000;
        uint32_t frontPhysical = frontVirtual & 0x1FFFFFFF;
        fetch[1] = (fetch[1] & 0xFFF) | frontPhysical;

        uint32_t o = 0;
        auto put = [&](uint32_t v) { Store32(buffer + o++ * 4, v); };
        put((5u << 16) | 0x4800);                                // type 0: 6 regs at fetch constant 0
        for (uint32_t v : fetch) put(v);
        put(0xC0000000u | (3u << 16) | (PM4_XE_SWAP << 8));       // type 3 XE_SWAP, 4 dwords
        put(0x53574150);                                         // 'SWAP'
        put(frontPhysical);
        put(width);
        put(height);
        while (o < 64) put(0x80000000u);                         // type 2 filler
    }
    uint64_t n = ++g_swapCount;
    if (n <= 5 || n % 600 == 0)
        LOG("[gpu] VdSwap: frame %llu (%ux%u)", (unsigned long long)n, width, height);
    RETURN(0);
}
