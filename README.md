# Svelte - VRAM Saver

Runtime VRAM reduction for D3D11 and D3D9 Windows games. Up to ~77% less VRAM.

## What It Does

Svelte is a small Windows DLL with one configuration knob. You drop it into a game's folder. It strips the finest mip levels from large textures at the DirectX API layer - the mip levels your screen physically cannot display at your resolution.

The result: a GTX 1060 6GB can run a heavily-modded Skyrim loadout. An RTX 3060 8GB can run heavier modpacks.


## How It Works

Svelte uses DLL hijacking - a standard Windows pattern also used by ENB, ReShade, and SpecialK - to intercept DirectX calls. When the game creates a texture, Svelte silently shrinks it before it hits VRAM. When the game creates a shader view, Svelte clamps the mip range to prevent black textures.

- **D3D11** (Skyrim SE, etc.): intercepts `CreateTexture2D` and `CreateShaderResourceView`
- **D3D9** (Fallout New Vegas, Fallout 3): uses a transient-buffer approach that handles D3D9's deferred texture upload pattern with zero permanent RAM cost. Works with and without DXVK.

### For Players

1. Download the latest release binary from NexusMods.
2. Extract the DLL and `svelte.ini` into your game's executable folder, next to the exe.
3. If you have ENB installed, rename ENB's DLL to `d3d11_enb.dll` (or `d3d9_enb.dll`) before installing Svelte. Svelte auto-detects and chains to it.
4. Launch the game.
5. To remove: delete the two files.

See `USAGE.md` for the guide.

### For Developers (Building from Source)

Requirements: Microsoft Visual Studio 2022 Build Tools with the C++ workload.

```cmd
cd d3d11
build_proxy.bat
```

Output: `d3d11\build\d3d11.dll` (~200 KB, 64-bit x64).

For D3D9 (Fallout New Vegas, 32-bit):

```cmd
cd d3d9
build_proxy.bat
```

Output: `d3d9\build\d3d9.dll` (~168 KB, 32-bit x86).

## Configuration

One knob: `max_resolution`. This is the maximum texture dimension in pixels after stripping.

```ini
[svelte]
max_resolution=1024
enabled=1
min_texture_dimension=64
log_level=2
```

### Texture Exclusion List (D3D9 v1.1+)

Create a `svelte_exclusions.txt` file next to the DLL to prevent specific
textures from being stripped. Patterns are substring-matched against a
fingerprint like `4096x4096_DXT5_mip13`. See `d3d9/svelte_exclusions.txt`
for examples.

## Project Structure

```
Svelte/
├── README.md This file
├── LICENSE MIT License
├── CHANGELOG.md Version history
├── USAGE.md Player-facing usage guide
├── CONTRIBUTING.md How to report bugs and contribute
├── .gitignore
├── d3d11/ D3D11 proxy source (Skyrim SE, etc.)
│ ├── README.md D3D11 architecture and build guide
│ ├── build_proxy.bat
│ ├── d3d11_proxy.def
│ ├── svelte.ini
│ └── *.cpp, *.h Active source files
├── d3d9/ D3D9 proxy source (Fallout New Vegas, Fallout 3)
│ ├── README.md D3D9 architecture and build guide
│ ├── build_proxy.bat
│ ├── d3d9_proxy.def
│ ├── svelte.ini
│ └── *.cpp, *.h Active source files
```

### D3D9
- Oblivion, Fallout 3 - same engine family, should work

### Known Not to Benefit
- Fallout 4 - texture streaming engine refills freed VRAM. Not fixable at the runtime layer.

## License

MIT License - see `LICENSE`. Free to use, modify, and distribute.

