// svelte_wrapped_texture.cpp - WrappedTexture9 COM wrapper implementation
//
// Maintains the illusion that the game has the original number of mips,
// while the underlying real texture is smaller (stripped). Uses transient
// buffers for stripped mip levels — zero permanent RAM cost.
//
// See svelte_wrapped_texture.h for the design rationale.
#include "svelte_wrapped_texture.h"
#include "svelte_util.h"
#include "svelte_registry.h"  // for UnregisterStrippedTexture
#include <cstdlib>  // for malloc/free (transient buffers)
#include <cstring>  // for memset (READONLY buffer zeroing)

// Static members
std::unordered_set<WrappedTexture9*> WrappedTexture9::s_liveWrappers;
SRWLOCK WrappedTexture9::s_wrapperLock = SRWLOCK_INIT;
std::unordered_map<IDirect3DBaseTexture9*, WrappedTexture9*> WrappedTexture9::s_realToWrapper;

WrappedTexture9::WrappedTexture9(IDirect3DTexture9* real, const StrippedTexEntry& entry, D3DPOOL pool)
    : m_real(real), m_entry(entry), m_refCount(1), m_pool(pool), m_locksLock(SRWLOCK_INIT)
{
    // m_real's refcount was already incremented by the caller (CreateTexture
    // returned it with refcount=1, and we hold that reference).

    // Register this wrapper in the live set so SetTexture can detect it.
    // Also register in the reverse map so GetTexture can re-wrap.
    AcquireSRWLockExclusive(&s_wrapperLock);
    s_liveWrappers.insert(this);
    s_realToWrapper[(IDirect3DBaseTexture9*)m_real] = this;
    ReleaseSRWLockExclusive(&s_wrapperLock);
}

WrappedTexture9::~WrappedTexture9() {
    // Free any remaining transient locks (defensive — should be none if
    // the game called UnlockRect for every LockRect, but just in case).
    AcquireSRWLockExclusive(&m_locksLock);
    for (auto& pair : m_transientLocks) {
        if (pair.second.buffer) {
            free(pair.second.buffer);
        }
    }
    m_transientLocks.clear();
    ReleaseSRWLockExclusive(&m_locksLock);

    // Remove from the live wrapper set AND the reverse map.
    AcquireSRWLockExclusive(&s_wrapperLock);
    s_liveWrappers.erase(this);
    if (m_real) {
        s_realToWrapper.erase((IDirect3DBaseTexture9*)m_real);
    }
    ReleaseSRWLockExclusive(&s_wrapperLock);

    if (m_real) {
        m_real->Release();
        m_real = NULL;
    }
}

// Static: check if a texture pointer is a WrappedTexture9 and unwrap it.
// Returns the real underlying texture if it's a wrapper, or the input if not.
IDirect3DBaseTexture9* WrappedTexture9::UnwrapIfWrapper(IDirect3DBaseTexture9* tex) {
    if (!tex) return tex;
    AcquireSRWLockShared(&s_wrapperLock);
    // We need to check if `tex` is actually a WrappedTexture9*. Since the
    // set stores WrappedTexture9* pointers, we cast and check membership.
    // This is safe because only WrappedTexture9 constructors insert into
    // the set, and only the destructor removes.
    auto it = s_liveWrappers.find(reinterpret_cast<WrappedTexture9*>(tex));
    if (it != s_liveWrappers.end()) {
        IDirect3DTexture9* real = (*it)->m_real;
        ReleaseSRWLockShared(&s_wrapperLock);
        return real;
    }
    ReleaseSRWLockShared(&s_wrapperLock);
    return tex;
}

