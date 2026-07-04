# Svelte D3D11 Architecture

How the D3D11 proxy DLL works internally, how to build it, and what features it has.

## At a Glance

- **Source files compiled into the DLL**: 6 .cpp files (~80KB total source).
- **Output**: `d3d11.dll`, ~200 KB, 64-bit x64.
- **Build toolchain**: Microsoft Visual Studio 2022 Build Tools, MSVC `cl.exe`, C++14.
- **External dependencies**: None (only Windows D3D11 headers).
- **Validated on**: Skyrim Special Edition, GTX 1080 8GB, 75.9% VRAM reduction.

## File Layout

```
d3d11/
├── build_proxy.bat Build script. Calls vcvars64 + cl.exe.
├── d3d11_proxy.def DLL exports. 2 intercepted, 5 forwarded.
├── dllmain.cpp DllMain + MyCreateDevice + MyCreateDeviceAndSwapChain.
├── svelte_util.cpp Config, logging, BCn helpers, exclusions, game profile.
├── svelte_strip.cpp StripMips function. The 15-condition safety filter + strip math.
├── svelte_registry.cpp Stripped-texture map. SRWLOCK. LRU eviction.
├── svelte_wrapped_device.cpp WrappedDevice COM wrapper. CreateTexture2D + CreateShaderResourceView interception.
├── svelte_wrapped_swapchain.cpp WrappedSwapChain. Present hook for stats + LRU sweep.
├── svelte.ini Local config.
├── build/ Compiled output.
└── *.h Headers for each .cpp.
```

## How to Build

### Prerequisites

Microsoft Visual Studio 2022 Build Tools with the "Desktop development with C++" workload installed.

### Build command

```cmd
cd d3d11
build_proxy.bat
```

The script calls vcvars64, then cl.exe with these flags:

```
cl.exe /nologo /O2 /MT /LD /EHsc /std:c++14 /D _CRT_SECURE_NO_WARNINGS /D _WIN32_WINNT=0x0A00 ^
 dllmain.cpp ^
 svelte_util.cpp ^
 svelte_strip.cpp ^
 svelte_registry.cpp ^
 svelte_wrapped_device.cpp ^
 svelte_wrapped_swapchain.cpp ^
 /Fe:"%OUTDIR%\d3d11.dll" /Fo:"%OUTDIR%\" ^
 /link /DEF:"%SRCDIR%d3d11_proxy.def" d3d11.lib ole32.lib kernel32.lib user32.lib
```

### Build output

- `build\d3d11.dll` - the proxy DLL (~200 KB).
- `build\*.obj` - intermediate object files (not needed for deployment).

Only `d3d11.dll` ships. Copy it + `svelte.ini` to the game folder.

## How It Works

### Three-layer interception

```
Game.exe
 |
 | (loads d3d11.dll from game folder instead of System32 via DLL search order)
 v
Svelte's d3d11.dll
 |
 | LAYER 1: D3D11CreateDevice / D3D11CreateDeviceAndSwapChain
 | Entry-point interception. Wraps the returned ID3D11Device* in a WrappedDevice.
 v
WrappedDevice (COM wrapper)
 |
 | LAYER 2: CreateTexture2D
 | Calls StripMips(). If strip succeeds, creates a smaller texture and registers it.
 |
 | LAYER 3: CreateShaderResourceView
 | Looks up the texture in the registry. If stripped, clamps mip range in the SRV desc.
 v
Real ID3D11Device (from System32 d3d11.dll, or chain DLL if present)
```

### DLL hijacking (load-time)

Windows DLL search order loads the application's own directory first. So `C:\Games\Skyrim\d3d11.dll` loads INSTEAD of `C:\Windows\System32\d3d11.dll`. Same technique ENB, ReShade, SpecialK use.

### Proxy exports (d3d11_proxy.def)

```def
EXPORTS
 D3D11CreateDevice = MyCreateDevice ; INTERCEPTED
 D3D11CreateDeviceAndSwapChain = MyCreateDeviceAndSwapChain ; INTERCEPTED
 D3D11CoreCreateDevice = d3d11.D3D11CoreCreateDevice ; FORWARDED
 D3D11CoreCreateLayeredDevice = d3d11.D3D11CoreCreateLayeredDevice ; FORWARDED
 D3D11CoreGetLayeredDeviceSize = d3d11.D3D11CoreGetLayeredDeviceSize ; FORWARDED
 D3D11CoreRegisterLayers = d3d11.D3D11CoreRegisterLayers ; FORWARDED
 D3D11On12CreateDevice = d3d11.D3D11On12CreateDevice ; FORWARDED
```

### Chain DLL detection (DllMain)

At `DLL_PROCESS_ATTACH` Svelte checks for chain DLLs in priority order:

```
d3d11_enb.dll (ENB)
d3d11_reshade.dll (ReShade)
d3d11_dxvk.dll (DXVK)
d3d11_orig.dll (any tool)
```

First found gets loaded as the real D3D11. If none found, falls back to `C:\Windows\System32\d3d11.dll`.

### Game profile auto-detection

At load time Svelte checks the host exe basename and sets a game profile:

- `skyrimse.exe` - Skyrim SE. All BCn formats safe to strip.
- `fallout4.exe` - Fallout 4. BC3/BC7 may be specular. Restricts aggressive modes to BC1.
- `witcher3.exe` - Witcher 3. Streaming engine. Conservative mode recommended.


## StripMips - the core algorithm

Source: `svelte_strip.cpp`.

### 15-condition safety filter

If ANY condition is true, the texture is skipped (not stripped):

