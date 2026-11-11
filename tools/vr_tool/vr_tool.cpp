// vr_tool - small helpers for setting up Vampire Rain from your own disc.
//
//   vr_tool extract <image.iso> <output folder>
//       Extracts an Xbox 360 disc image (Redump ISO or plain XISO).
//   vr_tool decrypt <default.xex> <default_dec.xex>
//       Writes an unencrypted, uncompressed copy of the game executable
//       (like XexTool -e u -c u), with XenonRecomp's AES and LZX code.
//   vr_tool icon <default.xex> <icon.ico>
//       Saves the game's own icon (from the executable's title resources) as
//       a Windows icon for the PC build.
//   vr_tool sha1 <file>
//       Prints a file's SHA-1 (to check that the disc is the expected version).
//
// Built together with XenonRecomp by tools/CMakeLists.txt.

#include <xex.h>
#include <aes.hpp>
#include <TinySHA1.hpp>
#include <xdbf_wrapper.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// LZX decompressor from XenonRecomp (XenonUtils/xex_patcher.cpp).
int lzxDecompress(const void* lzxData, size_t lzxLength, void* dst, size_t dstLength, uint32_t windowSize, void* windowData, size_t windowDataLength);

namespace
{
    // ---------------------------------------------------------------------
    // Disc image extraction (XDVDFS)
    // ---------------------------------------------------------------------
    constexpr uint64_t kSector = 2048;
    constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

    struct Disc
    {
        FILE* f = nullptr;
        uint64_t partition = 0; // byte offset of the game partition
        uint64_t files = 0, bytes = 0;

        bool Read(uint64_t offset, void* dst, size_t n)
        {
#ifdef _WIN32
            if (_fseeki64(f, int64_t(offset), SEEK_SET) != 0)
                return false;
#else
            if (fseeko(f, off_t(offset), SEEK_SET) != 0)
                return false;
#endif
            return fread(dst, 1, n, f) == n;
        }
    };

    uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
    uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

    bool CopyFile(Disc& d, uint64_t sector, uint32_t size, const fs::path& out)
    {
        std::ofstream o(out, std::ios::binary);
        if (!o)
        {
            fprintf(stderr, "error: can't create %s\n", out.string().c_str());
            return false;
        }
        std::vector<char> buf(4u << 20);
        uint64_t pos = d.partition + sector * kSector;
        uint32_t left = size;
        while (left)
        {
            uint32_t n = std::min<uint32_t>(left, uint32_t(buf.size()));
            if (!d.Read(pos, buf.data(), n))
            {
                fprintf(stderr, "error: read failed in %s (the image may be truncated)\n", out.string().c_str());
                return false;
            }
            o.write(buf.data(), n);
            pos += n;
            left -= n;
        }
        d.files++;
        d.bytes += size;
        if (d.files % 200 == 0)
            printf("  %llu files, %llu MB...\n", (unsigned long long)d.files, (unsigned long long)(d.bytes >> 20));
        return true;
    }

    bool ExtractDir(Disc& d, uint32_t sector, uint32_t size, const fs::path& out, int depth);

    // A directory is a binary tree of entries: left/right offsets (in 4-byte
    // units from the table start), start sector, size, attributes, name.
    bool WalkTree(Disc& d, const std::vector<uint8_t>& table, uint32_t offset, const fs::path& out, int depth, std::set<uint32_t>& seen)
    {
        if (offset == 0xFFFF)
            return true;
        uint64_t at = uint64_t(offset) * 4;
        if (at + 14 > table.size() || !seen.insert(offset).second)
            return true; // bad or repeated link: ignore
        const uint8_t* e = table.data() + at;
        uint16_t left = Le16(e), right = Le16(e + 2);
        uint32_t start = Le32(e + 4), fileSize = Le32(e + 8);
        uint8_t attr = e[12], nameLen = e[13];
        if (left == 0xFFFF && right == 0xFFFF && start == 0xFFFFFFFF)
            return true; // padding
        if (at + 14 + nameLen > table.size())
            return true;
        std::string name(reinterpret_cast<const char*>(e + 14), nameLen);
        if (!WalkTree(d, table, left, out, depth, seen))
            return false;
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
        {
            fprintf(stderr, "warning: skipping odd entry name '%s'\n", name.c_str());
        }
        else if (attr & 0x10)
        {
            fs::path sub = out / name;
            std::error_code ec;
            fs::create_directories(sub, ec);
            if (fileSize && !ExtractDir(d, start, fileSize, sub, depth + 1))
                return false;
        }
        else if (!CopyFile(d, start, fileSize, out / name))
            return false;
        return WalkTree(d, table, right, out, depth, seen);
    }

