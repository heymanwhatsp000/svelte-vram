// svelte_wrapped_texture.h - WrappedTexture9 COM wrapper
//
// Maintains the illusion that the game has the original number of mips,
// while the underlying real texture is smaller (stripped). This is the
// core of the transient-buffer approach.
//
// The game called CreateTexture(4096, 4096, 13, ...). Svelte stripped it to
// (2048, 2048, 12). The game now holds a 12-mip texture but thinks it has 13.
// This wrapper maintains that lie at the API surface:
//
//   GetLevelCount()       -> return 13 (original)
//   GetLevelDesc(level)   -> return original dims for stripped levels,
//                            real dims for valid levels
//   LockRect(level, ...)  -> for valid levels, lock real texture directly
//                            for stripped levels, allocate a transient buffer
//   GetSurfaceLevel(...)  -> return real surface for valid levels,
//                            D3DERR_INVALIDCALL for stripped levels
//   Release()             -> unregister from stripped-texture registry
//
// TRANSIENT BUFFER APPROACH:
// For stripped mip levels (level < mipsStripped), a temporary buffer is
// allocated during LockRect and freed during UnlockRect. The game writes
// its data into this buffer, which is then discarded — those mip levels
// were intentionally removed. Zero permanent RAM cost.
//
// The wrapper inherits from IDirect3DTexture9 (which inherits from
// IDirect3DBaseTexture9, IDirect3DResource9, IUnknown). All non-intercepted
// methods forward to m_real.
#pragma once
#include <d3d9.h>
#include <unordered_set>
#include <unordered_map>

#include "svelte_registry.h"  // for StrippedTexEntry

class WrappedTexture9 : public IDirect3DTexture9 {
private:
    IDirect3DTexture9* m_real;     // the stripped texture (actualLevels mips)
    StrippedTexEntry   m_entry;    // copy of the registry entry (original dims etc.)
    LONG               m_refCount;
    D3DPOOL            m_pool;     // pool type (DEFAULT=destroyed by Reset, MANAGED=survives)

    // Transient lock tracking. When the game locks a stripped mip level
    // (level < mipsStripped), we allocate a temporary buffer sized to what
    // the game expects (original dimensions for that level). The game writes
    // its data into this buffer. On UnlockRect, we free the buffer — the
    // data is discarded because that mip level was intentionally removed.
    //
    // For levels >= mipsStripped, we lock the real texture directly at
    // (level - mipsStripped). The dimensions match because stripping K mips
    // shifts all remaining levels down by K.
    //
    // RAM cost: only during the LockRect→UnlockRect window. Zero after unlock.
    // For static textures (which the safety filter ensures), this happens
    // once at load time, then the buffer is freed. No permanent RAM.
    struct TransientLock {
        void* buffer;   // malloc'd buffer for the game to write into
        UINT  pitch;    // row pitch (bytes per row)
    };
    std::unordered_map<UINT, TransientLock> m_transientLocks;
    SRWLOCK m_locksLock;  // protects m_transientLocks (for multi-threaded D3D9)

    // Static set of all live WrappedTexture9 pointers. Used by
    // WrappedDevice9::SetTexture to detect whether a texture pointer is
    // a wrapper and unwrap it before passing to the real device.
    //
    // Without this, the wrapper's GetLevelCount() lie (returns originalLevels)
    // would reach DXVK/driver, causing it to allocate descriptors for
    // non-existent mip levels.
    static std::unordered_set<WrappedTexture9*> s_liveWrappers;
    static SRWLOCK s_wrapperLock;

    // Reverse map: real texture pointer -> wrapper.
    // Used by GetTexture to re-wrap the result.
    static std::unordered_map<IDirect3DBaseTexture9*, WrappedTexture9*> s_realToWrapper;

public:
    WrappedTexture9(IDirect3DTexture9* real, const StrippedTexEntry& entry, D3DPOOL pool);
    ~WrappedTexture9();

    // Check if a texture pointer is a WrappedTexture9.
    // If so, returns the underlying real texture. If not, returns the input.
    // Thread-safe (uses SRWLOCK shared lock for the lookup).
    static IDirect3DBaseTexture9* UnwrapIfWrapper(IDirect3DBaseTexture9* tex);

    // Find the wrapper for a given real texture pointer.
    // Returns the wrapper (AddRef NOT taken — caller must AddRef if needed).
    // Returns NULL if the real texture is not a stripped texture.
    static WrappedTexture9* FindByReal(IDirect3DBaseTexture9* real);

    // Prepare all live wrappers for a device Reset.
    // Releases each wrapper's real texture and nulls m_real.
    // This prevents use-after-free when Reset destroys DEFAULT-pool textures.
    // After this call, all wrappers are still alive (the game holds refs),
    // but their m_real is NULL. The game should Release them and create
    // new textures after Reset.
    static void PrepareForDeviceReset();

    // Get the underlying real texture (for internal use).
    IDirect3DTexture9* GetReal() { return m_real; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDirect3DResource9 - intercepted: GetDevice, GetType, SetPrivateData,
    // GetPrivateData, FreePrivateData, SetPriority, GetPriority, SetLOD, GetLOD
    // (all forwarded)
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override;
    DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override;
    DWORD STDMETHODCALLTYPE GetPriority() override;
    void STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

    // IDirect3DBaseTexture9 - intercepted: GetLevelCount (lies!)
    DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) override;
    DWORD STDMETHODCALLTYPE GetLOD() override;
    DWORD STDMETHODCALLTYPE GetLevelCount() override;  // returns m_entry.originalLevels
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
    void STDMETHODCALLTYPE GenerateMipSubLevels() override;

    // IDirect3DTexture9 - intercepted: GetLevelDesc, GetSurfaceLevel, LockRect, UnlockRect
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* pDirtyRect) override;
};
