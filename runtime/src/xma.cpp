// XMA decoder. The context layout, register numbers and the packet/frame
// walking follow Xenia's XMA emulation (src/xenia/apu/xma_context.cc,
// BSD license - see third_party/xenia/LICENSE); decoding itself uses the XMA
// frame decoder in Xenia's FFmpeg fork (LGPL), built from its source by
// CMakeLists.txt.
//
// XMA data is a series of 2048-byte packets. Each packet has a 32-bit header
// (frame count, bit offset of its first frame, metadata, packets to skip) and
// a stream of variable-length frames (15-bit length prefix), which may span
// into the next packet. Each frame decodes to 512 samples per channel.
#include "xma.h"
#include "kernel.h"
#include "memory.h"
#include "thread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#ifdef VR_HAVE_FFMPEG
extern "C"
{
#include "libavcodec/avcodec.h"
}
#endif

namespace xma
{
    namespace
    {
        constexpr uint32_t kContextCount = 320;
        constexpr uint32_t kBytesPerPacket = 2048;
        constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
        constexpr uint32_t kBitsPerHeader = 33;
        constexpr uint32_t kSamplesPerFrame = 512;
        constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * 2;
        constexpr uint32_t kBytesPerSubframeChannel = 128 * 2;
        constexpr uint32_t kMaxFrameLength = 0x7FFF;

        // Registers (index = byte offset / 4).
        constexpr uint32_t kRegContextArray = 0x600;
        constexpr uint32_t kRegKick = 0x650, kRegLock = 0x690, kRegClear = 0x6A0;

        uint8_t* Phys(uint32_t p) { return g_base + mem::PHYSICAL_BASE + (p & 0x1FFFFFFF); }

        // ---- The 64-byte context (16 big-endian words in guest memory) ----
        struct Field { uint8_t word, shift, bits; };
        constexpr Field kIn0Packets{ 0, 0, 12 }, kLoopCount{ 0, 12, 8 }, kIn0Valid{ 0, 20, 1 }, kIn1Valid{ 0, 21, 1 },
            kOutBlocks{ 0, 22, 5 }, kOutWrite{ 0, 27, 5 };
        constexpr Field kIn1Packets{ 1, 0, 12 }, kSampleRate{ 1, 27, 2 }, kStereo{ 1, 29, 1 }, kOutValid{ 1, 31, 1 };
        constexpr Field kReadOffset{ 2, 0, 26 }, kLoopStart{ 3, 0, 26 }, kLoopEnd{ 4, 0, 26 }, kCurrentBuffer{ 4, 31, 1 };
        constexpr Field kOutRead{ 9, 0, 5 };
        constexpr uint32_t kIn0Ptr = 5, kIn1Ptr = 6, kOutPtr = 7;

        struct ContextData
        {
            uint32_t w[16];
            void Load(const uint8_t* p)
            {
                for (int i = 0; i < 16; i++)
                {
                    uint32_t v;
                    memcpy(&v, p + i * 4, 4);
                    w[i] = __builtin_bswap32(v);
                }
            }
            void Store(uint8_t* p) const
            {
                for (int i = 0; i < 16; i++)
                {
                    uint32_t v = __builtin_bswap32(w[i]);
                    memcpy(p + i * 4, &v, 4);
                }
            }
            uint32_t operator[](Field f) const { return (w[f.word] >> f.shift) & ((1u << f.bits) - 1u); }
            void Set(Field f, uint32_t v)
            {
                uint32_t mask = ((1u << f.bits) - 1u) << f.shift;
                w[f.word] = (w[f.word] & ~mask) | ((v << f.shift) & mask);
            }
        };

