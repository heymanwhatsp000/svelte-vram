// svelte_registry.cpp - Stripped texture registry for D3D9
#include "svelte_registry.h"
#include "svelte_util.h" // for Log()
#include <unordered_map>

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<IDirect3DBaseTexture9*, StrippedTexEntry> g_map;

void RegisterStrippedTexture(IDirect3DBaseTexture9* tex, const StrippedTexEntry& entry) {
 if (!tex) return;
 AcquireSRWLockExclusive(&g_lock);
 g_map[tex] = entry;
 ReleaseSRWLockExclusive(&g_lock);
}

void UnregisterStrippedTexture(IDirect3DBaseTexture9* tex) {
 if (!tex) return;
 AcquireSRWLockExclusive(&g_lock);
 g_map.erase(tex);
 ReleaseSRWLockExclusive(&g_lock);
}

// LRU sweep — safety net for leaked textures.
// If the map exceeds MAX_STRIPPED_ENTRIES, clear everything. The wrappers
// still hold their own copy of the entry (m_entry), so clearing the registry
// doesn't affect correctness — it just frees the tracking metadata.
// WrappedTexture9::Release calls UnregisterStrippedTexture, so properly-
// released textures are already cleaned up. This sweep only catches leaks.
void MaybeSweepStrippedMap(long long currentFrame) {
 AcquireSRWLockExclusive(&g_lock);
 size_t size = g_map.size();
 if (size <= MAX_STRIPPED_ENTRIES) {
 ReleaseSRWLockExclusive(&g_lock);
 return;
 }

 Log("Stripped-texture map LRU sweep: %zu entries -> 0 (nuclear sweep, frame=%lld)",
 size, currentFrame);
 g_map.clear();
 ReleaseSRWLockExclusive(&g_lock);
}