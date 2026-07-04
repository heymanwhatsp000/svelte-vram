// svelte_registry.h - Stripped texture registry with LRU eviction
// Tracks every texture that Svelte has stripped mips from, along with
// its actual (reduced) mip count. Used by CreateShaderResourceView to
// clamp the SRV's mip range - without this, the game requests non-existent
// mip levels → E_INVALIDARG → null SRV → black texture (the ).
//
// IMPROVEMENTS over
// 1. SRWLOCK instead of important_SECTION - readers (CreateShaderResourceView)
// acquire shared lock, writers (CreateTexture2D) acquire exclusive.
// Multiple SRV lookups can proceed in parallel. In, all calls
// serialized through one important_SECTION.
//
// 2. LRU eviction instead of clear-all - when the map exceeds
// MAX_STRIPPED_ENTRIES, we evict entries not accessed in the last
// 900 frames (~15 seconds at 60fps) instead of clearing everything.
// Frequently-used textures stay in the map; stale entries expire.
//
// 3. WrappedTexture hooks Release() - when the game releases a stripped
// texture, the WrappedTexture destructor removes the entry from the
// map immediately. This eliminates the "texture pointer reuse" bug
// where the OS could reuse a freed pointer for a new texture, causing
// the map to incorrectly clamp the new texture's SRV.
#pragma once
#include <d3d11.h>
#include <atomic>

// Maximum entries before LRU eviction kicks in.
// 50000 entries × ~32 bytes per entry = ~1.6 MB memory cap.
// In practice, a 4-hour Skyrim session creates ~2000 stripped textures,
// so this cap is rarely hit - it's a safety net for leaky games.
static const size_t MAX_STRIPPED_ENTRIES = 50000;

// LRU cutoff: evict entries not accessed in this many frames.
// 900 frames = 3 sweep cycles (sweep runs every 300 frames) = ~15 sec at 60fps.
// Rationale: if a texture hasn't been used for an SRV in 15 seconds, the game
// has probably moved on to a different area/scene. Safe to forget.
static const long long LRU_CUTOFF_FRAMES = 900;

// Register a stripped texture in the map.
// Called by WrappedDevice::CreateTexture2D after successful strip.
// Thread-safe (acquires exclusive SRWLOCK).
void RegisterStrippedTexture(ID3D11Texture2D* tex, UINT newMipCount);

// Look up a texture's actual mip count (for SRV clamping).
// Called by WrappedDevice::CreateShaderResourceView.
// Returns 0 if the texture is not in the map (not stripped).
// Updates lastAccessFrame for LRU tracking.
// Thread-safe (acquires shared SRWLOCK for lookup, brief exclusive for timestamp update).
UINT LookupStrippedMipCount(ID3D11Resource* resource);

// Remove a texture from the map (when it's released).
// Called by WrappedTexture::Release() when refcount hits 0.
// Thread-safe (acquires exclusive SRWLOCK).
void UnregisterStrippedTexture(ID3D11Texture2D* tex);

// Periodic cleanup. Called from WrappedSwapChain::Present every 300 frames.
// If map size > MAX_STRIPPED_ENTRIES, evict LRU entries (not accessed in
// LRU_CUTOFF_FRAMES).
void MaybeSweepStrippedMap();