1. `MipLevels <= 1` - no mip chain to strip.
2. `MipLevels > 16` - sanity cap. No real texture has more than 16 mips.
3. `MipLevels == 0` - auto-gen. Game plans to generate the mip chain itself.
4. `ArraySize > 1` - texture arrays.
5. `MiscFlags & TEXTURECUBE` - cubemaps. Skyboxes, 6 faces, must stay uniform.
6. `Width < min_tex_dim` or `Height < min_tex_dim` - too small.
7. Not BC1/BC3/BC7 format - BC5 (normal maps) excluded because stripping causes rainbow artifacts.
8. `BindFlags & RENDER_TARGET` - game renders INTO this texture.
9. `BindFlags & DEPTH_STENCIL` - depth buffer.
10. `BindFlags & UNORDERED_ACCESS` - compute shader writes.
11. `MiscFlags & GENERATE_MIPS` - game auto-generates mip chain.
12. `Usage == STAGING` - CPU-side copy buffer, not in VRAM.
13. `Usage == DYNAMIC` - game plans frequent UpdateSubresource.
14. `CPUAccessFlags & CPU_ACCESS_WRITE` - game plans to write.
15. Exclusion list match - user-defined substring match against fingerprint.

### Strip math

```cpp
int maxDim = max(Width, Height);
int mipsToStrip = 0;
int dim = maxDim;
while (dim > g_maxResolution && mipsToStrip < MipLevels - 1) {
 dim >>= 1;
 mipsToStrip++;
}
// Min result 4x4
```

After computing `mipsToStrip`:
- Build new `D3D11_TEXTURE2D_DESC` with reduced `Width`, `Height`, `MipLevels`.
- Copy `pInitialData[mipsToStrip..]` into a `thread_local D3D11_SUBRESOURCE_DATA buf[16]`. Thread-local because D3D11 can call `CreateTexture2D` from deferred contexts and worker threads.

### Format whitelist

Only BCn formats are safe to strip:

- **BC1 (DXT1)**: 8 bytes per 4x4 block. Color/diffuse only, no alpha.
- **BC3 (DXT5)**: 16 bytes per block. Color + alpha.
- **BC7**: 16 bytes per block. High quality, all uses.

Excluded: BC5 (normal maps - stripping causes rainbow artifacts), BC2 (rare), BC4 (single-channel), BC6H (HDR), uncompressed formats.

### The black-texture fix (Layer 3, CreateShaderResourceView)

Game creates a 4096x4096 texture with 13 mips. Svelte strips 1, texture is now 2048x2048 with 12 mips. Game later creates an SRV asking for 13 mips. D3D11 sees only 12 mips, returns `E_INVALIDARG`, null SRV, black texture.

The fix: when the game calls `CreateShaderResourceView`, look up the resource in the stripped-texture registry. If found, clamp `MostDetailedMip` and `MipLevels` in the SRV desc to the actual reduced count.

Stale-entry verification: before clamping, call `GetDesc()` on the texture and verify the mip count matches the registry. If not, the entry is stale (pointer was reused by a different texture), remove it and pass through without clamping.

## Stripped-texture registry

Source: `svelte_registry.cpp`.

Data structure: `unordered_map<ID3D11Texture2D*, {mipCount, lastAccessFrame}>` guarded by `SRWLOCK`.

- `RegisterStrippedTexture(tex, newMipCount)` - called from `CreateTexture2D` after successful strip.
- `LookupStrippedMipCount(resource)` - called from `CreateShaderResourceView`.
- `UnregisterStrippedTexture(tex)` - called when a texture is released.
- `MaybeSweepStrippedMap()` - called every 300 frames from `Present`. If map size > 50,000 entries, evicts stale entries.

## COM wrapper coverage

`WrappedDevice::QueryInterface` returns `this` for all 6 device interface versions:

- `IUnknown`, `ID3D11Device`, `ID3D11Device1/2/3/4/5`

This is critical. If the game `QueryInterface`s for `ID3D11Device3` and Svelte does not return self, the game gets the real device and bypasses `CreateTexture2D` interception.

## Present hook

Source: `svelte_wrapped_swapchain.cpp`.

Every `Present` call increments `g_frameCount`. Every 300th frame:

1. Log a stats line: `created`, `stripped`, `skippedFilter`, `skippedExcl`, `failed`, `saved (MB)`, `srvClamped`.
2. Call `MaybeSweepStrippedMap()` for LRU cleanup.

## Configuration loading

Source: `svelte_util.cpp` `LoadConfig()`.

At first call (lazy, from `MyCreateDevice`):

1. Find self path via `GetModuleHandleExA` + `GetModuleFileNameA`.
2. Read `svelte.ini` from the DLL's directory via `GetPrivateProfileIntA`.
3. Open `svelte.log` in the DLL's directory with `fopen("a")` (append mode).
4. Load `svelte_exclusions.txt` if present.

## Logging

`Log(fmt, ...)` writes to `g_logFile` under an `SRWLOCK` exclusive lock. `fflush` after every line so logs survive crashes.

Log levels:

- `0` - no log file.
- `1` - StripMips lines + periodic Stats (every 300 frames).
- `2` - also logs every safety-filter skip and SRV clamp.
- `3` - also logs every CreateTexture2D call (very verbose).

## Known limitations

1. **Streaming engines do not benefit.** Fallout 4's texture streaming engine refills freed VRAM the moment Svelte releases it. Net reduction near zero. Cannot be fixed at the D3D11 runtime layer.

2. **Deferred texture uploads skip stripping.** If a game creates a texture with `pInitialData = NULL` and later fills it via `UpdateSubresource`, Svelte cannot intercept this.

## Future work

- Per-game tuning profiles.
- D3D12 port (9 device interfaces, command-list interception, explicit residency control).
