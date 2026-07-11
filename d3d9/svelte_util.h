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

// DXVK detection (informational only). The transient-buffer approach
// works identically with or without DXVK — this flag is for logging.
extern bool g_dxvkBackend;


extern std::atomic<int> g_texturesCreated;
extern std::atomic<int> g_texturesStripped;
extern std::atomic<int> g_texturesSkipped;
extern std::atomic<int> g_texturesFailed;
extern std::atomic<long long> g_vramSavedBytes;
extern std::atomic<long long> g_frameCount;

// Exclusion list counter (textures skipped because they matched a pattern)
extern std::atomic<int> g_texturesSkippedByExclusion;

// Performance tracking (v1.1+)
// Tracks frame times to detect stutters. Logged every 300 frames
// alongside the existing stats block.
struct FrameTimeStats {
    long long minFrameUs;    // fastest frame (microseconds)
    long long maxFrameUs;    // slowest frame (stutter detector)
    long long avgFrameUs;    // average frame time
    long long frameCount;    // frames in this stats window
    long long stutterCount;  // frames > 33ms (30fps threshold)
    long long totalStripTimeUs;  // cumulative time in StripMips calls
    long long stripCallCount;    // number of StripMips calls
};
extern FrameTimeStats g_perf;

void PerfUpdate(long long frameDeltaUs);  // call from Present
void PerfLogAndReset();                    // call every 300 frames
void PerfRecordStrip(long long stripUs);   // call from CreateTexture after strip


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

// Texture exclusion list (ported from D3D11).
// Patterns loaded from svelte_exclusions.txt at startup.
// Fingerprint format: "WIDTHxHEIGHT_FORMAT_mipLEVELS" (lowercase)
// Example: "4096x4096_DXT5_mip13"
// Patterns are substring-matched against the fingerprint.
// A pattern of "4096x4096" matches all 4K textures.
// A pattern of "DXT5_mip13" matches all 13-mip DXT5 textures.
struct D3D9_TEXTURE_DESC; // forward declaration (defined in svelte_strip.h)
bool IsExcluded(const D3D9_TEXTURE_DESC* pDesc);

std::string GetBasenameLower(const char* path);