// Find the wrapper for a given real texture pointer.
// Used by GetTexture to re-wrap the result.
// Returns an AddRef'd wrapper pointer (caller must Release).
// AddRef is done UNDER the lock to prevent TOCTOU: without this, another
// thread could destroy the wrapper between FindByReal returning and the
// caller calling AddRef.
WrappedTexture9* WrappedTexture9::FindByReal(IDirect3DBaseTexture9* real) {
    if (!real) return NULL;
    AcquireSRWLockShared(&s_wrapperLock);
    auto it = s_realToWrapper.find(real);
    if (it != s_realToWrapper.end()) {
        WrappedTexture9* wrapper = it->second;
        wrapper->AddRef(); // AddRef UNDER the lock — prevents race
        ReleaseSRWLockShared(&s_wrapperLock);
        return wrapper;
    }
    ReleaseSRWLockShared(&s_wrapperLock);
    return NULL;
}

// Prepare all live wrappers for a device Reset.
// Called from WrappedDevice9::Reset / ResetEx BEFORE calling m_real->Reset.
// After this call, all wrappers have m_real == NULL.
// The game should Release all textures after Reset (standard D3D9 contract).
void WrappedTexture9::PrepareForDeviceReset() {
    AcquireSRWLockExclusive(&s_wrapperLock);
    for (auto it = s_liveWrappers.begin(); it != s_liveWrappers.end(); ++it) {
        WrappedTexture9* w = *it;

        // Free any active transient locks on this wrapper.
        // (Reset while a lock is active is unusual, but we must not leak.)
        AcquireSRWLockExclusive(&w->m_locksLock);
        for (auto& pair : w->m_transientLocks) {
            if (pair.second.buffer) {
                free(pair.second.buffer);
            }
        }
        w->m_transientLocks.clear();
        ReleaseSRWLockExclusive(&w->m_locksLock);

        if (w->m_real) {
            if (w->m_pool == D3DPOOL_DEFAULT) {
                // DEFAULT-pool: destroyed by Reset. Release and null m_real
                // so the wrapper returns D3DERR_INVALIDCALL if the game
                // tries to use it post-Reset.
                // Unregister from the stripped-texture registry BEFORE releasing
                // m_real, so the registry doesn't keep a dangling pointer as key.
                UnregisterStrippedTexture((IDirect3DBaseTexture9*)w->m_real);
                s_realToWrapper.erase((IDirect3DBaseTexture9*)w->m_real);
                w->m_real->Release();
                w->m_real = NULL;
            }
            // MANAGED / SYSTEMMEM: survive Reset. Do NOT release.
            // The driver re-uploads MANAGED textures to VRAM after Reset.
            // The wrapper continues to work post-Reset.
        }
    }
    ReleaseSRWLockExclusive(&s_wrapperLock);
    // NOTE: We do NOT clear s_liveWrappers here. The wrappers are still
    // alive (the game holds refs). They just have m_real == NULL.
    // When the game Releases them, the destructor checks m_real != NULL
    // before calling Release. The destructor also removes from
    // s_liveWrappers and s_realToWrapper (but s_realToWrapper is already
    // cleared for these entries above).
}

// --- IUnknown ---------------------------------------------------------------

HRESULT STDMETHODCALLTYPE WrappedTexture9::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) ||
        riid == __uuidof(IDirect3DTexture9)) {
        *ppv = this;
        AddRef();
        return S_OK;
    }
    // Unknown IID - defer to real texture (some games QI for IDirect3DTexture9
    // internals or for IDirect3DResource9 to get a handle).
    if (!m_real) return E_NOINTERFACE; // post-Reset: texture is invalid
    return m_real->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE WrappedTexture9::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE WrappedTexture9::Release() {
    ULONG c = InterlockedDecrement(&m_refCount);
    if (c == 0) {
        // Remove from the stripped-texture registry before destroying.
        // The registry key is m_real (the underlying stripped texture pointer),
        // NOT this wrapper (the game holds the wrapper).
        // Guard: after PrepareForDeviceReset, m_real may be NULL (DEFAULT-pool
        // textures released). UnregisterStrippedTexture handles NULL safely,
        // but we guard explicitly for clarity.
        if (m_real) {
            UnregisterStrippedTexture((IDirect3DBaseTexture9*)m_real);
        }
        delete this;
        return 0;
    }
    return c;
}