    bool ExtractDir(Disc& d, uint32_t sector, uint32_t size, const fs::path& out, int depth)
    {
        if (depth > 64 || size > (64u << 20))
            return true;
        std::vector<uint8_t> table(size);
        if (!d.Read(d.partition + uint64_t(sector) * kSector, table.data(), size))
        {
            fprintf(stderr, "error: can't read a directory (the image may be truncated)\n");
            return false;
        }
        std::set<uint32_t> seen;
        return WalkTree(d, table, 0, out, depth, seen);
    }

    int Extract(const char* iso, const char* outDir)
    {
        Disc d;
        d.f = fopen(iso, "rb");
        if (!d.f)
        {
            fprintf(stderr, "error: can't open %s\n", iso);
            return 1;
        }
        // Where the game partition starts: plain XISO, XGD3, XGD2, XGD1 (Redump).
        const uint64_t partitions[] = { 0, 0x2080000ull, 0xFD90000ull, 0x18300000ull };
        bool found = false;
        uint8_t vd[0x800];
        for (uint64_t p : partitions)
        {
            if (d.Read(p + 32 * kSector, vd, sizeof(vd)) && memcmp(vd, kMagic, 20) == 0 && memcmp(vd + 0x7EC, kMagic, 20) == 0)
            {
                d.partition = p;
                found = true;
                break;
            }
        }
        if (!found)
        {
            fprintf(stderr, "error: %s is not an Xbox 360 disc image (no XDVDFS volume found)\n", iso);
            fclose(d.f);
            return 1;
        }
        uint32_t rootSector = Le32(vd + 20), rootSize = Le32(vd + 24);
        printf("Extracting %s (game partition at 0x%llX) to %s\n", iso, (unsigned long long)d.partition, outDir);
        std::error_code ec;
        fs::create_directories(outDir, ec);
        bool ok = ExtractDir(d, rootSector, rootSize, fs::path(outDir), 0);
        fclose(d.f);
        printf("%s: %llu files, %llu MB\n", ok ? "Done" : "Stopped", (unsigned long long)d.files, (unsigned long long)(d.bytes >> 20));
        return ok ? 0 : 1;
    }

