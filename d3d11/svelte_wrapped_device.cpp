// svelte_wrapped_device.cpp - COM wrapper for ID3D11Device
#include "svelte_wrapped_device.h"
#include "svelte_strip.h"
#include "svelte_registry.h"
#include "svelte_util.h"


WrappedDevice::WrappedDevice(ID3D11Device* real) : m_real(real), m_refCount(1) {
 // Cache all device interface versions via QueryInterface.
 // This lets us forward Device1/2/3/4/5 methods to the real implementations.
 m_real1 = NULL; m_real2 = NULL; m_real3 = NULL; m_real4 = NULL; m_real5 = NULL;
 real->QueryInterface(__uuidof(ID3D11Device1), (void**)&m_real1);
 real->QueryInterface(__uuidof(ID3D11Device2), (void**)&m_real2);
 real->QueryInterface(__uuidof(ID3D11Device3), (void**)&m_real3);
 real->QueryInterface(__uuidof(ID3D11Device4), (void**)&m_real4);
 real->QueryInterface(__uuidof(ID3D11Device5), (void**)&m_real5);
}

WrappedDevice::~WrappedDevice() {
 if (m_real5) m_real5->Release();
 if (m_real4) m_real4->Release();
 if (m_real3) m_real3->Release();
 if (m_real2) m_real2->Release();
 if (m_real1) m_real1->Release();
 m_real->Release();
}


// QI returns `this` for ALL 6 device interfaces. This is critical:
// if the game QIs for ID3D11Device3 and we don't return self, it gets
// the real device and bypasses our CreateTexture2D/SRV interception.
HRESULT STDMETHODCALLTYPE WrappedDevice::QueryInterface(REFIID riid, void** ppv) {
 if (!ppv) return E_POINTER;
 if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device) ||
 riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
 riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) ||
 riid == __uuidof(ID3D11Device5)) {
 *ppv = this; AddRef(); return S_OK;
 }
 return m_real->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE WrappedDevice::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedDevice::Release() {
 ULONG c = InterlockedDecrement(&m_refCount);
 if (c == 0) delete this;
 return c;
}


