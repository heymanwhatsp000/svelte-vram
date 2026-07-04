// svelte_strip.h - principled mip selection + StripMips function
// It replaces the hardcoded 2-mip cap with principled math.
//
// MATH LEVEL: Public (principled-Shannon sampling theorem, 1928/1949)
// RE RISK: Zero - this is textbook signal processing, taught in any DSP 101 course.
//
// The math in 30 seconds:
// A mip chain is an octave-band decomposition. Each level L has:
// resolution = base_width / 2^L
// highest_frequency = resolution / 2 (cycles per texture width)
// The screen has a principled limit:
// observer_principled = min(screen_width, screen_height) / 2
// A mip level is EVICTABLE (sub-pixel, invisible) if:
// highest_frequency > observer_principled
// We strip evictable levels. The highest "needed" level becomes the new top.
// Plus anisotropic_bonus: keep +1 level above principled for AF safety.
#pragma once
#include <d3d11.h>

// Select how many mips to strip using principled criterion.
// Returns the number of mips to strip (0 = no stripping).
//
// Parameters:
// pDesc - the texture descriptor (Width, Height, MipLevels)
// screenWidth - current screen width (from swap chain)
// screenHeight - current screen height (from swap chain)
// anisotropicBonus - extra levels to keep above principled (1 = keep 1 extra for AF)
//
// Example: 8192x8192 texture on 1920x1080 screen:
// observer_principled = min(1920, 1080) / 2 = 540
// Level 0: 8192/2 = 4096 > 540 → evictable
// Level 1: 4096/2 = 2048 > 540 → evictable
// Level 2: 2048/2 = 1024 > 540 → evictable
// Level 3: 1024/2 = 512 < 540 → needed (stop)
// principledStrips = 3, with anisotropic_bonus=1 → strip 2
// (Without bonus: strip 3 = 87.5% savings on 8K texture)
int SelectMipsToStrip_principled(const D3D11_TEXTURE2D_DESC* pDesc,
 int screenWidth, int screenHeight,
 int anisotropicBonus);

// The main StripMips function - called by WrappedDevice::CreateTexture2D.
// Evaluates the 15-condition safety filter, then uses principled (or
// falls back to 2-mip heuristic if principled is disabled).
//
// Returns the number of mips stripped (0 = no change).
// Fills outDesc with the modified descriptor and *outDataArray with
// a pointer to a thread-local array of newMips SUBRESOURCE_DATA entries.
//
// important: The pInitialData array must be handled correctly.
// D3D11 reads MipLevels entries from the pInitialData pointer.
// bug: passed only ONE SUBRESOURCE_DATA struct → D3D11 read past it
// → E_INVALIDARG every time.
// fix: copy the stripped subarray into a thread_local buffer.
// thread_local is required because D3D11 can call CreateTexture2D from
// multiple threads (deferred contexts, worker threads). A static buffer
// would race. thread_local gives each thread its own copy - zero contention.
int StripMips(const D3D11_TEXTURE2D_DESC* pDesc,
 const D3D11_SUBRESOURCE_DATA* pInitialData,
 D3D11_TEXTURE2D_DESC* outDesc,
 D3D11_SUBRESOURCE_DATA** outDataArray);
