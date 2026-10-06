#include "shader_swap.h"

#include <windows.h>  // BYTE, used by the fxc-generated headers

#include "config.h"
#include "tonemap_replica.h"
#include "tonemap_hdr.h"
#include "tonemap_tint.h"

namespace bl2hdr::shader_swap {
namespace {
constexpr uint32_t kTonemapCrc = 0x54ED86A0u;
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
  return {};
}
}  // namespace bl2hdr::shader_swap
