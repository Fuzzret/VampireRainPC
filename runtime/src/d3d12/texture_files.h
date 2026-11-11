// Loading replacement textures (mods\textures): .dds files directly, and
// .png, .jpg and .bmp through the Windows Imaging Component.
#pragma once
#ifdef _WIN32
#include <cstdint>
#include <string>
#include <vector>
#include <dxgiformat.h>

namespace texfiles
{
    struct Image
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0, height = 0;
        // Mip levels, largest first, rows tightly packed (block rows for
        // compressed formats).
        struct Level { uint32_t width, height, rowBytes, rows; std::vector<uint8_t> data; };
        std::vector<Level> levels;
    };

    // Loads a .dds (BC1-BC5, BC7, 32-bit RGBA/BGRA) or, through WIC, a .png
    // or other image (converted to RGBA8 with mip levels made here).
    // Returns false (with a reason) if the file can't be used.
    bool Load(const std::wstring& path, Image& out, std::string& error);
}
#endif
