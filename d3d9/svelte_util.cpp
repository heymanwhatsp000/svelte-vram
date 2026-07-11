// svelte_util.cpp - Config, logging, BCn helpers, exclusions
#include "svelte_util.h"
#include "svelte_strip.h" // for D3D9_TEXTURE_DESC (used by IsExcluded)
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

const char* SVELTE_VERSION = "v1.2.0";

// Config globals
bool g_initialized = false;
bool g_enabled = true;
int g_maxResolution = 1024; // single knob
int g_minTexDim = 64; // D3D9 textures are often smaller, lower floor
int g_logLevel = 1; // default to basic logging

// Backend detection (for logging only — single code path)
bool g_dxvkBackend = false;

// Exclusion list counter
std::atomic<int> g_texturesSkippedByExclusion{0};

// Performance tracking
FrameTimeStats g_perf = {0, 0, 0, 0, 0, 0, 0};

void PerfUpdate(long long frameDeltaUs) {
    if (frameDeltaUs <= 0) return;
    if (g_perf.frameCount == 0) {
        g_perf.minFrameUs = frameDeltaUs;
        g_perf.maxFrameUs = frameDeltaUs;
        g_perf.avgFrameUs = frameDeltaUs;
    } else {
        if (frameDeltaUs < g_perf.minFrameUs) g_perf.minFrameUs = frameDeltaUs;
        if (frameDeltaUs > g_perf.maxFrameUs) g_perf.maxFrameUs = frameDeltaUs;
        g_perf.avgFrameUs = (g_perf.avgFrameUs * g_perf.frameCount + frameDeltaUs) / (g_perf.frameCount + 1);
    }
    g_perf.frameCount++;
    // Stutter = frame took longer than 33ms (below 30fps)
    if (frameDeltaUs > 33000) {
        g_perf.stutterCount++;
    }
}

void PerfRecordStrip(long long stripUs) {
    g_perf.totalStripTimeUs += stripUs;
    g_perf.stripCallCount++;
}

void PerfLogAndReset() {
    if (g_perf.frameCount == 0) return;
    Log("Perf: frames=%lld min=%.1fms avg=%.1fms max=%.1fms stutters(>33ms)=%lld | strips=%lld totalStripTime=%.1fms avgStrip=%.1fus",
        g_perf.frameCount,
        g_perf.minFrameUs / 1000.0,
        g_perf.avgFrameUs / 1000.0,
        g_perf.maxFrameUs / 1000.0,
        g_perf.stutterCount,
        g_perf.stripCallCount,
        g_perf.totalStripTimeUs / 1000.0,
        g_perf.stripCallCount > 0 ? (double)g_perf.totalStripTimeUs / g_perf.stripCallCount : 0.0);
    // Reset for next window
    g_perf = {0, 0, 0, 0, 0, 0, 0};
}

// Exclusion patterns - loaded once from svelte_exclusions.txt, read-only after init.
// Rationale for NO LOCK: populated only in LoadConfig() (single-threaded
// DLL_PROCESS_ATTACH). After that, read-only. Multiple threads can read
// a std::vector safely as long as no thread writes.
static std::vector<std::string> g_exclusionPatterns;

// Load exclusion patterns from svelte_exclusions.txt.
// Format: one pattern per line. Lines starting with # or ; are comments.
// Blank lines skipped. Patterns are lowercased on load.
// Patterns are substring-matched against the texture fingerprint.
static void LoadExclusions(const char* path) {
 FILE* f = fopen(path, "r");
 if (!f) return;
 char line[256];
 while (fgets(line, sizeof(line), f)) {
 char* p = line;
 while (*p && isspace((unsigned char)*p)) p++;
 if (*p == '#' || *p == ';' || *p == '\0') continue;
 size_t len = strlen(p);
 while (len > 0 && isspace((unsigned char)p[len-1])) { p[--len] = 0; }
 if (len == 0) continue;
 std::string pat(p);
 for (auto& c : pat) c = (char)tolower(c);
 g_exclusionPatterns.push_back(pat);
 }
 fclose(f);
 Log("Loaded %d exclusion patterns from %s", (int)g_exclusionPatterns.size(), path);
}

// Check if a texture matches any exclusion pattern.
// Returns true if the texture should NOT be stripped.
// Fingerprint format: "WIDTHxHEIGHT_FORMAT_mipLEVELS" (lowercase)
// Example: "4096x4096_DXT5_mip13"
// Early return on empty pattern list (zero-cost when no exclusions loaded).
bool IsExcluded(const D3D9_TEXTURE_DESC* pDesc) {
 if (g_exclusionPatterns.empty()) return false;

 char fp[128];
 snprintf(fp, sizeof(fp), "%ux%u_%s_mip%u",
 pDesc->Width, pDesc->Height, FormatName(pDesc->Format), pDesc->Levels);
 std::string fps(fp);
 for (auto& c : fps) c = (char)tolower(c);
 for (const auto& pat : g_exclusionPatterns) {
 if (fps.find(pat) != std::string::npos) return true;
 }
 return false;
}

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
 fprintf(g_logFile, "Backend: dxvk_detected=%d (single-path, no mode switch)\n",
 g_dxvkBackend ? 1 : 0);
 fflush(g_logFile);
 }

 // Load exclusion patterns (if svelte_exclusions.txt exists)
 char exclPath[MAX_PATH];
 snprintf(exclPath, MAX_PATH, "%s\\svelte_exclusions.txt", g_dllDir);
 LoadExclusions(exclPath);

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