    // ---------------------------------------------------------------------
    // Executable decryption
    // ---------------------------------------------------------------------
    bool ReadAll(const char* path, std::vector<uint8_t>& data)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        data.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    }

    uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

    // The plain image of an XEX (decrypted and decompressed). This mirrors
    // XenonRecomp's Xex2LoadImage up to the point where it starts changing
    // the image for its own use (it rewrites the import thunks), so the
    // result is the executable exactly as the console would load it.
    std::vector<uint8_t> PlainImage(const uint8_t* data, size_t dataSize)
    {
        auto* header = reinterpret_cast<const Xex2Header*>(data);
        auto* security = reinterpret_cast<const Xex2SecurityInfo*>(data + header->securityOffset);
        auto* ffi = reinterpret_cast<const Xex2OptFileFormatInfo*>(getOptHeaderPtr(data, XEX_HEADER_FILE_FORMAT_INFO));
        const uint32_t headerSize = header->headerSize;
        const uint32_t imageSize = security->imageSize;
        if (!ffi || headerSize > dataSize)
            return {};

        // Decrypt the payload (AES-128-CBC with the per-file key, which is
        // itself encrypted with the retail key).
        std::vector<uint8_t> payload(data + headerSize, data + dataSize);
        if (ffi->encryptionType == XEX_ENCRYPTION_NORMAL)
        {
            uint8_t key[16];
            memcpy(key, security->aesKey, 16);
            AES_ctx aes;
            AES_init_ctx_iv(&aes, Xex2RetailKey, AESBlankIV);
            AES_CBC_decrypt_buffer(&aes, key, 16);
            AES_init_ctx_iv(&aes, key, AESBlankIV);
            AES_CBC_decrypt_buffer(&aes, payload.data(), payload.size() & ~size_t(15));
        }

        std::vector<uint8_t> image(imageSize, 0);
        if (ffi->compressionType == XEX_COMPRESSION_NONE)
        {
            memcpy(image.data(), payload.data(), std::min<size_t>(imageSize, payload.size()));
        }
        else if (ffi->compressionType == XEX_COMPRESSION_BASIC)
        {
            // Blocks of (data size, zero size).
            auto* blocks = reinterpret_cast<const Xex2FileBasicCompressionBlock*>(ffi + 1);
            size_t count = ffi->infoSize / sizeof(Xex2FileBasicCompressionInfo) - 1;
            size_t src = 0, dst = 0;
            for (size_t i = 0; i < count; i++)
            {
                uint32_t n = blocks[i].dataSize;
                if (src + n > payload.size() || dst + n > image.size())
                    return {};
                memcpy(image.data() + dst, payload.data() + src, n);
                src += n;
                dst += n + blocks[i].zeroSize;
            }
        }
        else if (ffi->compressionType == XEX_COMPRESSION_NORMAL)
        {
            // A chain of hashed blocks holding LZX chunks; join the chunks,
            // then decompress them in one go.
            auto* normal = reinterpret_cast<const Xex2FileNormalCompressionInfo*>(ffi + 1);
            const Xex2CompressedBlockInfo* block = &normal->firstBlock;
            std::vector<uint8_t> lzx;
            lzx.reserve(payload.size());
            const uint8_t* p = payload.data();
            const uint8_t* end = payload.data() + payload.size();
            while (block->blockSize)
            {
                if (p + block->blockSize > end)
                    return {};
                const uint8_t* next = p + block->blockSize;
                uint8_t digest[20];
                sha1::SHA1 s;
                s.processBytes(p, block->blockSize);
                s.finalize(digest);
                if (memcmp(digest, block->blockHash, 20) != 0)
                {
                    fprintf(stderr, "error: block checksum mismatch - the file is damaged\n");
                    return {};
                }
                const auto* nextBlock = reinterpret_cast<const Xex2CompressedBlockInfo*>(p);
                p += 24;
                for (;;)
                {
                    size_t chunk = (size_t(p[0]) << 8) | p[1];
                    p += 2;
                    if (!chunk)
                        break;
                    lzx.insert(lzx.end(), p, p + chunk);
                    p += chunk;
                }
                p = next;
                block = nextBlock;
            }
            if (lzxDecompress(lzx.data(), lzx.size(), image.data(), imageSize, normal->windowSize, nullptr, 0) != 0)
                return {};
        }
        else
            return {};
        return image;
    }

    int Decrypt(const char* inPath, const char* outPath)
    {
        std::vector<uint8_t> data;
        if (!ReadAll(inPath, data) || data.size() < 0x18 || memcmp(data.data(), "XEX2", 4) != 0)
        {
            fprintf(stderr, "error: %s is not an XEX2 file\n", inPath);
            return 1;
        }
        uint32_t headerSize = Be32(&data[8]), securityOffset = Be32(&data[16]), headerCount = Be32(&data[20]);
        if (headerSize > data.size() || securityOffset + 8 > data.size())
        {
            fprintf(stderr, "error: %s has a damaged header\n", inPath);
            return 1;
        }
        std::vector<uint8_t> image = PlainImage(data.data(), data.size());
        if (image.empty())
        {
            fprintf(stderr, "error: couldn't decrypt/decompress %s\n", inPath);
            return 1;
        }
        // Same header, file format info changed to "not encrypted, not
        // compressed", then the plain image.
        std::vector<uint8_t> out(data.begin(), data.begin() + headerSize);
        bool patched = false;
        for (uint32_t i = 0; i < headerCount; i++)
        {
            size_t e = 24 + size_t(i) * 8;
            if (e + 8 > headerSize)
                break;
            if (Be32(&out[e]) == 0x000003FF)
            {
                uint32_t info = Be32(&out[e + 4]);
                if (info + 8 <= headerSize)
                {
                    out[info + 4] = out[info + 5] = 0; // encryption: none
                    out[info + 6] = out[info + 7] = 0; // compression: none
                    patched = true;
                }
            }
        }
        if (!patched)
        {
            fprintf(stderr, "error: %s has no file format header\n", inPath);
            return 1;
        }
        out.insert(out.end(), image.begin(), image.end());
        std::ofstream o(outPath, std::ios::binary);
        o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        if (!o)
        {
            fprintf(stderr, "error: can't write %s\n", outPath);
            return 1;
        }
        printf("Wrote %s (%zu-byte image)\n", outPath, image.size());
        return 0;
    }

    // The title icon: the executable's XDBF resource (named after the title
    // id) holds a 64x64 PNG as image 0x8000. Windows icons can contain a PNG
    // directly.
    int Icon(const char* inPath, const char* outPath)
    {
        std::vector<uint8_t> data;
        if (!ReadAll(inPath, data) || data.size() < 0x18 || memcmp(data.data(), "XEX2", 4) != 0)
        {
            fprintf(stderr, "error: %s is not an XEX2 file\n", inPath);
            return 1;
        }
        std::vector<uint8_t> image = PlainImage(data.data(), data.size());
        auto* security = reinterpret_cast<const Xex2SecurityInfo*>(data.data() + Be32(&data[16]));
        uint32_t base = security->loadAddress;
        auto* res = static_cast<const uint8_t*>(getOptHeaderPtr(data.data(), XEX_HEADER_RESOURCE_INFO));
        if (image.empty() || !res)
        {
            fprintf(stderr, "error: no resources in %s\n", inPath);
            return 1;
        }
        // { be32 size, then 16-byte entries: name[8], be32 address, be32 size }
        uint32_t count = (Be32(res) - 4) / 16;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* e = res + 4 + i * 16;
            uint32_t addr = Be32(e + 8), size = Be32(e + 12);
            if (addr < base || uint64_t(addr - base) + size > image.size() || size < 4 || memcmp(&image[addr - base], "XDBF", 4) != 0)
                continue;
            XDBFWrapper xdbf(&image[addr - base], size);
            XDBFBlock png = xdbf.GetResource(XDBF_SPA_NAMESPACE_IMAGE, 0x8000);
            if (!png || png.BufferSize < 24 || memcmp(png.pBuffer + 1, "PNG", 3) != 0)
                continue;
            uint32_t w = Be32(png.pBuffer + 16), h = Be32(png.pBuffer + 20);
            std::vector<uint8_t> ico = { 0, 0, 1, 0, 1, 0 }; // ICONDIR: icon, 1 image
            ico.push_back(uint8_t(w >= 256 ? 0 : w));
            ico.push_back(uint8_t(h >= 256 ? 0 : h));
            ico.insert(ico.end(), { 0, 0, 1, 0, 32, 0 }); // colours, reserved, planes 1, 32 bpp
            uint32_t n = uint32_t(png.BufferSize), off = 22;
            for (int b = 0; b < 4; b++) ico.push_back(uint8_t(n >> (8 * b)));
            for (int b = 0; b < 4; b++) ico.push_back(uint8_t(off >> (8 * b)));
            ico.insert(ico.end(), png.pBuffer, png.pBuffer + png.BufferSize);
            std::ofstream o(outPath, std::ios::binary);
            o.write(reinterpret_cast<const char*>(ico.data()), std::streamsize(ico.size()));
            if (!o)
            {
                fprintf(stderr, "error: can't write %s\n", outPath);
                return 1;
            }
            printf("Wrote %s (%ux%u)\n", outPath, w, h);
            return 0;
        }
        fprintf(stderr, "error: no title icon found in %s\n", inPath);
        return 1;
    }

    int Sha1(const char* path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            fprintf(stderr, "error: can't open %s\n", path);
            return 1;
        }
        sha1::SHA1 s;
        std::vector<char> buf(1 << 20);
        while (in)
        {
            in.read(buf.data(), std::streamsize(buf.size()));
            if (in.gcount() > 0)
                s.processBytes(buf.data(), size_t(in.gcount()));
        }
        sha1::SHA1::digest32_t digest;
        s.finalize(digest);
        printf("%08x%08x%08x%08x%08x\n", digest[0], digest[1], digest[2], digest[3], digest[4]);
        return 0;
    }
}

int main(int argc, char** argv)
{
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "extract" && argc == 4)
        return Extract(argv[2], argv[3]);
    if (cmd == "decrypt" && argc == 4)
        return Decrypt(argv[2], argv[3]);
    if (cmd == "icon" && argc == 4)
        return Icon(argv[2], argv[3]);
    if (cmd == "sha1" && argc == 3)
        return Sha1(argv[2]);
    fprintf(stderr,
            "usage:\n"
            "  vr_tool extract <image.iso> <output folder>\n"
            "  vr_tool decrypt <default.xex> <default_dec.xex>\n"
            "  vr_tool icon <default.xex> <icon.ico>\n"
            "  vr_tool sha1 <file>\n");
    return 2;
}
