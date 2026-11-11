#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct LoadedModule
{
    uint32_t imageBase = 0;
    uint32_t imageSize = 0;
    uint32_t entryPoint = 0;
    uint32_t stackSize = 0;
    uint32_t xexHeaderGuest = 0;   // copy of the XEX header in guest memory
    uint32_t moduleHandle = 0;     // fake LDR_DATA_TABLE_ENTRY
    std::vector<uint8_t> header;   // host copy of the XEX header
};

extern LoadedModule g_module;

// Loads a decrypted, decompressed XEX (XexTool -e u -c u output) into guest
// memory, resolves kernel variable imports and fills the function lookup table.
bool LoadXex(const std::string& path);

// Looks up an optional XEX header. Inline values (key size byte 0 or 1) are
// returned directly; others as a guest address of the data. 0 if absent.
uint32_t XexHeaderField(uint32_t key);

// Guest address of a kernel variable export (e.g. "KeTimeStampBundle"), or 0.
uint32_t KernelVariable(const char* name);
