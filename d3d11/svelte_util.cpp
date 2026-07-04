// svelte_util.cpp - Config, logging, BCn helpers, exclusions
#include "svelte_util.h"
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

// Version
const char* SVELTE_VERSION = "v1.0";

// Config globals
bool g_initialized = false;
bool g_enabled = true;
int g_targetMaxDim = 2048;
int g_minTexDim = 1024;
int g_logLevel = 1;

int g_screenWidth = 1920;
int g_screenHeight = 1080;
int g_anisotropicBonus = 1;
int g_maxResolution = 2048; // Max texture dimension after stripping. Lower = less VRAM = blurrier.

// changing dimensions/mips. Safe for Load()/SampleLevel() shaders.

// v3.1-experimental: VRAM lie for streaming engines (FO4). NOT YET IMPLEMENTED.

char g_dllDir[MAX_PATH] = {0};

// Stats counters
std::atomic<int> g_texturesCreated{0};
std::atomic<int> g_texturesStripped{0};
std::atomic<int> g_texturesSkippedByFilter{0};
std::atomic<int> g_texturesSkippedByExclusion{0};
std::atomic<int> g_texturesFailedStrip{0};
std::atomic<int> g_srvClamped{0};
std::atomic<long long> g_vramSavedBytes{0};
std::atomic<long long> g_frameCount{0};
std::atomic<long long> g_mapSweeps{0};
std::atomic<long long> g_mapLRUEvictions{0};

// Logging - uses SRWLOCK instead of important_SECTION
// Rationale for SRWLOCK: SRWLOCK (Slim Reader-Writer Lock) is lighter than
// important_SECTION (~20 bytes vs ~40 bytes) and faster for uncontended
// acquisition. Log writes are always exclusive (write-only), so we use
// the exclusive variant. The perf difference is minimal but SRWLOCK is
// the modern Win32 recommendation.
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

// Exclusion patterns - loaded once, read-only after init
// Rationale for NO LOCK: g_exclusionPatterns is populated only in LoadConfig()
// (single-threaded DLL_PROCESS_ATTACH). After that, it's read-only.
// Multiple threads can read a std::vector safely as long as no thread writes.
// This eliminates the g_exclLock that had - it was unnecessary.
static std::vector<std::string> g_exclusionPatterns;

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


std::string GetBasenameLower(const char* path) {
 if (!path) return "";
 const char* slash = strrchr(path, '\\');
 const char* slash2 = strrchr(path, '/');
 const char* base = (slash2 > slash) ? slash2 : slash;
 if (!base) base = path; else base++;
 std::string s(base);
 for (auto& c : s) c = (char)tolower(c);
 return s;
}


// Game profile auto-detection
// Different games use BCn formats differently:
// - Skyrim SE: BC3/BC7 are safe to strip (separate specular files)
// - Fallout 4: BC3/BC7 may be specular (causes rainbow if stripped aggressively)
// - Witcher 3: BC3 is safe but game creates swap chain via IDXGIFactory
//
// The DLL detects which game it's loaded into and overrides strip_formats
// for aggressive/maximum/custom modes to prevent artifacts.
// Conservative mode is always safe regardless of game (only strips 4K+ textures).
//
// This is stored as a global so StripMips can check it without re-detecting.
static int g_gameProfile = 0; // 0=unknown/generic, 1=skyrim_se, 2=fallout_4, 3=witcher_3

void DetectGameProfile() {
 char exePath[MAX_PATH] = {0};
 GetModuleFileNameA(NULL, exePath, MAX_PATH);
 std::string basename = GetBasenameLower(exePath);

 if (basename == "skyrimse.exe") {
 g_gameProfile = 1;
 Log("Game profile: Skyrim SE (all BCn formats safe for stripping)");
 } else if (basename == "fallout4.exe") {
 g_gameProfile = 2;
 Log("Game profile: Fallout 4 (BC3/BC7 may be specular - restricting aggressive modes to BC1)");
 } else if (basename == "witcher3.exe") {
 g_gameProfile = 3;
 Log("Game profile: Witcher 3 (streaming engine - conservative mode recommended)");
 } else {
 g_gameProfile = 0;
 Log("Game profile: Unknown (%s) - using generic safe defaults", basename.c_str());
 }
}

