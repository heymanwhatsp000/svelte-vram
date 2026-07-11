// svelte_wrapped_device.cpp - COM wrapper for IDirect3DDevice9/9Ex
#include "svelte_wrapped_device.h"
#include "svelte_util.h"
#include "svelte_strip.h"
#include "svelte_registry.h" // stripped-texture registry
#include "svelte_wrapped_texture.h" // WrappedTexture9 (transient buffer approach)

// Frame timing using QueryPerformanceCounter
static LARGE_INTEGER g_perfFreq = {0};
static LARGE_INTEGER g_lastFrameTime = {0};
static void InitFrameTimer() {
 if (g_perfFreq.QuadPart == 0) {
 QueryPerformanceFrequency(&g_perfFreq);
 QueryPerformanceCounter(&g_lastFrameTime);
 }
}
static long long GetFrameDeltaUs() {
 if (g_perfFreq.QuadPart == 0) return 0;
 LARGE_INTEGER now;
 QueryPerformanceCounter(&now);
 long long delta = (now.QuadPart - g_lastFrameTime.QuadPart) * 1000000 / g_perfFreq.QuadPart;
 g_lastFrameTime = now;
 return delta;
}

WrappedDevice9::WrappedDevice9(IDirect3DDevice9* real) : m_real(real), m_realEx(NULL), m_refCount(1) {
 real->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&m_realEx);
}

WrappedDevice9::~WrappedDevice9() {
 if (m_realEx) m_realEx->Release();
 m_real->Release();
}


