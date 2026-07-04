// svelte_util.h - Config, logging, BCn helpers, exclusions
// This module collects small utility functions that don't depend on
// D3D11 COM wrappers. Keeping them separate avoids circular includes
// between wrapped_device.cpp and strip_mips.cpp.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <unordered_set>
#include <vector>
#include <string>


extern const char* SVELTE_VERSION;


extern bool g_initialized;
extern bool g_enabled;
extern int g_targetMaxDim;
extern int g_minTexDim;
extern int g_logLevel;

// principled config (dead in, kept for compat)
extern int g_screenWidth;
extern int g_screenHeight;
extern int g_anisotropicBonus;

// Strip mode (dead in, kept for compat)

// Maximum texture dimension after stripping. Lower = less VRAM = blurrier.
extern int g_maxResolution;

// Format conversion (BC3→BC1, BC7→BC1). Reduces bytes-per-block
// without changing dimensions/mips. Safe for Load()/SampleLevel() shaders.

// v3.1-experimental: VRAM lie (for streaming engines like FO4).
// Intercepts IDXGIAdapter::GetDesc to report less VRAM than actual.
// Forces streaming engines to be more conservative. EXPERIMENTAL.

// Path to the folder containing this DLL (for finding svelte.ini, svelte.log, etc.)
extern char g_dllDir[MAX_PATH];


extern std::atomic<int> g_texturesCreated;
extern std::atomic<int> g_texturesStripped;
extern std::atomic<int> g_texturesSkippedByFilter;
extern std::atomic<int> g_texturesSkippedByExclusion;
extern std::atomic<int> g_texturesFailedStrip;
extern std::atomic<int> g_srvClamped;
extern std::atomic<long long> g_vramSavedBytes;
extern std::atomic<long long> g_frameCount;
extern std::atomic<long long> g_mapSweeps;
extern std::atomic<long long> g_mapLRUEvictions;


void Log(const char* fmt, ...);


void LoadConfig();


bool IsBCnFormat(DXGI_FORMAT fmt);
int BCnBytesPerBlock(DXGI_FORMAT fmt);
UINT CalcTextureBytes(UINT width, UINT height, UINT mips, DXGI_FORMAT fmt);
const char* FormatName(DXGI_FORMAT fmt);


bool IsExcluded(const D3D11_TEXTURE2D_DESC* pDesc);

std::string GetBasenameLower(const char* path);


void DetectGameProfile();
bool GameMayHaveSpecularBC3();