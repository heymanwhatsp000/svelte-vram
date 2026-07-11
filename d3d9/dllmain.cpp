// dllmain.cpp - D3D9 Proxy DLL entry point (Svelte)
// Kept small on purpose - just DllMain + the two exported
// functions (MyDirect3DCreate9, MyDirect3DCreate9Ex). All logic
// lives in the svelte_*.cpp modules.
//
// Modules:
// svelte_util.cpp - config, logging, BCn helpers, exclusions
// svelte_strip.cpp - mip selection + StripMips9
// svelte_registry.cpp - stripped-texture map (SRWLOCK + LRU)
// svelte_wrapped_texture.cpp - WrappedTexture9 (transient buffer approach)
// svelte_wrapped_device.cpp - WrappedDevice9 (CreateTexture + SetTexture)
// svelte_wrapped_d3d9.cpp - WrappedD3D9 (CreateDevice intercept)
//
// DLL hijack (Windows search order) → MyDirect3DCreate9 wraps IDirect3D9
// → WrappedD3D9::CreateDevice wraps device → WrappedDevice9::CreateTexture
// strips mips → WrappedTexture9 maintains the API-surface lie
// → WrappedDevice9::SetTexture unwraps before binding
//
// CHAIN SUPPORT:
// At load time, checks for chain DLLs in the game folder:
// d3d9_enb.dll (ENB), d3d9_reshade.dll (ReShade), d3d9_dxvk.dll (DXVK),
// d3d9_orig.dll (any tool), d3d9_chain.dll (generic)
// If found, loads that as the "real" d3d9.dll instead of System32.
// User just renames the other tool's DLL + drops Svelte in. No INI edit.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string>

#include "svelte_util.h"
#include "svelte_wrapped_device.h"
#include "svelte_wrapped_d3d9.h"

#pragma comment(lib, "ole32.lib")

// Real d3d9.dll handle + function pointers
static HMODULE g_realD3D9 = NULL;
typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);
typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);

// Chain DLL names (checked in priority order at load time)
static const char* g_chainNames[] = {
 "d3d9_enb.dll",
 "d3d9_reshade.dll",
 "d3d9_dxvk.dll",
 "d3d9_orig.dll",
 "d3d9_chain.dll",
 NULL
};
static const char* g_loadedChainName = NULL; // which chain DLL was loaded (for logging)

// D3DPERF_SetOptions - no-op (avoids forwarder circular dependency)
// FNV statically imports this. Forwarding to d3d9.D3DPERF_SetOptions
// creates a circular load. No-op is safe - it's just a PIX profiling hint.
extern "C" void WINAPI MyD3DPERF_SetOptions(DWORD dwOptions) {
 // No-op
}

// MyDirect3DCreate9 - intercepts Direct3DCreate9
extern "C" IDirect3D9* WINAPI MyDirect3DCreate9(UINT SDKVersion) {
 if (!g_initialized) LoadConfig();

 static PFN_Direct3DCreate9 realFunc = NULL;
 if (!realFunc) realFunc = (PFN_Direct3DCreate9)GetProcAddress(g_realD3D9, "Direct3DCreate9");

 char exePath[MAX_PATH] = {0};
 GetModuleFileNameA(NULL, exePath, MAX_PATH);
 std::string baseName = GetBasenameLower(exePath);
 Log("Host process: %s", exePath);
 Log("Host basename: %s", baseName.c_str());
 if (g_loadedChainName) {
 Log("Chain: proxy loaded from %s", g_loadedChainName);
 }

 // Launcher pass-through (FNV launcher, Skyrim launcher, etc.)
 bool isLauncher = (baseName.find("launcher") != std::string::npos);
 if (isLauncher) {
 Log("Svelte %s in launcher - pass-through (no wrapping)", SVELTE_VERSION);
 IDirect3D9* d3d = realFunc ? realFunc(SDKVersion) : NULL;
 return d3d;
 }

 Log("Svelte %s loaded - D3D9 COM wrapper + mip stripping", SVELTE_VERSION);
 Log(" max_resolution=%d min_tex=%d", g_maxResolution, g_minTexDim);

 IDirect3D9* d3d = realFunc ? realFunc(SDKVersion) : NULL;
 if (d3d) {
 Log("Direct3DCreate9: IDirect3D9 created (SDKVersion=%u) - wrapping", SDKVersion);
 return new WrappedD3D9(d3d);
 }
 return NULL;
}

// MyDirect3DCreate9Ex - intercepts Direct3DCreate9Ex
extern "C" HRESULT WINAPI MyDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D9Ex) {
 if (!g_initialized) LoadConfig();

 static PFN_Direct3DCreate9Ex realFunc = NULL;
 if (!realFunc) realFunc = (PFN_Direct3DCreate9Ex)GetProcAddress(g_realD3D9, "Direct3DCreate9Ex");

 char exePath[MAX_PATH] = {0};
 GetModuleFileNameA(NULL, exePath, MAX_PATH);
 std::string baseName = GetBasenameLower(exePath);
 Log("Host process: %s", exePath);
 Log("Host basename: %s", baseName.c_str());
 if (g_loadedChainName) {
 Log("Chain: proxy loaded from %s", g_loadedChainName);
 }

 bool isLauncher = (baseName.find("launcher") != std::string::npos);
 if (isLauncher) {
 Log("Svelte %s in launcher (Ex) - pass-through (no wrapping)", SVELTE_VERSION);
 return realFunc ? realFunc(SDKVersion, ppD3D9Ex) : E_FAIL;
 }

 Log("Svelte %s loaded (Ex) - D3D9 COM wrapper + mip stripping", SVELTE_VERSION);
 Log(" max_resolution=%d min_tex=%d", g_maxResolution, g_minTexDim);

 HRESULT hr = realFunc ? realFunc(SDKVersion, ppD3D9Ex) : E_FAIL;
 if (SUCCEEDED(hr) && ppD3D9Ex && *ppD3D9Ex) {
 Log("Direct3DCreate9Ex: IDirect3D9Ex created (SDKVersion=%u) - wrapping", SDKVersion);
 *ppD3D9Ex = (IDirect3D9Ex*)new WrappedD3D9((IDirect3D9*)*ppD3D9Ex);
 }
 return hr;
}

