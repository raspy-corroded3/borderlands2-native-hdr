// bl2hdr — proxy d3d9.dll for Borderlands 2 (native HDR project, Option B).
// DllMain only sets up logging and config; the real d3d9 is loaded lazily on the first export call
// (never call LoadLibrary under the loader lock).
//
// Only Borderlands2.exe gets the full proxy. Any other process that loads this DLL from the game
// folder (notably the 2K Launcher.exe, a WPF app that uses D3D9) runs in pass-through mode: it is
// forwarded to the system d3d9.dll with no hooks and no 9on12, and logs to its own file.
#include <windows.h>

#include <string>

#include "config.h"
#include "log.h"
#include "process.h"
#include "ue3.h"
#include "vtable_index.h"

#ifndef BL2HDR_VERSION
#define BL2HDR_VERSION "1.0.0-beta.1"
#endif

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(module);
      wchar_t exe[MAX_PATH], self[MAX_PATH];
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      GetModuleFileNameW(module, self, MAX_PATH);
      const wchar_t* exe_name = wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe;
      bl2hdr::process::SetIsGame(_wcsicmp(exe_name, L"Borderlands2.exe") == 0);

      std::wstring log_name = L"bl2hdr.log";
      if (!bl2hdr::process::IsGame()) {
        std::wstring stem = exe_name;
        stem = stem.substr(0, stem.find_last_of(L'.'));
        log_name = L"bl2hdr_" + stem + L".log";
      }
      bl2hdr::log::Init(log_name.c_str());
      bl2hdr::log::Info("bl2hdr %s (built %s %s) loaded into %ls (pid %lu)", BL2HDR_VERSION, __DATE__, __TIME__, exe,
                        GetCurrentProcessId());
      bl2hdr::log::Info("proxy module: %ls", self);
      if (bl2hdr::process::IsGame()) {
        bl2hdr::config::Load();
        // Game-script hooks are installed now, before the game runs any code (no race with its threads).
        if (bl2hdr::config::Get().game_hooks) bl2hdr::ue3::Init();
        bl2hdr::log::Info("vtable slots: D3D9::CreateDevice=%u Reset=%u Present=%u CreateTexture=%u "
                          "CreateRenderTarget=%u CreatePixelShader=%u",
                          kSlotD3D9_CreateDevice, kSlotDev_Reset, kSlotDev_Present, kSlotDev_CreateTexture,
                          kSlotDev_CreateRenderTarget, kSlotDev_CreatePixelShader);
      } else {
        bl2hdr::log::Info("not the game: pass-through to the system d3d9.dll (no hooks, no 9on12, config ignored)");
      }
      break;
    }
    case DLL_PROCESS_DETACH:
      bl2hdr::log::Info("process detach");
      bl2hdr::log::Shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
