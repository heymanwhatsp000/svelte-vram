// svelte_registry.h - Stripped texture registry for D3D9
// Tracks every D3D9 texture that Svelte has stripped mips from.
// Used to know the actual (reduced) mip count when the game queries
// the texture's level count or tries to lock a mip surface.
//
// D3D9 vs D3D11 difference:
// D3D11 needs SRV clamping (CreateShaderResourceView) - D3D9 doesn't
// have SRVs. D3D9 binds textures directly via SetTexture(). So the
// registry is used differently here:
// - GetLevelCount() override → return original count (lie to game)
// - LockRect() override → redirect mip index if needed
// - GetSurfaceLevel() override → return dummy surface for stripped mips
//
// This is the hard part of the D3D9 port (see README "LockRect interception").
#pragma once
#include <d3d9.h>
#include <atomic>

struct StrippedTexEntry {
 UINT originalLevels; // what the game thinks the mip count is
 UINT actualLevels; // what the real (stripped) texture has
 UINT originalWidth;
 UINT originalHeight;
 D3DFORMAT format;
};

void RegisterStrippedTexture(IDirect3DBaseTexture9* tex, const StrippedTexEntry& entry);
StrippedTexEntry* LookupStrippedTexture(IDirect3DBaseTexture9* tex); // returns nullptr if not stripped
void UnregisterStrippedTexture(IDirect3DBaseTexture9* tex);