        // ---- Big-endian bit reader over a byte buffer ----
        struct BitStream
        {
            const uint8_t* buf;
            size_t sizeBits, offset = 0;
            BitStream(const uint8_t* b, size_t bits) : buf(b), sizeBits(bits) {}
            void SetOffset(size_t o) { offset = std::min(o, sizeBits); }
            size_t Remaining() const { return sizeBits - offset; }
            uint64_t Peek(size_t n) const
            {
                size_t byte = offset >> 3, rel = offset & 7;
                uint64_t v;
                memcpy(&v, buf + byte, 8);
                v = __builtin_bswap64(v);
                v >>= 64 - (rel + n);
                return v & ((1ull << n) - 1);
            }
            uint64_t Read(size_t n) { uint64_t v = Peek(n); SetOffset(offset + n); return v; }
            void Advance(size_t n) { SetOffset(offset + n); }
            // Copies `n` bits to `dest`, keeping the bit alignment within the
            // first byte. Returns that alignment.
            size_t Copy(uint8_t* dest, size_t n)
            {
                size_t rel = offset & 7, left = n, out = 0, byte = offset >> 3;
                if (rel)
                {
                    uint64_t bits = Peek(8 - rel);
                    dest[out] = uint8_t((dest[out] & ~((1u << rel) - 1u) & 0xFF) | uint8_t(bits));
                    left -= 8 - rel;
                    Advance(8 - rel);
                    out++;
                }
                if (left >= 8)
                {
                    memcpy(dest + out, buf + byte + out, left / 8);
                    out += left / 8;
                    Advance((left / 8) * 8);
                    left -= (left / 8) * 8;
                }
                if (left)
                {
                    uint64_t bits = Peek(left) << (8 - left);
                    dest[out] = uint8_t((dest[out] & ((1u << left) - 1u)) | uint8_t(bits));
                    Advance(left);
                }
                return rel;
            }
        };

        uint32_t PacketFrameOffset(const uint8_t* p) { return (uint32_t((p[0] & 3) << 13) | (p[1] << 5) | (p[2] >> 3)) + 32; }
        uint32_t PacketSkipCount(const uint8_t* p) { return p[3]; }

        int SampleRate(uint32_t id)
        {
            static const int kRates[4] = { 24000, 32000, 44100, 48000 };
            return kRates[id & 3];
        }

        int PacketNumber(size_t sizeBytes, size_t bitOffset)
        {
            if (bitOffset >= sizeBytes * 8)
                return -1;
            return int((bitOffset >> 3) / kBytesPerPacket);
        }

        // Number of frames that start in a packet, and whether the last one
        // continues into the next packet.
        std::pair<int, bool> PacketFrameCount(const uint8_t* packet)
        {
            uint32_t first = PacketFrameOffset(packet);
            if (first > kBitsPerPacket - kBitsPerHeader)
                return { 0, false };
            BitStream s(packet, kBitsPerPacket);
            s.SetOffset(first);
            int count = 0;
            for (;;)
            {
                count++;
                if (s.Remaining() < 15)
                    return { count, true };
                uint64_t len = s.Read(15);
                if (len - 15 > s.Remaining() || len == 0x7FFF)
                    return { count, true };
                s.Advance(len - 16);
                if (s.Read(1) == 0)
                    return { count, false };
            }
        }

        bool ValidFrameOffset(const uint8_t* block, size_t sizeBytes, size_t bitOffset)
        {
            int pn = PacketNumber(sizeBytes, bitOffset);
            if (pn < 0)
                return false;
            const uint8_t* packet = block + pn * kBytesPerPacket;
            size_t rel = bitOffset % kBitsPerPacket;
            uint32_t first = PacketFrameOffset(packet);
            if (first > kBitsPerPacket)
                return false;
            BitStream s(packet, kBitsPerPacket);
            s.SetOffset(first);
            for (;;)
            {
                if (s.offset == rel)
                    return true;
                if (s.Remaining() < 15)
                    return false;
                uint64_t len = s.Read(15);
                if (len - 15 > s.Remaining() || len == 0x7FFF)
                    return false;
                s.Advance(len - 16);
                if (s.Read(1) == 0)
                    return false;
            }
        }

