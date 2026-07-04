// dllmain.cpp - D3D11 Proxy DLL entry point (Svelte)
// Kept small on purpose - just DllMain + the two exported
// functions (MyCreateDevice, MyCreateDeviceAndSwapChain). All logic
// lives in the svelte_*.cpp modules.
//
// Modules:
// svelte_util.cpp - config, logging, BCn helpers, exclusions
// svelte_strip.cpp - principled mip selection + StripMips
// svelte_registry.cpp - stripped-texture map (SRWLOCK + LRU)
// svelte_wrapped_texture.cpp - WrappedTexture (Release hook for map cleanup)
// svelte_wrapped_device.cpp - WrappedDevice (CreateTexture2D + SRV interception)
// svelte_wrapped_swapchain.cpp - WrappedSwapChain (Present stats + sweep)
//
// DLL hijack (Windows search order) → MyCreateDevice wraps real device
// → WrappedDevice::CreateTexture2D strips mips → WrappedTexture tracks lifecycle
// → WrappedDevice::CreateShaderResourceView clamps mip range
// → WrappedSwapChain::Present logs stats + sweeps map
//
// BUILD: build_proxy.bat compiles all .cpp files into one d3d11.dll
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <stdio.h>

#include "svelte_util.h"
#include "svelte_wrapped_device.h"
#include "svelte_wrapped_swapchain.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")


// (it's not in the header because it's only called from DllMain)
extern void CloseLogFile();

// Real d3d11.dll handle + function pointers
static HMODULE g_realD3D11 = NULL;
typedef HRESULT (WINAPI *PFN_RealCreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *PFN_RealCreateDeviceAndSwapChain)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

// MyCreateDevice - intercepts D3D11CreateDevice
extern "C" HRESULT WINAPI MyCreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
 const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
 ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
 if (!g_initialized) LoadConfig();
 static PFN_RealCreateDevice realFunc = NULL;
 if (!realFunc) realFunc = (PFN_RealCreateDevice)GetProcAddress(g_realD3D11, "D3D11CreateDevice");
 ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL;
 HRESULT hr = realFunc(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
 ppDevice ? &dev : NULL, pFeatureLevel, ppImmediateContext ? &ctx : NULL);
 if (SUCCEEDED(hr) && ppDevice && dev) {
 // Log adapter info
 if (pAdapter) {
 DXGI_ADAPTER_DESC adesc;
 if (SUCCEEDED(pAdapter->GetDesc(&adesc))) {
 char gpuName[256] = {0};
 WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, gpuName, sizeof(gpuName), NULL, NULL);
 Log("Adapter: %s VRAM=%llu MB VendorID=0x%04X DeviceID=0x%04X",
 gpuName, (unsigned long long)(adesc.DedicatedVideoMemory / (1024*1024)),
 adesc.VendorId, adesc.DeviceId);
 }
 }
 if (pFeatureLevel && *pFeatureLevel) {
 Log("Feature level: 0x%X", (unsigned)*pFeatureLevel);
 }
 WrappedDevice* wrapped = new WrappedDevice(dev);
 *ppDevice = wrapped;
 Log("D3D11CreateDevice: device wrapped (%s, mip stripping %s)",
 SVELTE_VERSION, g_enabled ? "active" : "DISABLED");
 }
 if (ppImmediateContext && ctx) *ppImmediateContext = ctx;
 else if (ctx) ctx->Release();
 return hr;
}

// MyCreateDeviceAndSwapChain - intercepts D3D11CreateDeviceAndSwapChain
extern "C" HRESULT WINAPI MyCreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
 const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
 const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
 ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
 if (!g_initialized) LoadConfig();
 static PFN_RealCreateDeviceAndSwapChain realFunc = NULL;
 if (!realFunc) realFunc = (PFN_RealCreateDeviceAndSwapChain)GetProcAddress(g_realD3D11, "D3D11CreateDeviceAndSwapChain");
 ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL;
 IDXGISwapChain* sc = NULL;
 HRESULT hr = realFunc(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
 pSwapChainDesc, ppSwapChain ? &sc : NULL, ppDevice ? &dev : NULL, pFeatureLevel, ppImmediateContext ? &ctx : NULL);
 if (SUCCEEDED(hr) && ppSwapChain && sc) {
 *ppSwapChain = new WrappedSwapChain(sc);
 // Capture screen resolution from swap chain desc
 if (pSwapChainDesc) {
 int newW = (int)pSwapChainDesc->BufferDesc.Width;
 int newH = (int)pSwapChainDesc->BufferDesc.Height;
 if (newW > 0 && newH > 0) {
 g_screenWidth = newW;
 g_screenHeight = newH;
 Log("Screen resolution: %dx%d", newW, newH);
 }
 }
 }
 if (SUCCEEDED(hr) && ppDevice && dev) {
 if (pAdapter) {
 DXGI_ADAPTER_DESC adesc;
 if (SUCCEEDED(pAdapter->GetDesc(&adesc))) {
 char gpuName[256] = {0};
 WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, gpuName, sizeof(gpuName), NULL, NULL);
 Log("Adapter: %s VRAM=%llu MB VendorID=0x%04X DeviceID=0x%04X",
 gpuName, (unsigned long long)(adesc.DedicatedVideoMemory / (1024*1024)),
 adesc.VendorId, adesc.DeviceId);
 }
 }
 if (pFeatureLevel && *pFeatureLevel) {
 Log("Feature level: 0x%X", (unsigned)*pFeatureLevel);
 }
 WrappedDevice* wrapped = new WrappedDevice(dev);
 *ppDevice = wrapped;
 Log("D3D11CreateDeviceAndSwapChain: device wrapped (%s, mip stripping %s)",
 SVELTE_VERSION, g_enabled ? "active" : "DISABLED");
 }
 if (ppImmediateContext && ctx) *ppImmediateContext = ctx;
 else if (ctx) ctx->Release();
 return hr;
}

