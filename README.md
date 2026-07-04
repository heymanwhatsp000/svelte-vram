# Svelte - VRAM Saver

Runtime VRAM reduction for D3D11 and D3D9 Windows games. Up to 77% less VRAM, one config knob, two files. Drop in, play.

## What It Does

Svelte is a small Windows DLL that reduces game VRAM usage by up to 77% with one configuration knob. You drop it into a game's folder. It silently strips the finest mip levels from large textures at the DirectX API layer - the mip levels your screen physically cannot display at your resolution.

The result: a GTX 1060 6GB can run a heavily-modded Skyrim loadout. An RTX 3060 8GB can run heavier modpacks. You avoid buying a new GPU.

## Measured Results

| Game | GPU | VRAM Before | VRAM After | Reduction |
|---|---|---|---|---|
| Skyrim SE (modded) | GTX 1080 8GB | 7,605 MiB | 1,830 MiB | 75.9% |
| Fallout New Vegas (with ENB) | GTX 1080 8GB | - | 920 MB saved | 74.7% strip rate |

## How It Works

Svelte uses DLL hijacking - a standard Windows pattern also used by ENB, ReShade, and SpecialK - to intercept DirectX calls. When the game creates a texture, Svelte silently shrinks it before it hits VRAM. When the game creates a shader view, Svelte clamps the mip range to prevent black textures.

- **D3D11** (Skyrim SE, Witcher 3, etc.): intercepts `CreateTexture2D` and `CreateShaderResourceView`
- **D3D9** (Fallout New Vegas, Skyrim LE, Fallout 3): uses a system-memory-backing approach that handles D3D9's deferred texture upload pattern

## Quick Start

### For Players

1. Download the latest release binary from NexusMods.
2. Extract the DLL and `svelte.ini` into your game's executable folder, next to the exe.
3. If you have ENB installed, rename ENB's DLL to `d3d11_enb.dll` (or `d3d9_enb.dll`) before installing Svelte. Svelte auto-detects and chains to it.
4. Launch the game.
5. Done. To remove: delete the two files.

See `USAGE.md` for the full player guide.

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

Output: `d3d9\build\d3d9.dll` (~155 KB, 32-bit x86).

## Configuration

One knob: `max_resolution`. This is the maximum texture dimension in pixels after stripping.

```ini
[svelte]
max_resolution=1024
enabled=1
min_texture_dimension=64
log_level=2
```

| max_resolution | Visual quality | Use case |
|---|---|---|
| 2048 | Looks fine | 8GB+ GPUs |
| 1024 | Slight softness | 6-8GB GPUs (default) |
| 512 | Noticeably soft | 4-6GB GPUs |
| 256 | Very blurry | 2-4GB GPUs |
| 128 | Extremely blurry | 2GB GPUs |

Only max_resolution=128 has been formally validated (76% VRAM reduction on Skyrim SE). Other values are estimates - start high and lower as needed.

## Project Structure

```
Svelte/
├── README.md This file
├── LICENSE MIT License
├── CHANGELOG.md Version history
├── USAGE.md Player-facing usage guide
├── CONTRIBUTING.md How to report bugs and contribute
├── .gitignore
├── d3d11/ D3D11 proxy source (Skyrim SE, Witcher 3, etc.)
│ ├── README.md D3D11 architecture and build guide
│ ├── build_proxy.bat
│ ├── d3d11_proxy.def
│ ├── svelte.ini
│ └── *.cpp, *.h Active source files
├── d3d9/ D3D9 proxy source (Fallout New Vegas, Skyrim LE, Fallout 3)
│ ├── README.md D3D9 architecture and build guide
│ ├── build_proxy.bat
│ ├── d3d9_proxy.def
│ ├── svelte.ini
│ └── *.cpp, *.h Active source files
```

## Supported Games

### D3D11
- Skyrim Special Edition - VALIDATED (75.9% VRAM reduction)
- Witcher 3 - untested

### D3D9
- Fallout New Vegas - VALIDATED (920 MB saved, ENB coexisting)
- Skyrim Legacy Edition, Fallout 3 - same engine family, should work

### Known Not to Benefit
- Fallout 4 - texture streaming engine refills freed VRAM. Not fixable at the runtime layer.


## License

MIT License - see `LICENSE`. Free to use, modify, and distribute.

