// svelte_wrapped_swapchain.cpp - COM wrapper for IDXGISwapChain
#include "svelte_wrapped_swapchain.h"
#include "svelte_util.h"
#include "svelte_registry.h"

WrappedSwapChain::WrappedSwapChain(IDXGISwapChain* real) : m_real(real), m_refCount(1) {}

HRESULT STDMETHODCALLTYPE WrappedSwapChain::QueryInterface(REFIID riid, void** ppv) {
 if (!ppv) return E_POINTER;
 if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
 riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
 riid == __uuidof(IDXGISwapChain1)) {
 *ppv = this; AddRef(); return S_OK;
 }
 return m_real->QueryInterface(riid, ppv);
}
ULONG STDMETHODCALLTYPE WrappedSwapChain::AddRef() { InterlockedIncrement(&m_refCount); return m_real->AddRef(); }
ULONG STDMETHODCALLTYPE WrappedSwapChain::Release() {
 ULONG c = InterlockedDecrement(&m_refCount);
 if (c == 0) { m_real->Release(); delete this; return 0; }
 return m_real->Release();
}

HRESULT STDMETHODCALLTYPE WrappedSwapChain::SetPrivateData(REFGUID g, UINT s, const void* p) { return m_real->SetPrivateData(g, s, p); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::SetPrivateDataInterface(REFGUID g, const IUnknown* p) { return m_real->SetPrivateDataInterface(g, p); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetPrivateData(REFGUID g, UINT* s, void* p) { return m_real->GetPrivateData(g, s, p); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetParent(REFIID riid, void** ppParent) { return m_real->GetParent(riid, ppParent); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetDevice(REFIID riid, void** ppDevice) { return m_real->GetDevice(riid, ppDevice); }

// Present - intercept for periodic stats + LRU sweep
// Every 300 frames (~5 sec at 60fps):
// 1. Log stats (created/stripped/skipped/saved/srvClamped)
// 2. Run LRU sweep on the stripped-texture map
HRESULT STDMETHODCALLTYPE WrappedSwapChain::Present(UINT SyncInterval, UINT Flags) {
 long long frame = ++g_frameCount;
 if (frame % 300 == 0) {
 Log("Stats frame=%lld: created=%d stripped=%d skippedFilter=%d skippedExcl=%d failed=%d saved=%lldMB srvClamped=%d",
            frame, g_texturesCreated.load(), g_texturesStripped.load(),
            g_texturesSkippedByFilter.load(), g_texturesSkippedByExclusion.load(),
            g_texturesFailedStrip.load(),
            g_vramSavedBytes.load() / (1024 * 1024), g_srvClamped.load());
 MaybeSweepStrippedMap();
 }
 return m_real->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) { return m_real->GetBuffer(Buffer, riid, ppSurface); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) { return m_real->SetFullscreenState(Fullscreen, pTarget); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) { return m_real->GetFullscreenState(pFullscreen, ppTarget); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) { return m_real->GetDesc(pDesc); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) { return m_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) { return m_real->ResizeTarget(pNewTargetParameters); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetContainingOutput(IDXGIOutput** ppOutput) { return m_real->GetContainingOutput(ppOutput); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) { return m_real->GetFrameStatistics(pStats); }
HRESULT STDMETHODCALLTYPE WrappedSwapChain::GetLastPresentCount(UINT* pLastPresentCount) { return m_real->GetLastPresentCount(pLastPresentCount); }