        // (packet index, frame index within it) for a frame's bit offset.
        std::pair<int, int> FrameNumber(const uint8_t* block, size_t sizeBytes, size_t bitOffset)
        {
            int pn = PacketNumber(sizeBytes, bitOffset);
            if (pn < 0 || (pn + 1) * kBytesPerPacket > sizeBytes)
                return { pn, -2 };
            if (bitOffset == 0)
                return { pn, -1 };
            const uint8_t* packet = block + pn * kBytesPerPacket;
            BitStream s(block, sizeBytes * 8);
            s.SetOffset(pn * kBitsPerPacket + PacketFrameOffset(packet));
            int idx = 0;
            for (;;)
            {
                if (s.Remaining() < 15 || s.offset == bitOffset)
                    break;
                uint64_t len = s.Read(15);
                if (len - 15 > s.Remaining() || len == 0x7FFF)
                    break;
                s.Advance(len - 16);
                if (s.Read(1) == 0)
                    break;
                idx++;
            }
            return { pn, idx };
        }

        // Bit offset of the frame after the one at `bitOffset`, or 0 if it
        // isn't in the same packet.
        size_t NextFrame(const uint8_t* block, size_t sizeBytes, size_t bitOffset)
        {
            int pn = PacketNumber(sizeBytes, bitOffset);
            BitStream s(block, sizeBytes * 8);
            s.SetOffset(bitOffset);
            if (s.Remaining() < 15)
                return 0;
            uint64_t len = s.Read(15);
            if (len - 15 > s.Remaining() || len >= kMaxFrameLength)
                return 0;
            s.Advance(len - 16);
            if (s.Read(1) == 0)
                return 0;
            bitOffset += len;
            if (pn < PacketNumber(sizeBytes, bitOffset))
                return 0;
            return bitOffset;
        }

        struct Context
        {
            uint32_t id = 0, guest = 0;
            std::mutex lock;
            bool allocated = false;
            std::atomic<bool> enabled{ false };
#ifdef VR_HAVE_FFMPEG
            const AVCodec* codec = nullptr;
            AVCodecContext* av = nullptr;
            AVPacket* avPacket = nullptr;
            AVFrame* frame = nullptr;
#endif
            uint32_t packetsSkip = 0;
            uint32_t splitLen = 0, splitLenPartial = 0;
            uint8_t splitPaddingStart = 0;
            std::array<uint8_t, 1 + 4096 + 8> frameBuf{};
            std::array<uint8_t, kBytesPerFrameChannel * 2> pcm{};

            void Reset()
            {
                packetsSkip = 0;
                splitLen = splitLenPartial = 0;
                splitPaddingStart = 0;
            }

            bool PrepareDecoder(uint32_t rateId, bool stereo)
            {
#ifdef VR_HAVE_FFMPEG
                int rate = SampleRate(rateId), channels = stereo ? 2 : 1;
                if (av && av->sample_rate == rate && av->channels == channels && avcodec_is_open(av))
                    return true;
                if (av)
                    avcodec_free_context(&av);
                av = avcodec_alloc_context3(codec);
                if (!av)
                    return false;
                av->sample_rate = rate;
                av->channels = channels;
                if (avcodec_open2(av, codec, nullptr) < 0)
                {
                    LOG("[xma] context %u: couldn't open the decoder (%d Hz, %d ch)", id, rate, channels);
                    avcodec_free_context(&av);
                    return false;
                }
                return true;
#else
                (void)rateId; (void)stereo;
                return false;
#endif
            }

            // Float planar -> big-endian 16-bit, interleaved when stereo.
            void ConvertFrame(const float* const* samples, bool stereo)
            {
                auto* out = reinterpret_cast<int16_t*>(pcm.data());
                uint32_t o = 0;
                for (uint32_t i = 0; i < kSamplesPerFrame; i++)
                    for (uint32_t c = 0; c <= uint32_t(stereo); c++)
                    {
                        float f = std::clamp(samples[c][i], -1.0f, 1.0f) * 32767.0f;
                        out[o++] = int16_t(__builtin_bswap16(uint16_t(int16_t(f))));
                    }
            }