// --- IDirect3DResource9 (all forwarded) -------------------------------------

HRESULT STDMETHODCALLTYPE WrappedTexture9::GetDevice(IDirect3DDevice9** ppDevice) {
    if (!m_real) return D3DERR_INVALIDCALL;
    return m_real->GetDevice(ppDevice);
}
HRESULT STDMETHODCALLTYPE WrappedTexture9::SetPrivateData(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) {
    if (!m_real) return D3DERR_INVALIDCALL;
    return m_real->SetPrivateData(refguid, pData, SizeOfData, Flags);
}
HRESULT STDMETHODCALLTYPE WrappedTexture9::GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) {
    if (!m_real) return D3DERR_INVALIDCALL;
    return m_real->GetPrivateData(refguid, pData, pSizeOfData);
}
HRESULT STDMETHODCALLTYPE WrappedTexture9::FreePrivateData(REFGUID refguid) {
    if (!m_real) return D3DERR_INVALIDCALL;
    return m_real->FreePrivateData(refguid);
}
DWORD STDMETHODCALLTYPE WrappedTexture9::SetPriority(DWORD PriorityNew) {
    if (!m_real) return 0;
    return m_real->SetPriority(PriorityNew);
}
DWORD STDMETHODCALLTYPE WrappedTexture9::GetPriority() {
    if (!m_real) return 0;
    return m_real->GetPriority();
}
void STDMETHODCALLTYPE WrappedTexture9::PreLoad() {
    if (!m_real) return;
    m_real->PreLoad();
}
D3DRESOURCETYPE STDMETHODCALLTYPE WrappedTexture9::GetType() {
    if (!m_real) return (D3DRESOURCETYPE)0; // post-Reset: invalid
    return m_real->GetType();
}

// --- IDirect3DBaseTexture9 --------------------------------------------------

DWORD STDMETHODCALLTYPE WrappedTexture9::SetLOD(DWORD LODNew) {
    if (!m_real) return 0;
    return m_real->SetLOD(LODNew);
}
DWORD STDMETHODCALLTYPE WrappedTexture9::GetLOD() {
    if (!m_real) return 0;
    return m_real->GetLOD();
}

// THE LIE: game thinks the texture has originalLevels mips.
DWORD STDMETHODCALLTYPE WrappedTexture9::GetLevelCount() {
    return m_entry.originalLevels;
}

HRESULT STDMETHODCALLTYPE WrappedTexture9::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
    if (!m_real) return D3DERR_INVALIDCALL;
    return m_real->SetAutoGenFilterType(FilterType);
}
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE WrappedTexture9::GetAutoGenFilterType() {
    if (!m_real) return (D3DTEXTUREFILTERTYPE)0;
    return m_real->GetAutoGenFilterType();
}
void STDMETHODCALLTYPE WrappedTexture9::GenerateMipSubLevels() {
    if (!m_real) return;
    m_real->GenerateMipSubLevels();
}

// --- IDirect3DTexture9 (the intercepts that matter) -------------------------

// GetLevelDesc: return what the GAME expects for each level.
//
// Level mapping: game's level N maps to real texture's level (N - mipsStripped).
// For levels < mipsStripped (stripped levels), we compute the original
// dimensions and return those — the game thinks the texture has full mips.
// For levels >= mipsStripped, the real texture's desc has matching dimensions.
HRESULT STDMETHODCALLTYPE WrappedTexture9::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) {
    if (!pDesc) return E_POINTER;
    if (!m_real) return D3DERR_INVALIDCALL;
    if (Level >= m_entry.originalLevels) return D3DERR_INVALIDCALL;

    if (Level < m_entry.mipsStripped) {
        // Stripped level — compute original dimensions for this level.
        // The game expects the original size, not the stripped size.
        UINT origW = m_entry.originalWidth >> Level;
        UINT origH = m_entry.originalHeight >> Level;
        if (origW < 1) origW = 1;
        if (origH < 1) origH = 1;
        pDesc->Width = origW;
        pDesc->Height = origH;
        pDesc->Format = m_entry.format;
        pDesc->Type = D3DRTYPE_TEXTURE;
        pDesc->Usage = 0;
        pDesc->Pool = m_pool;
        pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
        pDesc->MultiSampleQuality = 0;
        if (g_logLevel >= 2) {
            Log("WrappedTexture9::GetLevelDesc: level %u (stripped) -> orig dims %ux%u",
                Level, origW, origH);
        }
        return S_OK;
    }

    // Valid level — get from real texture (dimensions match what game expects)
    UINT realLevel = Level - m_entry.mipsStripped;
    return m_real->GetLevelDesc(realLevel, pDesc);
}

