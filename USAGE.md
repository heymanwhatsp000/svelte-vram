# Svelte - Usage Guide

## Installation

### Step 1: Pick the right build

- **For Skyrim SE, Witcher 3, and other D3D11 games**: use `d3d11.dll`
- **For Fallout New Vegas, Skyrim LE, Fallout 3, and other D3D9 games**: use `d3d9.dll`

### Step 2: Locate your game's executable folder

The DLL and `svelte.ini` must sit next to the game's exe. Examples:

- Skyrim SE: `C:\Games\Skyrim\` (where `SkyrimSE.exe` lives)
- Fallout New Vegas: `C:\Games\Fallout New Vegas\` (where `FalloutNV.exe` lives)

If you use Mod Organizer 2, you need the MO2 "Root Builder" plugin to get the DLL into the actual game folder.

### Step 3: Handle ENB if installed

If you already have ENB installed, rename ENB's `d3d11.dll` to `d3d11_enb.dll` (or `d3d9.dll` to `d3d9_enb.dll`) BEFORE dropping Svelte in. Svelte auto-detects chain DLLs and loads them as the real DirectX.

Chain priority Svelte checks (first found wins):
- `d3d11_enb.dll` (ENB)
- `d3d11_reshade.dll` (ReShade)
- `d3d11_dxvk.dll` (DXVK)
- `d3d11_orig.dll` (any tool)
- System32's `d3d11.dll` (fallback)

### Step 4: Launch the game

Launch normally. No special launcher, no command-line flags.

### Step 5: Verify it loaded

Check the game folder for `svelte.log`. The first lines should show Svelte loaded with config summary.

## Configuration

Edit `svelte.ini` in the game folder:

```ini
[svelte]
max_resolution=1024
enabled=1
min_texture_dimension=64
log_level=2
```

### Settings

- `max_resolution` - Maximum texture dimension after stripping. Lower = less VRAM = blurrier. Default 1024.
- `enabled` - 1 = on, 0 = off. Set to 0 to disable without removing files.
- `min_texture_dimension` - Don't strip textures smaller than this. Default 64. Raise to 128 if small textures look wrong.
- `log_level` - 0=off, 1=basic, 2=verbose (recommended for debugging), 3=debug (every texture).

### Tuning advice

- **8GB+ VRAM**: start at `max_resolution=2048`. ~50% reduction, no visible quality loss.
- **6-8GB VRAM**: `max_resolution=1024`. ~60% reduction, slight softness.
- **4-6GB VRAM**: `max_resolution=512`. ~70% reduction, visibly softer but playable.
- **2-4GB VRAM**: `max_resolution=256`. ~73% reduction, emergency setting.
- **Under 2GB**: `max_resolution=128`. ~77% reduction, very blurry.

## Troubleshooting

### Svelte did not load (no svelte.log)
1. Verify the DLL is named `d3d11.dll` (or `d3d9.dll`) and sits next to the game's exe.
2. Verify `svelte.ini` is in the same folder.
3. Check `enabled=1` in `svelte.ini`.

### Game crashes on launch
1. Set `enabled=0` and try again. If it runs with Svelte disabled, the crash is Svelte-related.
2. Check `svelte.log` for the last lines before the crash.
3. Try raising `min_texture_dimension` to 256.

### Black textures
This should not happen with current versions. If it does, check `svelte.log` for `STALE ENTRY` warnings and report on the bug tracker.

### Antivirus flags the DLL
This is a false positive. The DLL hijacking technique is also used by malware, so AVs flag clean proxy DLLs at 5-15% rates. ENB and ReShade have the same issue. Add the game folder to your AV exclusions.

### Game runs but no VRAM reduction
1. Check `svelte.log` for `Textures stripped: 0`. The game might use deferred texture uploads.
2. Some games (Fallout 4) use texture streaming engines that refill VRAM after Svelte releases it. Not fixable at the runtime layer.

## Removal

Delete `d3d11.dll` (or `d3d9.dll`) and `svelte.ini` from the game folder. The game reverts to System32's DLL instantly. No uninstaller, no leftovers.
