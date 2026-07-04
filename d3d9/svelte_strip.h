// svelte_strip.h - StripMips logic for D3D9
// Mirrors the D3D11 svelte_strip.h but uses D3D9 types.
// The strip logic is identical: while (dim > max_resolution) { dim >>= 1; mipsToStrip++; }
//
// Key D3D9 difference: textures are created EMPTY (no pInitialData).
// The game fills them later via LockRect/UnlockRect. So StripMips here
// only modifies the desc (Width, Height, Levels) - it doesn't touch any
// initial data. The data is uploaded later by the game, and we handle
// that in WrappedTexture9::LockRect (redirecting mip indices).
#pragma once
#include <d3d9.h>

// D3D9 doesn't have a D3D9_TEXTURE_DESC struct (unlike D3D11's D3D11_TEXTURE2D_DESC).
// We define our own to pass texture params through the safety filter.
struct D3D9_TEXTURE_DESC {
 UINT Width;
 UINT Height;
 UINT Levels;
 DWORD Usage;
 D3DFORMAT Format;
 D3DRESOURCETYPE Type;
};

// Compute how many mip levels to strip from a D3D9 texture.
// Returns 0 if the texture should not be stripped (fails safety filter).
//
// pDesc: the original D3D9 texture desc from the game
// outLevels: the new (reduced) level count
// outWidth, outHeight: the new (reduced) dimensions
int StripMips9(const D3D9_TEXTURE_DESC* pDesc,
 UINT* outLevels, UINT* outWidth, UINT* outHeight);