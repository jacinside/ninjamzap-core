#pragma once
#include <cstdint>

namespace AbNinjam {
namespace Common {

// Lee 4 bytes como uint32 en little-endian
inline uint32_t readUInt32LE(const uint8_t* ptr) {
  return (uint32_t(ptr[0]) |
          (uint32_t(ptr[1]) << 8) |
          (uint32_t(ptr[2]) << 16) |
          (uint32_t(ptr[3]) << 24));
}

} // namespace ab
}

