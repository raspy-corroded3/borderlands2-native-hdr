#include "shader_swap.h"

#include <windows.h>  // BYTE, used by the fxc-generated headers

#include "config.h"
#include "tonemap_replica.h"
#include "tonemap_hdr.h"
#include "tonemap_tint.h"
#include "video_clamped.h"

namespace bl2hdr::shader_swap {
namespace {
constexpr uint32_t kTonemapCrc = 0x54ED86A0u;
constexpr uint32_t kVideoCrc = 0x33244F80u;  // Bink YUV -> RGB
}

Replacement For(uint32_t original_crc) {
  if (original_crc == kTonemapCrc) {
    switch (config::Get().tonemap) {
      case config::TonemapVariant::kReplica: return {g_tonemap_replica, sizeof(g_tonemap_replica), "replica"};
      case config::TonemapVariant::kTint: return {g_tonemap_tint, sizeof(g_tonemap_tint), "tint"};
      case config::TonemapVariant::kHdr: return {g_tonemap_hdr, sizeof(g_tonemap_hdr), "hdr"};
      case config::TonemapVariant::kOriginal: break;
    }
  }
  // Movies: the same shader with its output clamped to 0..1, as the game's 8-bit back buffer did.
  if (original_crc == kVideoCrc) return {g_video_clamped, sizeof(g_video_clamped), "video clamped"};
  return {};
}
}  // namespace bl2hdr::shader_swap
