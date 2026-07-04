// svelte_texture_manager.cpp - System Memory Backing implementation
#include "svelte_texture_manager.h"
#include "svelte_util.h"
#include <unordered_map>
#include <cstring>

static SRWLOCK g_pairLock = SRWLOCK_INIT;
static std::unordered_map<IDirect3DBaseTexture9*, TexPair*> g_pairMap;

TexPair* LookupTexPair(IDirect3DBaseTexture9* tex) {
 AcquireSRWLockShared(&g_pairLock);
 auto it = g_pairMap.find(tex);
 if (it != g_pairMap.end()) {
 ReleaseSRWLockShared(&g_pairLock);
 return it->second;
 }
 ReleaseSRWLockShared(&g_pairLock);
 return NULL;
}

void UnregisterTexPair(IDirect3DBaseTexture9* tex) {
 AcquireSRWLockExclusive(&g_pairLock);
 auto it = g_pairMap.find(tex);
 if (it != g_pairMap.end()) {
 TexPair* pair = it->second;
 if (pair->vramTex) pair->vramTex->Release();
 if (pair->stagingTex) pair->stagingTex->Release();
 // sysmemTex is released by the game (it holds the pointer)
 g_pairMap.erase(it);
 delete pair;
 }
 ReleaseSRWLockExclusive(&g_pairLock);
}

// hack: D3D9 makes this way harder than it should be.
// CreateStrippedPair - create sysmem (full) + staging (stripped) + VRAM (stripped)
HRESULT CreateStrippedPair(
 IDirect3DDevice9* realDevice,
 UINT origWidth, UINT origHeight, UINT origLevels,
 DWORD Usage, D3DFORMAT Format,
 UINT strippedWidth, UINT strippedHeight, UINT strippedLevels,
 int mipsStripped,
 IDirect3DTexture9** outSysmem)
{
 if (!realDevice || !outSysmem) return E_POINTER;
 *outSysmem = NULL;

 // 1. Create VRAM texture (D3DPOOL_DEFAULT, stripped dims)
 IDirect3DTexture9* vramTex = NULL;
 HRESULT hr = realDevice->CreateTexture(
 strippedWidth, strippedHeight, strippedLevels,
 Usage, Format, D3DPOOL_DEFAULT, &vramTex, NULL);
 if (FAILED(hr) || !vramTex) {
 Log(" Texture pair: FAILED to create VRAM (hr=0x%08X, %ux%u/%u)", hr, strippedWidth, strippedHeight, strippedLevels);
 return hr;
 }

 // 2. Create staging sysmem texture (D3DPOOL_SYSTEMMEM, stripped dims)
 IDirect3DTexture9* stagingTex = NULL;
 hr = realDevice->CreateTexture(
 strippedWidth, strippedHeight, strippedLevels,
 Usage, Format, D3DPOOL_SYSTEMMEM, &stagingTex, NULL);
 if (FAILED(hr) || !stagingTex) {
 Log(" Texture pair: FAILED to create staging (hr=0x%08X)", hr);
 vramTex->Release();
 return hr;
 }

 // 3. Create full sysmem texture (D3DPOOL_SYSTEMMEM, full dims)
 IDirect3DTexture9* sysmemTex = NULL;
 hr = realDevice->CreateTexture(
 origWidth, origHeight, origLevels,
 Usage, Format, D3DPOOL_SYSTEMMEM, &sysmemTex, NULL);
 if (FAILED(hr) || !sysmemTex) {
 Log(" Texture pair: FAILED to create sysmem (hr=0x%08X, %ux%u/%u)", hr, origWidth, origHeight, origLevels);
 vramTex->Release();
 stagingTex->Release();
 return hr;
 }

 // 4. Create and register the pair
 TexPair* pair = new TexPair();
 pair->sysmemTex = sysmemTex;
 pair->stagingTex = stagingTex;
 pair->vramTex = vramTex;
 pair->origWidth = origWidth;
 pair->origHeight = origHeight;
 pair->origLevels = origLevels;
 pair->strippedWidth = strippedWidth;
 pair->strippedHeight = strippedHeight;
 pair->strippedLevels = strippedLevels;
 pair->format = Format;
 pair->mipsStripped = mipsStripped;
 pair->dirty = true; // start dirty (needs initial copy)
 pair->lastAccess = 0; // will be set on first SetTexture

 AcquireSRWLockExclusive(&g_pairLock);
 g_pairMap[(IDirect3DBaseTexture9*)sysmemTex] = pair;
 ReleaseSRWLockExclusive(&g_pairLock);

 *outSysmem = sysmemTex;

 if (g_logLevel >= 1) {
 Log(" Texture pair: created %ux%u/%u sysmem + %ux%u/%u staging and VRAM (stripped=%d)",
 origWidth, origHeight, origLevels,
 strippedWidth, strippedHeight, strippedLevels, mipsStripped);
 }

 return S_OK;
}

