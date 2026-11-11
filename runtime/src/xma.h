// XMA audio decoder (the Xbox 360's hardware decoder for its compressed audio
// format). The game's audio library fills 64-byte context structures in
// memory and starts them by writing bit masks to the decoder's registers at
// 0x7FEA0000; a worker thread decodes into the PCM buffers the contexts name.
#pragma once
#include <cstdint>

namespace xma
{
    void Init();
    // XMACreateContext / XMAReleaseContext.
    uint32_t AllocateContext(); // guest address, 0 if none are free
    void ReleaseContext(uint32_t guestAddress);
    // A store to the register block (address in 0x7FEA0000-0x7FEAFFFF). The
    // value is the register's (little-endian) value.
    void RegisterWrite(uint32_t address, uint32_t value);
    // Whether decoding is available (built with FFmpeg).
    bool Available();
}
