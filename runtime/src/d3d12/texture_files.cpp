// Replacement texture files: .dds read directly, other images (.png, .jpg,
// .bmp) through the Windows Imaging Component.
#ifdef _WIN32
#include "texture_files.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace texfiles
{
    namespace
    {
        uint32_t FourCC(char a, char b, char c, char d) { return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24); }

        // Bytes per 4x4 block (compressed) or per pixel (0 if unsupported).
        bool Describe(DXGI_FORMAT f, bool& block, uint32_t& bytes)
        {
            switch (f)
            {
            case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC4_UNORM: block = true; bytes = 8; return true;
            case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC7_UNORM:
                block = true; bytes = 16; return true;
            case DXGI_FORMAT_R8G8B8A8_UNORM: block = false; bytes = 4; return true;
            default: return false;
            }
        }

        void SetLevel(Image::Level& l, uint32_t w, uint32_t h, bool block, uint32_t bytes)
        {
            l.width = w;
            l.height = h;
            l.rowBytes = block ? std::max(1u, (w + 3) / 4) * bytes : w * bytes;
            l.rows = block ? std::max(1u, (h + 3) / 4) : h;
        }

        bool LoadDds(const std::vector<uint8_t>& file, Image& out, std::string& error)
        {
            if (file.size() < 128 || memcmp(file.data(), "DDS ", 4) != 0)
            {
                error = "not a DDS file";
                return false;
            }
            auto u32 = [&](size_t off) { uint32_t v; memcpy(&v, file.data() + off, 4); return v; };
            uint32_t height = u32(12), width = u32(16), mips = (u32(8) & 0x20000) ? std::max(1u, u32(28)) : 1;
            uint32_t pfFlags = u32(80), fourcc = u32(84), bits = u32(88), rMask = u32(92), aMask = u32(104);
            if (u32(112) & 0x200) // DDSCAPS2_CUBEMAP
            {
                error = "cube maps can't be replaced";
                return false;
            }
            size_t offset = 128;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            bool swapRB = false, noAlpha = false;
            if (pfFlags & 0x4) // FOURCC
            {
                if (fourcc == FourCC('D', 'X', 'T', '1')) format = DXGI_FORMAT_BC1_UNORM;
                else if (fourcc == FourCC('D', 'X', 'T', '2') || fourcc == FourCC('D', 'X', 'T', '3')) format = DXGI_FORMAT_BC2_UNORM;
                else if (fourcc == FourCC('D', 'X', 'T', '4') || fourcc == FourCC('D', 'X', 'T', '5')) format = DXGI_FORMAT_BC3_UNORM;
                else if (fourcc == FourCC('A', 'T', 'I', '1') || fourcc == FourCC('B', 'C', '4', 'U')) format = DXGI_FORMAT_BC4_UNORM;
                else if (fourcc == FourCC('A', 'T', 'I', '2') || fourcc == FourCC('B', 'C', '5', 'U')) format = DXGI_FORMAT_BC5_UNORM;
                else if (fourcc == FourCC('D', 'X', '1', '0') && file.size() >= 148)
                {
                    uint32_t dxgi = u32(128), dim = u32(132), array = u32(140);
                    offset = 148;
                    if (dim != 3 || array > 1)
                    {
                        error = "only 2D textures can be used";
                        return false;
                    }
                    switch (dxgi)
                    {
                    case 71: case 72: format = DXGI_FORMAT_BC1_UNORM; break;
                    case 74: case 75: format = DXGI_FORMAT_BC2_UNORM; break;
                    case 77: case 78: format = DXGI_FORMAT_BC3_UNORM; break;
                    case 80: format = DXGI_FORMAT_BC4_UNORM; break;
                    case 83: format = DXGI_FORMAT_BC5_UNORM; break;
                    case 98: case 99: format = DXGI_FORMAT_BC7_UNORM; break;
                    case 28: case 29: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                    case 87: case 91: format = DXGI_FORMAT_R8G8B8A8_UNORM; swapRB = true; break;
                    case 88: case 93: format = DXGI_FORMAT_R8G8B8A8_UNORM; swapRB = true; noAlpha = true; break;
                    }
                }
            }
            else if ((pfFlags & 0x40) && bits == 32) // RGB, 32 bits a pixel
            {
                format = DXGI_FORMAT_R8G8B8A8_UNORM;
                swapRB = rMask == 0x00FF0000;
                noAlpha = !(pfFlags & 0x1) || aMask == 0;
            }
            bool block = false;
            uint32_t bytes = 0;
            if (format == DXGI_FORMAT_UNKNOWN || !Describe(format, block, bytes) || !width || !height || width > 16384 || height > 16384)
            {
                error = "unsupported DDS format (use BC1/DXT1, BC2/DXT3, BC3/DXT5, BC4, BC5, BC7 or 32-bit RGBA)";
                return false;
            }
            out.format = format;
            out.width = width;
            out.height = height;
            out.levels.clear();
            uint32_t w = width, h = height;
            for (uint32_t m = 0; m < mips && m < 16; m++)
            {
                Image::Level l;
                SetLevel(l, w, h, block, bytes);
                size_t size = size_t(l.rowBytes) * l.rows;
                if (offset + size > file.size())
                    break; // (fewer levels than the header says: use what's there)
                l.data.assign(file.begin() + ptrdiff_t(offset), file.begin() + ptrdiff_t(offset + size));
                if (swapRB || noAlpha)
                    for (size_t i = 0; i + 3 < l.data.size(); i += 4)
                    {
                        if (swapRB) std::swap(l.data[i], l.data[i + 2]);
                        if (noAlpha) l.data[i + 3] = 255;
                    }
                offset += size;
                out.levels.push_back(std::move(l));
                if (w == 1 && h == 1)
                    break;
                w = std::max(1u, w / 2);
                h = std::max(1u, h / 2);
            }
            if (out.levels.empty())
            {
                error = "the file is too short";
                return false;
            }
            return true;
        }

        bool LoadWic(const std::wstring& path, Image& out, std::string& error)
        {
            static thread_local bool comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
            (void)comReady;
            ComPtr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            {
                error = "Windows Imaging Component unavailable";
                return false;
            }
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> conv;
            UINT w = 0, h = 0;
            if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
                FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&conv)) ||
                FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
                FAILED(conv->GetSize(&w, &h)) || !w || !h || w > 16384 || h > 16384)
            {
                error = "couldn't decode the image";
                return false;
            }
            out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            out.width = w;
            out.height = h;
            out.levels.clear();
            Image::Level base;
            SetLevel(base, w, h, false, 4);
            base.data.resize(size_t(base.rowBytes) * h);
            if (FAILED(conv->CopyPixels(nullptr, base.rowBytes, UINT(base.data.size()), base.data.data())))
            {
                error = "couldn't decode the image";
                return false;
            }
            out.levels.push_back(std::move(base));
            // Smaller levels: average 2x2 pixels.
            while (out.levels.back().width > 1 || out.levels.back().height > 1)
            {
                const Image::Level& src = out.levels.back();
                Image::Level l;
                SetLevel(l, std::max(1u, src.width / 2), std::max(1u, src.height / 2), false, 4);
                l.data.resize(size_t(l.rowBytes) * l.rows);
                for (uint32_t y = 0; y < l.height; y++)
                    for (uint32_t x = 0; x < l.width; x++)
                        for (uint32_t c = 0; c < 4; c++)
                        {
                            uint32_t sum = 0;
                            for (uint32_t dy = 0; dy < 2; dy++)
                                for (uint32_t dx = 0; dx < 2; dx++)
                                {
                                    uint32_t sx = std::min(src.width - 1, x * 2 + dx), sy = std::min(src.height - 1, y * 2 + dy);
                                    sum += src.data[size_t(sy) * src.rowBytes + sx * 4 + c];
                                }
                            l.data[size_t(y) * l.rowBytes + x * 4 + c] = uint8_t((sum + 2) / 4);
                        }
                out.levels.push_back(std::move(l));
            }
            return true;
        }
    }

    bool Load(const std::wstring& path, Image& out, std::string& error)
    {
        std::wstring ext = path.substr(path.find_last_of(L'.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
        if (ext == L"dds")
        {
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            if (!in)
            {
                error = "can't open the file";
                return false;
            }
            std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
            return LoadDds(file, out, error);
        }
        return LoadWic(path, out, error);
    }
}
#endif
