// svelte_registry.cpp - Stripped texture registry with LRU eviction
#include "svelte_registry.h"
#include "svelte_util.h"
#include <unordered_map>
#include <mutex>

// Registry data structure
struct StrippedEntry {
 UINT mipCount; // actual (reduced) mip count after stripping
 long long lastAccessFrame; // updated on each SRV lookup (for LRU)
 // NOTE: Not using std::atomic here because all reads/writes happen
 // under the SRWLOCK. Using a plain long long allows copy assignment
 // (needed by unordered_map::operator[]). The atomic would delete the
 // copy constructor, breaking map insertion.
};

// The map: texture pointer → entry.
// Key is ID3D11Texture2D* (the real texture, or WrappedTexture* if we wrap them).
// In, we store WrappedTexture* as the key - when WrappedTexture is
// destroyed, it calls UnregisterStrippedTexture to remove itself.
static std::unordered_map<ID3D11Texture2D*, StrippedEntry> g_strippedTextures;

// SRWLOCK (Slim Reader-Writer Lock) - replaces v1.0's important_SECTION.
// Rationale:
// - CreateShaderResourceView (READ) is called frequently → shared lock
// - CreateTexture2D (WRITE) is called less frequently → exclusive lock
// - Multiple SRV lookups can proceed in parallel (read-read no contention)
// - important_SECTION serialized ALL access, even read-read
// - SRWLOCK is ~20 bytes, important_SECTION is ~40 bytes
// - SRWLOCK is the modern Win32 recommendation for read-heavy patterns
static SRWLOCK g_strippedLock = SRWLOCK_INIT;

// Register - called after successful strip in CreateTexture2D
void RegisterStrippedTexture(ID3D11Texture2D* tex, UINT newMipCount) {
 if (!tex) return;
 AcquireSRWLockExclusive(&g_strippedLock);
 StrippedEntry entry;
 entry.mipCount = newMipCount;
 entry.lastAccessFrame = g_frameCount.load();
 g_strippedTextures[tex] = entry;
 ReleaseSRWLockExclusive(&g_strippedLock);
}

// Lookup - called from CreateShaderResourceView
// Uses shared lock (read) for the map lookup, then a brief exclusive lock
// to update the lastAccessFrame timestamp.
//
// Alternative considered: use shared lock only and skip the timestamp update
// if we can't get exclusive quickly. But the timestamp is important for LRU
// accuracy - stale entries would be evicted prematurely. The brief exclusive
// lock is worth the correctness.
UINT LookupStrippedMipCount(ID3D11Resource* resource) {
 if (!resource) return 0;

 // Try to get an ID3D11Texture2D interface from the resource.
 // The resource might be a 1D or 3D texture (which we don't strip),
 // in which case QI fails and we return 0.
 ID3D11Texture2D* tex = NULL;
 HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
 if (FAILED(hr) || !tex) return 0;

 UINT actualMips = 0;
 AcquireSRWLockShared(&g_strippedLock);
 auto it = g_strippedTextures.find(tex);
 if (it != g_strippedTextures.end()) {
 actualMips = it->second.mipCount;
 }
 ReleaseSRWLockShared(&g_strippedLock);

 // Update last access frame for LRU (only if found)
 if (actualMips > 0) {
 AcquireSRWLockExclusive(&g_strippedLock);
 auto it2 = g_strippedTextures.find(tex);
 if (it2 != g_strippedTextures.end()) {
 it2->second.lastAccessFrame = g_frameCount.load();
 }
 ReleaseSRWLockExclusive(&g_strippedLock);
 }

 tex->Release();
 return actualMips;
}

// Unregister - called when a texture is released
void UnregisterStrippedTexture(ID3D11Texture2D* tex) {
 if (!tex) return;
 AcquireSRWLockExclusive(&g_strippedLock);
 g_strippedTextures.erase(tex);
 ReleaseSRWLockExclusive(&g_strippedLock);
}

// LRU sweep - called every 300 frames from Present()
// approach: if map > MAX, clear everything.
// approach: if map > MAX, evict entries not accessed in LRU_CUTOFF_FRAMES.
//
// This is smarter because:
// 1. Frequently-used textures (player's current area) stay in the map
// 2. Only stale entries (old area, already left) are evicted
// 3. If the game doesn't leak textures, the map stays small naturally
// (WrappedTexture removes entries on Release)
// 4. The sweep is a safety net for leaky games, not the primary cleanup
void MaybeSweepStrippedMap() {
 AcquireSRWLockExclusive(&g_strippedLock);

 size_t size = g_strippedTextures.size();
 if (size <= MAX_STRIPPED_ENTRIES) {
 ReleaseSRWLockExclusive(&g_strippedLock);
 return;
 }

 // Map is too large - evict stale entries
 long long cutoff = g_frameCount.load() - LRU_CUTOFF_FRAMES;
 size_t evicted = 0;
 for (auto it = g_strippedTextures.begin(); it != g_strippedTextures.end(); ) {
 if (it->second.lastAccessFrame < cutoff) {
 it = g_strippedTextures.erase(it);
 evicted++;
 } else {
 ++it;
 }
 }

 g_mapSweeps.fetch_add(1);
 g_mapLRUEvictions.fetch_add((long long)evicted);

 if (g_logLevel >= 1) {
 Log("Stripped-texture map LRU sweep: %zu entries → %zu entries (%zu evicted, sweep #%lld)",
 size, g_strippedTextures.size(), evicted, g_mapSweeps.load());
 }

 ReleaseSRWLockExclusive(&g_strippedLock);
}
