// svelte_wrapped_device.h - COM wrapper for ID3D11Device (all 6 versions)
// Wraps the real ID3D11Device so we can intercept:
// - CreateTexture2D → strip mips (the main VRAM reduction)
// - CreateShaderResourceView → clamp mip range (prevent black textures)
//
// All other ~40 methods are forwarded to the real device unchanged.
//
// QUERYINTERFACE TRICK:
// The game may call QueryInterface for ID3D11Device1/2/3/4/5 (newer interface
// versions). If we let this fall through to the real device, the game gets
// an unwrapped ID3D11Device3* and bypasses us entirely. So QI returns `this`
// for ALL 6 device interfaces. The game always holds our wrapper.
//
// This is why we inherit from ID3D11Device5 (the latest version) - we must
// implement all methods of all 6 interfaces to satisfy the vtable layout.
#pragma once
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_3.h>
#include <d3d11_4.h>

class WrappedDevice : public ID3D11Device5 {
private:
 ID3D11Device* m_real;
 ID3D11Device1* m_real1;
 ID3D11Device2* m_real2;
 ID3D11Device3* m_real3;
 ID3D11Device4* m_real4;
 ID3D11Device5* m_real5;
 LONG m_refCount;

public:
 WrappedDevice(ID3D11Device* real);
 ~WrappedDevice();

 // IUnknown
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
 ULONG STDMETHODCALLTYPE AddRef() override;
 ULONG STDMETHODCALLTYPE Release() override;

 // ID3D11DeviceChild
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override;
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override;
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override;

 // ID3D11Device - the methods we intercept
 void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext**) override;
 HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**) override;
 HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**) override;

 // ID3D11Device - forwarded methods (all other ~35)
 HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**) override;
 HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**) override;
 HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**) override;
 HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**) override;
 HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**) override;
 HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**) override;
 HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**) override;
 HRESULT STDMETHODCALLTYPE CreateVertexShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**) override;
 HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**) override;
 HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**) override;
 HRESULT STDMETHODCALLTYPE CreatePixelShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**) override;
 HRESULT STDMETHODCALLTYPE CreateHullShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**) override;
 HRESULT STDMETHODCALLTYPE CreateDomainShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**) override;
 HRESULT STDMETHODCALLTYPE CreateComputeShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**) override;
 HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage**) override;
 HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC*, ID3D11BlendState**) override;
 HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC*, ID3D11DepthStencilState**) override;
 HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**) override;
 HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC*, ID3D11SamplerState**) override;
 HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC*, ID3D11Query**) override;
 HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC*, ID3D11Predicate**) override;
 HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC*, ID3D11Counter**) override;
 HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT, ID3D11DeviceContext**) override;
 HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE, REFIID, void**) override;
 HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT, UINT*) override;
 HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT, UINT, UINT*) override;
 void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO*) override;
 HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC*, D3D11_COUNTER_TYPE*, UINT*, LPSTR, UINT*, LPSTR, UINT*, LPSTR, UINT*) override;
 HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE, void*, UINT) override;
 D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override;
 UINT STDMETHODCALLTYPE GetCreationFlags() override;
 HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
 HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT) override;
 UINT STDMETHODCALLTYPE GetExceptionMode() override;

 // ID3D11Device1
 void STDMETHODCALLTYPE GetImmediateContext1(ID3D11DeviceContext1**) override;
 HRESULT STDMETHODCALLTYPE CreateDeferredContext1(UINT, ID3D11DeviceContext1**) override;
 HRESULT STDMETHODCALLTYPE CreateBlendState1(const D3D11_BLEND_DESC1*, ID3D11BlendState1**) override;
 HRESULT STDMETHODCALLTYPE CreateRasterizerState1(const D3D11_RASTERIZER_DESC1*, ID3D11RasterizerState1**) override;
 HRESULT STDMETHODCALLTYPE OpenSharedResource1(HANDLE, REFIID, void**) override;
 HRESULT STDMETHODCALLTYPE OpenSharedResourceByName(LPCWSTR, DWORD, REFIID, void**) override;
 HRESULT STDMETHODCALLTYPE CreateDeviceContextState(UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const IID&, D3D_FEATURE_LEVEL*, ID3DDeviceContextState**) override;

 // ID3D11Device2
 void STDMETHODCALLTYPE GetImmediateContext2(ID3D11DeviceContext2**) override;
 HRESULT STDMETHODCALLTYPE CreateDeferredContext2(UINT, ID3D11DeviceContext2**) override;
 void STDMETHODCALLTYPE GetResourceTiling(ID3D11Resource*, UINT*, D3D11_PACKED_MIP_DESC*, D3D11_TILE_SHAPE*, UINT*, UINT, D3D11_SUBRESOURCE_TILING*) override;
 HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels1(DXGI_FORMAT, UINT, UINT, UINT*) override;

 // ID3D11Device3
 void STDMETHODCALLTYPE GetImmediateContext3(ID3D11DeviceContext3**) override;
 HRESULT STDMETHODCALLTYPE CreateDeferredContext3(UINT, ID3D11DeviceContext3**) override;
 HRESULT STDMETHODCALLTYPE CreateTexture2D1(const D3D11_TEXTURE2D_DESC1*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D1**) override;
 HRESULT STDMETHODCALLTYPE CreateTexture3D1(const D3D11_TEXTURE3D_DESC1*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D1**) override;
 HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC1*, ID3D11ShaderResourceView1**) override;
 HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView1(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC1*, ID3D11UnorderedAccessView1**) override;
 HRESULT STDMETHODCALLTYPE CreateRenderTargetView1(ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC1*, ID3D11RenderTargetView1**) override;
 HRESULT STDMETHODCALLTYPE CreateQuery1(const D3D11_QUERY_DESC1*, ID3D11Query1**) override;
 HRESULT STDMETHODCALLTYPE CreateRasterizerState2(const D3D11_RASTERIZER_DESC2*, ID3D11RasterizerState2**) override;
 void STDMETHODCALLTYPE WriteToSubresource(ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT) override;
 void STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*) override;

 // ID3D11Device4
 HRESULT STDMETHODCALLTYPE RegisterDeviceRemovedEvent(HANDLE, DWORD*) override;
 void STDMETHODCALLTYPE UnregisterDeviceRemoved(DWORD) override;

 // ID3D11Device5
 HRESULT STDMETHODCALLTYPE OpenSharedFence(HANDLE, REFIID, void**) override;
 HRESULT STDMETHODCALLTYPE CreateFence(UINT64, D3D11_FENCE_FLAG, REFIID, void**) override;
};
