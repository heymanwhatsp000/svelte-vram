// svelte_strip.cpp - principled mip selection + StripMips
#include "svelte_strip.h"
#include "svelte_util.h"
#include "svelte_registry.h" // for RegisterStrippedTexture

// StripMips - the main texture reduction function
// Safety filter - skip anything that might break.
// TODO: test with more games to find edge cases.
// 15-condition safety filter. Each condition has a documented reason.
// If ANY condition is true, we skip this texture (return 0).
//
// The conditions are ordered by frequency of occurrence (most common first)
// to minimize average check time. But readability matters more than
// micro-optimization here - the order is: mip checks → dimension checks →
// format checks → bind flag checks → usage checks → exclusion check.
int StripMips(const D3D11_TEXTURE2D_DESC* pDesc,
 const D3D11_SUBRESOURCE_DATA* pInitialData,
 D3D11_TEXTURE2D_DESC* outDesc,
 D3D11_SUBRESOURCE_DATA** outDataArray) {
 if (!g_enabled) return 0;
 if (!pInitialData) return 0; // Game defers upload via UpdateSubresource - can't intercept
 if (!outDataArray) return 0;
 *outDataArray = NULL;

 // (logging disabled in shipped build)
 if (g_logLevel >= 3) {
 Log("CreateTex: %ux%u/%u fmt=%s arraySize=%u bindFlags=0x%X usage=%d cpuAccess=0x%X miscFlags=0x%X",
 pDesc->Width, pDesc->Height, pDesc->MipLevels, FormatName(pDesc->Format),
 pDesc->ArraySize, pDesc->BindFlags, pDesc->Usage, pDesc->CPUAccessFlags, pDesc->MiscFlags);
 }

 // Safety filter: 15 conditions ──
 // Each condition protects a specific class of texture that must NOT be stripped.

 // Condition 1: No mip chain to strip
 if (pDesc->MipLevels <= 1) {
 if (g_logLevel >= 2) Log(" skip: MipLevels<=1 (%ux%u)", pDesc->Width, pDesc->Height);
 return 0;
 }
 // Condition 2: Sanity cap (no real texture has >16 mips; 8K = 14 mips max)
 if (pDesc->MipLevels > 16) {
 if (g_logLevel >= 2) Log(" skip: MipLevels>16 (%u)", pDesc->MipLevels);
 return 0;
 }
 // Condition 3: Auto-gen mips (game plans to generate mip chain itself)
 if (pDesc->MipLevels == 0) {
 if (g_logLevel >= 2) Log(" skip: MipLevels==0 (auto-gen)");
 return 0;
 }
 // Condition 4: Texture arrays (multiple textures in one resource)
 if (pDesc->ArraySize > 1) {
 if (g_logLevel >= 2) Log(" skip: ArraySize>1 (%u)", pDesc->ArraySize);
 return 0;
 }
 // Condition 5: Cube maps (skyboxes - 6 faces, must stay uniform)
 if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
 if (g_logLevel >= 2) Log(" skip: TEXTURECUBE");
 return 0;
 }
 // Condition 6+7: Too small to strip
 if (pDesc->Width < (UINT)g_minTexDim || pDesc->Height < (UINT)g_minTexDim) {
 if (g_logLevel >= 2) Log(" skip: dim < min_texture_dimension (%ux%u < %d)",
 pDesc->Width, pDesc->Height, g_minTexDim);
 return 0;
 }
 // Condition 8: Not a BCn format (uncompressed, BC2, BC4, BC6H)
 if (!IsBCnFormat(pDesc->Format)) {
 if (g_logLevel >= 2) Log(" skip: not BCn (fmt=%u)", pDesc->Format);
 return 0;
 }
 // Condition 9: Render target (game renders INTO this texture - can't shrink)
 if (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) {
 if (g_logLevel >= 2) Log(" skip: RENDER_TARGET");
 return 0;
 }
 // Condition 10: Depth stencil (depth buffer - critical for rendering)
 if (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) {
 if (g_logLevel >= 2) Log(" skip: DEPTH_STENCIL");
 return 0;
 }
 // Condition 11: Unordered access (compute shader writes - can't shrink)
 if (pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
 if (g_logLevel >= 2) Log(" skip: UNORDERED_ACCESS");
 return 0;
 }
 // Condition 12: Generate mips flag (game auto-generates mip chain)
 if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) {
 if (g_logLevel >= 2) Log(" skip: GENERATE_MIPS");
 return 0;
 }
 // Condition 13: Staging texture (CPU-side buffer for copying - not in VRAM)
 if (pDesc->Usage == D3D11_USAGE_STAGING) {
 if (g_logLevel >= 2) Log(" skip: STAGING");
 return 0;
 }
 // Condition 14: Dynamic texture (game plans to UpdateSubresource frequently)
 if (pDesc->Usage == D3D11_USAGE_DYNAMIC) {
 if (g_logLevel >= 2) Log(" skip: DYNAMIC");
 return 0;
 }
 // Condition 15: CPU write access (game plans to write - can't shrink safely)
 if (pDesc->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) {
 if (g_logLevel >= 2) Log(" skip: CPU_ACCESS_WRITE");
 return 0;
 }

 // Exclusion list check
 if (IsExcluded(pDesc)) {
 if (g_logLevel >= 2) Log(" skip: EXCLUSION LIST match (%ux%u_%s_mip%u)",
 pDesc->Width, pDesc->Height, FormatName(pDesc->Format), pDesc->MipLevels);
 g_texturesSkippedByExclusion.fetch_add(1);
 return 0;
 }

 // Compute mips to strip ──
 // Single mode. Strip until texture <= max_resolution.
 // No modes, no principled, no content analysis. Just the number.
 int maxDim = (pDesc->Width > pDesc->Height) ? (int)pDesc->Width : (int)pDesc->Height;
 int mipsToStrip = 0;
 int dim = maxDim;
 while (dim > g_maxResolution && mipsToStrip < (int)pDesc->MipLevels - 1) {
 dim >>= 1;
 mipsToStrip++;
 }

 if (g_logLevel >= 2) {
 Log(" strip: %ux%u/%u → strip %d mips (target %d, result %dx%d)",
 pDesc->Width, pDesc->Height, pDesc->MipLevels, mipsToStrip,
 g_maxResolution, maxDim >> mipsToStrip, maxDim >> mipsToStrip);
 }

 if (mipsToStrip == 0) {
 if (g_logLevel >= 2) Log(" skip: already <= max_resolution (%ux%u <= %d)",
 pDesc->Width, pDesc->Height, g_maxResolution);
 return 0;
 }

 // Compute new dimensions after stripping
 int newW = (int)pDesc->Width >> mipsToStrip;
 int newH = (int)pDesc->Height >> mipsToStrip;
 if (newW < 4 || newH < 4) {
 if (g_logLevel >= 2) Log(" skip: would be too small (%dx%d)", newW, newH);
 return 0;
 }

 int newMips = (int)pDesc->MipLevels - mipsToStrip;

 // Build modified descriptor ──
 *outDesc = *pDesc;
 outDesc->Width = (UINT)newW;
 outDesc->Height = (UINT)newH;
 outDesc->MipLevels = (UINT)newMips;

 // Copy the stripped pInitialData subarray ──
 // important:
 // D3D11 reads MipLevels entries from the pInitialData pointer.
 // passed a single SUBRESOURCE_DATA struct → D3D11 read past it
 // for textures with >1 mip → E_INVALIDARG every time.
 //
 // Fix: copy the stripped subarray (mips [mipsToStrip..MipLevels-1])
 // into a thread_local buffer. thread_local because D3D11 can call
 // CreateTexture2D from multiple threads (deferred contexts, worker
 // threads). A static buffer would race. thread_local gives each
 // thread its own copy - zero contention, no locks needed.
 //
 // The buffer is sized [16] because no real texture has >16 mips
 // (8K texture = 14 mips max). The safety filter already rejects
 // MipLevels > 16 (condition 2 above).
 static thread_local D3D11_SUBRESOURCE_DATA buf[16];
 for (int i = 0; i < newMips && i < 16; i++) {
 buf[i] = pInitialData[mipsToStrip + i];
 }
 *outDataArray = buf;

 if (g_logLevel >= 2) {
 Log(" pInitialData: shifted %d entries (mips %d-%d -> 0-%d), %d bytes copied",
 mipsToStrip, mipsToStrip, pDesc->MipLevels - 1, newMips - 1,
 newMips * (int)sizeof(D3D11_SUBRESOURCE_DATA));
 }

 return mipsToStrip;
}