// DLL Entry
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
 switch (dwReason) {
 case DLL_PROCESS_ATTACH: {
 DisableThreadLibraryCalls(hModule);

 // Get our DLL's directory (for chain DLL detection)
 char dllPath[MAX_PATH] = {0};
 GetModuleFileNameA(hModule, dllPath, MAX_PATH);
 char* lastSlash = strrchr(dllPath, '\\');
 if (lastSlash) *lastSlash = 0;

 // Chain DLL auto-detection.
 // Check for common chain DLL names in our folder. If found,
 // load that as the real d3d9.dll (ENB/ReShade/other tool).
 // User just renames the other tool's d3d9.dll, drops ours in.
 g_realD3D9 = NULL;
 g_loadedChainName = NULL;
 for (int i = 0; g_chainNames[i]; i++) {
 char chainPath[MAX_PATH];
 snprintf(chainPath, MAX_PATH, "%s\\%s", dllPath, g_chainNames[i]);
 g_realD3D9 = LoadLibraryA(chainPath);
 if (g_realD3D9) {
 g_loadedChainName = g_chainNames[i];
 break;
 }
 }

 if (!g_realD3D9) {
 // No chain DLL found - load real System32 d3d9.dll
 char sysDir[MAX_PATH];
 GetSystemDirectoryA(sysDir, MAX_PATH);
 char realPath[MAX_PATH];
 snprintf(realPath, MAX_PATH, "%s\\d3d9.dll", sysDir);
 g_realD3D9 = LoadLibraryA(realPath);
 if (!g_realD3D9) g_realD3D9 = LoadLibraryA("d3d9.dll");
 }
 if (!g_realD3D9) return FALSE;

 // DXVK backend detection. Two checks:
 //
 // 1. Did WE load d3d9_dxvk.dll directly? (DXVK is the immediate backend)
 // 2. Does d3d9_dxvk.dll exist in our folder? (DXVK is behind another
 //    chain DLL like d3d9_enb.dll — ENB forwards to DXVK internally)
 //
 // Both must set the flag BEFORE LoadConfig so it logs the correct backend.
 if (g_loadedChainName && strstr(g_loadedChainName, "dxvk")) {
 g_dxvkBackend = true;
 }
 if (!g_dxvkBackend) {
 // Check if d3d9_dxvk.dll exists in our folder. Covers the ENB+DXVK
 // triple-chain case where Svelte loads d3d9_enb.dll, but ENB chains
 // to d3d9_dxvk.dll internally. DXVK is still the ultimate backend,
 // so the transient buffer approach is safe and preferred.
 char dxvkPath[MAX_PATH];
 snprintf(dxvkPath, MAX_PATH, "%s\\d3d9_dxvk.dll", dllPath);
 if (GetFileAttributesA(dxvkPath) != INVALID_FILE_ATTRIBUTES) {
 g_dxvkBackend = true;
 // Note: actual logging happens in LoadConfig() once g_logFile opens.
 // The log line "Backend: dxvk_detected=1" will show the detection result.
 }
 }

 LoadConfig();
 break;
 }
 case DLL_PROCESS_DETACH: {
 Log("============================================================");
 Log("Svelte %s unloading - FINAL SESSION STATS:", SVELTE_VERSION);
 Log("============================================================");
 Log(" Textures created: %d", g_texturesCreated.load());
 Log(" Textures stripped: %d", g_texturesStripped.load());
 Log(" Textures skipped (filter): %d", g_texturesSkipped.load());
 Log(" Textures skipped (exclusion): %d", g_texturesSkippedByExclusion.load());
 Log(" Textures failed: %d", g_texturesFailed.load());
 Log(" Backend: dxvk_detected=%d", g_dxvkBackend ? 1 : 0);
 Log(" VRAM saved: %lld MB (%lld KB)",
 g_vramSavedBytes.load() / (1024 * 1024),
 g_vramSavedBytes.load() / 1024);
 Log(" Frames rendered: %lld", g_frameCount.load());
 if (g_texturesCreated.load() > 0) {
 double stripRate = 100.0 * g_texturesStripped.load() / g_texturesCreated.load();
 Log(" Strip rate: %.1f%%", stripRate);
 if (stripRate < 5.0) {
 Log(" WARNING: Low strip rate - Svelte may not be helping.");
 } else if (stripRate > 30.0) {
 Log(" Good strip rate - Svelte is actively reducing VRAM.");
 }
 }
 Log("============================================================");
 CloseLogFile();
 if (g_realD3D9) FreeLibrary(g_realD3D9);
 break;
 }
 }
 return TRUE;
}