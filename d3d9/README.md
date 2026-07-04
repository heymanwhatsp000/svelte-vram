# Svelte D3D9 Architecture

How the D3D9 proxy DLL works internally, how to build it, and what features it has.

## At a Glance

- **Source files compiled into the DLL**: 7 .cpp files (~75KB total source).
- **Output**: `d3d9.dll`, ~155 KB, **32-bit x86** (D3D9 games are old, almost all 32-bit).
- **Build toolchain**: Microsoft Visual Studio 2022 Build Tools, MSVC `cl.exe` (32-bit), C++14.
- **External dependencies**: None (only Windows D3D9 headers).
- **Validated on**: Fallout New Vegas, GTX 1080 8GB, 920 MB VRAM saved per session, ENB coexisting.

## File Layout

```
d3d9/
├── build_proxy.bat Build script. Calls vcvars32 (32-bit) + cl.exe.
├── d3d9_proxy.def DLL exports. 2 intercepted, 15 forwarded.
├── dllmain.cpp DllMain + MyDirect3DCreate9 + MyDirect3DCreate9Ex.
├── svelte_util.cpp Config, logging, BCn helpers (D3D9 variants).
├── svelte_strip.cpp StripMips9 function. The safety filter + strip math.
├── svelte_registry.cpp Stripped-texture map (smaller than D3D11 version).
├── svelte_texture_manager.cpp System Memory Backing implementation. THE breakthrough.
├── svelte_wrapped_d3d9.cpp WrappedD3D9. Intercepts CreateDevice.
├── svelte_wrapped_device.cpp WrappedDevice9. 127 method forwards + CreateTexture + SetTexture.
├── svelte.ini Local config.
├── build/ Compiled output (d3d9.dll + .obj files).
└── *.h Headers.
```

## How to Build

### Prerequisites

Same as D3D11: Visual Studio 2022 Build Tools with C++ workload. You need both 32-bit and 64-bit toolchains installed.

### Build command

```cmd
cd d3d9
build_proxy.bat
```

The script calls `vcvars32.bat` (NOT vcvars64) to get the 32-bit `cl.exe`. This is critical: D3D9 games are 32-bit. A 64-bit DLL will not load.

## How It Works (the system-memory-backing breakthrough)

### Why D3D9 is fundamentally different from D3D11

D3D11 textures are created with `pInitialData` - the texture data is uploaded at creation time. Svelte can intercept, modify the desc, shift the data array, and create a smaller texture in one call.

D3D9 textures are created EMPTY. The game calls `CreateTexture(width, height, levels, ...)`, gets back an empty texture, then fills it later via `LockRect` / `UnlockRect`:

```cpp
device->CreateTexture(4096, 4096, 13, 0, D3DFMT_DXT5, D3DPOOL_DEFAULT, &tex, NULL);
tex->LockRect(0, &lockedRect, NULL, D3DLOCK_DISCARD);
memcpy(lockedRect.pBits, data, size);
tex->UnlockRect(0);
```

If Svelte strips the texture at creation (reduces 4096x4096/13 to 2048x2048/12), the game still calls `LockRect(level=12, ...)` later on a mip that no longer exists. On Windows 10/11, D3D9 is translated to D3D12 by the D3D9On12 layer. D3D9On12 validates texture dimensions during rendering and crashes when they do not match.

### System Memory Backing (the solution)

Per stripped texture, create THREE textures:

1. `sysmemTex` = `CreateTexture(W, H, Levels, D3DPOOL_SYSTEMMEM)` - full size, lives in system RAM. **Returned to the game.** The game's `GetLevelDesc`/`GetLevelCount`/`LockRect` calls hit this and see full dimensions. No crash, no lies at the API surface.
2. `stagingTex` = `CreateTexture(W', H', Levels', D3DPOOL_SYSTEMMEM)` - stripped, staging buffer. Used for the copy.
3. `vramTex` = `CreateTexture(W', H', Levels', D3DPOOL_DEFAULT)` - stripped, in VRAM. Used for actual rendering.

When the game calls `SetTexture(stage, sysmemTex)`:

1. Look up the `TexPair` by `sysmemTex` pointer in the pair map.
2. Copy `sysmemTex` to `stagingTex` via `LockRect` + `memcpy` (both are SYSTEMMEM so `LockRect` always works). For each level L in staging, source level = L + `mipsStripped` in sysmem. BCn-aware row copy.
3. Call `device->UpdateTexture(stagingTex, vramTex)`. D3D9 internally copies from SYSTEMMEM to DEFAULT. No `LockRect` on VRAM needed.
4. Call `m_real->SetTexture(stage, vramTex)`. Bind the stripped VRAM texture.

The truth is just split across two allocations. Game sees full dimensions, VRAM holds stripped dimensions, the copy happens at `SetTexture` time.

Additional hardening:
- `dirty` flag on `TexPair` - skip `CopySysmemToVRAM` if sysmem has not been modified since last copy.
- `lastAccess` frame counter + LRU sweep (cutoff 900 frames, max 5000 entries).
- Verify `srcLevel < origLevels` before `LockRect` (skip if out of range).

### D3D9 safety filter (8 conditions)

1. Must be a 2D texture (`D3DRTYPE_TEXTURE`), not cube or volume.
2. Must have mips (`Levels > 1`). `Levels == 0` in D3D9 means "default" = autogen, skip.
3. Sanity cap (`Levels <= 16`).
4. Not a render target (`Usage & D3DUSAGE_RENDERTARGET`).
5. Not a depth stencil (`Usage & D3DUSAGE_DEPTHSTENCIL`).
6. Not dynamic (`Usage & D3DUSAGE_DYNAMIC`).
7. Format must be `D3DFMT_DXT1`, `D3DFMT_DXT3`, or `D3DFMT_DXT5` (BC1/BC2/BC3 equivalents).
8. `Width >= min_texture_dimension` and `Height >= min_texture_dimension`.

