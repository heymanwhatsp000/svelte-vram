{'status': 'success', 'content': '# Svelte D3D9 Architecture\n\nHow the D3D9 proxy DLL works internally, how to build it, and what features it has.\n\n## At a Glance\n\n- **Source files compiled into the DLL**: 7 .cpp files (~85KB total source).\n- **Output**: `d3d9.dll`, ~168 KB, **32-bit x86** (D3D9 games are old, almost all 32-bit).\n- **Build toolchain**: Microsoft Visual Studio 2022 Build Tools, MSVC `cl.exe` (32-bit), C++14.\n- **External dependencies**: None (only Windows D3D9 headers).\n- **Architecture**: Transient buffer approach — zero permanent RAM, works with and without DXVK.\n- **Validated on**: Fallout New Vegas, GTX 1080 8GB, 1.3+ GB VRAM saved per session.\n\n## File Layout\n\n```\nd3d9/\n├── build_proxy.bat Build script. Calls vcvars32 (32-bit) + cl.exe.\n├── d3d9_proxy.def DLL exports. 2 intercepted, 15 forwarded.\n├── dllmain.cpp DllMain + MyDirect3DCreate9 + MyDirect3DCreate9Ex.\n├── svelte_util.cpp Config, logging, BCn helpers, exclusions, perf tracking.\n├── svelte_strip.cpp StripMips9 function. The safety filter + strip math.\n├── svelte_registry.cpp Stripped-texture map (SRWLOCK + LRU sweep).\n├── svelte_wrapped_texture.cpp WrappedTexture9. Transient buffer approach.\n├── svelte_wrapped_d3d9.cpp WrappedD3D9. Intercepts CreateDevice.\n├── svelte_wrapped_device.cpp WrappedDevice9. CreateTexture + SetTexture intercept.\n├── svelte.ini Local config.\n├── svelte_exclusions.txt Optional texture exclusion patterns.\n├── build/ Compiled output (d3d9.dll + .obj files).\n└── *.h Headers.\n```\n\n## How to Build\n\n### Prerequisites\n\nSame as D3D11: Visual Studio 2022 Build Tools with C++ workload. You need both 32-bit and 64-bit toolchains installed.\n\n### Build command\n\n```cmd\ncd d3d9\nbuild_proxy.bat\n```\n\nThe script calls `vcvars32.bat` (NOT vcvars64) to get the 32-bit `cl.exe`. This is critical: D3D9 games are 32-bit. A 64-bit DLL will not load.\n\n## How It Works (transient buffer approach)

### Why D3D9 is fundamentally different from D3D11

D3D11 textures are created with `pInitialData` - the texture data is uploaded at creation time. Svelte can intercept, modify the desc, shift the data array, and create a smaller texture in one call.

D3D9 textures are created EMPTY. The game calls `CreateTexture(width, height, levels, ...)`, gets back an empty texture, then fills it later via `LockRect` / `UnlockRect`:

```cpp
device->CreateTexture(4096, 4096, 13, 0, D3DFMT_DXT5, D3DPOOL_DEFAULT, &tex, NULL);
tex->LockRect(0, &lockedRect, NULL, D3DLOCK_DISCARD);
memcpy(lockedRect.pBits, data, size);
tex->UnlockRect(0);
```

If Svelte strips the texture at creation (reduces 4096x4096/13 to 2048x2048/12), the game still calls `LockRect(level=12, ...)` later on a mip that no longer exists.

### Transient buffer approach (the solution)

Svelte creates ONE stripped texture and wraps it in `WrappedTexture9`. The wrapper maintains the illusion that the game still has the original mip count:

- `GetLevelCount()` returns the original count (the "lie")
- `GetLevelDesc(level)` returns original dimensions for stripped levels, real dimensions for valid levels
- `LockRect(level, ...)` for valid levels: locks the real texture directly (dimensions match)
- `LockRect(level, ...)` for stripped levels: allocates a **transient buffer** sized to what the game expects. The game writes its data into this buffer. On `UnlockRect`, the buffer is freed. The data is discarded — those mip levels were intentionally removed.
- `GetSurfaceLevel(level)` returns the real surface for valid levels, `D3DERR_INVALIDCALL` for stripped levels
- `Release()` unregisters from the stripped-texture registry

**Zero permanent RAM cost.** Transient buffers exist only during the LockRect→UnlockRect window (microseconds). For static textures (which the safety filter ensures), this happens once at load time, then never again.

### DXVK compatibility

Svelte auto-detects DXVK (checks for `d3d9_dxvk.dll` in the game folder). Detection is for logging only — the code path is identical with or without DXVK. The transient buffer approach works on both DXVK (Vulkan backend) and native D3D9On12.

### D3D9 safety filter (9 conditions)

1. Must be a 2D texture (`D3DRTYPE_TEXTURE`), not cube or volume.
2. Must have mips (`Levels > 1`). `Levels == 0` in D3D9 means "default" = autogen, skip.
3. Sanity cap (`Levels <= 16`).
4. Not a render target (`Usage & D3DUSAGE_RENDERTARGET`).
5. Not a depth stencil (`Usage & D3DUSAGE_DEPTHSTENCIL`).
6. Not dynamic (`Usage & D3DUSAGE_DYNAMIC`).
7. Not auto-gen mips (`Usage & D3DUSAGE_AUTOGENMIPMAP`).
8. Format must be `D3DFMT_DXT1`, `D3DFMT_DXT3`, or `D3DFMT_DXT5` (BC1/BC2/BC3 equivalents).
9. `Width >= min_texture_dimension` and `Height >= min_texture_dimension`.

Textures matching an exclusion pattern in `svelte_exclusions.txt` are also skipped.

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

## WrappedDevice9 coverage

`WrappedDevice9::QueryInterface` returns `this` for `IUnknown`, `IDirect3DDevice9`, `IDirect3DDevice9Ex`.

About 127 mechanical `m_real->X()` forwards fill out the rest of the vtable. The intercepted methods:

- `CreateTexture` - calls `StripMips9`. If strip succeeds and pool is DEFAULT or MANAGED, creates a stripped texture, wraps it in `WrappedTexture9`, and returns the wrapper. Otherwise passes through.
- `SetTexture` - unwraps `WrappedTexture9` before passing to `m_real->SetTexture`. This prevents the wrapper's mip-count lie from reaching the driver/DXVK descriptor sizing.
- `UpdateTexture` - unwraps both source and destination textures before passing through.
- `GetTexture` - re-wraps the result if the texture is one of our stripped real textures, so the game always sees the wrapper.
- `Reset` / `ResetEx` - calls `PrepareForDeviceReset()` before forwarding, which releases DEFAULT-pool textures and frees transient locks to prevent use-after-free.
- `Present` / `PresentEx` - logs performance stats every 300 frames and runs the LRU sweep.

## Validated Results

Fallout New Vegas, 1920x1080, with DXVK, `max_resolution=128`:

```
Textures stripped: 1,934
VRAM saved: 1,364 MB
Frames rendered: 14,400+
Stutters from Svelte: 0 (strip time = 0ms)
Errors: 0
Crashes: 0
```

Also tested without DXVK (native D3D9On12): zero crashes, zero errors, 927 MB VRAM saved. The transient buffer approach works identically on both backends.