HRESULT STDMETHODCALLTYPE WrappedDevice::GetPrivateData(REFGUID g, UINT* s, void* p) { return m_real->GetPrivateData(g, s, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::SetPrivateData(REFGUID g, UINT s, const void* p) { return m_real->SetPrivateData(g, s, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::SetPrivateDataInterface(REFGUID g, const IUnknown* p) { return m_real->SetPrivateDataInterface(g, p); }

void STDMETHODCALLTYPE WrappedDevice::GetImmediateContext(ID3D11DeviceContext** ppCtx) {
 m_real->GetImmediateContext(ppCtx);
}

// CreateTexture2D - interception point (mip stripping + format conversion)
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc,
 const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {

 // Defensive defensive checks
 if (!pDesc || !ppTexture2D) return m_real->CreateTexture2D(pDesc, pInitialData, ppTexture2D);
 *ppTexture2D = NULL; // Initialize output to NULL (prevent dangling pointer)

 // Two-phase reduction pipeline.
 D3D11_TEXTURE2D_DESC workDesc = *pDesc;
 const D3D11_SUBRESOURCE_DATA* workData = pInitialData;

 // Phase 2: Mip stripping ──
 D3D11_TEXTURE2D_DESC modDesc;
 D3D11_SUBRESOURCE_DATA* modDataArray = NULL;
 int stripped = 0;

 if (workData && ppTexture2D) {
 stripped = StripMips(&workDesc, workData, &modDesc, &modDataArray);
 }

 g_texturesCreated.fetch_add(1);

 // Phase 3: Create texture with final desc + data ──
 if (stripped > 0 && modDataArray) {
 // Mip-stripped (and possibly format-converted)
 UINT origBytes = CalcTextureBytes(pDesc->Width, pDesc->Height, pDesc->MipLevels, pDesc->Format);
 UINT newBytes = CalcTextureBytes(modDesc.Width, modDesc.Height, modDesc.MipLevels, modDesc.Format);
 UINT saved = origBytes - newBytes;

 ID3D11Texture2D* realTex = NULL;
 HRESULT hr = m_real->CreateTexture2D(&modDesc, modDataArray, &realTex);
 if (SUCCEEDED(hr) && realTex) {
 g_texturesStripped.fetch_add(1);
 g_vramSavedBytes.fetch_add(saved);
 RegisterStrippedTexture(realTex, modDesc.MipLevels);

 // Also register format conversion for SRV fixup
 *ppTexture2D = realTex;

 if (g_logLevel >= 1) {
 Log("StripMips: %ux%u/%u → %ux%u/%u fmt=%s stripped=%d saved=%uKB (total=%lldMB)",
 pDesc->Width, pDesc->Height, pDesc->MipLevels,
 modDesc.Width, modDesc.Height, modDesc.MipLevels, FormatName(modDesc.Format),
 stripped, saved / 1024,
 g_vramSavedBytes.load() / (1024 * 1024));
 }
 return hr;
 }
 g_texturesFailedStrip.fetch_add(1);
 Log("CreateTex FAILED hr=0x%08X - retrying with original %ux%u", hr, pDesc->Width, pDesc->Height);
 return m_real->CreateTexture2D(pDesc, pInitialData, ppTexture2D);
 }

 // No strip - pass through
 if (pInitialData && ppTexture2D && g_enabled) {
 if (pDesc->MipLevels > 1 && IsBCnFormat(pDesc->Format)) {
 g_texturesSkippedByFilter.fetch_add(1);
 }
 }

 if (g_logLevel >= 3) Log(" CreateTexture2D: pass-through (not stripped)");
 return m_real->CreateTexture2D(pDesc, pInitialData, ppTexture2D);
}

// CreateShaderResourceView - clamp mip range
// black texture bug:
// Game creates 4096x4096 texture with 13 mips. Svelte strips 1 → texture is
// now 2048x2048 with 12 mips. Game later creates an SRV asking for 13 mips.
// D3D11 sees only 12 mips → E_INVALIDARG → null SRV → black texture.
//
// the fix:
// Look up the texture in the stripped-texture registry. If found, clamp
// the SRV desc's MostDetailedMip/MipLevels to the actual (reduced) count.
//
// stale entry fix:
// On streaming engines (), textures are constantly created/destroyed.
// When a stripped texture is released, the OS may reuse its pointer for a NEW
// texture. The map still has the old entry → the new texture gets incorrectly
// clamped → black textures.
//
// Fix: before clamping, verify by calling GetDesc() on the texture. If the
// actual mip count doesn't match what's in the map, the entry is stale.
// Remove it and pass through without clamping.
//
// This is safe because:
// - For a correctly stripped texture: GetDesc returns the STRIPPED mip count
// (e.g., 8), which matches the map entry (8) → proceed with clamping
// - For a stale entry (pointer reuse): GetDesc returns the NEW texture's mip
// count (e.g., 13), which doesn't match the map entry (8) → stale, skip
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateShaderResourceView(ID3D11Resource* r,
 const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** p) {
 // Defensive - validate all params before proceeding
 if (!r || !d || !p) return m_real->CreateShaderResourceView(r, d, p);
 *p = NULL; // Initialize output (prevent dangling pointer)

 // Look up in registry (uses shared SRWLOCK)
 UINT actualMips = LookupStrippedMipCount(r);

 if (actualMips == 0) {
 // Not stripped - pass through unchanged
 if (g_logLevel >= 3) Log(" SRV: pass-through (not stripped)");
 return m_real->CreateShaderResourceView(r, d, p);
 }

 // STALE ENTRY VERIFICATION ──
 // Verify the texture actually has the mip count we think it does.
 // If not, the pointer was reused by a different texture → stale entry.
 ID3D11Texture2D* tex = NULL;
 if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
 D3D11_TEXTURE2D_DESC realDesc;
 tex->GetDesc(&realDesc);
 tex->Release();

 if (realDesc.MipLevels != actualMips) {
 // STALE ENTRY! Remove and pass through without clamping.
 if (g_logLevel >= 2) {
 Log(" STALE ENTRY: map=%u mips, actual=%u mips - removing",
 actualMips, realDesc.MipLevels);
 }
 UnregisterStrippedTexture(tex);
 // Also check format match (extra safety)
 return m_real->CreateShaderResourceView(r, d, p);
 }
 // Verify format matches too (catches pointer reuse with different format)
 if (realDesc.Format != d->Format && d->Format != DXGI_FORMAT_UNKNOWN) {
 // Format mismatch - likely stale entry or the game is doing something unexpected
 // Don't clamp, just pass through
 UnregisterStrippedTexture(tex);
 return m_real->CreateShaderResourceView(r, d, p);
 }
 }

 // Proceed with SRV clamping (verified non-stale) ──
 D3D11_SHADER_RESOURCE_VIEW_DESC clampedDesc = *d;
 bool clamped = false;

 if (clampedDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
 UINT reqMips = clampedDesc.Texture2D.MipLevels;
 UINT reqStart = clampedDesc.Texture2D.MostDetailedMip;
 if (reqMips == 0xFFFFFFFF) reqMips = actualMips - reqStart;
 if (reqStart >= actualMips) { reqStart = 0; reqMips = actualMips; clamped = true; }
 else if (reqStart + reqMips > actualMips) { reqMips = actualMips - reqStart; clamped = true; }
 clampedDesc.Texture2D.MostDetailedMip = reqStart;
 clampedDesc.Texture2D.MipLevels = reqMips;
 } else if (clampedDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
 UINT reqMips = clampedDesc.Texture2DArray.MipLevels;
 UINT reqStart = clampedDesc.Texture2DArray.MostDetailedMip;
 if (reqMips == 0xFFFFFFFF) reqMips = actualMips - reqStart;
 if (reqStart >= actualMips) { reqStart = 0; reqMips = actualMips; clamped = true; }
 else if (reqStart + reqMips > actualMips) { reqMips = actualMips - reqStart; clamped = true; }
 clampedDesc.Texture2DArray.MostDetailedMip = reqStart;
 clampedDesc.Texture2DArray.MipLevels = reqMips;
 }

 if (clamped) {
 g_srvClamped.fetch_add(1);
 if (g_logLevel >= 2) Log("SRV clamped: actualMips=%u", actualMips);
 }
 return m_real->CreateShaderResourceView(r, &clampedDesc, p);
}


// These are mechanical forwards. No interception, no logic.
// If we forget any, the game crashes immediately when it calls the unimplemented method.
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateBuffer(const D3D11_BUFFER_DESC* d, const D3D11_SUBRESOURCE_DATA* i, ID3D11Buffer** p) { return m_real->CreateBuffer(d, i, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateTexture1D(const D3D11_TEXTURE1D_DESC* d, const D3D11_SUBRESOURCE_DATA* i, ID3D11Texture1D** p) { return m_real->CreateTexture1D(d, i, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateTexture3D(const D3D11_TEXTURE3D_DESC* d, const D3D11_SUBRESOURCE_DATA* i, ID3D11Texture3D** p) { return m_real->CreateTexture3D(d, i, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateUnorderedAccessView(ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** p) { return m_real->CreateUnorderedAccessView(r, d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateRenderTargetView(ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* d, ID3D11RenderTargetView** p) { return m_real->CreateRenderTargetView(r, d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDepthStencilView(ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* d, ID3D11DepthStencilView** p) { return m_real->CreateDepthStencilView(r, d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* d, UINT n, const void* s, SIZE_T l, ID3D11InputLayout** p) { return m_real->CreateInputLayout(d, n, s, l, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateVertexShader(const void* s, SIZE_T l, ID3D11ClassLinkage* c, ID3D11VertexShader** p) { return m_real->CreateVertexShader(s, l, c, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateGeometryShader(const void* s, SIZE_T l, ID3D11ClassLinkage* c, ID3D11GeometryShader** p) { return m_real->CreateGeometryShader(s, l, c, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateGeometryShaderWithStreamOutput(const void* s, SIZE_T l, const D3D11_SO_DECLARATION_ENTRY* d, UINT n1, const UINT* b, UINT n2, UINT rs, ID3D11ClassLinkage* c, ID3D11GeometryShader** p) { return m_real->CreateGeometryShaderWithStreamOutput(s, l, d, n1, b, n2, rs, c, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreatePixelShader(const void* s, SIZE_T l, ID3D11ClassLinkage* c, ID3D11PixelShader** p) { return m_real->CreatePixelShader(s, l, c, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateHullShader(const void* s, SIZE_T l, ID3D11ClassLinkage* c, ID3D11HullShader** p) { return m_real->CreateHullShader(s, l, c, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDomainShader(const void* s, SIZE_T l, ID3D11ClassLinkage* c, ID3D11DomainShader** p) { return m_real->CreateDomainShader(s, l, c, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateComputeShader(const void* s, SIZE_T l, ID3D11ClassLinkage* c, ID3D11ComputeShader** p) { return m_real->CreateComputeShader(s, l, c, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateClassLinkage(ID3D11ClassLinkage** p) { return m_real->CreateClassLinkage(p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateBlendState(const D3D11_BLEND_DESC* d, ID3D11BlendState** p) { return m_real->CreateBlendState(d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* d, ID3D11DepthStencilState** p) { return m_real->CreateDepthStencilState(d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateRasterizerState(const D3D11_RASTERIZER_DESC* d, ID3D11RasterizerState** p) { return m_real->CreateRasterizerState(d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateSamplerState(const D3D11_SAMPLER_DESC* d, ID3D11SamplerState** p) { return m_real->CreateSamplerState(d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateQuery(const D3D11_QUERY_DESC* d, ID3D11Query** p) { return m_real->CreateQuery(d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreatePredicate(const D3D11_QUERY_DESC* d, ID3D11Predicate** p) { return m_real->CreatePredicate(d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateCounter(const D3D11_COUNTER_DESC* d, ID3D11Counter** p) { return m_real->CreateCounter(d, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDeferredContext(UINT f, ID3D11DeviceContext** p) { return m_real->CreateDeferredContext(f, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::OpenSharedResource(HANDLE h, REFIID r, void** p) { return m_real->OpenSharedResource(h, r, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CheckFormatSupport(DXGI_FORMAT f, UINT* p) { return m_real->CheckFormatSupport(f, p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CheckMultisampleQualityLevels(DXGI_FORMAT f, UINT c, UINT* p) { return m_real->CheckMultisampleQualityLevels(f, c, p); }
void STDMETHODCALLTYPE WrappedDevice::CheckCounterInfo(D3D11_COUNTER_INFO* p) { m_real->CheckCounterInfo(p); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CheckCounter(const D3D11_COUNTER_DESC* d, D3D11_COUNTER_TYPE* t, UINT* a, LPSTR n, UINT* nl, LPSTR u, UINT* ul, LPSTR de, UINT* dl) { return m_real->CheckCounter(d, t, a, n, nl, u, ul, de, dl); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CheckFeatureSupport(D3D11_FEATURE f, void* p, UINT s) { return m_real->CheckFeatureSupport(f, p, s); }
D3D_FEATURE_LEVEL STDMETHODCALLTYPE WrappedDevice::GetFeatureLevel() { return m_real->GetFeatureLevel(); }
UINT STDMETHODCALLTYPE WrappedDevice::GetCreationFlags() { return m_real->GetCreationFlags(); }
HRESULT STDMETHODCALLTYPE WrappedDevice::GetDeviceRemovedReason() { return m_real->GetDeviceRemovedReason(); }
HRESULT STDMETHODCALLTYPE WrappedDevice::SetExceptionMode(UINT f) { return m_real->SetExceptionMode(f); }
UINT STDMETHODCALLTYPE WrappedDevice::GetExceptionMode() { return m_real->GetExceptionMode(); }

// ID3D11Device1
void STDMETHODCALLTYPE WrappedDevice::GetImmediateContext1(ID3D11DeviceContext1** ppCtx) { if (m_real1) m_real1->GetImmediateContext1(ppCtx); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDeferredContext1(UINT f, ID3D11DeviceContext1** p) { return m_real1 ? m_real1->CreateDeferredContext1(f, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateBlendState1(const D3D11_BLEND_DESC1* d, ID3D11BlendState1** p) { return m_real1 ? m_real1->CreateBlendState1(d, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateRasterizerState1(const D3D11_RASTERIZER_DESC1* d, ID3D11RasterizerState1** p) { return m_real1 ? m_real1->CreateRasterizerState1(d, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::OpenSharedResource1(HANDLE h, REFIID riid, void** ppv) { return m_real1 ? m_real1->OpenSharedResource1(h, riid, ppv) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess, REFIID returnedInterface, void** ppResource) { return m_real1 ? m_real1->OpenSharedResourceByName(lpName, dwDesiredAccess, returnedInterface, ppResource) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDeviceContextState(UINT Flags, const D3D_FEATURE_LEVEL* pFLs, UINT FLs, UINT SDKVer, const IID& EmulatedInterface, D3D_FEATURE_LEVEL* pChosenFL, ID3DDeviceContextState** ppCtxState) { return m_real1 ? m_real1->CreateDeviceContextState(Flags, pFLs, FLs, SDKVer, EmulatedInterface, pChosenFL, ppCtxState) : E_NOINTERFACE; }

// ID3D11Device2
void STDMETHODCALLTYPE WrappedDevice::GetImmediateContext2(ID3D11DeviceContext2** ppCtx) { if (m_real2) m_real2->GetImmediateContext2(ppCtx); else if (ppCtx) *ppCtx = NULL; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDeferredContext2(UINT f, ID3D11DeviceContext2** p) { return m_real2 ? m_real2->CreateDeferredContext2(f, p) : E_NOINTERFACE; }
void STDMETHODCALLTYPE WrappedDevice::GetResourceTiling(ID3D11Resource* r, UINT* pNumTiles, D3D11_PACKED_MIP_DESC* pMipDesc, D3D11_TILE_SHAPE* pTileShape, UINT* pNumSubresourceTilings, UINT FirstSubresourceTiling, D3D11_SUBRESOURCE_TILING* pSubresourceTilings) { if (m_real2) m_real2->GetResourceTiling(r, pNumTiles, pMipDesc, pTileShape, pNumSubresourceTilings, FirstSubresourceTiling, pSubresourceTilings); }
HRESULT STDMETHODCALLTYPE WrappedDevice::CheckMultisampleQualityLevels1(DXGI_FORMAT f, UINT SampleCount, UINT Flags, UINT* pNumQualityLevels) { return m_real2 ? m_real2->CheckMultisampleQualityLevels1(f, SampleCount, Flags, pNumQualityLevels) : E_NOINTERFACE; }

// ID3D11Device3
void STDMETHODCALLTYPE WrappedDevice::GetImmediateContext3(ID3D11DeviceContext3** ppCtx) { if (m_real3) m_real3->GetImmediateContext3(ppCtx); else if (ppCtx) *ppCtx = NULL; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateDeferredContext3(UINT f, ID3D11DeviceContext3** p) { return m_real3 ? m_real3->CreateDeferredContext3(f, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateTexture2D1(const D3D11_TEXTURE2D_DESC1* d, const D3D11_SUBRESOURCE_DATA* i, ID3D11Texture2D1** p) { return m_real3 ? m_real3->CreateTexture2D1(d, i, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateTexture3D1(const D3D11_TEXTURE3D_DESC1* d, const D3D11_SUBRESOURCE_DATA* i, ID3D11Texture3D1** p) { return m_real3 ? m_real3->CreateTexture3D1(d, i, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateShaderResourceView1(ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC1* d, ID3D11ShaderResourceView1** p) { return m_real3 ? m_real3->CreateShaderResourceView1(r, d, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateUnorderedAccessView1(ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC1* d, ID3D11UnorderedAccessView1** p) { return m_real3 ? m_real3->CreateUnorderedAccessView1(r, d, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateRenderTargetView1(ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC1* d, ID3D11RenderTargetView1** p) { return m_real3 ? m_real3->CreateRenderTargetView1(r, d, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateQuery1(const D3D11_QUERY_DESC1* d, ID3D11Query1** p) { return m_real3 ? m_real3->CreateQuery1(d, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateRasterizerState2(const D3D11_RASTERIZER_DESC2* d, ID3D11RasterizerState2** p) { return m_real3 ? m_real3->CreateRasterizerState2(d, p) : E_NOINTERFACE; }
void STDMETHODCALLTYPE WrappedDevice::WriteToSubresource(ID3D11Resource* r, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) { if (m_real3) m_real3->WriteToSubresource(r, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch); }
void STDMETHODCALLTYPE WrappedDevice::ReadFromSubresource(void* pDst, UINT DstRowPitch, UINT DstDepthPitch, ID3D11Resource* r, UINT SrcSubresource, const D3D11_BOX* pSrcBox) { if (m_real3) m_real3->ReadFromSubresource(pDst, DstRowPitch, DstDepthPitch, r, SrcSubresource, pSrcBox); }

// ID3D11Device4
HRESULT STDMETHODCALLTYPE WrappedDevice::RegisterDeviceRemovedEvent(HANDLE h, DWORD* p) { return m_real4 ? m_real4->RegisterDeviceRemovedEvent(h, p) : E_NOINTERFACE; }
void STDMETHODCALLTYPE WrappedDevice::UnregisterDeviceRemoved(DWORD p) { if (m_real4) m_real4->UnregisterDeviceRemoved(p); }

// ID3D11Device5
HRESULT STDMETHODCALLTYPE WrappedDevice::OpenSharedFence(HANDLE h, REFIID r, void** p) { return m_real5 ? m_real5->OpenSharedFence(h, r, p) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedDevice::CreateFence(UINT64 v, D3D11_FENCE_FLAG f, REFIID r, void** p) { return m_real5 ? m_real5->CreateFence(v, f, r, p) : E_NOINTERFACE; }