// DLL Entry
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
 switch (dwReason) {
 case DLL_PROCESS_ATTACH: {
 DisableThreadLibraryCalls(hModule);
 // Chain DLL auto-detection (same as D3D9)
 // Checks for ENB/ReShade/DXVK chain DLLs in our folder.
 char dllPath[MAX_PATH] = {0};
 GetModuleFileNameA(hModule, dllPath, MAX_PATH);
 char* lastSlash = strrchr(dllPath, '\\');
 if (lastSlash) *lastSlash = 0;
 const char* chainNames[] = {
 "d3d11_enb.dll", "d3d11_reshade.dll",
 "d3d11_dxvk.dll", "d3d11_orig.dll", NULL
 };
 g_realD3D11 = NULL;
 for (int i = 0; chainNames[i]; i++) {
 char chainPath[MAX_PATH];
 snprintf(chainPath, MAX_PATH, "%s\\%s", dllPath, chainNames[i]);
 g_realD3D11 = LoadLibraryA(chainPath);
 if (g_realD3D11) break;
 }
 if (!g_realD3D11) {
 char sysDir[MAX_PATH]; GetSystemDirectoryA(sysDir, MAX_PATH);
 char realPath[MAX_PATH]; snprintf(realPath, MAX_PATH, "%s\\d3d11.dll", sysDir);
 g_realD3D11 = LoadLibraryA(realPath);
 if (!g_realD3D11) g_realD3D11 = LoadLibraryA("d3d11.dll");
 }
 if (!g_realD3D11) return FALSE;

 LoadConfig();

 // Log chain status (which chain DLL was found, or none)
 bool chainFound = false;
 for (int i = 0; chainNames[i]; i++) {
 char chainPath[MAX_PATH];
 snprintf(chainPath, MAX_PATH, "%s\\%s", dllPath, chainNames[i]);
 if (GetFileAttributesA(chainPath) != INVALID_FILE_ATTRIBUTES) {
 Log("Chain: proxy loaded from %s", chainNames[i]);
 chainFound = true;
 break;
 }
 }
 if (!chainFound) {
 Log("Chain: no chain DLL found, forwarding to System32");
 }

 // Log host process info
 char exePath[MAX_PATH] = {0};
 GetModuleFileNameA(NULL, exePath, MAX_PATH);
 std::string exeBase = GetBasenameLower(exePath);
 Log("Host process: %s", exePath);
 Log("Host basename: %s", exeBase.c_str());
 // Auto-detect game profile (applies safe per-game defaults)
 DetectGameProfile();

 Log("Svelte %s loaded - COM wrapper + mip stripping + SRV clamping", SVELTE_VERSION);
 Log(" Device3/4/5 wrapping: enabled");
 Log(" Exclusion list: %s", "see config");
 Log(" Map cleanup: LRU eviction at %zu entries", (size_t)50000);
 Log(" WrappedTexture: enabled (pointer reuse fix)");
 Log(" SRWLOCK: enabled (replaces important_SECTION)");
 Log(" Log level: %d (1=basic, 2=+filter reasons, 3=+every texture)", g_logLevel);
 Log(" max_resolution=%d min_tex=%d", g_maxResolution, g_minTexDim);
 break;
 }
 case DLL_PROCESS_DETACH: {
 // Final stats summary
 Log("============================================================");
 Log("Svelte %s unloading - FINAL SESSION STATS:", SVELTE_VERSION);
 Log("============================================================");
 Log(" Textures created: %d", g_texturesCreated.load());
 Log(" Textures stripped: %d", g_texturesStripped.load());
 Log(" Skipped by filter: %d", g_texturesSkippedByFilter.load());
 Log(" Skipped by exclusion: %d", g_texturesSkippedByExclusion.load());
 Log(" Failed strip: %d", g_texturesFailedStrip.load());
 Log(" VRAM saved: %lld MB (%lld KB)",
 g_vramSavedBytes.load() / (1024 * 1024),
 g_vramSavedBytes.load() / 1024);
 Log(" SRV clamps: %d", g_srvClamped.load());
 Log(" Map sweeps: %lld", g_mapSweeps.load());
 Log(" Map LRU evictions: %lld", g_mapLRUEvictions.load());
 Log(" Frames rendered: %lld", g_frameCount.load());
 if (g_texturesCreated.load() > 0) {
 double stripRate = 100.0 * g_texturesStripped.load() / g_texturesCreated.load();
 double savedPerTex = g_texturesStripped.load() > 0
 ? (double)g_vramSavedBytes.load() / g_texturesStripped.load() / 1024.0
 : 0.0;
 Log(" Strip rate: %.1f%%", stripRate);
 Log(" Avg saved per strip: %.1f KB", savedPerTex);
 if (stripRate < 5.0) {
 Log(" WARNING: Low strip rate - Svelte may not be helping this game.");
 Log(" Common cause: game uses deferred texture uploads (no pInitialData).");
 // (log hint removed - logging is hardcoded off)
 } else if (stripRate > 30.0) {
 Log(" Good strip rate - Svelte is actively reducing VRAM.");
 }
 }
 Log("============================================================");
 CloseLogFile();
 if (g_realD3D11) FreeLibrary(g_realD3D11);
 break;
 }
 }
 return TRUE;
}