            static void SwapInputBuffer(ContextData& d)
            {
                d.Set(d[kCurrentBuffer] ? kIn1Valid : kIn0Valid, 0);
                d.Set(kCurrentBuffer, d[kCurrentBuffer] ^ 1);
                d.Set(kReadOffset, 0);
            }

            static bool TrySetupNextLoop(ContextData& d, bool ignoreOffset)
            {
                if (d[kLoopCount] > 0 && d[kLoopStart] < d[kLoopEnd] && (ignoreOffset || d[kReadOffset] >= d[kLoopEnd]))
                {
                    d.Set(kReadOffset, d[kLoopStart]);
                    if (d[kLoopCount] < 255)
                        d.Set(kLoopCount, d[kLoopCount] - 1);
                    return true;
                }
                return false;
            }

            void Decode(ContextData& d);
        };

        Context g_contexts[kContextCount];
        uint32_t g_arrayGuest = 0;
        std::mutex g_allocMutex;
        std::mutex g_workMutex;
        std::condition_variable g_workCv;
        bool g_work = false;
        bool g_available = false;
        std::atomic<uint64_t> g_framesDecoded{ 0 };

        void Context::Decode(ContextData& d)
        {
#ifdef VR_HAVE_FFMPEG
            if (!d[kOutValid])
                return;
            if (!d[kIn0Valid] && !d[kIn1Valid])
            {
                d.Set(kOutValid, 0);
                return;
            }
            uint8_t* in0 = d[kIn0Valid] ? Phys(d.w[kIn0Ptr]) : nullptr;
            uint8_t* in1 = d[kIn1Valid] ? Phys(d.w[kIn1Ptr]) : nullptr;
            uint8_t* input = d[kCurrentBuffer] ? in1 : in0;
            size_t inputSize = size_t(d[kCurrentBuffer] ? d[kIn1Packets] : d[kIn0Packets]) * kBytesPerPacket;
            int inputPackets = int(inputSize / kBytesPerPacket);
            if (!input || !inputSize)
            {
                SwapInputBuffer(d);
                return;
            }

            // Output ring buffer, in 256-byte blocks.
            uint8_t* output = Phys(d.w[kOutPtr]);
            uint32_t capacity = d[kOutBlocks] * kBytesPerSubframeChannel;
            uint32_t readOff = d[kOutRead] * kBytesPerSubframeChannel;
            uint32_t writeOff = d[kOutWrite] * kBytesPerSubframeChannel;
            if (!capacity)
                return;
            auto writable = [&]() -> uint32_t {
                // Read == write means empty (the game marks it valid to fill).
                return readOff > writeOff ? readOff - writeOff : capacity - writeOff + readOff;
            };
            bool stereo = d[kStereo] != 0;
            uint32_t frameBytes = kBytesPerFrameChannel << uint32_t(stereo);
            uint32_t remaining = writable();
            remaining -= remaining % frameBytes;

            bool reuseInput = false;
            // Moves on `count` packets; false if that ran off the end of the buffer.
            auto skipPackets = [&](int& packetIdx) -> bool {
                while (packetsSkip > 0)
                {
                    packetsSkip--;
                    packetIdx++;
                    if (packetIdx >= inputPackets)
                    {
                        if (!reuseInput)
                            reuseInput = TrySetupNextLoop(d, true);
                        if (!reuseInput)
                            SwapInputBuffer(d);
                        return false;
                    }
                }
                return true;
            };

            while (remaining > 0)
            {
                if (!d[kIn0Valid] && !d[kIn1Valid])
                    break;
                reuseInput = TrySetupNextLoop(d, false);

                int packetIdx, frameIdx, frameCount;
                bool lastSplit;
                const uint8_t* packet;

                if (packetsSkip > 0)
                {
                    packetIdx = PacketNumber(inputSize, d[kReadOffset]);
                    if (!skipPackets(packetIdx))
                        return;
                    d.Set(kReadOffset, uint32_t(packetIdx) * kBitsPerPacket);
                }

                if (splitLen)
                {
                    // Finish a frame that started in the previous packet.
                    packetIdx = PacketNumber(inputSize, d[kReadOffset]);
                    if (packetIdx < 0)
                        return;
                    packet = input + packetIdx * kBytesPerPacket;
                    std::tie(frameCount, lastSplit) = PacketFrameCount(packet);
                    frameIdx = -1;
                    BitStream s(input, size_t(packetIdx + 1) * kBitsPerPacket);
                    s.SetOffset(size_t(packetIdx) * kBitsPerPacket + 32);
                    if (splitLen > kMaxFrameLength)
                    {
                        // The length itself was split: read the rest of it.
                        size_t at = s.offset;
                        s.Copy(frameBuf.data() + 1 + ((splitLenPartial + splitPaddingStart) / 8), 15 - splitLenPartial);
                        s.SetOffset(at);
                        BitStream len(frameBuf.data() + 1, 15 + splitPaddingStart);
                        len.Advance(splitPaddingStart);
                        splitLen = uint32_t(len.Read(15));
                    }
                    if (splitLen < splitLenPartial || splitLen > kMaxFrameLength)
                    {
                        Reset();
                        SwapInputBuffer(d);
                        return;
                    }
                    s.Copy(frameBuf.data() + 1 + ((splitLenPartial + splitPaddingStart) / 8), splitLen - splitLenPartial);
                }
                else
                {
                    if (d[kReadOffset] % kBitsPerPacket == 0)
                    {
                        int pn = PacketNumber(inputSize, d[kReadOffset]);
                        if (pn < 0)
                            return;
                        d.Set(kReadOffset, PacketFrameOffset(input + kBytesPerPacket * pn) + d[kReadOffset]);
                    }
                    if (!ValidFrameOffset(input, inputSize, d[kReadOffset]))
                    {
                        SwapInputBuffer(d);
                        return;
                    }
                    std::tie(packetIdx, frameIdx) = FrameNumber(input, inputSize, d[kReadOffset]);
                    if (packetIdx < 0 || frameIdx < 0)
                    {
                        SwapInputBuffer(d);
                        return;
                    }
                    packet = input + packetIdx * kBytesPerPacket;
                    std::tie(frameCount, lastSplit) = PacketFrameCount(packet);
                    if (!PrepareDecoder(d[kSampleRate], stereo))
                    {
                        d.Set(kOutValid, 0);
                        return;
                    }
                    bool frameIsSplit = lastSplit && frameIdx >= frameCount - 1;

                    BitStream s(input, size_t(packetIdx + 1) * kBitsPerPacket);
                    s.SetOffset(d[kReadOffset]);
                    splitLenPartial = uint32_t(s.Remaining());
                    splitLen = splitLenPartial >= 15 ? uint32_t(s.Peek(15)) : kMaxFrameLength + 1;
                    frameBuf.fill(0);
                    splitPaddingStart = uint8_t(s.Copy(frameBuf.data() + 1, std::min(splitLen, splitLenPartial)));

                    if (frameIsSplit)
                    {
                        // The rest is in a later packet.
                        packetsSkip = PacketSkipCount(packet) + 1;
                        if (!skipPackets(packetIdx))
                            return;
                        d.Set(kReadOffset, uint32_t(packetIdx) * kBitsPerPacket);
                        continue;
                    }
                }

                // Decode the frame: one header byte (padding before/after
                // the frame's bits), then the bits.
                uint32_t bits = splitPaddingStart + splitLen;
                int size = int(1 + bits / 8 + ((bits % 8) ? 1 : 0));
                avPacket->data = frameBuf.data();
                avPacket->size = size;
                uint32_t paddingEnd = uint32_t(size) * 8 - (8 + bits);
                frameBuf[0] = uint8_t(((splitPaddingStart & 7) << 5) | ((paddingEnd & 7) << 2));
                splitLen = splitLenPartial = 0;
                splitPaddingStart = 0;

                if (avcodec_send_packet(av, avPacket) < 0 || avcodec_receive_frame(av, frame) < 0)
                {
                    static int logged = 0;
                    if (logged++ < 20)
                        LOG("[xma] context %u: frame failed to decode", id);
                    Reset();
                    SwapInputBuffer(d);
                    return;
                }
                if (av->sample_fmt != AV_SAMPLE_FMT_FLTP || frame->nb_samples < int(kSamplesPerFrame))
                {
                    Reset();
                    SwapInputBuffer(d);
                    return;
                }
                const float* planes[2] = { reinterpret_cast<const float*>(frame->data[0]),
                                           reinterpret_cast<const float*>(frame->data[stereo ? 1 : 0]) };
                ConvertFrame(planes, stereo);

                // Into the ring buffer.
                for (uint32_t done = 0; done < frameBytes;)
                {
                    uint32_t n = std::min(frameBytes - done, capacity - writeOff);
                    memcpy(output + writeOff, pcm.data() + done, n);
                    done += n;
                    writeOff = (writeOff + n) % capacity;
                }
                remaining -= frameBytes;
                d.Set(kOutWrite, writeOff / 256);
                g_framesDecoded++;

                // Next frame.
                size_t offset = NextFrame(input, inputSize, d[kReadOffset]);
                if (frameIdx + 1 >= frameCount)
                {
                    packetsSkip = PacketSkipCount(packet) + 1;
                    if (!skipPackets(packetIdx))
                        return;
                    packet = input + packetIdx * kBytesPerPacket;
                    offset = PacketFrameOffset(packet) + size_t(packetIdx) * kBitsPerPacket;
                }
                if (offset == 0 || frameIdx == -1)
                {
                    if (packetIdx >= inputPackets)
                    {
                        if (!reuseInput)
                            reuseInput = TrySetupNextLoop(d, true);
                        if (!reuseInput)
                            SwapInputBuffer(d);
                        break;
                    }
                    offset = PacketFrameOffset(packet) + size_t(packetIdx) * kBitsPerPacket;
                }
                if (offset <= d[kReadOffset] && !reuseInput)
                {
                    // No progress - corrupt data; drop the buffer.
                    SwapInputBuffer(d);
                    break;
                }
                d.Set(kReadOffset, uint32_t(offset));
            }
            if (writeOff == readOff)
                d.Set(kOutValid, 0);
#else
            (void)d;
#endif
        }

