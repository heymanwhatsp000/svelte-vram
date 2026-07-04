// svelte_wrapped_swapchain.h - COM wrapper for IDXGISwapChain
// Intercepts Present() for periodic stats logging + LRU sweep.
#pragma once
#include <dxgi.h>
#include <dxgi1_2.h>

class WrappedSwapChain : public IDXGISwapChain {
private:
 IDXGISwapChain* m_real;
 LONG m_refCount;
public:
 WrappedSwapChain(IDXGISwapChain* real);

 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
 ULONG STDMETHODCALLTYPE AddRef() override;
 ULONG STDMETHODCALLTYPE Release() override;
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override;
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override;
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override;
 HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override;
 HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override;
 HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
 HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID, void**) override;
 HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override;
 HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override;
 HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override;
 HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override;
 HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override;
 HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override;
 HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override;
 HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override;
};