### Strip math with BCn multiple-of-4 fix

D3D9's `D3DFMT_DXT*` formats require dimensions to be multiples of 4. Non-power-of-2 textures (like 1920x1080) produce non-multiple-of-4 results when stripped. D3D9 rejects these with `E_INVALIDARG`.

Fix: strip one fewer mip until both dimensions are multiples of 4:

```cpp
while (mipsToStrip > 0 && ((newW % 4) != 0 || (newH % 4) != 0)) {
 mipsToStrip--;
 newW = (int)pDesc->Width >> mipsToStrip;
 newH = (int)pDesc->Height >> mipsToStrip;
 newLevels = (int)pDesc->Levels - mipsToStrip;
}
if ((newW % 4) != 0 || (newH % 4) != 0) {
 return 0; // skip entirely
}
```

## Chain DLL detection (DllMain)

Same pattern as D3D11. At `DLL_PROCESS_ATTACH` Svelte checks for chain DLLs in priority order:

```
d3d9_enb.dll (ENB)
d3d9_reshade.dll (ReShade)
d3d9_dxvk.dll (DXVK)
d3d9_orig.dll (any tool)
d3d9_chain.dll (generic chain name)
```

First found gets loaded as the real D3D9. Falls back to System32's `d3d9.dll`.

### D3DPERF_SetOptions no-op

FNV statically imports `D3DPERF_SetOptions` from `d3d9.dll`. If Svelte forwarded this to `d3d9.D3DPERF_SetOptions`, it would create a circular load: Svelte's d3d9.dll would try to load the real d3d9.dll, which is Svelte itself.

Fix: Svelte exports `MyD3DPERF_SetOptions` as a no-op. This is safe - `D3DPERF_SetOptions` is just a PIX profiling hint, doing nothing is correct behavior.

### Launcher pass-through

FNV has a launcher (`FalloutNVLauncher.exe`). If Svelte wraps the launcher, the launcher creates a D3D9 device for its UI, Svelte intercepts and wraps, then the launcher quits and the real game launches without Svelte loaded.

Fix: at `MyDirect3DCreate9`, check if the host basename contains "launcher". If yes, return the unwrapped real `IDirect3D9`. No wrapping, no interception.

## Proxy exports (d3d9_proxy.def)

```def
LIBRARY d3d9
EXPORTS
 Direct3DCreate9 = MyDirect3DCreate9 ; INTERCEPTED
 Direct3DCreate9Ex = MyDirect3DCreate9Ex ; INTERCEPTED
 D3DPERF_SetOptions = MyD3DPERF_SetOptions ; NO-OP (avoid circular load)
 D3DPERF_BeginEvent = d3d9.D3DPERF_BeginEvent ; FORWARDED
 D3DPERF_EndEvent = d3d9.D3DPERF_EndEvent ; FORWARDED
 ... (14 forwarded total)
```

17 exports total. 2 intercepted, 1 no-op, 14 forwarded.

## TexPair registry

Source: `svelte_texture_manager.cpp`.

Data structure: `unordered_map<IDirect3DBaseTexture9*, TexPair*>` guarded by `SRWLOCK`. The key is the sysmemTex pointer (what the game holds).

`TexPair` struct:
```cpp
struct TexPair {
 IDirect3DTexture9* sysmemTex; // full size, returned to game
 IDirect3DTexture9* stagingTex; // stripped, sysmem, for copy
 IDirect3DTexture9* vramTex; // stripped, in VRAM, for rendering
 UINT origWidth, origHeight, origLevels;
 UINT strippedWidth, strippedHeight, strippedLevels;
 D3DFORMAT format;
 int mipsStripped;
 bool dirty; // needs copy?
 long long lastAccess; // for LRU
};
```

## WrappedDevice9 coverage

`WrappedDevice9::QueryInterface` returns `this` for `IUnknown`, `IDirect3DDevice9`, `IDirect3DDevice9Ex`.

About 127 mechanical `m_real->X()` forwards fill out the rest of the vtable. The two intercepted methods:

- `CreateTexture` - calls `StripMips9`. If strip succeeds and pool is DEFAULT or MANAGED, calls `CreateStrippedPair` and returns sysmemTex. Otherwise passes through.
- `SetTexture` - looks up the texture in the pair map. If found, calls `CopySysmemToVRAM` then `m_real->SetTexture(stage, pair->vramTex)`. Otherwise passes through.

## Validated Results

Fallout New Vegas, 1920x1080, with ENB chained, `max_resolution=128`:

```
Textures created: 1,434
Textures stripped: 1,071 (74.7% strip rate)
Textures skipped: 0
Textures failed: 0
VRAM saved: 920 MB
Frames rendered: 6,660
```

Zero crashes. ENB coexisting via the chain mechanism.

## Known limitations

1. **System RAM cost.** Each stripped texture holds a full-size sysmem copy. For FNV's 1,071 stripped textures at ~256x256 average, that is about 30-50 MB of system RAM. For games with larger textures it could be 200+ MB.

2. **`SetTexture` overhead.** Each `SetTexture` call now does a `LockRect`+`memcpy`+`UpdateTexture` if the pair is dirty. For static textures this happens once. For dynamic textures (which the safety filter rejects anyway) it would happen every frame.

3. **32-bit only.** D3D9 games are 32-bit. Svelte's D3D9 build is 32-bit. It will not load into a 64-bit process.
