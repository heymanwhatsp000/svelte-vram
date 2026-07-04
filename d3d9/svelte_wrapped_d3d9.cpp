// svelte_wrapped_d3d9.cpp - COM wrapper for IDirect3D9/9Ex
#include "svelte_wrapped_d3d9.h"
#include "svelte_wrapped_device.h"
#include "svelte_util.h"

WrappedD3D9::WrappedD3D9(IDirect3D9* real) : m_real(real), m_realEx(NULL), m_refCount(1) {
 real->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&m_realEx);
}

WrappedD3D9::~WrappedD3D9() {
 if (m_realEx) m_realEx->Release();
 m_real->Release();
}


HRESULT STDMETHODCALLTYPE WrappedD3D9::QueryInterface(REFIID riid, void** ppv) {
 if (!ppv) return E_POINTER;
 if (riid == __uuidof(IUnknown) ||
 riid == __uuidof(IDirect3D9) ||
 riid == __uuidof(IDirect3D9Ex)) {
 *ppv = this;
 AddRef();
 return S_OK;
 }
 return m_real->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE WrappedD3D9::AddRef() {
 return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE WrappedD3D9::Release() {
 ULONG c = InterlockedDecrement(&m_refCount);
 if (c == 0) delete this;
 return c;
}


HRESULT STDMETHODCALLTYPE WrappedD3D9::RegisterSoftwareDevice(void* pInitializeFunction) {
 return m_real->RegisterSoftwareDevice(pInitializeFunction);
}

UINT STDMETHODCALLTYPE WrappedD3D9::GetAdapterCount() {
 return m_real->GetAdapterCount();
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) {
 return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
}

UINT STDMETHODCALLTYPE WrappedD3D9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
 return m_real->GetAdapterModeCount(Adapter, Format);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) {
 return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) {
 return m_real->GetAdapterDisplayMode(Adapter, pMode);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) {
 return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps);
}

HMONITOR STDMETHODCALLTYPE WrappedD3D9::GetAdapterMonitor(UINT Adapter) {
 return m_real->GetAdapterMonitor(Adapter);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::CheckDeviceType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) {
 return m_real->CheckDeviceType(Adapter, DeviceType, DisplayFormat, BackBufferFormat, Windowed);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
 return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) {
 return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
 return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
 return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
}

// CreateDevice - interception point
// Call real CreateDevice, then wrap the returned device in WrappedDevice9.
// The game holds our wrapper, so all future calls go through us.
HRESULT STDMETHODCALLTYPE WrappedD3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
 DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
 IDirect3DDevice9** ppReturnedDeviceInterface) {
 if (!ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;

 IDirect3DDevice9* realDev = NULL;
 HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
 pPresentationParameters, &realDev);
 if (SUCCEEDED(hr) && realDev) {
 // Log adapter info
 D3DADAPTER_IDENTIFIER9 id;
 if (SUCCEEDED(m_real->GetAdapterIdentifier(Adapter, 0, &id))) {
 Log("Adapter: %s VendorID=0x%04X DeviceID=0x%04X Driver=%d.%d.%d.%d",
 id.Description, id.VendorId, id.DeviceId,
 HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
 HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart));
 }
 WrappedDevice9* wrapped = new WrappedDevice9(realDev);
 *ppReturnedDeviceInterface = wrapped;
 Log("CreateDevice: device wrapped (%s, mip stripping %s)",
 SVELTE_VERSION, g_enabled ? "active" : "DISABLED");
 }
 return hr;
}


UINT STDMETHODCALLTYPE WrappedD3D9::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) {
 return m_realEx ? m_realEx->GetAdapterModeCountEx(Adapter, pFilter) : 0;
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode) {
 return m_realEx ? m_realEx->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode) : E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
 return m_realEx ? m_realEx->GetAdapterDisplayModeEx(Adapter, pMode, pRotation) : E_NOTIMPL;
}

// CreateDeviceEx - intercept like CreateDevice but for IDirect3DDevice9Ex
HRESULT STDMETHODCALLTYPE WrappedD3D9::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
 DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
 D3DDISPLAYMODEEX* pFullscreenDisplayMode,
 IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
 if (!ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;

 IDirect3DDevice9Ex* realDevEx = NULL;
 HRESULT hr = m_realEx ? m_realEx->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
 pPresentationParameters, pFullscreenDisplayMode, &realDevEx) : E_NOTIMPL;
 if (SUCCEEDED(hr) && realDevEx) {
 // WrappedDevice9 inherits from IDirect3DDevice9Ex, so it satisfies both interfaces
 WrappedDevice9* wrapped = new WrappedDevice9((IDirect3DDevice9*)realDevEx);
 *ppReturnedDeviceInterface = (IDirect3DDevice9Ex*)wrapped;
 Log("CreateDeviceEx: device wrapped (%s)", SVELTE_VERSION);
 }
 return hr;
}

HRESULT STDMETHODCALLTYPE WrappedD3D9::GetAdapterLUID(UINT Adapter, LUID* pLUID) {
 return m_realEx ? m_realEx->GetAdapterLUID(Adapter, pLUID) : E_NOTIMPL;
}