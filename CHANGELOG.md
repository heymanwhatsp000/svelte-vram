# Changelog

## v1.2.1

### D3D9 — performance fixes

- Fixed verbose logging enabled by default in release config (caused disk I/O during texture loading)
- Optimized SetTexture hot path: vtable comparison instead of hash lookup (zero lock contention)

### D3D9 — rewritten texture handling

D3D9 got a full rewrite of how it handles stripped textures. The old approach
was safe but heavy on RAM and stuttered badly under DXVK. The new approach
solves both problems.

- **Transient buffer approach.** Instead of keeping a full-size copy of every
  stripped texture in RAM (the old sysmem-backing method), Svelte now creates
  one stripped texture and uses temporary buffers that exist only during the
  LockRect → UnlockRect window. The buffers are freed immediately after.
  Zero permanent RAM cost.

- **Works with DXVK.** The old sysmem path did ~50 D3D9 calls per SetTexture
  to copy data around. Under DXVK, each of those became a Vulkan operation,
  causing stutter. The new approach does zero copying during SetTexture.
  Svelte auto-detects DXVK (checks for `d3d9_dxvk.dll`) and works identically
  with or without it.

- **Texture exclusion list.** You can now create `svelte_exclusions.txt` next
  to the DLL with patterns like `4096x4096` or `DXT5_mip13` to prevent
  specific textures from being stripped. Costs nothing if the file is empty
  or missing.

- **Performance logging.** `svelte.log` now includes `Perf:` lines every 300
  frames showing frame times (min/avg/max), stutter count, and how long
  texture stripping took. Useful for diagnosing issues.

- **Safer device reset.** Alt-tabbing or changing resolution no longer risks
  use-after-free crashes. Svelte releases DEFAULT-pool textures and frees any
  active transient locks before forwarding the Reset call to the driver.

- **Auto-gen mips check.** The safety filter now rejects textures with
  `D3DUSAGE_AUTOGENMIPMAP`. These were missing from the v1.0 filter and could
  cause issues if stripped.

- **Thread-safe locks.** Transient lock tracking is now protected by a
  SRWLOCK, safe for multi-threaded D3D9 devices.

## v1.1.0

- Initial open-source release.
- D3D11 proxy for Skyrim Special Edition. Validated: 75.9% VRAM reduction.
- D3D9 proxy for Fallout New Vegas. Validated: 920 MB VRAM saved per session.
- Chain DLL support (ENB, ReShade, DXVK, orig).
- BCn format whitelist (BC1/BC3/BC7 for D3D11, DXT1/DXT3/DXT5 for D3D9).
- Safety filter (15 conditions D3D11, 8 conditions D3D9).
- LRU eviction for stripped-texture registry.