// GetSurfaceLevel: return the real texture's surface for valid levels.
// For stripped levels, return D3DERR_INVALIDCALL — we can't provide a surface
// for a mip that doesn't exist in the real texture. This is safe because
// GetSurfaceLevel is typically called for render targets / depth stencils,
// which the safety filter excludes from stripping.
HRESULT STDMETHODCALLTYPE WrappedTexture9::GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) {
    if (!ppSurfaceLevel) return E_POINTER;
    *ppSurfaceLevel = NULL;
    if (!m_real) return D3DERR_INVALIDCALL;
    if (Level >= m_entry.originalLevels) return D3DERR_INVALIDCALL;

    // Stripped levels: no surface available
    if (Level < m_entry.mipsStripped) {
        return D3DERR_INVALIDCALL;
    }

    // Valid level — get surface from real texture
    UINT realLevel = Level - m_entry.mipsStripped;
    return m_real->GetSurfaceLevel(realLevel, ppSurfaceLevel);
}

// LockRect: the core of the transient buffer approach.
//
// Level mapping: game's level N → real texture's level (N - mipsStripped).
//
// For levels >= mipsStripped (valid levels):
//   Lock the real texture at (Level - mipsStripped). The dimensions match
//   because stripping K mips shifts all remaining levels down by K.
//   The game writes data that fits perfectly — no overflow, no corruption.
//
// For levels < mipsStripped (stripped levels):
//   Allocate a transient buffer sized to what the GAME expects (original
//   dimensions for that level). The game writes its data into this buffer.
//   On UnlockRect, we free the buffer — the data is discarded because that
//   mip level was intentionally removed. No overflow, no corruption.
//
// RAM cost: only during the LockRect→UnlockRect window. Zero after unlock.
// For static textures (safety filter ensures), this happens once at load.
HRESULT STDMETHODCALLTYPE WrappedTexture9::LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) {
    if (!pLockedRect) return E_POINTER;
    if (!m_real) return D3DERR_INVALIDCALL;
    if (Level >= m_entry.originalLevels) return D3DERR_INVALIDCALL;

    // Valid level — lock the real texture directly
    if (Level >= m_entry.mipsStripped) {
        UINT realLevel = Level - m_entry.mipsStripped;
        HRESULT hr = m_real->LockRect(realLevel, pLockedRect, pRect, Flags);
        if (g_logLevel >= 2) {
            Log("WrappedTexture9::LockRect: level %u -> real level %u (pitch=%u)",
                Level, realLevel, pLockedRect ? pLockedRect->Pitch : 0);
        }
        return hr;
    }

    // Stripped level — allocate transient buffer
    // Compute original dimensions for this level
    UINT origW = m_entry.originalWidth >> Level;
    UINT origH = m_entry.originalHeight >> Level;
    if (origW < 1) origW = 1;
    if (origH < 1) origH = 1;

    // Compute buffer size based on DXT format
    // DXT1: 8 bytes per 4x4 block. DXT3/DXT5: 16 bytes per 4x4 block.
    int bpb = BCnBytesPerBlock(m_entry.format);
    if (bpb == 0) {
        // Non-BCn format — shouldn't reach here (safety filter rejects)
        // but handle defensively: assume 4 bytes per pixel
        bpb = 4;
    }

    UINT blocksX = (origW + 3) / 4;
    UINT blocksY = (origH + 3) / 4;
    UINT pitch = blocksX * bpb;
    UINT size = blocksY * pitch;

    // Allocate the transient buffer
    void* buffer = malloc(size);
    if (!buffer) {
        Log("WrappedTexture9::LockRect: FAILED to allocate %u bytes for transient buffer (level %u)",
            size, Level);
        return E_OUTOFMEMORY;
    }

    // If the game is reading (D3DLOCK_READONLY), zero the buffer
    // (there's no data to return for a stripped level)
    if (Flags & D3DLOCK_READONLY) {
        memset(buffer, 0, size);
    }

    // Store the transient lock (thread-safe — multi-threaded D3D9 may
    // lock different levels of the same texture concurrently)
    AcquireSRWLockExclusive(&m_locksLock);
    // Check for double-lock: if this level already has a transient lock,
    // the game violated the D3D9 contract (lock without unlock).
    // Free the new buffer and return D3DERR_INVALIDCALL (matching D3D9 behavior).
    if (m_transientLocks.find(Level) != m_transientLocks.end()) {
        ReleaseSRWLockExclusive(&m_locksLock);
        free(buffer);
        Log("WrappedTexture9::LockRect: double-lock detected on level %u (stripped) - returning D3DERR_INVALIDCALL",
            Level);
        return D3DERR_INVALIDCALL;
    }
    m_transientLocks[Level] = {buffer, pitch};
    ReleaseSRWLockExclusive(&m_locksLock);

    // Return the locked rect
    pLockedRect->pBits = buffer;
    pLockedRect->Pitch = pitch;

    if (g_logLevel >= 2) {
        Log("WrappedTexture9::LockRect: level %u (stripped) -> transient buffer %ux%u pitch=%u size=%u",
            Level, origW, origH, pitch, size);
    }

    return S_OK;
}

