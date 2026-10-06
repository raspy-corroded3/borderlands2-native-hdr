#pragma once
// Chooses replacement bytecode for known BL2 shaders, per bl2hdr.ini [shaders].
#include <cstddef>
#include <cstdint>

namespace bl2hdr::shader_swap {
struct Replacement {
  const void* bytecode = nullptr;
  size_t size = 0;
  const char* variant = nullptr;  // for logging
};
// Returns a replacement for this original shader CRC, or {} to keep the original.
Replacement For(uint32_t original_crc);
}  // namespace bl2hdr::shader_swap