        void Work(Context& c)
        {
            std::lock_guard lock(c.lock);
            if (!c.allocated || !c.enabled.exchange(false))
                return;
            uint8_t* p = Phys(c.guest);
            ContextData before, d;
            before.Load(p);
            d = before;
            c.Decode(d);
            // The game may have changed the context while it was decoded
            // (marking a buffer valid, moving the output read position):
            // keep its changes, apply only what the decoder changed.
            ContextData now;
            now.Load(p);
            for (Field f : { kIn0Valid, kIn1Valid, kOutValid })
                if (before[f] && !d[f])
                    now.Set(f, 0);
            for (Field f : { kReadOffset, kCurrentBuffer, kOutWrite, kLoopCount })
                if (d[f] != before[f])
                    now.Set(f, d[f]);
            now.Store(p);
        }

        void WorkerThread()
        {
            ProfileRegisterCurrentThread("xma decoder");
            for (;;)
            {
                {
                    std::unique_lock lock(g_workMutex);
                    g_workCv.wait_for(lock, std::chrono::milliseconds(4), [] { return g_work; });
                    g_work = false;
                }
                for (auto& c : g_contexts)
                    if (c.enabled.load(std::memory_order_relaxed))
                        Work(c);
            }
        }

        void SetRegister(uint32_t index, uint32_t value)
        {
            // Registers are little-endian; reads by the game come straight
            // from this memory.
            memcpy(g_base + mem::XMA_MMIO_BASE + index * 4, &value, 4);
        }
    }

