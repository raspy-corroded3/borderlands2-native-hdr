# Third-party notices and credits

bl2hdr is original code under the MIT licence. It does **not** include code or assets from the game or
from the projects below; they were used as references for facts and techniques, and are credited here.

| Project | Licence | How it was used |
|---|---|---|
| [RenoDX](https://github.com/clshortfuse/renodx) (Carlos Lopez and contributors), including the Borderlands 2 RenoDX mod (Steve161803) | MIT | Reference for the approach: replacing D3D9 shaders by bytecode CRC32, the D3D9 → modern-API output bridge and swapchain settings (flip model, scRGB), and the list of BL2 shaders worth replacing. No RenoDX code is included; our shaders are written from the game's disassembly. |
| [bl-sdk/unrealsdk](https://github.com/bl-sdk/unrealsdk) (Willow2 SDK) | LGPL-3.0 | Reference for facts about Borderlands 2's UE3 build: byte patterns for ProcessEvent/CallFunction/GObjects/GNames/GMalloc, object/struct field offsets, and the shipping console's `say` prefix. All facts were re-verified against the game binary; no unrealsdk code is included. |
| Unreal Engine 3 shader sources shipped with the game (`Engine/Shaders/*.usf`, © Epic Games) | proprietary | Read to understand the post-processing pass (names of parameters, structure of the tone curve). Our HLSL is an independent re-implementation of the compiled shader's behaviour; no source is copied. |
| Microsoft Windows SDK, Direct3D 9/12, D3D9on12, DXGI | Windows SDK licence | Headers and the `fxc` shader compiler at build time; D3D9on12 at runtime (part of Windows). |

Borderlands 2 is © 2012 Gearbox Software / 2K Games. This project is not affiliated with or endorsed by
Gearbox, 2K, Epic Games, NVIDIA or Microsoft. It requires a legally owned copy of the game.
