# Svelte - Changelog

## v1.0 (2026-07-03) - Initial Release

### D3D11 build (Skyrim SE)
- 6 source files compiled (dllmain, svelte_util, svelte_strip, svelte_registry, svelte_wrapped_device, svelte_wrapped_swapchain).
- Output: ~212 KB, 64-bit x64.
- Validated on Skyrim SE: 7,605 to 1,830 MiB VRAM (75.9 percent reduction).
- 15-condition safety filter. SRV clamping with stale-entry verification. LRU eviction.
- ENB chain auto-detection.

### D3D9 build (Fallout New Vegas)
- 7 source files compiled (dllmain, svelte_util, svelte_strip, svelte_registry, svelte_wrapped_device, svelte_wrapped_d3d9, svelte_texture_manager).
- Output: ~160 KB, 32-bit x86.
- Validated on FNV: 920 MB saved per session, ENB coexisting, zero crashes.
- System memory backing approach (3 textures per strip: sysmem + staging + VRAM).
- 8-condition safety filter. BCn multiple-of-4 dimension fix.
- Launcher pass-through. D3DPERF_SetOptions no-op.

### Documentation
- README.md, USAGE.md, CHANGELOG.md
- d3d11/README.md, d3d9/README.md (architecture and build guides)

## Planned
- Per-game tuning profiles
- D3D12 port (9 device interfaces, command-list interception, explicit residency control)