// Returns true if the current game's BC3/BC7 textures may be specular
// (and thus shouldn't be stripped in aggressive/maximum/custom modes).
// Conservative mode ignores this (only strips 4K+ textures, always safe).
bool GameMayHaveSpecularBC3() {
 return g_gameProfile == 2; // Fallout 4
}

bool IsExcluded(const D3D11_TEXTURE2D_DESC* pDesc) {
 // Early return: if no patterns loaded, skip entirely.
 // This avoids building the fingerprint string (snprintf + lowercasing)
 // which costs ~200ns per call. With 1000+ CreateTexture2D calls per
 // session, this saves ~200μs - small but free.
 if (g_exclusionPatterns.empty()) return false;

 char fp[128];
 snprintf(fp, sizeof(fp), "%ux%u_%s_mip%u",
 pDesc->Width, pDesc->Height, FormatName(pDesc->Format), pDesc->MipLevels);
 std::string fps(fp);
 for (auto& c : fps) c = (char)tolower(c);
 for (const auto& pat : g_exclusionPatterns) {
 if (fps.find(pat) != std::string::npos) return true;
 }
 return false;
}

// Config loading
void LoadConfig() {
 char proxyPath[MAX_PATH];
 HMODULE hSelf = NULL;
 GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&LoadConfig, &hSelf);
 GetModuleFileNameA(hSelf, proxyPath, MAX_PATH);
 char* ls = strrchr(proxyPath, '\\'); if (ls) *ls = '\0';
 strncpy(g_dllDir, proxyPath, MAX_PATH - 1);

 char cfgPath[MAX_PATH]; snprintf(cfgPath, MAX_PATH, "%s\\svelte.ini", proxyPath);

 // Backwards compat: try [svelte] section first, fall back to [optimizer]
 if (GetPrivateProfileIntA("svelte", "enabled", -1, cfgPath) == -1) {
 g_enabled = GetPrivateProfileIntA("optimizer", "enabled", 1, cfgPath) != 0;
 g_targetMaxDim = GetPrivateProfileIntA("optimizer", "target_max_dimension", 2048, cfgPath);
 g_minTexDim = GetPrivateProfileIntA("optimizer", "min_texture_dimension", 1024, cfgPath);
 g_logLevel = GetPrivateProfileIntA("optimizer", "log_level", 1, cfgPath);
 } else {
 g_enabled = GetPrivateProfileIntA("svelte", "enabled", 1, cfgPath) != 0;
 g_targetMaxDim = GetPrivateProfileIntA("svelte", "target_max_dimension", 2048, cfgPath);
 g_minTexDim = GetPrivateProfileIntA("svelte", "min_texture_dimension", 1024, cfgPath);
 g_logLevel = GetPrivateProfileIntA("svelte", "log_level", 1, cfgPath);
 }

 g_screenWidth = GetPrivateProfileIntA("svelte", "screen_width", 1920, cfgPath);
 g_screenHeight = GetPrivateProfileIntA("svelte", "screen_height", 1080, cfgPath);
 g_anisotropicBonus = GetPrivateProfileIntA("svelte", "anisotropic_bonus", 1, cfgPath);

 // Single setting - max_resolution controls everything
 g_maxResolution = GetPrivateProfileIntA("svelte", "max_resolution", 2048, cfgPath);

 char fmtStr[32] = {0};
 GetPrivateProfileStringA("svelte", "strip_formats", "ALL", fmtStr, sizeof(fmtStr), cfgPath);

 // v3.1-experimental: VRAM lie (NOT YET IMPLEMENTED in device wrapper)


 char logPath[MAX_PATH]; snprintf(logPath, MAX_PATH, "%s\\svelte.log", proxyPath);
 g_logFile = fopen(logPath, "a");
 if (g_logFile) {
 fprintf(g_logFile, "\n=== Svelte %s (D3D11 Proxy - Mip Stripping) ===\n", SVELTE_VERSION);
 fprintf(g_logFile, "Config: enabled=%d target_max=%d min_tex=%d log=%d\n",
 g_enabled ? 1 : 0, g_targetMaxDim, g_minTexDim, g_logLevel);
 fflush(g_logFile);
 }
 g_initialized = true;

 char exclPath[MAX_PATH]; snprintf(exclPath, MAX_PATH, "%s\\svelte_exclusions.txt", proxyPath);
 LoadExclusions(exclPath);
}