// UnlockRect: free the transient buffer (for stripped levels) or unlock
// the real texture (for valid levels).
//
// For stripped levels: the data the game wrote is discarded. That mip was
// intentionally removed — the finest detail level that the screen can't
// display at the target resolution. Discarding is correct behavior.
HRESULT STDMETHODCALLTYPE WrappedTexture9::UnlockRect(UINT Level) {
    if (!m_real) return D3DERR_INVALIDCALL;
    if (Level >= m_entry.originalLevels) return D3DERR_INVALIDCALL;

    // Valid level — unlock the real texture
    if (Level >= m_entry.mipsStripped) {
        UINT realLevel = Level - m_entry.mipsStripped;
        return m_real->UnlockRect(realLevel);
    }

    // Stripped level — free the transient buffer (thread-safe)
    AcquireSRWLockExclusive(&m_locksLock);
    auto it = m_transientLocks.find(Level);
    if (it != m_transientLocks.end()) {
        if (it->second.buffer) {
            free(it->second.buffer);
        }
        m_transientLocks.erase(it);
        ReleaseSRWLockExclusive(&m_locksLock);
        if (g_logLevel >= 2) {
            Log("WrappedTexture9::UnlockRect: level %u (stripped) -> transient buffer freed",
                Level);
        }
        return S_OK;
    }
    ReleaseSRWLockExclusive(&m_locksLock);

    // No transient lock found for this level — game called UnlockRect
    // without LockRect. Return D3DERR_INVALIDCALL per D3D9 contract.
    return D3DERR_INVALIDCALL;
}

// AddDirtyRect: only meaningful for D3DPOOL_DEFAULT textures with auto-gen
// mips. We don't strip those (safety filter rejects GENERATE_MIPS). Pass through.
HRESULT STDMETHODCALLTYPE WrappedTexture9::AddDirtyRect(const RECT* pDirtyRect) {
    if (!m_real) return D3DERR_INVALIDCALL;
    return m_real->AddDirtyRect(pDirtyRect);
}
