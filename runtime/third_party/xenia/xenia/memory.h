// Shim for Xenia's Memory class: only physical address translation is needed
// by the vendored shader interpreter (Vampire Rain runtime).
#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_
#include <cstdint>
namespace xe {
class Memory {
 public:
  explicit Memory(uint8_t* physical_membase) : physical_membase_(physical_membase) {}
  uint8_t* physical_membase() const { return physical_membase_; }
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t address) const {
    return reinterpret_cast<T>(physical_membase_ + (address & 0x1FFFFFFF));
  }
 private:
  uint8_t* physical_membase_;
};
}  // namespace xe
#endif
