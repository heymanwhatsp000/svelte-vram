// svelte_wrapped_d3d9.h - COM wrapper for IDirect3D9
// Wraps IDirect3D9 so that CreateDevice returns our WrappedDevice9.
// Without this wrapper, the game gets the real IDirect3DDevice9 and
// bypasses our CreateTexture interception entirely.
//
// IDirect3D9 has only ~9 methods (vs IDirect3DDevice9's ~127), so this
// wrapper is small. The only method we intercept is CreateDevice - all
// others are forwarded.
//
// For Direct3DCreate9Ex (returns IDirect3D9Ex), we wrap that too by
// inheriting from IDirect3D9Ex and forwarding Ex methods.
#pragma once
#include <d3d9.h>

class WrappedD3D9 : public IDirect3D9Ex {
private:
 IDirect3D9* m_real;
 IDirect3D9Ex* m_realEx;
 LONG m_refCount;

public:
 WrappedD3D9(IDirect3D9* real);
 ~WrappedD3D9();

 // IUnknown
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
 ULONG STDMETHODCALLTYPE AddRef() override;
 ULONG STDMETHODCALLTYPE Release() override;

 // IDirect3D9 - intercept CreateDevice
 HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override;
 UINT STDMETHODCALLTYPE GetAdapterCount() override;
 HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override;
 UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override;
 HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override;
 HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override;
 HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override;
 HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override;
 HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) override;
 HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override;
 HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override;
 HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override;
 HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override;
 HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
 DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
 IDirect3DDevice9** ppReturnedDeviceInterface) override;

 // IDirect3D9Ex - forwarded to m_realEx
 UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) override;
 HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode) override;
 HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) override;
 HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
 DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
 D3DDISPLAYMODEEX* pFullscreenDisplayMode,
 IDirect3DDevice9Ex** ppReturnedDeviceInterface) override;
 HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID* pLUID) override;
};