// Shim for the parts of Xenia's xenia/base/memory.h used by the vendored GPU
// files (Vampire Rain runtime).
#ifndef XENIA_BASE_MEMORY_H_
#define XENIA_BASE_MEMORY_H_
#include <cstdint>
#include <cstring>
#include <type_traits>
namespace xe {
typedef uint32_t fourcc_t;
inline constexpr fourcc_t make_fourcc(char a, char b, char c, char d) {
  return fourcc_t((uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
                  (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d)));
}
inline constexpr fourcc_t make_fourcc(const char (&s)[5]) {
  return make_fourcc(s[0], s[1], s[2], s[3]);
}
namespace memory {
template <typename T, typename F>
inline T Reinterpret(F from) {
  static_assert(sizeof(T) == sizeof(F));
  T to;
  std::memcpy(&to, &from, sizeof(T));
  return to;
}
}  // namespace memory
}  // namespace xe
#endif
