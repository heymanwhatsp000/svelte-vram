// svelte_registry.cpp - Stripped texture registry for D3D9
#include "svelte_registry.h"
#include <unordered_map>

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<IDirect3DBaseTexture9*, StrippedTexEntry> g_map;

void RegisterStrippedTexture(IDirect3DBaseTexture9* tex, const StrippedTexEntry& entry) {
 if (!tex) return;
 AcquireSRWLockExclusive(&g_lock);
 g_map[tex] = entry;
 ReleaseSRWLockExclusive(&g_lock);
}

StrippedTexEntry* LookupStrippedTexture(IDirect3DBaseTexture9* tex) {
 if (!tex) return nullptr;
 AcquireSRWLockShared(&g_lock);
 auto it = g_map.find(tex);
 if (it != g_map.end()) {
 ReleaseSRWLockShared(&g_lock);
 return &it->second;
 }
 ReleaseSRWLockShared(&g_lock);
 return nullptr;
}

void UnregisterStrippedTexture(IDirect3DBaseTexture9* tex) {
 if (!tex) return;
 AcquireSRWLockExclusive(&g_lock);
 g_map.erase(tex);
 ReleaseSRWLockExclusive(&g_lock);
}