HRESULT STDMETHODCALLTYPE WrappedDevice9::QueryInterface(REFIID riid, void** ppv) {
 if (!ppv) return E_POINTER;
 if (riid == __uuidof(IUnknown) ||
 riid == __uuidof(IDirect3DDevice9) ||
 riid == __uuidof(IDirect3DDevice9Ex)) {
 *ppv = this;
 AddRef();
 return S_OK;
 }
 return m_real->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE WrappedDevice9::AddRef() {
 return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE WrappedDevice9::Release() {
 ULONG c = InterlockedDecrement(&m_refCount);
 if (c == 0) delete this;
 return c;
}

// CreateTexture - interception point (mip stripping)
//
// Creates ONE stripped texture and wraps it in WrappedTexture9. The wrapper
// uses transient buffers for stripped mip levels — zero permanent RAM,
// works on both DXVK and native D3D9On12.
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
 D3DFORMAT Format, D3DPOOL Pool,
 IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
 g_texturesCreated.fetch_add(1);

 // Defensive - validate params and init output
 if (!ppTexture) return D3DERR_INVALIDCALL;
 *ppTexture = NULL;
 if (!g_enabled) {
 return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
 }

 // Build a desc for the safety filter + strip logic
 D3D9_TEXTURE_DESC desc;
 desc.Type = D3DRTYPE_TEXTURE;
 desc.Width = Width;
 desc.Height = Height;
 desc.Levels = Levels;
 desc.Usage = Usage;
 desc.Format = Format;

 UINT newLevels = Levels, newW = Width, newH = Height;
 // Measure strip time for perf logging
 LARGE_INTEGER stripStart, stripEnd, stripFreq;
 QueryPerformanceFrequency(&stripFreq);
 QueryPerformanceCounter(&stripStart);
 int stripped = StripMips9(&desc, &newLevels, &newW, &newH);
 QueryPerformanceCounter(&stripEnd);
 if (stripped > 0) {
 long long stripUs = (stripEnd.QuadPart - stripStart.QuadPart) * 1000000 / stripFreq.QuadPart;
 PerfRecordStrip(stripUs);
 }

 if (stripped == 0) {
 // Texture passed through the safety filter but was not stripped
 // (either didn't need stripping, or was rejected by a condition).
 // Track it for stats. Note: exclusion-list skips are counted
 // separately in g_texturesSkippedByExclusion (incremented in svelte_strip.cpp).
 g_texturesSkipped.fetch_add(1);
 }

 if (stripped > 0 && (Pool == D3DPOOL_DEFAULT || Pool == D3DPOOL_MANAGED) && !pSharedHandle) {
 // Single path: create ONE stripped texture, wrap in WrappedTexture9.
 // The wrapper uses transient buffers for stripped mip levels — zero
 // permanent RAM, works on both DXVK and native D3D9On12.
 IDirect3DTexture9* realStrippedTex = NULL;
 HRESULT hr = m_real->CreateTexture(newW, newH, newLevels, Usage, Format, Pool, &realStrippedTex, pSharedHandle);
 if (SUCCEEDED(hr) && realStrippedTex) {
 StrippedTexEntry entry;
 entry.originalLevels = Levels;
 entry.originalWidth  = Width;
 entry.originalHeight = Height;
 entry.mipsStripped   = stripped;
 entry.format         = Format;
 RegisterStrippedTexture((IDirect3DBaseTexture9*)realStrippedTex, entry);

 WrappedTexture9* wrapped = new WrappedTexture9(realStrippedTex, entry, Pool);

 g_texturesStripped.fetch_add(1);

 UINT origBytes = CalcTextureBytes(Width, Height, Levels, Format);
 UINT newBytes  = CalcTextureBytes(newW, newH, newLevels, Format);
 long long saved = (long long)origBytes - (long long)newBytes;
 g_vramSavedBytes.fetch_add(saved);

 *ppTexture = wrapped;

 if (g_logLevel >= 1) {
 Log("StripMips: %ux%u/%u -> %ux%u/%u fmt=%s stripped=%d saved=%uKB (total=%lldMB)",
 Width, Height, Levels, newW, newH, newLevels, FormatName(Format),
 stripped, (UINT)(saved / 1024), g_vramSavedBytes.load() / (1024 * 1024));
 }
 return hr;
 }
 // Fallback: try with original desc
 g_texturesFailed.fetch_add(1);
 Log("StripMips FAILED hr=0x%08X - retrying with original %ux%u", hr, Width, Height);
 return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
 }

 // Not stripped - pass through
 return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
}

// ALL OTHER METHODS - mechanical forwards to m_real
HRESULT STDMETHODCALLTYPE WrappedDevice9::TestCooperativeLevel() { return m_real->TestCooperativeLevel(); }
UINT STDMETHODCALLTYPE WrappedDevice9::GetAvailableTextureMem() { return m_real->GetAvailableTextureMem(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::EvictManagedResources() { return m_real->EvictManagedResources(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetDirect3D(IDirect3D9** ppD3D9) { return m_real->GetDirect3D(ppD3D9); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetDeviceCaps(D3DCAPS9* pCaps) { return m_real->GetDeviceCaps(pCaps); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) { return m_real->GetDisplayMode(iSwapChain, pMode); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) { return m_real->GetCreationParameters(pParameters); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) { return m_real->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
void STDMETHODCALLTYPE WrappedDevice9::SetCursorPosition(int X, int Y, DWORD Flags) { m_real->SetCursorPosition(X, Y, Flags); }
BOOL STDMETHODCALLTYPE WrappedDevice9::ShowCursor(BOOL bShow) { return m_real->ShowCursor(bShow); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) { return m_real->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) { return m_real->GetSwapChain(iSwapChain, pSwapChain); }
UINT STDMETHODCALLTYPE WrappedDevice9::GetNumberOfSwapChains() { return m_real->GetNumberOfSwapChains(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
 // Prepare all wrappers before Reset. Reset destroys DEFAULT-pool textures.
 // Our wrappers hold pointers to those textures. We must release and null
 // them BEFORE Reset, otherwise the game's later Release on the wrapper
 // calls Release on a destroyed texture -> use-after-free -> crash.
 WrappedTexture9::PrepareForDeviceReset();
 return m_real->Reset(pPresentationParameters);
}
HRESULT STDMETHODCALLTYPE WrappedDevice9::Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
 InitFrameTimer();
 long long frameDelta = GetFrameDeltaUs();
 PerfUpdate(frameDelta);

 g_frameCount.fetch_add(1);
 // Periodic LRU sweep + perf log (every 300 frames)
 if (g_frameCount.load() % 300 == 0) {
 PerfLogAndReset();
 MaybeSweepStrippedMap(g_frameCount.load());
 }
 return m_real->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) { return m_real->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) { return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetDialogBoxMode(BOOL bEnableDialogs) { return m_real->SetDialogBoxMode(bEnableDialogs); }
void STDMETHODCALLTYPE WrappedDevice9::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) { m_real->SetGammaRamp(iSwapChain, Flags, pRamp); }
void STDMETHODCALLTYPE WrappedDevice9::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) { m_real->GetGammaRamp(iSwapChain, pRamp); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) { return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) { return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) { return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) { return m_real->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { return m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { return m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::UpdateSurface(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, const POINT* pDestPoint) { return m_real->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) {
 // Unwrap both textures before passing to m_real.
 IDirect3DBaseTexture9* realSrc = WrappedTexture9::UnwrapIfWrapper(pSourceTexture);
 IDirect3DBaseTexture9* realDst = WrappedTexture9::UnwrapIfWrapper(pDestinationTexture);
 return m_real->UpdateTexture(realSrc, realDst);
}
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) { return m_real->GetRenderTargetData(pRenderTarget, pDestSurface); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) { return m_real->GetFrontBufferData(iSwapChain, pDestSurface); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::StretchRect(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) { return m_real->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::ColorFill(IDirect3DSurface9* pSurface, const RECT* pRect, D3DCOLOR color) { return m_real->ColorFill(pSurface, pRect, color); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) { return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) { return m_real->SetRenderTarget(RenderTargetIndex, pRenderTarget); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) { return m_real->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) { return m_real->SetDepthStencilSurface(pNewZStencil); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) { return m_real->GetDepthStencilSurface(ppZStencilSurface); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::BeginScene() { return m_real->BeginScene(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::EndScene() { return m_real->EndScene(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) { return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_real->SetTransform(State, pMatrix); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) { return m_real->GetTransform(State, pMatrix); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_real->MultiplyTransform(State, pMatrix); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetViewport(const D3DVIEWPORT9* pViewport) { return m_real->SetViewport(pViewport); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetViewport(D3DVIEWPORT9* pViewport) { return m_real->GetViewport(pViewport); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetMaterial(const D3DMATERIAL9* pMaterial) { return m_real->SetMaterial(pMaterial); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetMaterial(D3DMATERIAL9* pMaterial) { return m_real->GetMaterial(pMaterial); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetLight(DWORD Index, const D3DLIGHT9* pLight) { return m_real->SetLight(Index, pLight); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetLight(DWORD Index, D3DLIGHT9* pLight) { return m_real->GetLight(Index, pLight); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::LightEnable(DWORD Index, BOOL Enable) { return m_real->LightEnable(Index, Enable); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetLightEnable(DWORD Index, BOOL* pEnable) { return m_real->GetLightEnable(Index, pEnable); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetClipPlane(DWORD Index, const float* pPlane) { return m_real->SetClipPlane(Index, pPlane); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetClipPlane(DWORD Index, float* pPlane) { return m_real->GetClipPlane(Index, pPlane); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) { return m_real->SetRenderState(State, Value); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) { return m_real->GetRenderState(State, pValue); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) { return m_real->CreateStateBlock(Type, ppSB); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::BeginStateBlock() { return m_real->BeginStateBlock(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::EndStateBlock(IDirect3DStateBlock9** ppSB) { return m_real->EndStateBlock(ppSB); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) { return m_real->SetClipStatus(pClipStatus); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) { return m_real->GetClipStatus(pClipStatus); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
 HRESULT hr = m_real->GetTexture(Stage, ppTexture);
 // If the returned texture is one of our stripped real textures,
 // re-wrap it so the game sees the wrapper (with the mip-count lie).
 if (SUCCEEDED(hr) && ppTexture && *ppTexture) {
 // FindByReal returns an AddRef'd wrapper (or NULL if not a stripped texture).
 WrappedTexture9* wrapper = WrappedTexture9::FindByReal(*ppTexture);
 if (wrapper) {
 // Release the real texture (GetTexture AddRef'd it).
 // The wrapper is already AddRef'd by FindByReal — return it directly.
 (*ppTexture)->Release();
 *ppTexture = wrapper;
 }
 }
 return hr;
}
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
 if (!pTexture) return m_real->SetTexture(Stage, pTexture);

 // Unwrap WrappedTexture9 before passing to m_real. The wrapper's
 // GetLevelCount() returns the original count (the lie), which would
 // confuse DXVK/driver descriptor sizing. UnwrapIfWrapper returns the
 // real underlying texture if pTexture is a wrapper, or unchanged if not.
 IDirect3DBaseTexture9* realTex = WrappedTexture9::UnwrapIfWrapper(pTexture);
 return m_real->SetTexture(Stage, realTex);
}
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) { return m_real->GetTextureStageState(Stage, Type, pValue); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) { return m_real->SetTextureStageState(Stage, Type, Value); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) { return m_real->GetSamplerState(Sampler, Type, pValue); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) { return m_real->SetSamplerState(Sampler, Type, Value); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::ValidateDevice(DWORD* pNumPasses) { return m_real->ValidateDevice(pNumPasses); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) { return m_real->SetPaletteEntries(PaletteNumber, pEntries); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) { return m_real->GetPaletteEntries(PaletteNumber, pEntries); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetCurrentTexturePalette(UINT PaletteNumber) { return m_real->SetCurrentTexturePalette(PaletteNumber); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetCurrentTexturePalette(UINT *PaletteNumber) { return m_real->GetCurrentTexturePalette(PaletteNumber); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetScissorRect(const RECT* pRect) { return m_real->SetScissorRect(pRect); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetScissorRect(RECT* pRect) { return m_real->GetScissorRect(pRect); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetSoftwareVertexProcessing(BOOL bSoftware) { return m_real->SetSoftwareVertexProcessing(bSoftware); }
BOOL STDMETHODCALLTYPE WrappedDevice9::GetSoftwareVertexProcessing() { return m_real->GetSoftwareVertexProcessing(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetNPatchMode(float nSegments) { return m_real->SetNPatchMode(nSegments); }
float STDMETHODCALLTYPE WrappedDevice9::GetNPatchMode() { return m_real->GetNPatchMode(); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) { return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) { return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) { return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) { return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) { return m_real->SetVertexDeclaration(pDecl); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) { return m_real->GetVertexDeclaration(ppDecl); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetFVF(DWORD FVF) { return m_real->SetFVF(FVF); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetFVF(DWORD* pFVF) { return m_real->GetFVF(pFVF); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) { return m_real->CreateVertexShader(pFunction, ppShader); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetVertexShader(IDirect3DVertexShader9* pShader) { return m_real->SetVertexShader(pShader); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetVertexShader(IDirect3DVertexShader9** ppShader) { return m_real->GetVertexShader(ppShader); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) { return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT Vector4bCount) { return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, Vector4bCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT Vector4bCount) { return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, Vector4bCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) { return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) { return m_real->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) { return m_real->SetStreamSourceFreq(StreamNumber, Setting); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) { return m_real->GetStreamSourceFreq(StreamNumber, pSetting); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetIndices(IDirect3DIndexBuffer9* pIndexData) { return m_real->SetIndices(pIndexData); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetIndices(IDirect3DIndexBuffer9** ppIndexData) { return m_real->GetIndices(ppIndexData); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) { return m_real->CreatePixelShader(pFunction, ppShader); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetPixelShader(IDirect3DPixelShader9* pShader) { return m_real->SetPixelShader(pShader); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetPixelShader(IDirect3DPixelShader9** ppShader) { return m_real->GetPixelShader(ppShader); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) { return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT Vector4bCount) { return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, Vector4bCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT Vector4bCount) { return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, Vector4bCount); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) { return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) { return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::DeletePatch(UINT Handle) { return m_real->DeletePatch(Handle); }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) { return m_real->CreateQuery(Type, ppQuery); }


HRESULT STDMETHODCALLTYPE WrappedDevice9::SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns) { return m_realEx ? m_realEx->SetConvolutionMonoKernel(width, height, rows, columns) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset) { return m_realEx ? m_realEx->ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::PresentEx(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion, DWORD dwFlags) {
 InitFrameTimer();
 long long frameDelta = GetFrameDeltaUs();
 PerfUpdate(frameDelta);

 g_frameCount.fetch_add(1);
 if (g_frameCount.load() % 300 == 0) {
 PerfLogAndReset();
 MaybeSweepStrippedMap(g_frameCount.load());
 }
 return m_realEx ? m_realEx->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags) : E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetGPUThreadPriority(INT* pPriority) { return m_realEx ? m_realEx->GetGPUThreadPriority(pPriority) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetGPUThreadPriority(INT Priority) { return m_realEx ? m_realEx->SetGPUThreadPriority(Priority) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::WaitForVBlank(UINT iSwapChain) { return m_realEx ? m_realEx->WaitForVBlank(iSwapChain) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT NumResources) { return m_realEx ? m_realEx->CheckResourceResidency(pResourceArray, NumResources) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::SetMaximumFrameLatency(UINT MaxLatency) { return m_realEx ? m_realEx->SetMaximumFrameLatency(MaxLatency) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetMaximumFrameLatency(UINT* pMaxLatency) { return m_realEx ? m_realEx->GetMaximumFrameLatency(pMaxLatency) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CheckDeviceState(HWND hDestinationWindow) { return m_realEx ? m_realEx->CheckDeviceState(hDestinationWindow) : E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) { return m_realEx ? m_realEx->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) { return m_realEx ? m_realEx->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) { return m_realEx ? m_realEx->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage) : E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE WrappedDevice9::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
 // Prepare all wrappers before ResetEx (same as Reset).
 WrappedTexture9::PrepareForDeviceReset();
 return m_realEx ? m_realEx->ResetEx(pPresentationParameters, pFullscreenDisplayMode) : E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE WrappedDevice9::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) { return m_realEx ? m_realEx->GetDisplayModeEx(iSwapChain, pMode, pRotation) : E_NOTIMPL; }