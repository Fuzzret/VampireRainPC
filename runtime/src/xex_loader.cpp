#include "xex_loader.h"
#include "kernel.h"
#include "memory.h"

#include <fstream>
#include <map>
#include <unordered_map>

LoadedModule g_module;

namespace
{
    std::map<std::string, uint32_t> g_kernelVariables;

    uint32_t HBe32(const std::vector<uint8_t>& d, size_t o)
    {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
    uint16_t HBe16(const std::vector<uint8_t>& d, size_t o) { return uint16_t((d[o] << 8) | d[o + 1]); }

    // Kernel variable exports this game imports (ordinal -> name), from
    // XenonRecomp's xboxkrnl_table.inc. Each needs guest storage.
    struct VarExport { uint16_t ordinal; const char* name; uint32_t size; };
    constexpr VarExport kKernelVars[] = {
        { 0x01B, "ExThreadObjectType",        0x40 },
        { 0x059, "KeDebugMonitorData",        0x100 },
        { 0x0AD, "KeTimeStampBundle",         0x18 },
        { 0x158, "XboxKrnlVersion",           0x8 },
        { 0x193, "XexExecutableModuleHandle", 0x4 },
        { 0x1AE, "ExLoadedCommandLine",       0x400 },
        { 0x1BE, "VdGlobalDevice",            0x4 },
        { 0x1C0, "VdGpuClockInMHz",           0x4 },
        { 0x1C1, "VdHSIOCalibrationLock",     0x1C },
        { 0x266, "KeCertMonitorData",         0x100 },
    };

    uint32_t AllocVariable(const VarExport& v)
    {
        auto it = g_kernelVariables.find(v.name);
        if (it != g_kernelVariables.end())
            return it->second;
        uint32_t addr = mem::g_systemHeap.Alloc(v.size, 16);
        g_kernelVariables[v.name] = addr;
        return addr;
    }

    void InitVariables()
    {
        if (uint32_t a = KernelVariable("XboxKrnlVersion"))
        {
            Store16(a + 0, 2);
            Store16(a + 2, 0);
            Store16(a + 4, 17559);
            Store16(a + 6, 0);
        }
        if (uint32_t a = KernelVariable("XexExecutableModuleHandle"))
            Store32(a, g_module.moduleHandle);
        if (uint32_t a = KernelVariable("ExLoadedCommandLine"))
            strcpy(reinterpret_cast<char*>(GuestToHost(a)), "\"default.xex\"");
        if (uint32_t a = KernelVariable("VdGpuClockInMHz"))
            Store32(a, 500);
        if (uint32_t a = KernelVariable("VdHSIOCalibrationLock"))
            Store32(a + 0x10, uint32_t(-1)); // critical section lock_count
    }

    // Fake LDR_DATA_TABLE_ENTRY for the title module.
    void CreateModuleHandle()
    {
        uint32_t ldr = mem::g_systemHeap.Alloc(0x100, 16);
        Store32(ldr + 0x18, g_module.imageBase);      // dll_base
        Store32(ldr + 0x1C, g_module.imageBase);      // image_base
        Store32(ldr + 0x20, g_module.imageSize);      // image_size
        Store32(ldr + 0x34, 0x40000000);              // flags
        Store32(ldr + 0x38, g_module.imageSize);      // full_image_size
        Store32(ldr + 0x3C, g_module.entryPoint);     // entry_point
        Store16(ldr + 0x40, 1);                       // load_count
        Store32(ldr + 0x58, g_module.xexHeaderGuest); // xex_header_base
        g_module.moduleHandle = ldr;
    }
}

uint32_t KernelVariable(const char* name)
{
    auto it = g_kernelVariables.find(name);
    return it == g_kernelVariables.end() ? 0 : it->second;
}

uint32_t XexHeaderField(uint32_t key)
{
    const auto& h = g_module.header;
    uint32_t count = HBe32(h, 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t k = HBe32(h, 0x18 + i * 8);
        uint32_t v = HBe32(h, 0x1C + i * 8);
        if (k != key)
            continue;
        if ((k & 0xFF) <= 1)
            return v;
        return g_module.xexHeaderGuest + v;
    }
    return 0;
}

bool LoadXex(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        LOG("[loader] cannot open %s", path.c_str());
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 0x100 || memcmp(data.data(), "XEX2", 4) != 0)
    {
        LOG("[loader] %s is not an XEX2 file", path.c_str());
        return false;
    }

    uint32_t peOffset = HBe32(data, 0x08);
    uint32_t optCount = HBe32(data, 0x14);
    uint32_t fileFormat = 0, importLibs = 0;
    for (uint32_t i = 0; i < optCount; i++)
    {
        uint32_t k = HBe32(data, 0x18 + i * 8);
        uint32_t v = HBe32(data, 0x1C + i * 8);
        switch (k)
        {
        case 0x000003FF: fileFormat = v; break;
        case 0x000103FF: importLibs = v; break;
        case 0x00010100: g_module.entryPoint = v; break;
        case 0x00010201: g_module.imageBase = v; break;
        case 0x00020200: g_module.stackSize = v; break;
        }
    }

