# Architecture

bl2hdr is a proxy `d3d9.dll` loaded by `Borderlands2.exe` from `Binaries\Win32`. Other processes that load
it (e.g. the 2K launcher, a WPF app) are passed straight through to Windows' d3d9.dll.

## Frame pipeline
```
game (D3D9) ──► D3D9on12 ──► D3D12 device ──► our D3D12 encode pass ──► our DXGI FP16 scRGB swapchain ──► Windows/display
  │ scene in FP16 render targets (unchanged)
  │ tonemap pass  ← our HDR shader (swapped by bytecode CRC)       writes into ↓
  │ HUD/Scaleform draws on top                                      FP16 substitute back buffer
```
1. **D3D9on12** (`proxy.cpp`): `Direct3DCreate9` is redirected to `Direct3DCreate9On12`. The game's
   D3D9 runs on D3D12, and `IDirect3DDevice9On12::UnwrapUnderlyingResource` gives us any D3D9 resource as
   a D3D12 resource. (D3D9on12's own windowed Present does not reach the screen on the test system, so we
   never use it.)
2. **Substitute back buffer** (`backbuffer.cpp`): a frame trace showed the tonemap pass and the HUD draw
   straight into the 8-bit back buffer. `GetBackBuffer` returns our back-buffer-sized `A16B16G16R16F`
   render target instead, so values above 1.0 survive. It is released before `Reset` and recreated after.
3. **Shader swap** (`shader_swap.cpp`, `shaders/tonemap.ps_3_0.hlsl`): `CreatePixelShader` hashes the
   bytecode (CRC32). The tonemap shader `0x54ED86A0` (UberPostProcessBlend) is replaced by our rewrite,
   compiled with `fxc` at build time and embedded. Variants: `hdr`, `replica` (pixel-equivalent to the
   original — verified), `tint` (test).
4. **HDR tonemapper**: the original pipeline (bloom → vignette → tone curve → `saturate` → 2D LUT) is kept
   as the SDR result; highlights are extended using the game's own linear segment (scene × steepness),
   rolled off towards the peak, and only the energy above SDR white is added (midtones unchanged, hue from
   the colour grade). Parameters in our constant register **c50**, set when the shader is bound:
   x = peak / game white, y = strength (0 = "HDR Off"), z = game white / UI white.
5. **Brightness units**: the back buffer is in *UI-white* units. The tonemapper pre-scales the scene by
   game/UI white; the HUD draws at 1.0 = UI white; the encode pass scales by UI white / 80 nits.
6. **Output** (`output_dxgi.cpp`, `shaders/encode.hlsl`): on our own D3D12 queue: unwrap the substitute,
   draw a full-screen triangle into the swapchain buffer (linear = c^2.2 × UIWhite/80, clamped to the
   peak), fence, return the resource to 9on12, `Present` (flip-discard, tearing allowed). The swapchain is
   sized like the source image and rebuilt when it changes. Also: debug highlight view, test pattern,
   frame capture to PFM + statistics.
7. **Window** (`window.cpp`): an exclusive-fullscreen request is converted to a borderless window
   covering the monitor; windowed requests are left alone.

## Game-script hooks (`ue3.cpp`, `menu.cpp`)
- Located at runtime by byte patterns (the exe is relocated): ProcessEvent, CallFunction (two matches;
  the first is UObject::CallFunction), GObjects, GNames, GMalloc. Hooked in `DllMain` with 5-byte jumps
  (both functions start `55 8B EC 6A FF`; checked before patching).
- Object/name access with verified layout offsets (see `ue3.h`). Parameter blocks are built by
  parameter **name** from the function's property chain; strings are allocated with the engine's
  GMalloc. Function lookups run on the game thread (first ProcessEvent after the first frame).
- **Video menu**: after `WillowScrollingListDataProviderVideoOptions.Populate` (script call → CallFunction
  post-hook; the list is the calling object), `AddSpinnerListItem(9417, "HDR", …, ["Off","On"])`.
  `WillowScrollingList.OnSpinnerValueChange` (from Flash, via ProcessEvent) → row → `IndexToEventId` →
  if 9417: switch HDR live, save `[hdr] Enabled`, block the call.
- `[debug] ConsoleAtSec` runs `PlayerController.ConsoleCommand` on the game thread (the shipping console
  prefixes `say`, so typed commands do not run).

## Milestones
| # | Goal | Status |
|---|---|---|
| 1 | Pass-through proxy + logging | done |
| 2 | D3D9 base for interop: D3D9on12 (D3D9Ex was the fallback) | done |
| 3 | Shader swap (tint live, replica pixel-equivalent) | done |
| 4 | HDR output, HDR tonemapper, HUD brightness, test pattern | done |
| 5 | Game-script hooks + Video-menu option | done |
| 6 | Robustness and publishing (alt-tab ✓, fullscreen setting ✓, resolution change ✓) | in progress |
