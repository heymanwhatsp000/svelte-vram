// svelte_registry.h - Stripped texture registry for D3D9
// Tracks every D3D9 texture that Svelte has stripped mips from, along with
// its original dimensions and mip count. Used by WrappedTexture9 to maintain
// the API-surface illusion (game thinks texture has original mips).
//
// The registry is a safety net for leaked textures (MaybeSweepStrippedMap).
// WrappedTexture9::Release calls UnregisterStrippedTexture on destruction.
// PrepareForDeviceReset also unregisters DEFAULT-pool textures before release.
//
// D3D9 vs D3D11 difference:
// D3D11 needs SRV clamping (CreateShaderResourceView) - D3D9 doesn't
// have SRVs. D3D9 binds textures directly via SetTexture(). The wrapper
// (WrappedTexture9) handles the API-surface lie instead.
#pragma once
#include <d3d9.h>
#include <atomic>

struct StrippedTexEntry {
 UINT originalLevels; // what the game thinks the mip count is
 UINT originalWidth;
 UINT originalHeight;
 UINT mipsStripped;   // how many mips were stripped (for level mapping)
 D3DFORMAT format;
};

void RegisterStrippedTexture(IDirect3DBaseTexture9* tex, const StrippedTexEntry& entry);
void UnregisterStrippedTexture(IDirect3DBaseTexture9* tex);

// LRU sweep for the stripped-texture registry. Called from Present every
// 300 frames. If the map exceeds MAX_STRIPPED_ENTRIES, clears all entries
// (nuclear sweep). This is a safety net for games that leak textures
// (never call Release on WrappedTexture9).
//
// NOTE: WrappedTexture9::Release calls UnregisterStrippedTexture, so
// properly-released textures are already cleaned up. This sweep only
// catches leaked textures.
void MaybeSweepStrippedMap(long long currentFrame);

// Constants (also used by svelte_registry.cpp)
static const size_t MAX_STRIPPED_ENTRIES = 50000;
static const long long LRU_CUTOFF_FRAMES = 18000; // ~5 min at 60fps