    g_module.header.assign(data.begin(), data.begin() + peOffset);
    g_module.imageSize = uint32_t(PPC_IMAGE_SIZE);
    if (g_module.imageBase != PPC_IMAGE_BASE)
    {
        LOG("[loader] image base %08X does not match the recompiled code (%08llX)",
            g_module.imageBase, (unsigned long long)PPC_IMAGE_BASE);
        return false;
    }

    uint16_t encryption = fileFormat ? HBe16(data, fileFormat + 4) : 0;
    uint16_t compression = fileFormat ? HBe16(data, fileFormat + 6) : 0;
    if (encryption != 0 || compression > 1)
    {
        LOG("[loader] XEX is encrypted or compressed (enc=%u comp=%u); run build.ps1 to make work\\default_dec.xex", encryption, compression);
        return false;
    }

    // Image + the recompiler's function lookup table right after it.
    mem::Commit(g_module.imageBase, g_module.imageSize);
    uint8_t* dst = GuestToHost(g_module.imageBase);
    if (compression == 0)
    {
        size_t n = std::min<size_t>(data.size() - peOffset, g_module.imageSize);
        memcpy(dst, data.data() + peOffset, n);
    }
    else
    {
        // "Basic" compression: blocks of (data_size, zero_size).
        uint32_t infoSize = HBe32(data, fileFormat);
        uint32_t blocks = (infoSize - 8) / 8;
        size_t src = peOffset, out = 0;
        for (uint32_t b = 0; b < blocks; b++)
        {
            uint32_t dataSize = HBe32(data, fileFormat + 8 + b * 8);
            uint32_t zeroSize = HBe32(data, fileFormat + 12 + b * 8);
            memcpy(dst + out, data.data() + src, dataSize);
            src += dataSize;
            out += dataSize + zeroSize;
        }
    }

    uint32_t tableBase = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
    uint32_t tableSize = uint32_t(PPC_CODE_SIZE * 2 + 8);
    mem::Commit(tableBase, tableSize);
    size_t mapped = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++, mapped++)
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;

    // XEX header in guest memory (RtlImageXexHeaderField, XamGetExecutionId).
    g_module.xexHeaderGuest = mem::g_systemHeap.Alloc(peOffset, 0x1000);
    memcpy(GuestToHost(g_module.xexHeaderGuest), data.data(), peOffset);
    CreateModuleHandle();

    // Imports. Each import has a record (type 0) the game reads through, and
    // functions also have a thunk (type 1) that the recompiler already mapped
    // to __imp__<Name>. Variable records get pointers to runtime storage;
    // function records get the thunk address so calls through them still work.
    size_t vars = 0, funcs = 0;
    if (importLibs)
    {
        uint32_t strTableSize = HBe32(data, importLibs + 4);
        uint32_t libCount = HBe32(data, importLibs + 8);
        std::vector<std::string> names;
        size_t s = importLibs + 12;
        for (uint32_t i = 0; i < libCount; i++)
        {
            names.emplace_back(reinterpret_cast<const char*>(data.data() + s));
            s += (names.back().size() + 1 + 3) & ~size_t(3);
        }

        size_t lib = importLibs + 12 + strTableSize;
        for (uint32_t i = 0; i < libCount; i++)
        {
            uint32_t libSize = HBe32(data, lib);
            uint16_t nameIndex = HBe16(data, lib + 0x24);
            uint16_t count = HBe16(data, lib + 0x26);
            const std::string& libName = names[nameIndex];

            std::unordered_map<uint16_t, uint32_t> thunks;
            std::vector<uint32_t> records;
            for (uint16_t r = 0; r < count; r++)
            {
                uint32_t addr = HBe32(data, lib + 0x28 + r * 4);
                records.push_back(addr);
                uint32_t value = Load32(addr);
                if ((value >> 24) == 1)
                    thunks[uint16_t(value & 0xFFFF)] = addr;
            }

            for (uint32_t addr : records)
            {
                uint32_t value = Load32(addr);
                if ((value >> 24) != 0)
                    continue;
                uint16_t ordinal = uint16_t(value & 0xFFFF);

                const VarExport* var = nullptr;
                if (libName == "xboxkrnl.exe")
                    for (auto& v : kKernelVars)
                        if (v.ordinal == ordinal)
                            var = &v;

                if (var)
                {
                    Store32(addr, AllocVariable(*var));
                    vars++;
                }
                else if (auto t = thunks.find(ordinal); t != thunks.end())
                {
                    Store32(addr, t->second);
                    funcs++;
                }
                else
                {
                    LOG("[loader] unresolved import %s ordinal 0x%X", libName.c_str(), ordinal);
                    Store32(addr, 0xD000BEEF);
                }
            }
            lib += libSize;
        }
    }
    InitVariables();

    LOG("[loader] %s: image %08X-%08X, entry %08X, %zu functions mapped, %zu kernel variables, %zu import slots",
        path.c_str(), g_module.imageBase, g_module.imageBase + g_module.imageSize, g_module.entryPoint, mapped, vars, funcs);
    return true;
}
