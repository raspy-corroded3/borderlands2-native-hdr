#include "shader_hash.h"

#include <array>

namespace bl2hdr::shader {
namespace {
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    t[i] = c;
  }
  return t;
}
constexpr auto kTable = MakeTable();
}  // namespace

size_t BytecodeSize(const uint32_t* tokens, size_t max_bytes) {
  if (!tokens) return 0;
  const size_t max_tokens = max_bytes / 4;
  size_t i = 1;  // skip version token
  while (i < max_tokens) {
    const uint32_t tok = tokens[i];
    if (tok == 0x0000FFFFu) return (i + 1) * 4;      // END
    if ((tok & 0xFFFFu) == 0xFFFEu) {                 // comment block
      i += 1 + ((tok >> 16) & 0x7FFFu);
    } else {
      if (tok & 0x80000000u) return 0;               // not an opcode token: malformed
      i += 1 + ((tok >> 24) & 0x0Fu);                 // SM2+: operand count in bits 24..27
    }
  }
  return 0;
}

uint32_t Crc32(const void* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) crc = (crc >> 8) ^ kTable[(crc ^ p[i]) & 0xFFu];
  return ~crc;
}

const char* KnownName(uint32_t crc) {
  switch (crc) {
    case 0x54ED86A0u: return "tonemap (UberPostProcessBlend)";
    case 0xD8E2DDE0u: return "shading1 (cel edge filter)";
    case 0xB902DF59u: return "shading2 (cel edge filter)";
    case 0x33244F80u: return "video (Bink YUV->RGB)";
    default: return nullptr;
  }
}
}  // namespace bl2hdr::shader