    bool Available() { return g_available; }

    void Init()
    {
        g_arrayGuest = mem::g_physicalHeap.Alloc(kContextCount * 64, 256);
        if (!g_arrayGuest)
        {
            LOG("[xma] couldn't allocate the context array");
            return;
        }
        memset(g_base + g_arrayGuest, 0, kContextCount * 64);
        SetRegister(kRegContextArray, g_arrayGuest & 0x1FFFFFFF);
        SetRegister(0x607, 1); // next context index
#ifdef VR_HAVE_FFMPEG
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
        if (!codec)
        {
            LOG("[xma] FFmpeg has no XMA frame decoder - no game audio");
            return;
        }
        for (uint32_t i = 0; i < kContextCount; i++)
        {
            Context& c = g_contexts[i];
            c.id = i;
            c.guest = g_arrayGuest + i * 64;
            c.codec = codec;
            c.avPacket = av_packet_alloc();
            c.frame = av_frame_alloc();
        }
        g_available = true;
        StartHostThread(WorkerThread, 1 << 20);
        LOG("[xma] decoder ready (%u contexts at %08X)", kContextCount, g_arrayGuest);
#else
        for (uint32_t i = 0; i < kContextCount; i++)
        {
            g_contexts[i].id = i;
            g_contexts[i].guest = g_arrayGuest + i * 64;
        }
        LOG("[xma] built without FFmpeg - no game audio");
#endif
    }

