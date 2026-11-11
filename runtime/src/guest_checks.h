// Force-included into the generated code (see CMakeLists.txt).
//
// Indirect calls (function pointers, virtual calls) look up the target in the
// recompiler's function table. A target the recompiler never found as a
// function has no entry; instead of jumping to address 0, report it. The fix
// is usually to add the function to `functions` in vampirerain.toml.
#pragma once

#include <cstdint>

[[noreturn]] void ReportBadIndirectCall(uint32_t target, uint32_t returnAddress);

#define PPC_CALL_INDIRECT_FUNC(x)                                                              \
    do                                                                                         \
    {                                                                                          \
        uint32_t target_ = uint32_t(x);                                                        \
        PPCFunc* fn_ = (target_ - uint32_t(PPC_CODE_BASE) < uint32_t(PPC_CODE_SIZE))           \
                           ? PPC_LOOKUP_FUNC(base, target_) : nullptr;                         \
        if (!fn_) [[unlikely]]                                                                 \
            ReportBadIndirectCall(target_, uint32_t(ctx.lr));                                  \
        fn_(ctx, base);                                                                        \
    } while (0)

// Stores the recompiler identified as hardware register writes (a store
// followed by eieio). Writes to the XMA audio decoder's registers start and
// stop decoding, so they are passed on; everything else is a plain store.
void XmaMmioStore(uint32_t address, uint32_t value);

#define PPC_MM_STORE_U32(x, y)                                                                 \
    do                                                                                         \
    {                                                                                          \
        uint32_t mmAddr_ = uint32_t(x), mmValue_ = uint32_t(y);                                \
        PPC_STORE_U32(mmAddr_, mmValue_);                                                      \
        if ((mmAddr_ & 0xFFFF0000u) == 0x7FEA0000u)                                            \
            XmaMmioStore(mmAddr_, __builtin_bswap32(mmValue_));                                \
    } while (0)
