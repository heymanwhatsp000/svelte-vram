// svelte_util.h - Config, logging, BCn helpers
// Mirrors the D3D11 svelte_util.h structure but uses D3D9 types (D3DFORMAT
// instead of DXGI_FORMAT, IDirect3DDevice9 instead of ID3D11Device, etc).
//
// D3D9 format codes:
// D3DFMT_DXT1 = BC1 (8 bytes/block, no alpha)
// D3DFMT_DXT3 = BC2 (16 bytes/block, explicit alpha)
// D3DFMT_DXT5 = BC3 (16 bytes/block, interpolated alpha)
//
// No BC4/BC5/BC6H/BC7 in D3D9 - those are D3D11-only formats. D3D9 games
// (FNV, Skyrim LE, FO3) use only DXT1/DXT3/DXT5.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <unordered_set>
#include <vector>
#include <string>


extern const char* SVELTE_VERSION;


extern bool g_initialized;
extern bool g_enabled;
extern int g_maxResolution; // max texture dimension after stripping (single knob)
extern int g_minTexDim;
extern int g_logLevel;

// Path to the folder containing this DLL (for finding svelte.ini, svelte.log)
extern char g_dllDir[MAX_PATH];


extern std::atomic<int> g_texturesCreated;
extern std::atomic<int> g_texturesStripped;
extern std::atomic<int> g_texturesSkipped;
extern std::atomic<int> g_texturesFailed;
extern std::atomic<long long> g_vramSavedBytes;
extern std::atomic<long long> g_frameCount;


// Thread-safe logging to svelte.log. Uses SRWLOCK.
void Log(const char* fmt, ...);
void CloseLogFile();


void LoadConfig();


// D3D9 uses D3DFORMAT (FourCC-based) instead of DXGI_FORMAT.
// DXT1=BC1, DXT3=BC2, DXT5=BC3. No BC4/BC5/BC6H/BC7 in D3D9.
bool IsStrippableFormat(D3DFORMAT fmt);
int BCnBytesPerBlock(D3DFORMAT fmt);
UINT CalcTextureBytes(UINT width, UINT height, UINT mips, D3DFORMAT fmt);
const char* FormatName(D3DFORMAT fmt);

std::string GetBasenameLower(const char* path);