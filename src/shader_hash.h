#pragma once
// D3D9 shader bytecode helpers. The hash is plain CRC32 (zlib-compatible) over the whole bytecode,
// identical to RenoDX's, so hashes can be matched against the game's shader cache.
#include <cstddef>
#include <cstdint>

namespace bl2hdr::shader {
// Size in bytes of SM1-3 bytecode from the version token through the END token; 0 if malformed.
size_t BytecodeSize(const uint32_t* tokens, size_t max_bytes = 1u << 20);
uint32_t Crc32(const void* data, size_t size);
// Known BL2 shaders (from GlobalShaderCache-PC-D3D-SM3.bin); nullptr if not a known one.
const char* KnownName(uint32_t crc);
}  // namespace bl2hdr::shader
