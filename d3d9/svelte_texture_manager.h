// svelte_texture_manager.h - System Memory Backing + Stripped VRAM Copy
// Works at ANY resolution.
//
// Game calls CreateTexture(256, 256, 9, D3DPOOL_DEFAULT):
// 1. sysmemTex = CreateTexture(256, 256, 9, D3DPOOL_SYSTEMMEM) -- returned to game
// 2. stagingTex = CreateTexture(128, 128, 8, D3DPOOL_SYSTEMMEM) -- staging buffer
// 3. vramTex = CreateTexture(128, 128, 8, D3DPOOL_DEFAULT) -- rendering
// 4. Return sysmemTex to game
//
// Game calls SetTexture(stage, sysmemTex):
// 1. Copy sysmemTex -> stagingTex (LockRect+memcpy on both sysmem, always works)
// 2. UpdateTexture(stagingTex, vramTex) (D3D9 copies sysmem -> VRAM)
// 3. m_real->SetTexture(stage, vramTex) (bind VRAM texture)
//
// Game calls GetLevelDesc/GetLevelCount/LockRect on sysmemTex:
// Returns FULL dimensions. No crash.
#pragma once
#include <d3d9.h>

struct TexPair {
 IDirect3DTexture9* sysmemTex; // full-size, D3DPOOL_SYSTEMMEM, returned to game
 IDirect3DTexture9* stagingTex; // stripped-size, D3DPOOL_SYSTEMMEM, staging buffer
 IDirect3DTexture9* vramTex; // stripped-size, D3DPOOL_DEFAULT, used for rendering
 UINT origWidth;
 UINT origHeight;
 UINT origLevels;
 UINT strippedWidth;
 UINT strippedHeight;
 UINT strippedLevels;
 D3DFORMAT format;
 int mipsStripped;
 bool dirty; // true if sysmem modified since last VRAM copy
 long long lastAccess; // frame count of last SetTexture (for LRU)
};

// LRU eviction - call periodically to free old TexPairs
void MaybeSweepTexPairs(long long currentFrame);

HRESULT CreateStrippedPair(
 IDirect3DDevice9* realDevice,
 UINT origWidth, UINT origHeight, UINT origLevels,
 DWORD Usage, D3DFORMAT Format,
 UINT strippedWidth, UINT strippedHeight, UINT strippedLevels,
 int mipsStripped,
 IDirect3DTexture9** outSysmem
);

HRESULT CopySysmemToVRAM(IDirect3DDevice9* device, TexPair* pair);

TexPair* LookupTexPair(IDirect3DBaseTexture9* tex);

void UnregisterTexPair(IDirect3DBaseTexture9* tex);