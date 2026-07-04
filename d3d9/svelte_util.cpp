// svelte_util.cpp - Config, logging, BCn helpers
#include "svelte_util.h"
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

// Version
const char* SVELTE_VERSION = "v1.0";

// Config globals
bool g_initialized = false;
bool g_enabled = true;
int g_maxResolution = 1024; // single knob
int g_minTexDim = 64; // D3D9 textures are often smaller, lower floor
int g_logLevel = 1; // default to basic logging

char g_dllDir[MAX_PATH] = {0};

// Stats counters
std::atomic<int> g_texturesCreated{0};
std::atomic<int> g_texturesStripped{0};
std::atomic<int> g_texturesSkipped{0};
std::atomic<int> g_texturesFailed{0};
std::atomic<long long> g_vramSavedBytes{0};
std::atomic<long long> g_frameCount{0};

// Logging - uses SRWLOCK (same as D3D11)
static SRWLOCK g_logLock = SRWLOCK_INIT;
static FILE* g_logFile = NULL;

void Log(const char* fmt, ...) {
 if (!g_logFile) return;
 AcquireSRWLockExclusive(&g_logLock);
 va_list args; va_start(args, fmt);
 vfprintf(g_logFile, fmt, args); va_end(args);
 fprintf(g_logFile, "\n"); fflush(g_logFile);
 ReleaseSRWLockExclusive(&g_logLock);
}

void CloseLogFile() {
 if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
}

// Config loading
void LoadConfig() {
 // Get this DLL's path
 HMODULE hSelf = NULL;
 GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
 (LPCSTR)&LoadConfig, &hSelf);
 char dllPath[MAX_PATH] = {0};
 GetModuleFileNameA(hSelf, dllPath, MAX_PATH);

 // Extract directory
 char* lastSlash = strrchr(dllPath, '\\');
 if (lastSlash) {
 *lastSlash = 0;
 strncpy(g_dllDir, dllPath, MAX_PATH - 1);
 }

 char cfgPath[MAX_PATH];
 snprintf(cfgPath, MAX_PATH, "%s\\svelte.ini", g_dllDir);

 // Read config (same keys as D3D11)
 g_enabled = GetPrivateProfileIntA("svelte", "enabled", 1, cfgPath) != 0;
 g_maxResolution = GetPrivateProfileIntA("svelte", "max_resolution", 1024, cfgPath);
 g_minTexDim = GetPrivateProfileIntA("svelte", "min_texture_dimension", 64, cfgPath);
 g_logLevel = GetPrivateProfileIntA("svelte", "log_level", 2, cfgPath);

 // Open log file (append mode - like D3D11)
 char logPath[MAX_PATH];
 snprintf(logPath, MAX_PATH, "%s\\svelte.log", g_dllDir);
 g_logFile = fopen(logPath, "a");
 if (g_logFile) {
 fprintf(g_logFile, "\n=== Svelte %s (D3D9 Proxy - Mip Stripping) ===\n", SVELTE_VERSION);
 fprintf(g_logFile, "Config: enabled=%d max_resolution=%d min_tex=%d log=%d\n",
 g_enabled ? 1 : 0, g_maxResolution, g_minTexDim, g_logLevel);
 fflush(g_logFile);
 }
 g_initialized = true;
}

// BCn format helpers (D3D9 versions)
// D3D9 format codes are FourCC-based:
// D3DFMT_DXT1 = MAKEFOURCC('D','X','T','1') = BC1
// D3DFMT_DXT3 = MAKEFOURCC('D','X','T','3') = BC2
// D3DFMT_DXT5 = MAKEFOURCC('D','X','T','5') = BC3
// (d3d9.h defines these as D3DFMT_DXT1 etc.)

bool IsStrippableFormat(D3DFORMAT fmt) {
 return fmt == D3DFMT_DXT1 ||
 fmt == D3DFMT_DXT3 ||
 fmt == D3DFMT_DXT5;
}

int BCnBytesPerBlock(D3DFORMAT fmt) {
 if (fmt == D3DFMT_DXT1) return 8; // BC1: 8 bytes per 4x4 block
 if (fmt == D3DFMT_DXT3) return 16; // BC2: 16 bytes per 4x4 block
 if (fmt == D3DFMT_DXT5) return 16; // BC3: 16 bytes per 4x4 block
 return 0; // Non-BCn or unsupported
}

const char* FormatName(D3DFORMAT fmt) {
 if (fmt == D3DFMT_DXT1) return "DXT1";
 if (fmt == D3DFMT_DXT3) return "DXT3";
 if (fmt == D3DFMT_DXT5) return "DXT5";
 if (fmt == D3DFMT_A8R8G8B8) return "A8R8G8B8";
 if (fmt == D3DFMT_X8R8G8B8) return "X8R8G8B8";
 if (fmt == D3DFMT_R5G6B5) return "R5G6B5";
 if (fmt == D3DFMT_A1R5G5B5) return "A1R5G5B5";
 if (fmt == D3DFMT_A4R4G4B4) return "A4R4G4B4";
 if (fmt == D3DFMT_A8) return "A8";
 if (fmt == D3DFMT_A8B8G8R8) return "A8B8G8R8";
 if (fmt == D3DFMT_X8B8G8R8) return "X8B8G8R8";
 if (fmt == D3DFMT_L8) return "L8";
 if (fmt == D3DFMT_A8L8) return "A8L8";
 static char buf[16];
 snprintf(buf, sizeof(buf), "0x%08X", (unsigned)fmt);
 return buf;
}

UINT CalcTextureBytes(UINT width, UINT height, UINT mips, D3DFORMAT fmt) {
 int bpb = BCnBytesPerBlock(fmt);
 if (bpb == 0) return 0; // Non-BCn - can't calculate
 UINT total = 0;
 for (UINT i = 0; i < mips; i++) {
 UINT mw = (width >> i); if (mw < 1) mw = 1;
 UINT mh = (height >> i); if (mh < 1) mh = 1;
 UINT blocksX = (mw + 3) / 4;
 UINT blocksY = (mh + 3) / 4;
 total += blocksX * blocksY * bpb;
 }
 return total;
}


std::string GetBasenameLower(const char* path) {
 const char* slash = strrchr(path, '\\');
 const char* base = slash ? slash + 1 : path;
 std::string s(base);
 for (auto& c : s) c = (char)tolower(c);
 return s;
}