// Allow dllmain.cpp to close the log file on detach
void CloseLogFile() {
 if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
}

// BCn format helpers
// BCn (Block Compression) is the GPU-native compressed texture format family.
// Each 4x4 pixel block is compressed to a fixed-size unit:
// BC1 (DXT1): 8 bytes per block = 0.5 bytes/pixel - color/diffuse only, no alpha
// BC3 (DXT5): 16 bytes per block = 1.0 bytes/pixel - color + alpha
// BC5 (ATI2): 16 bytes per block = 1.0 bytes/pixel - 2-channel (normal maps)
// BC7: 16 bytes per block = 1.0 bytes/pixel - high quality, all uses
//
// Why only BCn is safe to strip:
// Uncompressed formats (R8G8B8A8, B8G8R8A8) have 4 bytes/pixel and no block
// structure. Stripping mips from them would change the memory layout in ways
// the GPU's texture sampling hardware doesn't expect. BCn's block structure
// means each mip level is an independent set of 4x4 blocks - removing the
// finest mip just removes the highest-resolution blocks, leaving the rest intact.
//
// Why NOT BC2/BC4/BC6H:
// BC2 (DXT3) is rare and has a quirky alpha representation.
// BC4 is single-channel (used for height/gloss maps) - stripping would lose data.
// BC6H is HDR (float16) - used for skyboxes/environment maps, different decode path.
// We skip these to be conservative. Could add them later if a game needs it.

// BC5 is EXCLUDED from stripping. BC5 is almost exclusively used for normal maps.
// Stripping normal map mips causes rainbow/banded lighting artifacts because
// the lighting calculation uses per-pixel normals - when those normals are
// low-resolution (128x128 instead of 512x512), surfaces show rainbow patterns.
// This is worse than the VRAM savings are worth.
// VRAMR also skips normal maps (_normal.dds in their exclusion list).
bool IsBCnFormat(DXGI_FORMAT fmt) {
 return fmt == DXGI_FORMAT_BC1_UNORM || fmt == DXGI_FORMAT_BC1_UNORM_SRGB ||
 fmt == DXGI_FORMAT_BC3_UNORM || fmt == DXGI_FORMAT_BC3_UNORM_SRGB ||
 // BC5_UNORM EXCLUDED - normal maps, stripping causes rainbow artifacts
 fmt == DXGI_FORMAT_BC7_UNORM || fmt == DXGI_FORMAT_BC7_UNORM_SRGB;
}

int BCnBytesPerBlock(DXGI_FORMAT fmt) {
 // BC1 is 8 bytes/block (0.5 bytes/pixel). All others are 16 bytes/block (1 byte/pixel).
 // This is fixed by the BCn specification - not configurable.
 if (fmt == DXGI_FORMAT_BC1_UNORM || fmt == DXGI_FORMAT_BC1_UNORM_SRGB) return 8;
 return 16;
}

// SIMD note: This function is called once per CreateTexture2D (not per frame).
// With ~2000 CreateTexture2D calls per session, the total time is ~200μs.
// SIMD vectorization would save maybe 50μs over the entire session - invisible.
// Not worth the code complexity. Kept as scalar for readability.
UINT CalcTextureBytes(UINT width, UINT height, UINT mips, DXGI_FORMAT fmt) {
 int bpb = BCnBytesPerBlock(fmt);
 UINT total = 0;
 UINT w = width, h = height;
 for (UINT m = 0; m < mips; m++) {
 // Each mip level has ((w+3)/4) * ((h+3)/4) blocks, each block is bpb bytes.
 // The +3 rounds up to the nearest 4 (a partial block at the edge still takes a full block).
 total += ((w + 3) / 4) * ((h + 3) / 4) * bpb;
 w = (w > 1) ? w / 2 : 1;
 h = (h > 1) ? h / 2 : 1;
 }
 return total;
}

const char* FormatName(DXGI_FORMAT fmt) {
 switch (fmt) {
 case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
 case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
 case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
 case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
 case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
 case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
 case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
 default: return "OTHER";
 }
}
