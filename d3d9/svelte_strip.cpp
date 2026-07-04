// svelte_strip.cpp - StripMips logic for D3D9
#include "svelte_strip.h"
#include "svelte_util.h"

// StripMips9 - the D3D9 texture reduction function
// Safety filter (mirrors D3D11's 15 conditions, adapted for D3D9):
// 1. Must be a 2D texture (D3DRTYPE_TEXTURE) - not cube or volume
// 2. Must have mips (Levels > 1)
// 3. Sanity cap (Levels <= 16)
// 4. Not a render target (Usage & D3DUSAGE_RENDERTARGET)
// 5. Not a depth stencil (Usage & D3DUSAGE_DEPTHSTENCIL)
// 6. Not dynamic (Usage & D3DUSAGE_DYNAMIC)
// 7. Format must be DXT1/DXT3/DXT5 (BCn)
// 8. Width/Height >= min_texture_dimension
//
// Same simplified loop as D3D11
// while (dim > max_resolution) { dim >>= 1; mipsToStrip++; }
int StripMips9(const D3D9_TEXTURE_DESC* pDesc,
 UINT* outLevels, UINT* outWidth, UINT* outHeight) {
 if (!g_enabled) return 0;
 if (!pDesc || !outLevels || !outWidth || !outHeight) return 0;

 *outLevels = pDesc->Levels;
 *outWidth = pDesc->Width;
 *outHeight = pDesc->Height;

 // Condition 1: Must be a 2D texture
 if (pDesc->Type != D3DRTYPE_TEXTURE) {
 if (g_logLevel >= 2) Log(" skip: not D3DRTYPE_TEXTURE (type=%u)", pDesc->Type);
 return 0;
 }

 // Condition 2: Must have mips (Levels=0 means "default" = autogen, skip)
 // D3D9 Levels=0 means "all mips down to 1x1" - but the actual count is
 // computed by the driver. We can't strip what we can't count.
 if (pDesc->Levels <= 1) {
 if (g_logLevel >= 2) Log(" skip: Levels<=1 (%ux%u)", pDesc->Width, pDesc->Height);
 return 0;
 }

 // Condition 3: Sanity cap
 if (pDesc->Levels > 16) {
 if (g_logLevel >= 2) Log(" skip: Levels>16 (%u)", pDesc->Levels);
 return 0;
 }

 // Condition 4: Render target
 if (pDesc->Usage & D3DUSAGE_RENDERTARGET) {
 if (g_logLevel >= 2) Log(" skip: RENDERTARGET");
 return 0;
 }

 // Condition 5: Depth stencil
 if (pDesc->Usage & D3DUSAGE_DEPTHSTENCIL) {
 if (g_logLevel >= 2) Log(" skip: DEPTHSTENCIL");
 return 0;
 }

 // Condition 6: Dynamic
 if (pDesc->Usage & D3DUSAGE_DYNAMIC) {
 if (g_logLevel >= 2) Log(" skip: DYNAMIC");
 return 0;
 }

 // Condition 7: Must be BCn (DXT1/DXT3/DXT5)
 if (!IsStrippableFormat(pDesc->Format)) {
 if (g_logLevel >= 2) Log(" skip: not BCn (fmt=%s)", FormatName(pDesc->Format));
 return 0;
 }

 // Condition 8: Dimensions check
 if (pDesc->Width < (UINT)g_minTexDim || pDesc->Height < (UINT)g_minTexDim) {
 if (g_logLevel >= 2) Log(" skip: dim < min (%ux%u < %d)", pDesc->Width, pDesc->Height, g_minTexDim);
 return 0;
 }

 // Compute mips to strip ──
 int maxDim = (pDesc->Width > pDesc->Height) ? (int)pDesc->Width : (int)pDesc->Height;
 int mipsToStrip = 0;
 int dim = maxDim;
 while (dim > g_maxResolution && mipsToStrip < (int)pDesc->Levels - 1) {
 dim >>= 1;
 mipsToStrip++;
 }

 if (mipsToStrip == 0) {
 if (g_logLevel >= 2) Log(" skip: already <= max_resolution (%ux%u <= %d)",
 pDesc->Width, pDesc->Height, g_maxResolution);
 return 0;
 }

 // Compute new dimensions
 int newW = (int)pDesc->Width >> mipsToStrip;
 int newH = (int)pDesc->Height >> mipsToStrip;
 if (newW < 4 || newH < 4) {
 if (g_logLevel >= 2) Log(" skip: would be too small (%dx%d)", newW, newH);
 return 0;
 }

 int newLevels = (int)pDesc->Levels - mipsToStrip;

 // fix: BCn formats require dimensions to be multiples of 4.
 // Non-power-of-2 textures (like 1920x1080) can produce non-multiple-of-4
 // results when stripped (1920>>4=120, which is not a multiple of 4).
 // D3D9 rejects these with E_INVALIDARG. Fix: strip one fewer mip until
 // both dimensions are multiples of 4.
 while (mipsToStrip > 0 && ((newW % 4) != 0 || (newH % 4) != 0)) {
 mipsToStrip--;
 newW = (int)pDesc->Width >> mipsToStrip;
 newH = (int)pDesc->Height >> mipsToStrip;
 newLevels = (int)pDesc->Levels - mipsToStrip;
 }
 // If even 0 strips gives non-multiple-of-4, skip entirely
 if ((newW % 4) != 0 || (newH % 4) != 0) {
 if (g_logLevel >= 2) Log(" skip: non-multiple-of-4 (%dx%d)", newW, newH);
 return 0;
 }

 *outLevels = (UINT)newLevels;
 *outWidth = (UINT)newW;
 *outHeight = (UINT)newH;

 if (g_logLevel >= 2) {
 Log(" strip: %ux%u/%u → strip %d mips (target %d, result %ux%u/%u)",
 pDesc->Width, pDesc->Height, pDesc->Levels, mipsToStrip,
 g_maxResolution, newW, newH, newLevels);
 }

 return mipsToStrip;
}