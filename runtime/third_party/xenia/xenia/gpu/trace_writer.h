// Shim: trace recording is not used by the Vampire Rain runtime.
#ifndef XENIA_GPU_TRACE_WRITER_H_
#define XENIA_GPU_TRACE_WRITER_H_
#include <cstdint>
namespace xe {
namespace gpu {
class TraceWriter {
 public:
  void WriteMemoryRead(uint32_t, uint32_t) {}
  void WriteMemoryReadCached(uint32_t, uint32_t) {}
  void WriteMemoryReadCachedNop(uint32_t, uint32_t) {}
  void WriteMemoryWrite(uint32_t, uint32_t) {}
};
}  // namespace gpu
}  // namespace xe
#endif