// CopySysmemToVRAM - copy via staging texture + UpdateTexture
// 1. Copy sysmemTex (full) -> stagingTex (stripped) via LockRect+memcpy
// Both are D3DPOOL_SYSTEMMEM, so LockRect always works.
// For each level L in staging: source level = L + mipsStripped in sysmem.
// Dimensions match (stripping K mips shifts levels down by K).
//
// 2. device->UpdateTexture(stagingTex, vramTex)
// D3D9 copies from D3DPOOL_SYSTEMMEM to D3DPOOL_DEFAULT internally.
// No LockRect on VRAM needed!
HRESULT CopySysmemToVRAM(IDirect3DDevice9* device, TexPair* pair) {
 if (!device || !pair || !pair->sysmemTex || !pair->stagingTex || !pair->vramTex)
 return E_POINTER;

 // Skip copy if sysmem hasn't been modified since last copy
 if (!pair->dirty) return S_OK;

 // Step 1: Copy sysmem -> staging (both sysmem, LockRect always works)
 for (UINT dstLevel = 0; dstLevel < pair->strippedLevels; dstLevel++) {
 UINT srcLevel = dstLevel + pair->mipsStripped;
 // Defensive - verify srcLevel is within sysmem mip range
 if (srcLevel >= pair->origLevels) {
 if (g_logLevel >= 2) Log(" Texture pair: srcLevel %u >= origLevels %u - skipping", srcLevel, pair->origLevels);
 continue;
 }

 D3DLOCKED_RECT srcLocked, dstLocked;
 HRESULT hr1 = pair->sysmemTex->LockRect(srcLevel, &srcLocked, NULL, D3DLOCK_READONLY);
 HRESULT hr2 = pair->stagingTex->LockRect(dstLevel, &dstLocked, NULL, 0);

 if (SUCCEEDED(hr1) && SUCCEEDED(hr2)) {
 D3DSURFACE_DESC desc;
 pair->stagingTex->GetLevelDesc(dstLevel, &desc);

 int bpb = 0;
 if (desc.Format == D3DFMT_DXT1) bpb = 8;
 else if (desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5) bpb = 16;

 if (bpb > 0) {
 UINT blocksX = (desc.Width + 3) / 4;
 UINT rowBytes = blocksX * bpb;
 UINT blocksY = (desc.Height + 3) / 4;
 for (UINT row = 0; row < blocksY; row++) {
 memcpy((BYTE*)dstLocked.pBits + row * dstLocked.Pitch,
 (BYTE*)srcLocked.pBits + row * srcLocked.Pitch,
 rowBytes);
 }
 } else {
 // Non-BCn: copy row by row
 for (UINT row = 0; row < desc.Height; row++) {
 memcpy((BYTE*)dstLocked.pBits + row * dstLocked.Pitch,
 (BYTE*)srcLocked.pBits + row * srcLocked.Pitch,
 desc.Width * 4);
 }
 }
 }

 if (SUCCEEDED(hr1)) pair->sysmemTex->UnlockRect(srcLevel);
 if (SUCCEEDED(hr2)) pair->stagingTex->UnlockRect(dstLevel);
 if (FAILED(hr1) && g_logLevel >= 2) Log(" Texture pair: sysmem LockRect FAILED (level=%u, hr=0x%08X)", srcLevel, hr1);
 if (FAILED(hr2) && g_logLevel >= 2) Log(" Texture pair: staging LockRect FAILED (level=%u, hr=0x%08X)", dstLevel, hr2);
 }

 // Step 2: UpdateTexture (staging sysmem -> VRAM)
 // D3D9 copies from SYSTEMMEM to DEFAULT internally. No LockRect on VRAM!
 HRESULT hr = device->UpdateTexture(pair->stagingTex, pair->vramTex);

 if (FAILED(hr) && g_logLevel >= 2) {
 Log(" Texture pair: UpdateTexture FAILED (hr=0x%08X)", hr);
 }

 // Mark as clean (data is now in VRAM)
 pair->dirty = false;

 return hr;
}

// LRU eviction - free TexPairs not accessed in 900 frames
static const long long TEXPAIR_LRU_CUTOFF = 900; // ~15 sec at 60fps
static const size_t TEXPAIR_MAX_ENTRIES = 5000;

void MaybeSweepTexPairs(long long currentFrame) {
 AcquireSRWLockExclusive(&g_pairLock);
 if (g_pairMap.size() <= TEXPAIR_MAX_ENTRIES) {
 ReleaseSRWLockExclusive(&g_pairLock);
 return;
 }

 long long cutoff = currentFrame - TEXPAIR_LRU_CUTOFF;
 int evicted = 0;
 for (auto it = g_pairMap.begin(); it != g_pairMap.end(); ) {
 TexPair* pair = it->second;
 if (pair->lastAccess < cutoff) {
 // Free VRAM + staging textures (sysmem is owned by the game)
 if (pair->vramTex) pair->vramTex->Release();
 if (pair->stagingTex) pair->stagingTex->Release();
 delete pair;
 it = g_pairMap.erase(it);
 evicted++;
 } else {
 ++it;
 }
 }
 ReleaseSRWLockExclusive(&g_pairLock);

 if (evicted > 0 && g_logLevel >= 1) {
 Log(" TexPair LRU sweep: evicted %d entries (cutoff frame=%lld)", evicted, cutoff);
 }
}