    uint32_t AllocateContext()
    {
        std::lock_guard lock(g_allocMutex);
        for (auto& c : g_contexts)
        {
            std::lock_guard cl(c.lock);
            if (!c.allocated && c.guest)
            {
                c.allocated = true;
                c.enabled = false;
                c.Reset();
                memset(g_base + c.guest, 0, 64);
                return c.guest;
            }
        }
        return 0;
    }

    void ReleaseContext(uint32_t guestAddress)
    {
        if (guestAddress < g_arrayGuest || guestAddress >= g_arrayGuest + kContextCount * 64)
            return;
        Context& c = g_contexts[(guestAddress - g_arrayGuest) / 64];
        std::lock_guard cl(c.lock);
        c.allocated = false;
        c.enabled = false;
        c.Reset();
        memset(g_base + c.guest, 0, 64);
    }

    void RegisterWrite(uint32_t address, uint32_t value)
    {
        uint32_t r = (address & 0xFFFF) / 4;
        SetRegister(r, value);
        auto forEach = [&](uint32_t base, auto&& fn) {
            uint32_t first = (r - base) * 32;
            for (uint32_t i = 0; value && i < 32; i++, value >>= 1)
                if ((value & 1) && first + i < kContextCount)
                    fn(g_contexts[first + i]);
        };
        if (r >= kRegKick && r < kRegKick + 10)
        {
            forEach(kRegKick, [](Context& c) { c.enabled = true; });
            {
                std::lock_guard lock(g_workMutex);
                g_work = true;
            }
            g_workCv.notify_one();
        }
        else if (r >= kRegLock && r < kRegLock + 10)
            forEach(kRegLock, [](Context& c) { c.enabled = false; });
        else if (r >= kRegClear && r < kRegClear + 10)
            forEach(kRegClear, [](Context& c) {
                std::lock_guard cl(c.lock);
                uint8_t* p = Phys(c.guest);
                ContextData d;
                d.Load(p);
                d.Set(kIn0Valid, 0);
                d.Set(kIn1Valid, 0);
                d.Set(kOutValid, 0);
                d.Set(kOutRead, 0);
                d.Set(kOutWrite, 0);
                d.Store(p);
                c.Reset();
            });
    }
}

// Called by the recompiled code for stores the recompiler marked as register
// writes (see guest_checks.h).
void XmaMmioStore(uint32_t address, uint32_t value)
{
    xma::RegisterWrite(address, value);
}
