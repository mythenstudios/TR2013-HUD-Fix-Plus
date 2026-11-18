// =============================================================================
//  TR2013 HUD Fix+ (TR_HUD_Fix) - Tomb Raider (2013) multiplayer-safe HUD position fix
//
//  Single target: build\d3d11.dll (proxy, exports via src\exports.def). The
//  former ASI variant was removed (ASI loaders cannot load into TombRaider.exe).
//
//  Architecture:
//    * Proxy + chain loading: D3D11CreateDevice* are forwarded to ProxyDll
//      (ini), d3d11_orig.dll / dxvk_d3d11.dll, or the system d3d11.dll; all
//      other exports are lazily resolved pass-through stubs.
//    * ID3D11DeviceContext vtable hooks per table, with draw-entry "re-arm"
//      (the D3D11 runtime rewrites its own draw entries). Device hooks keep a
//      shader-bytecode hash registry; DXGI hooks provide Present/frame
//      boundaries, the FPS limiter and the [Display] options.
//    * Fix rules (gameplay only, at most one per HUD draw): P prompt, U popup
//      group, u popup carry-over, O popup at the stage origin, S salvage
//      counter, B backdrop. A menu gate (G1/G2/G3) disables all rules in menus.
//      Corrections are written into OUR OWN dynamic 192-byte constant buffer,
//      bound for exactly one draw - game memory is never modified.
//    * Dear ImGui in-game menu / FPS overlay (initialised and window subclass
//      installed only on first use).
//    * Diagnostics (log, F10 capture, watchdog, [INV], [MEM]) run only with
//      EnableLogging=1/2 (crash handler: see below, always installed). With
//      EnableLogging=0 (default) every diagnostic path exits on a single flag
//      check, no log file is created and no diagnostic container is allocated.
//    * v1.4.6 always-on crash report: a log-only vectored exception handler
//      (never handles anything) is installed in every configuration. With a
//      log file it writes [CRASH] lines into tr_hud_fix.log; with
//      EnableLogging=0 it creates tr_hud_fix_crash.txt in the game folder
//      (append, %TEMP% fallback) ONLY when a qualifying exception occurs (max.
//      3 reports per process): fault module+offset (incl. execute at a null /
//      unmapped address with the return address at [esp]), registers,
//      address-space state, map-redirect/re-arm counters and the last
//      DXGI/display/device events. Nothing is written otherwise.
//    * v1.4.6 low address-space warning ([Memory] MemWarning): a time-sliced
//      address-space walk from the Present hook (max. 64 VirtualQuery calls
//      per frame, a new pass every 10 s), an ImGui toast when the 32-bit
//      address space runs low (only if the overlay is already initialised).
//
//  Multiplayer rule: this code NEVER touches code or data of TombRaider.exe,
//  steam_api.dll or EOSSDK-Win32-Shipping.dll (no IAT hooks in the game EXE).
//  Only D3D11/DXGI object vtables are patched.
//
//  Build: x86 only, cl /O2 /MT /std:c++17 /EHsc /W4 (Dear ImGui: /O1; the
//  IMGUI_DISABLE_* defines in build.bat must match for all translation
//  units) - never link d3d11.lib or
//  dxgi.lib (the proxy would import itself). All D3D entry points are resolved
//  with GetProcAddress.
// =============================================================================

#if !defined(_M_IX86)
#error "TR_HUD_Fix must be compiled for 32-bit x86 (use vcvars32.bat)."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <psapi.h>   // PROCESS_MEMORY_COUNTERS_EX only; K32GetProcessMemoryInfo is resolved from kernel32
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_5.h> // IDXGIFactory5 / allow tearing (flip model)

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// -----------------------------------------------------------------------------
//  Marker string. build.bat uses `findstr /m /c:"TR_HUD_Fix_Marker_v1"` to
//  recognise our own d3d11.dll in the game folder (never overwrite foreign
//  DLLs such as DXVK or ReShade). /INCLUDE guarantees the linker keeps it.
// -----------------------------------------------------------------------------
extern "C" const char TRHF_Marker[] = "TR_HUD_Fix_Marker_v1";
#pragma comment(linker, "/INCLUDE:_TRHF_Marker")

static const char* const kVersion = "1.4.6";

static const char* const kBuildTarget = "PROXY (d3d11.dll)";

// =============================================================================
//  Small helpers
// =============================================================================
template <class T> static inline T Min(T a, T b) { return (a < b) ? a : b; }
template <class T> static inline T Max(T a, T b) { return (a > b) ? a : b; }

// 64-bit FNV-1a used for all dedup signatures.
struct Fnv
{
    uint64_t h = 14695981039346656037ull;
    void Bytes(const void* p, size_t n)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template <class T> void Val(const T& v) { Bytes(&v, sizeof(v)); }
};

// Fixed-size line builder (no heap).
struct LineBuf
{
    char   s[3072];
    size_t n;
    LineBuf() : n(0) { s[0] = 0; }
    void Add(const char* fmt, ...)
    {
        if (n + 1 >= sizeof(s))
            return;
        va_list ap;
        va_start(ap, fmt);
        const int r = _vsnprintf_s(s + n, sizeof(s) - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (r < 0)
            n = sizeof(s) - 1;
        else
            n += static_cast<size_t>(r);
    }
};

// Copies memory that might be unreadable (mapped GPU memory, foreign code
// bytes). No C++ objects with destructors in this function (SEH + /EHsc).
static bool SafeCopy(void* dst, const void* src, size_t n)
{
    __try
    {
        memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static const char* ToUtf8(const wchar_t* w, char* buf, int cap)
{
    if (!w)
        w = L"";
    const int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, cap, nullptr, nullptr);
    if (r <= 0)
        buf[0] = 0;
    return buf;
}

static bool FileExists(const wchar_t* path)
{
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void TrimW(wchar_t* s)
{
    size_t len = wcslen(s);
    while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t' || s[len - 1] == L'\r' || s[len - 1] == L'\n'))
        s[--len] = 0;
    size_t start = 0;
    while (s[start] == L' ' || s[start] == L'\t')
        ++start;
    if (start > 0)
        memmove(s, s + start, (wcslen(s + start) + 1) * sizeof(wchar_t));
}

// =============================================================================
//  "ours" heap counter for the [MEM] telemetry
//
//  Replaces the global operator new/delete of THIS module only (static CRT,
//  /MT - the game and other DLLs are not affected). Counted: every C++ heap
//  allocation of our code (std containers). Not counted: ImGui (malloc),
//  D3D/DXGI objects and Windows-internal allocations. The value is the _msize
//  sum of the live blocks (approximate). Counting is enabled once in DllMain
//  (before our first allocation) only if [MEM] telemetry is on.
// =============================================================================
static volatile LONG g_oursBytes = 0;
static bool          g_heapCount = false; // set once in DllMain, never cleared

static void* CountedAlloc(size_t n) noexcept
{
    void* p = malloc(n ? n : 1u);
    if (p && g_heapCount)
        InterlockedExchangeAdd(&g_oursBytes, static_cast<LONG>(_msize(p)));
    return p;
}

static void CountedFree(void* p) noexcept
{
    if (!p)
        return;
    if (g_heapCount)
        InterlockedExchangeAdd(&g_oursBytes, -static_cast<LONG>(_msize(p)));
    free(p);
}

void* operator new(size_t n)
{
    void* p = CountedAlloc(n);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n)
{
    void* p = CountedAlloc(n);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return CountedAlloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return CountedAlloc(n); }
void operator delete(void* p) noexcept { CountedFree(p); }
void operator delete[](void* p) noexcept { CountedFree(p); }
void operator delete(void* p, size_t) noexcept { CountedFree(p); }
void operator delete[](void* p, size_t) noexcept { CountedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { CountedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { CountedFree(p); }

// =============================================================================
//  Global state
// =============================================================================
static HMODULE g_hSelf         = nullptr;
static wchar_t g_selfPath[1024] = {};
static wchar_t g_selfDir[1024]  = {};
static wchar_t g_iniPath[1024]  = {};
static wchar_t g_logPath[1024]  = {};
static bool    g_active         = true;   // false if another TR_HUD_Fix instance owns this process
static volatile bool g_shutdown = false;

struct Config
{
    int     enableFix        = 1;
    int     enableLogging    = 0;    // 0 = off (default), 1 = full analysis log, 2 = watchdog/essential only
    wchar_t proxyDll[1024]   = {};
    UINT    maxUniqueEntries = 4096;
    UINT    cbDumpMaxBytes   = 256;
    UINT    cbDumpSamples    = 3;
    UINT    maxLogMB         = 50;   // [Logging] size limit (1..1024)
    UINT    autoCaptureMax   = 5;    // [Logging] watchdog auto-captures per session
    UINT    autoCaptureMinGapSec = 120;
    UINT    watchSampleMs    = 1000; // [Logging] OFF watchdog sample period, 0 = off (v1.3.0)
    UINT    memLogSec        = 60;   // [Logging] [MEM] telemetry period, 0 = off (v1.3.0)
    int     inventoryEnable  = 1;    // [Logging] [INV] HUD inventory, 0 = off (v1.3.2)
    int     rearmDrawHooks   = 1;
    // [Capture]
    UINT    captureHotkey    = 0x79; // VK_F10, 0 = off
    UINT    captureFrames    = 3;    // used when a Present hook is active
    UINT    captureSeconds   = 15;   // fallback without Present hook
    UINT    captureMaxCBBytes = 512;
    UINT    captureMaxLines  = 400000;
    UINT    captureMaxVBBytes = 262144;
    // [Fix]
    uint64_t hudVs[16]          = {};
    UINT     hudVsCount         = 0;
    float    fixShiftNdcX       = -0.5200f;
    UINT     fixToggleHotkey    = 0x78; // VK_F9, 0 = off
    float    fixShiftNdcY       = 0.0f;
    float    popupScale         = 1.0f;
    float    popupPivotX        = 0.97f;
    float    popupPivotY        = -0.71f;
    // Backdrop ("black stain"): 0 = off, 1 = move to target, 2 = hide
    int      fixBackdrop        = 1;
    uint64_t backdropPsHash     = 0xEF336D585C6DE33Eull;
    float    backdropX          = 0.3963f;
    float    backdropY          = 0.9306f;
    // Prompt layer ("Press [E]" prompts left at the stage origin), v1.1.2
    bool     fixPrompt          = true;
    float    promptLayerW       = 12313.5f;  // c3.w of the prompt layer
    float    promptShiftX       = 0.99844f;  // missing root position (NDC)
    float    promptShiftY       = -1.83333f;
    float    hudLayerW          = 24627.0f;  // c3.w of the regular HUD layer
    float    hudLayerTol        = 15.0f;     // v1.3.3 absolute tolerance for the HUD layer w (varies with aspect)
    float    promptLayerTol     = 8.0f;      // v1.3.3 absolute tolerance for the prompt layer w
    UINT     menuMinSceneDraws  = 150;       // G1: fewer non-HUD draws before the first HUD draw = menu
    float    menuLargeScale     = 0.00014f;  // G3: element scale (c0.x/w, c1.y/w) of full-screen images
    int      menuHoldFrames     = 2;         // keep the menu state N frames after the last signal
    // Popup group left at the stage origin (top-left), v1.1.3
    bool     fixPopupOrigin     = true;
    float    popupOriginShiftX  = 1.7238f;   // moves the group to its original bottom-right place
    float    popupOriginShiftY  = -1.7258f;
    uint64_t popupOriginPsHash  = 0x300305651E200543ull; // popup background quads (trigger)
    // Salvage counter group left at the top left (v1.3.1): back to the pre-2021 top-right place
    bool     fixSalvage         = true;
    float    salvageShiftX      = 1.6055f;
    float    salvageShiftY      = -0.0667f;
    // [Menu]
    UINT     menuHotkey        = 0x2D; // VK_INSERT, 0 = off
    float    menuScale          = 0.0f; // 0 = auto (back-buffer height / 1080)
    bool     showAdvanced       = false; // v1.4.5: popup offset/scale sliders in the menu
    // [Perf]
    int      fpsLimit           = 0;    // 0 = off, else 20..360
    bool     showFpsOverlay     = false;
    UINT     overlayHotkey      = 0;    // 0 = none
    UINT     perfTestHotkey     = 0;    // 0 = none (diagnostics: live hook A/B test)
    UINT     benchHotkey        = 0;    // 0 = none (diagnostics: automatic hook benchmark)
    // [Display] (applied at the next start)
    bool     borderless         = false;
    bool     flipModel          = false;
    bool     allowTearing       = false;
    // [Memory] (v1.4.6) low address-space warning
    bool     memWarning         = true;
    UINT     memWarnFreeMB      = 512;  // warn below this much free address space
    UINT     memWarnBlockMB     = 96;   // ... or below this largest free block
};
static Config g_cfg;
// Fix infrastructure (CB shadows, shader registry, own CB, menu gate) is active
// if the fix is enabled at start, the menu exists (so the fix can be switched
// on live) or logging is on (the watchdog needs the shadows).
static bool g_fixInfra = false;

// [Display] values active since start (menu changes apply after a restart).
static bool g_startBorderless = false;
static bool g_startFlip       = false;
static bool g_startTearing    = false;

// Hotkey binding in the menu: all hotkey actions are suppressed while a key is
// being bound and for 300 ms afterwards (the new key is usually still held).
static volatile LONG g_bindActive = 0;
static DWORD g_suppressTick = 0;
static bool HotkeysSuppressed()
{
    return g_bindActive != 0 || (g_suppressTick != 0 && GetTickCount() - g_suppressTick < 300u);
}

static const UINT kMaxCBDumpBytes = 4096;

// =============================================================================
//  Logging (tr_hud_fix.log next to the module, overwritten on start)
// =============================================================================
static HANDLE           g_logFile   = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static ULONGLONG        g_startTick = 0;
static bool             g_logOn     = false;
static bool             g_logVerbose = true;  // EnableLogging=1; false for 2 (watchdog only)
static ULONGLONG        g_logBytes  = 0;      // bytes written (size limit, no GetFileSize per line)
static bool             g_logStopped = false; // MaxLogMB reached

static inline bool LoggingActive() { return g_logOn && !g_shutdown; }


// =============================================================================
//  Crash / exit diagnostics
//  v1.4.6: the crash handler runs in every configuration (log-only). With a
//  log file the report goes into the log; otherwise into tr_hud_fix_crash.txt,
//  created lazily on the first qualifying exception (never created otherwise).
// =============================================================================
static const UINT kEventRing = 16;
static char g_eventRing[kEventRing][192] = {}; // last [DXGI]/[DISPLAY] log lines + EventPush events, g_logCs
static UINT g_eventPos = 0;
static volatile LONG g_crashLogged = 0;
static volatile LONG g_crashBusy   = 0;                     // one report at a time
static HANDLE        g_crashFile   = INVALID_HANDLE_VALUE;  // tr_hud_fix_crash.txt (logging off only)
static HANDLE        g_crashOut    = INVALID_HANDLE_VALUE;  // target of the current report

// One line straight into a file WITHOUT g_logCs: on a crash or at process
// exit the lock may be owned by a crashed or already terminated thread.
// Static buffer keeps the stack footprint small (stack-overflow handler).
static void WriteLineUnsafeV(HANDLE h, const char* fmt, va_list ap)
{
    static char buf[768];
    const ULONGLONG t = GetTickCount64() - g_startTick;
    int p = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%7llu.%03llu] ", t / 1000ull, t % 1000ull);
    if (p < 0)
        p = 0;
    int m = _vsnprintf_s(buf + p, sizeof(buf) - static_cast<size_t>(p) - 2, _TRUNCATE, fmt, ap);
    if (m < 0)
        m = static_cast<int>(strlen(buf + p));
    size_t len = static_cast<size_t>(p) + static_cast<size_t>(m);
    buf[len++] = '\r';
    buf[len++] = '\n';
    DWORD written = 0;
    WriteFile(h, buf, static_cast<DWORD>(len), &written, nullptr);
    if (h == g_logFile)
        g_logBytes += len;
}

static void LogUnsafe(const char* fmt, ...)
{
    if (g_logFile == INVALID_HANDLE_VALUE || g_logStopped)
        return;
    va_list ap;
    va_start(ap, fmt);
    WriteLineUnsafeV(g_logFile, fmt, ap);
    va_end(ap);
}

// Crash-report line into the current report target (log or crash file).
static void CrashLine(const char* fmt, ...)
{
    if (g_crashOut == INVALID_HANDLE_VALUE)
        return;
    va_list ap;
    va_start(ap, fmt);
    WriteLineUnsafeV(g_crashOut, fmt, ap);
    va_end(ap);
}

// v1.4.6: event for the crash report ring, also with logging off (no file
// I/O). Callers log the same text themselves if logging is active.
static void EventPush(const char* fmt, ...)
{
    char line[192];
    const ULONGLONG t = GetTickCount64() - g_startTick;
    int p = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%7llu.%03llu] ", t / 1000ull, t % 1000ull);
    if (p < 0)
        p = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line + p, sizeof(line) - static_cast<size_t>(p), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logCs);
    strncpy_s(g_eventRing[g_eventPos], sizeof(g_eventRing[0]), line, _TRUNCATE);
    g_eventPos = (g_eventPos + 1u) % kEventRing;
    LeaveCriticalSection(&g_logCs);
}

// v1.4.6: user address space summary (VirtualQuery walk, no heap, no locks;
// safe inside the crash handler). Synchronous full walk - used by the crash
// handler only; the Present path uses the time-sliced MemWalkSlice. GetSystemInfo bounds = 0x10000..0xFFFEFFFF
// for a LAA 32-bit process on 64-bit Windows.
struct AddrSpaceInfo
{
    ULONGLONG committed, reserved, freeTotal, largestFree;
};
static void AddrSpaceWalk(AddrSpaceInfo* r)
{
    r->committed = r->reserved = r->freeTotal = r->largestFree = 0;
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    UINT_PTR a = reinterpret_cast<UINT_PTR>(si.lpMinimumApplicationAddress);
    const UINT_PTR end = reinterpret_cast<UINT_PTR>(si.lpMaximumApplicationAddress);
    while (a < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) || mbi.RegionSize == 0)
            break;
        if (mbi.State == MEM_FREE)
        {
            r->freeTotal += mbi.RegionSize;
            r->largestFree = Max<ULONGLONG>(r->largestFree, mbi.RegionSize);
        }
        else if (mbi.State == MEM_COMMIT)
        {
            r->committed += mbi.RegionSize;
        }
        else
        {
            r->reserved += mbi.RegionSize;
        }
        const UINT_PTR next = reinterpret_cast<UINT_PTR>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a)
            break; // wrapped at the top of the address space
        a = next;
    }
}

// Lazily opened crash file (append) - only called for a qualifying exception
// with no log file open. Static buffers (stack-overflow handler).
static HANDLE OpenCrashFile()
{
    if (g_crashFile != INVALID_HANDLE_VALUE)
        return g_crashFile;
    static wchar_t path[1024];
    wcsncpy_s(path, g_selfDir, _TRUNCATE);
    wcsncat_s(path, L"tr_hud_fix_crash.txt", _TRUNCATE);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        // Game folder not writable -> %TEMP% (same fallback as the log).
        const DWORD n = GetTempPathW(MAX_PATH, path);
        if (n > 0 && n < MAX_PATH)
        {
            wcsncat_s(path, L"tr_hud_fix_crash.txt", _TRUNCATE);
            h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
        }
    }
    g_crashFile = h;
    if (h != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        g_crashOut = h;
        CrashLine("==== TR2013 HUD Fix+ v%s %s - crash report %04u-%02u-%02u %02u:%02u:%02u (PID %lu) ====",
                  kVersion, kBuildTarget, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  GetCurrentProcessId());
    }
    return h;
}

// Module base name of an address ("?" if none). Static result buffer.
static const char* CrashModName(const void* p, HMODULE* outMod)
{
    static char path[MAX_PATH];
    HMODULE m = nullptr;
    if (p)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(p), &m);
    *outMod = m;
    if (!m || !GetModuleFileNameA(m, path, MAX_PATH))
        return "?";
    const char* b = strrchr(path, '\\');
    return b ? b + 1 : path;
}

static void CrashReportExtra(); // counters + memory, defined after the fix section

static const char* const kWatchedDlls[] = { "d3d11.dll", "dxgi.dll" };

// Vectored handler: logs (max 3x) severe exceptions raised in our module, the
// game EXE, d3d11.dll or dxgi.dll, or (v1.4.6) at a null/unmapped EIP - then
// always continues the search, i.e.
// nothing is ever swallowed. First-chance exceptions the game handles itself
// may therefore show up too (hence the cap).
static LONG CALLBACK CrashVeh(PEXCEPTION_POINTERS ep)
{
    if (!ep || !ep->ExceptionRecord || g_shutdown)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Cheap cap pre-check (read only) before any module lookups; the actual
    // reservation below stays the InterlockedIncrement.
    if (g_crashLogged >= 3)
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    void* addr = er->ExceptionAddress;
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &mod);
    static char modName[MAX_PATH];
    bool nullEip = false;
    if (!mod)
    {
        // v1.4.6: execute at a null/unmapped address (e.g. a call through a
        // cleared function pointer) - the most common crash without a module.
        const bool exec = code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 &&
                          er->ExceptionInformation[0] == 8;
        if (!exec && reinterpret_cast<UINT_PTR>(addr) >= 0x10000u)
            return EXCEPTION_CONTINUE_SEARCH;
        nullEip = true;
    }
    char modPath[MAX_PATH] = {};
    const char* base = modPath;
    if (!nullEip)
    {
        GetModuleFileNameA(mod, modPath, MAX_PATH);
        base = strrchr(modPath, '\\');
        base = base ? base + 1 : modPath;
        bool watched = (mod == g_hSelf || mod == GetModuleHandleW(nullptr));
        for (size_t i = 0; !watched && i < ARRAYSIZE(kWatchedDlls); ++i)
            watched = _stricmp(base, kWatchedDlls[i]) == 0;
        if (!watched)
            return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&g_crashLogged) > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_crashBusy, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH; // another thread is reporting (static buffers)

    // Target: the log if open, else the lazily created tr_hud_fix_crash.txt.
    g_crashOut = (g_logFile != INVALID_HANDLE_VALUE && !g_logStopped) ? g_logFile : OpenCrashFile();
    if (g_crashOut == INVALID_HANDLE_VALUE)
    {
        InterlockedExchange(&g_crashBusy, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    strcpy_s(modName, nullEip ? "<no module>" : base);
    const UINT_PTR offset = reinterpret_cast<UINT_PTR>(addr) - reinterpret_cast<UINT_PTR>(mod);
    CrashLine("[CRASH] code=0x%08lX addr=%p module=%s+0x%IX thread=%lu%s", code, addr, modName, offset,
              GetCurrentThreadId(), mod && mod == g_hSelf ? " (TR_HUD_Fix)" : "");
    const CONTEXT* c = ep->ContextRecord;
    if (c)
        CrashLine("[CRASH] eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX",
                  c->Eip, c->Esp, c->Ebp, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
    {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        CrashLine("[CRASH] access violation %s %p",
                  kind == 0 ? "reading" : (kind == 1 ? "writing" : (kind == 8 ? "executing (DEP)" : "?")),
                  reinterpret_cast<void*>(er->ExceptionInformation[1]));
    }
    if (nullEip)
    {
        // The faulting call pushed its return address: [esp] = caller.
        DWORD ret = 0;
        SIZE_T got = 0;
        if (c && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<UINT_PTR>(c->Esp)), &ret,
                                   sizeof(ret), &got) &&
            got == sizeof(ret))
        {
            HMODULE rm = nullptr;
            const char* rn = CrashModName(reinterpret_cast<const void*>(static_cast<UINT_PTR>(ret)), &rm);
            CrashLine("[CRASH] null/unmapped eip: return address [esp]=%08lX -> %s+0x%IX%s", ret, rn,
                      rm ? static_cast<UINT_PTR>(ret) - reinterpret_cast<UINT_PTR>(rm) : static_cast<UINT_PTR>(0),
                      rm && rm == g_hSelf ? " (TR_HUD_Fix)" : "");
        }
        else
        {
            CrashLine("[CRASH] null/unmapped eip: [esp] not readable");
        }
    }
    CrashReportExtra();
    CrashLine("[CRASH] last DXGI/DISPLAY/DEVICE events (oldest first):");
    for (UINT i = 0; i < kEventRing; ++i)
    {
        const char* e = g_eventRing[(g_eventPos + i) % kEventRing];
        if (e[0])
            CrashLine("[CRASH]   %s", e);
    }
    CrashLine("[CRASH] (first-chance: the game may still handle this exception)");
    FlushFileBuffers(g_crashOut);
    InterlockedExchange(&g_crashBusy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void LogV(const char* fmt, va_list ap)
{
    if (!g_logOn)
        return;
    char buf[4096];
    const ULONGLONG t = GetTickCount64() - g_startTick;
    int p = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%7llu.%03llu] ", t / 1000ull, t % 1000ull);
    if (p < 0)
        p = 0;
    const size_t room = sizeof(buf) - static_cast<size_t>(p) - 2; // keep space for CRLF
    int m = _vsnprintf_s(buf + p, room, _TRUNCATE, fmt, ap);
    if (m < 0)
        m = static_cast<int>(strlen(buf + p));
    size_t len = static_cast<size_t>(p) + static_cast<size_t>(m);
    buf[len++] = '\r';
    buf[len++] = '\n';

    // One WriteFile per line, no user-space buffering: every line reaches the
    // OS immediately and survives a game crash.
    // EnableLogging=2: drop the verbose analysis categories.
    if (!g_logVerbose)
    {
        static const char* const kVerboseTags[] = { "[VS]", "[SHADER]", "[LAYOUT]", "[VSCB]", "[VP]", "[SC]",
                                                    "[DRAW]", "[CBDATA]", "[REARM]", "[FIX] applied" };
        for (size_t i = 0; i < ARRAYSIZE(kVerboseTags); ++i)
        {
            if (strncmp(buf + p, kVerboseTags[i], strlen(kVerboseTags[i])) == 0)
                return;
        }
    }

    EnterCriticalSection(&g_logCs);
    if (!g_logOn || g_logStopped)
    {
        LeaveCriticalSection(&g_logCs);
        return;
    }
    // Size limit: one final line, then no more writes (this also stops tracking
    // and captures, because LoggingActive() becomes false).
    const ULONGLONG maxBytes = static_cast<ULONGLONG>(g_cfg.maxLogMB) * 1024ull * 1024ull;
    if (g_logBytes + len > maxBytes)
    {
        char last[128];
        const int n = _snprintf_s(last, sizeof(last), _TRUNCATE,
                                  "[LOG] size limit reached (%u MB) - logging stopped\r\n", g_cfg.maxLogMB);
        DWORD w = 0;
        if (n > 0)
            WriteFile(g_logFile, last, static_cast<DWORD>(n), &w, nullptr);
        FlushFileBuffers(g_logFile);
        g_logStopped = true;
        g_logOn = false;
        LeaveCriticalSection(&g_logCs);
        return;
    }
    g_logBytes += len;
    // Remember the last [DXGI]/[DISPLAY] events for the crash report.
    if (strncmp(buf + p, "[DXGI]", 6) == 0 || strncmp(buf + p, "[DISPLAY]", 9) == 0)
    {
        strncpy_s(g_eventRing[g_eventPos], sizeof(g_eventRing[0]), buf, Min(len - 2, sizeof(g_eventRing[0]) - 1));
        g_eventPos = (g_eventPos + 1u) % kEventRing;
    }
    DWORD written = 0;
    WriteFile(g_logFile, buf, static_cast<DWORD>(len), &written, nullptr);
    LeaveCriticalSection(&g_logCs);
}

static void Log(const char* fmt, ...)
{
    if (!g_logOn)
        return;
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

static void OpenLog()
{
    // Keep the previous run (e.g. a crash log): tr_hud_fix.log -> tr_hud_fix.prev.log
    if (FileExists(g_logPath))
    {
        wchar_t prev[1024] = {};
        wcscpy_s(prev, g_selfDir);
        wcscat_s(prev, L"tr_hud_fix.prev.log");
        MoveFileExW(g_logPath, prev, MOVEFILE_REPLACE_EXISTING);
    }
    g_logFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE)
    {
        // Game folder not writable -> fall back to %TEMP%.
        wchar_t tmp[MAX_PATH + 32] = {};
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH)
        {
            wcscat_s(tmp, L"tr_hud_fix.log");
            wcscpy_s(g_logPath, tmp);
            g_logFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    g_logOn = (g_logFile != INVALID_HANDLE_VALUE);
}

// =============================================================================
//  Paths, mode detection, ini
// =============================================================================
static void InitPaths()
{
    const DWORD n = GetModuleFileNameW(g_hSelf, g_selfPath, ARRAYSIZE(g_selfPath));
    if (n == 0 || n >= ARRAYSIZE(g_selfPath))
        g_selfPath[0] = 0;

    wcscpy_s(g_selfDir, g_selfPath);
    wchar_t* slash = wcsrchr(g_selfDir, L'\\');
    if (slash)
        slash[1] = 0;
    else
        g_selfDir[0] = 0;

    wcscpy_s(g_iniPath, g_selfDir);
    wcscat_s(g_iniPath, L"tr_hud_fix.ini");
    wcscpy_s(g_logPath, g_selfDir);
    wcscat_s(g_logPath, L"tr_hud_fix.log");
}

// Float from the ini; empty or unparsable -> def.
static float ReadIniFloat(const wchar_t* section, const wchar_t* key, float def)
{
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(section, key, L"", buf, ARRAYSIZE(buf), g_iniPath);
    TrimW(buf);
    if (!buf[0])
        return def;
    wchar_t* end = nullptr;
    const double v = wcstod(buf, &end); // our static CRT: locale "C"
    if (end == buf || !(v == v))
        return def;
    return static_cast<float>(v);
}

// v1.4.5: fixed key-name table (no user32 calls, safe while loading the ini).
// Returns nullptr for keys without a table name.
static const char* VkNameTable(UINT vk, char* buf, size_t cap)
{
    if (!vk)
        return "none";
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
    {
        _snprintf_s(buf, cap, _TRUNCATE, "%c", static_cast<char>(vk));
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        _snprintf_s(buf, cap, _TRUNCATE, "F%u", vk - VK_F1 + 1u);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
    {
        _snprintf_s(buf, cap, _TRUNCATE, "NUM %u", vk - VK_NUMPAD0);
        return buf;
    }
    switch (vk)
    {
    case VK_BACK:     return "BACKSPACE";
    case VK_TAB:      return "TAB";
    case VK_RETURN:   return "ENTER";
    case VK_PAUSE:    return "PAUSE";
    case VK_CAPITAL:  return "CAPS LOCK";
    case VK_ESCAPE:   return "ESC";
    case VK_SPACE:    return "SPACE";
    case VK_PRIOR:    return "PAGE UP";
    case VK_NEXT:     return "PAGE DOWN";
    case VK_END:      return "END";
    case VK_HOME:     return "HOME";
    case VK_LEFT:     return "LEFT";
    case VK_UP:       return "UP";
    case VK_RIGHT:    return "RIGHT";
    case VK_DOWN:     return "DOWN";
    case VK_SNAPSHOT: return "PRINT";
    case VK_INSERT:   return "INSERT";
    case VK_DELETE:   return "DELETE";
    case VK_MULTIPLY: return "NUM *";
    case VK_ADD:      return "NUM +";
    case VK_SUBTRACT: return "NUM -";
    case VK_DECIMAL:  return "NUM .";
    case VK_DIVIDE:   return "NUM /";
    case VK_NUMLOCK:  return "NUM LOCK";
    case VK_SCROLL:   return "SCROLL LOCK";
    default:          return nullptr;
    }
}

// Uppercase ASCII, without spaces / '_' / '-' ("Page Up" == "PAGE_UP" == "PAGEUP").
static void HotkeyNorm(const char* in, char* out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 1 < cap; ++in)
    {
        char c = *in;
        if (c == ' ' || c == '_' || c == '-' || c == '\t')
            continue;
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        out[n++] = c;
    }
    out[n] = 0;
}

// v1.4.5: hotkey value from the ini - key name (F9, INSERT, PAGE UP, NUM 5, K,
// German EINFG/ENTF/POS1/ENDE/BILDAUF/BILDAB) or a virtual-key code (0x78 / 120).
// 0 / NONE / OFF / empty = no key. Unknown text -> defVk.
static UINT ParseHotkey(const wchar_t* text, UINT defVk)
{
    char a[64] = {};
    size_t i = 0;
    for (; text[i] && i + 1 < sizeof(a); ++i)
        a[i] = (text[i] < 128) ? static_cast<char>(text[i]) : '?';
    a[i] = 0;
    char n[64];
    HotkeyNorm(a, n, sizeof(n));
    if (!n[0] || !strcmp(n, "NONE") || !strcmp(n, "OFF"))
        return 0;
    if (n[0] >= '0' && n[0] <= '9' && (n[1] == 0 || n[1] == 'X' || (n[1] >= '0' && n[1] <= '9')))
    {
        const unsigned long v = strtoul(n, nullptr, 0);
        return v <= 0xFEul ? static_cast<UINT>(v) : defVk;
    }
    static const struct { const char* name; UINT vk; } kAlias[] = {
        { "EINFG", VK_INSERT }, { "INS", VK_INSERT }, { "ENTF", VK_DELETE }, { "DEL", VK_DELETE },
        { "POS1", VK_HOME },    { "ENDE", VK_END },   { "BILDAUF", VK_PRIOR }, { "PGUP", VK_PRIOR },
        { "BILDAB", VK_NEXT },  { "PGDN", VK_NEXT },  { "DRUCK", VK_SNAPSHOT }, { "LEERTASTE", VK_SPACE },
        { "RUECKTASTE", VK_BACK },
    };
    for (const auto& al : kAlias)
    {
        if (!strcmp(n, al.name))
            return al.vk;
    }
    for (UINT vk = 1; vk <= 0xFE; ++vk)
    {
        char b[32];
        const char* name = VkNameTable(vk, b, sizeof(b));
        if (!name)
            continue;
        char nn[32];
        HotkeyNorm(name, nn, sizeof(nn));
        if (!strcmp(n, nn))
            return vk;
    }
    return defVk;
}

static UINT ReadIniHotkey(const wchar_t* section, const wchar_t* key, const wchar_t* defText, UINT defVk)
{
    wchar_t t[64] = {};
    GetPrivateProfileStringW(section, key, defText, t, ARRAYSIZE(t), g_iniPath);
    return ParseHotkey(t, defVk);
}

static void ReadIni()
{
    g_cfg.enableFix     = static_cast<int>(GetPrivateProfileIntW(L"General", L"EnableFix", 1, g_iniPath));
    g_cfg.enableLogging = static_cast<int>(GetPrivateProfileIntW(L"General", L"EnableLogging", 0, g_iniPath));
    if (g_cfg.enableLogging < 0 || g_cfg.enableLogging > 2)
        g_cfg.enableLogging = 0;
    g_logVerbose = (g_cfg.enableLogging == 1); // 2 = watchdog/essential lines only
    g_cfg.rearmDrawHooks = static_cast<int>(GetPrivateProfileIntW(L"General", L"RearmDrawHooks", 1, g_iniPath));

    GetPrivateProfileStringW(L"Proxy", L"ProxyDll", L"", g_cfg.proxyDll, ARRAYSIZE(g_cfg.proxyDll), g_iniPath);
    TrimW(g_cfg.proxyDll);
    // GetPrivateProfileString does not strip trailing comments; treat a value
    // that starts with ';' or '#' as empty.
    if (g_cfg.proxyDll[0] == L';' || g_cfg.proxyDll[0] == L'#')
        g_cfg.proxyDll[0] = 0;

    UINT u = GetPrivateProfileIntW(L"Logging", L"MaxUniqueEntries", 4096, g_iniPath);
    g_cfg.maxUniqueEntries = Min<UINT>(Max<UINT>(u, 16u), 1000000u);

    u = GetPrivateProfileIntW(L"Logging", L"CBDumpMaxBytes", 256, g_iniPath);
    u = Min<UINT>(Max<UINT>(u, 16u), kMaxCBDumpBytes);
    g_cfg.cbDumpMaxBytes = u & ~15u;

    u = GetPrivateProfileIntW(L"Logging", L"MaxLogMB", 50, g_iniPath);
    g_cfg.maxLogMB = Min<UINT>(Max<UINT>(u, 1u), 1024u);
    u = GetPrivateProfileIntW(L"Logging", L"AutoCaptureMax", 5, g_iniPath);
    g_cfg.autoCaptureMax = Min<UINT>(u, 100u);
    u = GetPrivateProfileIntW(L"Logging", L"AutoCaptureMinGapSec", 120, g_iniPath);
    g_cfg.autoCaptureMinGapSec = Min<UINT>(u, 3600u);
    u = GetPrivateProfileIntW(L"Logging", L"WatchSampleMs", 1000, g_iniPath);
    g_cfg.watchSampleMs = (u == 0) ? 0u : Min<UINT>(Max<UINT>(u, 100u), 60000u);
    u = GetPrivateProfileIntW(L"Logging", L"MemLogSec", 60, g_iniPath);
    g_cfg.memLogSec = (u == 0) ? 0u : Min<UINT>(Max<UINT>(u, 5u), 3600u);
    g_cfg.inventoryEnable = GetPrivateProfileIntW(L"Logging", L"InventoryEnable", 1, g_iniPath) ? 1 : 0;

    u = GetPrivateProfileIntW(L"Logging", L"CBDumpSamplesPerSig", 3, g_iniPath);
    g_cfg.cbDumpSamples = Min<UINT>(u, 100u);

    // Hotkeys: key name (F10) or virtual-key code (0x79 / 121), see ParseHotkey.
    g_cfg.captureHotkey = ReadIniHotkey(L"Capture", L"CaptureHotkey", L"F10", VK_F10);

    u = GetPrivateProfileIntW(L"Capture", L"CaptureFrames", 3, g_iniPath);
    g_cfg.captureFrames = Min<UINT>(Max<UINT>(u, 1u), 60u);

    u = GetPrivateProfileIntW(L"Capture", L"CaptureSeconds", 15, g_iniPath);
    g_cfg.captureSeconds = Min<UINT>(Max<UINT>(u, 1u), 600u);

    u = GetPrivateProfileIntW(L"Capture", L"CaptureMaxCBBytes", 512, g_iniPath);
    u = Min<UINT>(Max<UINT>(u, 16u), kMaxCBDumpBytes);
    g_cfg.captureMaxCBBytes = u & ~15u;

    u = GetPrivateProfileIntW(L"Capture", L"CaptureMaxVBBytes", 262144, g_iniPath);
    g_cfg.captureMaxVBBytes = Min<UINT>(Max<UINT>(u, 4096u), 16u * 1024u * 1024u);

    // [Fix]
    wchar_t hashes[512] = {};
    GetPrivateProfileStringW(L"Fix", L"HudVsHashes", L"8FE65884BC7EA6DE,95CD56AF476CBD7D,15A962EB1FFFCD0D", hashes,
                             ARRAYSIZE(hashes), g_iniPath);
    wchar_t* tokCtx = nullptr;
    for (wchar_t* tok = wcstok_s(hashes, L", ;\t", &tokCtx); tok && g_cfg.hudVsCount < ARRAYSIZE(g_cfg.hudVs);
         tok = wcstok_s(nullptr, L", ;\t", &tokCtx))
    {
        wchar_t* end = nullptr;
        const unsigned long long hv = _wcstoui64(tok, &end, 16);
        if (end != tok && hv)
            g_cfg.hudVs[g_cfg.hudVsCount++] = hv;
    }
    g_cfg.fixShiftNdcX = Min(Max(ReadIniFloat(L"Fix", L"ShiftNdcX", -0.5200f), -2.0f), 2.0f);
    g_cfg.fixToggleHotkey = ReadIniHotkey(L"Fix", L"ToggleHotkey", L"F9", VK_F9);
    g_cfg.fixShiftNdcY = Min(Max(ReadIniFloat(L"Fix", L"ShiftNdcY", 0.0f), -1.0f), 1.0f);
    g_cfg.popupScale   = Min(Max(ReadIniFloat(L"Fix", L"PopupScale", 1.0f), 0.5f), 1.5f);
    g_cfg.popupPivotX  = ReadIniFloat(L"Fix", L"PopupPivotNdcX", 0.97f);
    g_cfg.popupPivotY  = ReadIniFloat(L"Fix", L"PopupPivotNdcY", -0.71f);

    // Backdrop ("black stain" behind the top-right pickup message)
    const int bd = static_cast<int>(GetPrivateProfileIntW(L"Fix", L"FixBackdrop", 1, g_iniPath));
    g_cfg.fixBackdrop = (bd < 0 || bd > 2) ? 1 : bd;
    wchar_t psh[64] = {};
    GetPrivateProfileStringW(L"Fix", L"BackdropPsHash", L"EF336D585C6DE33E", psh, ARRAYSIZE(psh), g_iniPath);
    TrimW(psh);
    {
        wchar_t* end = nullptr;
        const unsigned long long hv = _wcstoui64(psh, &end, 16);
        g_cfg.backdropPsHash = (end != psh) ? hv : 0ull;
    }
    g_cfg.backdropX = Min(Max(ReadIniFloat(L"Fix", L"BackdropTargetNdcX", 0.3963f), -1.0f), 1.0f);
    g_cfg.backdropY = Min(Max(ReadIniFloat(L"Fix", L"BackdropTargetNdcY", 0.9306f), -1.0f), 1.0f);

    // Prompt layer fix
    g_cfg.fixPrompt   = GetPrivateProfileIntW(L"Fix", L"FixPrompt", 1, g_iniPath) != 0;
    g_cfg.promptLayerW = ReadIniFloat(L"Fix", L"PromptLayerW", 12313.5f);
    g_cfg.promptShiftX = Min(Max(ReadIniFloat(L"Fix", L"PromptShiftNdcX", 0.99844f), -2.0f), 2.0f);
    g_cfg.promptShiftY = Min(Max(ReadIniFloat(L"Fix", L"PromptShiftNdcY", -1.83333f), -2.0f), 2.0f);
    g_cfg.hudLayerW    = ReadIniFloat(L"Fix", L"HudLayerW", 24627.0f);
    g_cfg.hudLayerTol    = Min(Max(ReadIniFloat(L"Fix", L"HudLayerTol", 15.0f), 0.0f), 100.0f);
    g_cfg.promptLayerTol = Min(Max(ReadIniFloat(L"Fix", L"PromptLayerTol", 8.0f), 0.0f), 100.0f);
    {
        const int msd = static_cast<int>(GetPrivateProfileIntW(L"Fix", L"MenuMinSceneDraws", 150, g_iniPath));
        g_cfg.menuMinSceneDraws = static_cast<UINT>(Min(Max(msd, 0), 100000));
        g_cfg.menuLargeScale = Min(Max(ReadIniFloat(L"Fix", L"MenuLargeScale", 0.00014f), 0.0f), 1.0f);
        const int mhf = static_cast<int>(GetPrivateProfileIntW(L"Fix", L"MenuHoldFrames", 2, g_iniPath));
        g_cfg.menuHoldFrames = Min(Max(mhf, 1), 600);
    }

    // Popup-origin fix
    g_cfg.fixPopupOrigin   = GetPrivateProfileIntW(L"Fix", L"FixPopupOrigin", 1, g_iniPath) != 0;
    g_cfg.popupOriginShiftX = Min(Max(ReadIniFloat(L"Fix", L"PopupOriginShiftNdcX", 1.7238f), -3.0f), 3.0f);
    g_cfg.popupOriginShiftY = Min(Max(ReadIniFloat(L"Fix", L"PopupOriginShiftNdcY", -1.7258f), -3.0f), 3.0f);
    GetPrivateProfileStringW(L"Fix", L"PopupOriginTriggerPsHash", L"300305651E200543", psh, ARRAYSIZE(psh), g_iniPath);
    TrimW(psh);
    {
        wchar_t* end = nullptr;
        const unsigned long long hv = _wcstoui64(psh, &end, 16);
        g_cfg.popupOriginPsHash = (end != psh) ? hv : 0ull;
    }

    // Salvage counter fix
    g_cfg.fixSalvage   = GetPrivateProfileIntW(L"Fix", L"FixSalvage", 1, g_iniPath) != 0;
    g_cfg.salvageShiftX = Min(Max(ReadIniFloat(L"Fix", L"SalvageShiftNdcX", 1.6055f), -3.0f), 3.0f);
    g_cfg.salvageShiftY = Min(Max(ReadIniFloat(L"Fix", L"SalvageShiftNdcY", -0.0667f), -3.0f), 3.0f);

    // [Menu]
    g_cfg.menuHotkey = ReadIniHotkey(L"Menu", L"MenuHotkey", L"INSERT", VK_INSERT);
    const float ms = ReadIniFloat(L"Menu", L"MenuScale", 0.0f);
    g_cfg.menuScale = (ms > 0.0f) ? Min(Max(ms, 0.5f), 4.0f) : 0.0f;
    g_cfg.showAdvanced = GetPrivateProfileIntW(L"Menu", L"ShowAdvanced", 0, g_iniPath) != 0;

    // [Perf]
    const int fl = static_cast<int>(GetPrivateProfileIntW(L"Perf", L"FpsLimit", 0, g_iniPath));
    g_cfg.fpsLimit = (fl <= 0) ? 0 : Min(Max(fl, 20), 360);
    g_cfg.showFpsOverlay = GetPrivateProfileIntW(L"Perf", L"ShowFpsOverlay", 0, g_iniPath) != 0;
    g_cfg.overlayHotkey = ReadIniHotkey(L"Perf", L"OverlayHotkey", L"0", 0u);
    g_cfg.perfTestHotkey = ReadIniHotkey(L"Perf", L"PerfTestHotkey", L"0", 0u);
    g_cfg.benchHotkey = ReadIniHotkey(L"Perf", L"BenchHotkey", L"0", 0u);

    // [Display] (applied at start only)
    g_cfg.borderless   = GetPrivateProfileIntW(L"Display", L"Borderless", 0, g_iniPath) != 0;
    g_cfg.flipModel    = GetPrivateProfileIntW(L"Display", L"FlipModel", 0, g_iniPath) != 0;
    g_cfg.allowTearing = GetPrivateProfileIntW(L"Display", L"AllowTearing", 0, g_iniPath) != 0;
    g_startBorderless  = g_cfg.borderless;
    g_startFlip        = g_cfg.flipModel;
    g_startTearing     = g_cfg.flipModel && g_cfg.allowTearing;

    // [Memory] (v1.4.6) low address-space warning
    g_cfg.memWarning     = GetPrivateProfileIntW(L"Memory", L"MemWarning", 1, g_iniPath) != 0;
    g_cfg.memWarnFreeMB  = Min<UINT>(Max<UINT>(GetPrivateProfileIntW(L"Memory", L"MemWarnFreeMB", 512, g_iniPath), 16u), 3072u);
    g_cfg.memWarnBlockMB = Min<UINT>(Max<UINT>(GetPrivateProfileIntW(L"Memory", L"MemWarnBlockMB", 96, g_iniPath), 4u), 2048u);

    u = GetPrivateProfileIntW(L"Capture", L"CaptureMaxLines", 400000, g_iniPath);
    g_cfg.captureMaxLines = Min<UINT>(Max<UINT>(u, 100u), 10000000u);
}

// Only one TR_HUD_Fix instance may be active per process,
// otherwise both would hook and fight over tr_hud_fix.log.
static void AcquireInstance()
{
    wchar_t name[64] = {};
    swprintf_s(name, ARRAYSIZE(name), L"Local\\TR_HUD_Fix_Instance_%lu", GetCurrentProcessId());
    HANDLE m = CreateMutexW(nullptr, FALSE, name);
    if (m && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        g_active = false;
        CloseHandle(m);
    }
    // Otherwise the handle is intentionally kept open for the process lifetime.
}

// =============================================================================
//  DPI report (diagnostics only, logging on; process DPI awareness is never
//  changed). Windows DPI is not a cause of the HUD bug; the values are logged
//  for bug reports only and affect nothing.
// =============================================================================
typedef UINT    (WINAPI* TRHF_PFN_GetDpiForSystem)(void);
typedef UINT    (WINAPI* TRHF_PFN_GetDpiForWindow)(HWND);
typedef HANDLE  (WINAPI* TRHF_PFN_GetThreadDpiAwarenessContext)(void);
typedef HANDLE  (WINAPI* TRHF_PFN_SetThreadDpiAwarenessContext)(HANDLE);
typedef int     (WINAPI* TRHF_PFN_GetAwarenessFromDpiAwarenessContext)(HANDLE);
typedef HRESULT (WINAPI* TRHF_PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
typedef HRESULT (WINAPI* TRHF_PFN_GetProcessDpiAwareness)(HANDLE, int*);

static HMODULE g_shcore = nullptr;

static HMODULE LoadSystemDll(const wchar_t* name)
{
    wchar_t path[MAX_PATH + 64] = {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return nullptr;
    wcscat_s(path, L"\\");
    wcscat_s(path, name);
    return LoadLibraryW(path);
}

static void QueryAndLogDpi(const char* when, HWND hwnd)
{
    if (!LoggingActive())
        return;
    int lpx = 0, lpy = 0;
    HDC dc = GetDC(nullptr);
    if (dc)
    {
        lpx = GetDeviceCaps(dc, LOGPIXELSX);
        lpy = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
    }

    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    TRHF_PFN_GetDpiForSystem pGetDpiForSystem = nullptr;
    TRHF_PFN_GetDpiForWindow pGetDpiForWindow = nullptr;
    TRHF_PFN_GetThreadDpiAwarenessContext pGetThreadCtx = nullptr;
    TRHF_PFN_SetThreadDpiAwarenessContext pSetThreadCtx = nullptr;
    TRHF_PFN_GetAwarenessFromDpiAwarenessContext pAwareness = nullptr;
    if (u32)
    {
        pGetDpiForSystem = reinterpret_cast<TRHF_PFN_GetDpiForSystem>(GetProcAddress(u32, "GetDpiForSystem"));
        pGetDpiForWindow = reinterpret_cast<TRHF_PFN_GetDpiForWindow>(GetProcAddress(u32, "GetDpiForWindow"));
        pGetThreadCtx = reinterpret_cast<TRHF_PFN_GetThreadDpiAwarenessContext>(GetProcAddress(u32, "GetThreadDpiAwarenessContext"));
        pSetThreadCtx = reinterpret_cast<TRHF_PFN_SetThreadDpiAwarenessContext>(GetProcAddress(u32, "SetThreadDpiAwarenessContext"));
        pAwareness = reinterpret_cast<TRHF_PFN_GetAwarenessFromDpiAwarenessContext>(GetProcAddress(u32, "GetAwarenessFromDpiAwarenessContext"));
    }

    if (!g_shcore)
        g_shcore = LoadSystemDll(L"shcore.dll"); // absent on Win7 - fine
    TRHF_PFN_GetDpiForMonitor pGetDpiForMonitor = nullptr;
    TRHF_PFN_GetProcessDpiAwareness pGetProcessDpiAwareness = nullptr;
    if (g_shcore)
    {
        pGetDpiForMonitor = reinterpret_cast<TRHF_PFN_GetDpiForMonitor>(GetProcAddress(g_shcore, "GetDpiForMonitor"));
        pGetProcessDpiAwareness = reinterpret_cast<TRHF_PFN_GetProcessDpiAwareness>(GetProcAddress(g_shcore, "GetProcessDpiAwareness"));
    }

    const BOOL legacyAware = IsProcessDPIAware();
    int procAwareness = -2; // -2 = n/a
    if (pGetProcessDpiAwareness)
    {
        int v = -1;
        if (SUCCEEDED(pGetProcessDpiAwareness(nullptr, &v)))
            procAwareness = v;
    }
    int threadAwareness = -2;
    if (pGetThreadCtx && pAwareness)
        threadAwareness = pAwareness(pGetThreadCtx());

    // Values as seen by this (possibly DPI-unaware => virtualised 96) thread.
    const UINT sysDpi = pGetDpiForSystem ? pGetDpiForSystem() : 0;
    POINT origin = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT monX = 0, monY = 0;
    HRESULT monHr = E_NOTIMPL;
    if (pGetDpiForMonitor && mon)
        monHr = pGetDpiForMonitor(mon, 0 /*MDT_EFFECTIVE_DPI*/, &monX, &monY);

    // Real values: temporarily switch only THIS THREAD to per-monitor awareness,
    // query, and restore immediately. The process awareness is not touched.
    UINT realSysDpi = 0, realMonX = 0, realMonY = 0;
    bool realOk = false;
    if (pSetThreadCtx)
    {
        HANDLE old = pSetThreadCtx(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4))); // PER_MONITOR_AWARE_V2
        if (!old)
            old = pSetThreadCtx(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-3)));    // PER_MONITOR_AWARE
        if (old)
        {
            realOk = true;
            realSysDpi = pGetDpiForSystem ? pGetDpiForSystem() : 0;
            if (pGetDpiForMonitor && mon && FAILED(pGetDpiForMonitor(mon, 0, &realMonX, &realMonY)))
                realMonX = realMonY = 0;
            pSetThreadCtx(old);
        }
    }

    const UINT wndDpi = (hwnd && pGetDpiForWindow) ? pGetDpiForWindow(hwnd) : 0;

    UINT best = static_cast<UINT>(Max(lpx, 0));
    best = Max(best, SUCCEEDED(monHr) ? monX : 0u);
    best = Max(best, realMonX);
    best = Max(best, realSysDpi);
    best = Max(best, sysDpi);
    const float scale = best > 0 ? static_cast<float>(best) / 96.0f : 1.0f;

    Log("[DPI] --- %s ---", when);
    Log("[DPI] GetDeviceCaps LOGPIXELSX=%d LOGPIXELSY=%d (scale %.3f)", lpx, lpy, lpx > 0 ? lpx / 96.0 : 0.0);
    Log("[DPI] IsProcessDPIAware=%d GetProcessDpiAwareness=%s%d GetThreadDpiAwareness=%s%d (0=unaware 1=system 2=per-monitor)",
        legacyAware ? 1 : 0, pGetProcessDpiAwareness ? "" : "n/a:", procAwareness,
        (pGetThreadCtx && pAwareness) ? "" : "n/a:", threadAwareness);
    if (pGetDpiForSystem)
        Log("[DPI] GetDpiForSystem (this thread) = %u", sysDpi);
    else
        Log("[DPI] GetDpiForSystem: n/a (pre Win10 1607)");
    if (pGetDpiForMonitor)
        Log("[DPI] GetDpiForMonitor(primary, EFFECTIVE) (this thread) hr=0x%08lX dpi=%ux%u", static_cast<unsigned long>(monHr), monX, monY);
    else
        Log("[DPI] GetDpiForMonitor: n/a (shcore.dll missing, pre Win8.1)");
    if (realOk)
        Log("[DPI] per-monitor-aware query (thread-local, restored): system=%u monitor=%ux%u", realSysDpi, realMonX, realMonY);
    else
        Log("[DPI] per-monitor-aware query: n/a (SetThreadDpiAwarenessContext missing or failed)");
    if (hwnd)
    {
        if (pGetDpiForWindow)
            Log("[DPI] GetDpiForWindow(hwnd=%p) = %u", static_cast<void*>(hwnd), wndDpi);
        else
            Log("[DPI] GetDpiForWindow: n/a");
    }
    Log("[DPI] => scale %.4f (max detected dpi %u; informational only)", static_cast<double>(scale), best);
}

static INIT_ONCE g_lateOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK LateInitCb(PINIT_ONCE, PVOID, PVOID*)
{
    QueryAndLogDpi("startup (first D3D11 activity)", nullptr);
    return TRUE;
}
// Diagnostics that must not run inside DllMain (LoadLibrary of shcore etc.).
static void LateInit()
{
    if (LoggingActive())
        InitOnceExecuteOnce(&g_lateOnce, LateInitCb, nullptr, nullptr);
}

// =============================================================================
//  Tracked pipeline state + dedup sets (all protected by g_stateLock)
// =============================================================================
enum Category { CAT_VP, CAT_SC, CAT_VSCB, CAT_VS, CAT_DRAW, CAT_COUNT };
static const char* const kCatNames[CAT_COUNT] = { "VP", "SC", "VSCB", "VS", "DRAW" };

static const UINT kCBSlots      = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; // 14
static const UINT kMaxVpSc      = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; // 16
static const UINT kMaxOpenMaps  = 64;
static const size_t kSlotMapCap = 4096;

struct SlotState
{
    const void* buf;
    UINT byteWidth, usage, cpuAccess, bind, misc;
};

static const UINT kMaxVbSlots = 4; // IA vertex-buffer slots tracked

struct VbBinding
{
    const void* buf;
    UINT        stride, offset, bw, usage;
};

struct IbBinding
{
    const void* buf;
    DXGI_FORMAT fmt;
    UINT        offset, bw, usage;
};

struct TrackedState
{
    const void*    il;   // ID3D11InputLayout identity
    VbBinding      vb[kMaxVbSlots];
    IbBinding      ib;
    const void*    vs;
    const void*    ps;   // identity only
    UINT           topo; // D3D11_PRIMITIVE_TOPOLOGY
    SlotState      vscb[kCBSlots];
    SlotState      pscb[kCBSlots];
    D3D11_VIEWPORT vp0;
    bool           hasVp;
    D3D11_RECT     sc0;
    bool           hasSc;
};

struct OpenMap
{
    const void* res;  // ID3D11Resource* as passed to Map (identity only)
    const void* buf;  // ID3D11Buffer* identity (QI result, already released)
    const void* data; // mapped pointer
    UINT        bw;
    UINT        kind;  // 0 = constant buffer, 1 = vertex/index buffer
    UINT        bind, usage;
};

static SRWLOCK      g_stateLock = SRWLOCK_INIT;
static TrackedState g_state     = {};
static OpenMap      g_openMaps[kMaxOpenMaps] = {};
static volatile LONG g_openMapCount = 0; // written under g_stateLock; read lock-free (Unmap early-out)
static bool         g_capLogged[CAT_COUNT] = {};

// Heap-allocated once and intentionally never freed: hooks may still be called
// by other modules during process shutdown after our CRT destructors ran.
static std::unordered_set<uint64_t>*          g_sets[CAT_COUNT] = {};
static std::unordered_map<const void*, int>*   g_bufSlot      = nullptr;
static std::unordered_map<uint64_t, UINT>*     g_cbDumpCount  = nullptr;

// --- Capture containers (diagnostics, allocated only with logging on) ---------
// Created empty at init; entries are inserted ONLY while a capture is running.
struct Shadow
{
    UINT                       bw;
    uint64_t                   hash; // content hash of the first captureMaxCBBytes bytes
    std::vector<unsigned char> bytes;
};
static const size_t kShadowCap = 8192;
static std::unordered_set<uint64_t>*            g_capCbSet   = nullptr; // CB content hashes already printed
static std::unordered_set<uint64_t>*            g_capVpSet   = nullptr;
static std::unordered_set<uint64_t>*            g_capScSet   = nullptr;
static std::unordered_map<const void*, Shadow>* g_shadows    = nullptr;

// Vertex/index buffer shadows (capture only, cleared at capture start).
struct VbShadow
{
    UINT                       bw, usage, bind;
    std::vector<unsigned char> bytes; // first min(bw, CaptureMaxVBBytes) bytes
};
static const size_t kVbShadowCap = 4096;
static std::unordered_map<const void*, VbShadow>* g_vbShadows = nullptr;

// Shader bytecode hashes and input layouts (inserted at Create* time, not in the hot path).
struct ShaderInfo
{
    uint64_t hash;
    UINT     size;
};
struct LayoutInfo
{
    UINT        id;
    uint64_t    hash;
    UINT        numElems;
    bool        hasPos;
    bool        posOffsetKnown;
    DXGI_FORMAT posFmt;
    UINT        posSlot, posOffset;
};
static const UINT kShaderLogCap = 4096;
static std::unordered_map<const void*, ShaderInfo>* g_shaders = nullptr;
static std::unordered_map<const void*, LayoutInfo>* g_layouts = nullptr;
static UINT g_layoutNextId = 0;    // protected by g_stateLock
static UINT g_shaderLogCount = 0;  // protected by g_stateLock
static bool g_shaderCapLogged = false;

// The shader registry is needed by the fix rules (and the logging); every other
// container serves diagnostics only and is allocated only with logging on.
static void InitState()
{
    try
    {
        if (g_fixInfra || g_cfg.enableLogging)
        {
            g_shaders = new std::unordered_map<const void*, ShaderInfo>();
            g_shaders->reserve(1024);
        }
        if (!g_cfg.enableLogging)
            return;
        g_capCbSet   = new std::unordered_set<uint64_t>();
        g_capVpSet   = new std::unordered_set<uint64_t>();
        g_capScSet   = new std::unordered_set<uint64_t>();
        g_shadows    = new std::unordered_map<const void*, Shadow>();
        g_vbShadows  = new std::unordered_map<const void*, VbShadow>();
        g_layouts    = new std::unordered_map<const void*, LayoutInfo>();
        g_layouts->reserve(256);
        for (int c = 0; c < CAT_COUNT; ++c)
        {
            g_sets[c] = new std::unordered_set<uint64_t>();
            g_sets[c]->reserve(Min<UINT>(g_cfg.maxUniqueEntries, 8192u));
        }
        g_bufSlot = new std::unordered_map<const void*, int>();
        g_bufSlot->reserve(256);
        g_cbDumpCount = new std::unordered_map<uint64_t, UINT>();
        g_cbDumpCount->reserve(256);
    }
    catch (...)
    {
        // Without containers no logging is possible; hooks stay pass-through.
        g_logOn = false;
        g_cfg.enableLogging = 0;
    }
}

// Returns true if h was not seen before in this category (and was inserted).
static bool DedupInsertLocked(int cat, uint64_t h, bool& capJustReached) noexcept
{
    if (!g_logVerbose)
        return false; // EnableLogging=2: no [VP]/[SC]/[VSCB]/[VS]/[DRAW] analysis lines
    std::unordered_set<uint64_t>* s = g_sets[cat];
    if (!s)
        return false;
    if (s->find(h) != s->end())
        return false;
    if (s->size() >= g_cfg.maxUniqueEntries)
    {
        if (!g_capLogged[cat])
        {
            g_capLogged[cat] = true;
            capJustReached = true;
        }
        return false;
    }
    try
    {
        return s->insert(h).second;
    }
    catch (...)
    {
        return false;
    }
}

static void LogCapReached(int cat)
{
    Log("[%s] unique-entry cap (%u) reached - further new signatures of this category are not logged",
        kCatNames[cat], g_cfg.maxUniqueEntries);
}

static void SlotMapSetLocked(const void* buf, int slot) noexcept
{
    if (!g_bufSlot || !buf)
        return;
    try
    {
        auto it = g_bufSlot->find(buf);
        if (it != g_bufSlot->end())
        {
            it->second = slot;
            return;
        }
        if (g_bufSlot->size() >= kSlotMapCap)
            g_bufSlot->clear();
        g_bufSlot->emplace(buf, slot);
    }
    catch (...)
    {
    }
}

static int SlotMapGetLocked(const void* buf) noexcept
{
    if (!g_bufSlot || !buf)
        return -1;
    auto it = g_bufSlot->find(buf);
    return (it == g_bufSlot->end()) ? -1 : it->second;
}

static uint64_t CbSignature(UINT byteWidth, int slot)
{
    Fnv f;
    f.Val(byteWidth);
    f.Val(slot);
    return f.h;
}

static bool CbNeedsDumpLocked(uint64_t sig) noexcept
{
    if (!g_logVerbose)
        return false; // EnableLogging=2: no [CBDATA] samples
    if (!g_cbDumpCount || g_cfg.cbDumpSamples == 0)
        return false;
    auto it = g_cbDumpCount->find(sig);
    if (it == g_cbDumpCount->end())
        return g_cbDumpCount->size() < g_cfg.maxUniqueEntries;
    return it->second < g_cfg.cbDumpSamples;
}

// Reserves one dump for sig. Returns the 1-based sample number or 0.
static UINT CbTakeDumpLocked(uint64_t sig) noexcept
{
    if (!CbNeedsDumpLocked(sig))
        return 0;
    try
    {
        UINT& c = (*g_cbDumpCount)[sig];
        ++c;
        return c;
    }
    catch (...)
    {
        return 0;
    }
}

// Returns the buffer identity (any bind flags) if res is a buffer, else nullptr.
// The QI reference is released immediately - the caller (game) keeps its own.
static const void* QueryBuffer(ID3D11Resource* res, D3D11_BUFFER_DESC* desc)
{
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim); // cheap check first, avoids QI for textures
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER)
        return nullptr;
    ID3D11Buffer* b = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&b))) || !b)
        return nullptr;
    b->GetDesc(desc);
    b->Release();
    return static_cast<const void*>(b);
}

// Prints data as float4 rows "  cN: f f f f". Caller holds g_logCs.
static void LogFloatRows(const unsigned char* data, UINT n)
{
    for (UINT off = 0; off < n; off += 16)
    {
        float f[4] = {};
        const UINT cnt = Min<UINT>(16u, n - off) / 4u;
        memcpy(f, data + off, cnt * 4u);
        LineBuf lb;
        lb.Add("  c%u:", off / 16u);
        for (UINT k = 0; k < cnt; ++k)
            lb.Add(" %.6g", static_cast<double>(f[k]));
        Log("%s", lb.s);
    }
}

static void LogCBData(const char* via, const void* buf, UINT bw, int slot, UINT sample,
                      const unsigned char* data, UINT n)
{
    EnterCriticalSection(&g_logCs); // recursive: keeps the dump block contiguous
    Log("[CBDATA] via=%s buf=%p bw=%u slot=%d sample=%u/%u dumped=%u bytes", via, buf, bw, slot,
        sample, g_cfg.cbDumpSamples, n);
    LogFloatRows(data, n);
    LeaveCriticalSection(&g_logCs);
}

// =============================================================================
//  ID3D11DeviceContext hooks
//
//  The D3D11 runtime gives every context its own function table (inline in
//  the object, on the heap) and rewrites the Draw* entries itself (dirty-state
//  / fast-path variants). Therefore:
//    * originals are stored PER TABLE (HookedTable), found via *(void***)This
//    * the Draw* entries are "re-armed": after the original call of a state
//      hook (and of every draw hook) on the context's own thread, any draw
//      entry that no longer points to our hook is taken as the new original
//      and replaced by our hook again (plain aligned 4-byte interlocked write,
//      only if the table memory is writable - checked once per table).
// =============================================================================
enum VtIndex : UINT
{
    VT_VSSetConstantBuffers         = 7,
    VT_PSSetShader                  = 9,
    VT_VSSetShader                  = 11,
    VT_DrawIndexed                  = 12,
    VT_Draw                         = 13,
    VT_Map                          = 14,
    VT_Unmap                        = 15,
    VT_PSSetConstantBuffers         = 16,
    VT_IASetInputLayout             = 17,
    VT_IASetVertexBuffers           = 18,
    VT_IASetIndexBuffer             = 19,
    VT_DrawIndexedInstanced         = 20,
    VT_DrawInstanced                = 21,
    VT_IASetPrimitiveTopology       = 24,
    VT_DrawIndexedInstancedIndirect = 39,
    VT_DrawInstancedIndirect        = 40,
    VT_RSSetViewports               = 44,
    VT_RSSetScissorRects            = 45,
    VT_UpdateSubresource            = 48,
    VT_COUNT                        = 49
};

typedef void    (STDMETHODCALLTYPE* TRHF_PFN_SetConstantBuffers)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_VSSetShader)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_PSSetShader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_DrawIndirect)(ID3D11DeviceContext*, ID3D11Buffer*, UINT); // 39 and 40
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_IASetPrimitiveTopology)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_IASetInputLayout)(ID3D11DeviceContext*, ID3D11InputLayout*);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_IASetVertexBuffers)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_IASetIndexBuffer)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_RSSetViewports)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_RSSetScissorRects)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
typedef void    (STDMETHODCALLTYPE* TRHF_PFN_UpdateSubresource)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

// Fallback original per index (first table seen) - used only if a table is
// not found (should not happen).
static void* volatile g_orig[VT_COUNT] = {};

struct HookedTable
{
    void** volatile vtbl;
    void* volatile  orig[VT_COUNT];
    void* volatile  rejected[VT_COUNT]; // v1.4.6: last foreign entry Rearm left alone (no repeated ModuleOf)
    bool            writable; // RW memory -> re-arm without VirtualProtect
    // Fix state of the context that owns this table
    ID3D11Buffer*   fixCB;    // our dynamic 192-byte CB (game device only)
    const void*     vsS0;     // game buffer bound to VS slot 0
    uint64_t        vsHash;   // bytecode hash of the bound VS
    bool            hudVs;    // vsHash is listed in HudVsHashes
    uint64_t        psHash;   // bytecode hash of the bound PS
    // Menu gate / groups (per context, per frame)
    LONG            gateFrame;   // frame number of the current gate state (-1 = none yet)
    UINT            drawsBefore; // non-HUD draws before the first HUD draw of the frame
    bool            seenHud;
    bool            menuThis;    // menu signal in this frame
    char            menuReason;  // '1'/'2'/'3' = G1/G2/G3
    char            lastReason;
    int             menuHold;    // remaining frames gated after the last menu frame
    char            grp;         // 0, 'U' (popup), 'O' (popup at the origin) or 'S' (salvage counter)
    bool            uTriggerThisFrame; // a U trigger fired in this frame
    bool            uTriggerPrevFrame; // a U trigger fired in the previous gameplay frame
    bool            gateLogged;  // last logged gate state (diagnostics)
};
static const UINT    kMaxTables = 64;
static HookedTable   g_tables[kMaxTables] = {};
static volatile LONG g_tableCount = 0;
static UINT          g_tableEvict = 0;          // protected by g_hookLock
static void*         g_hookByIdx[VT_COUNT] = {}; // filled before the first patch
static bool          g_rearmOn = false;         // RearmDrawHooks && (EnableLogging || EnableFix)
static thread_local HookedTable* t_lastTable = nullptr;
// v1.4.3 performance A/B test (PerfTestHotkey): 0 = all context hooks,
// 1 = draw hooks removed (runtime draws run natively, no re-arm),
// 2 = all context hooks removed (pure pass-through). Fix inactive in 1 and 2.
static volatile LONG g_perfMode = 0;
// v1.4.3 test modes 3..7: all context hooks off except one group.
static const LONG kPerfModes = 11; // 8..10: only Map / Unmap / UpdateSubresource
enum { PG_DRAW = 0, PG_CB, PG_SHADER, PG_IA, PG_MAP, PG_RS, PG_COUNT };
static const char* const kPerfGroupNames[PG_COUNT] = { "draw", "cb", "shader", "ia", "map", "rs" };
// Calls per hook index (diagnostics, unsynchronised; render thread).
static LONG  g_callCnt[VT_COUNT] = {};
static float g_callPerFrame[PG_COUNT] = {};
static LONG  g_callCntSnap[VT_COUNT] = {};
static float g_callPerFrameMap[3] = {}; // Map, Unmap, UpdateSubresource
// Hit path of the CB shadow: Map/Update of a HUD-set 192-byte CB, shadow copies.
static LONG  g_mapHitCnt = 0, g_shadowCnt = 0;
static float g_mapHitPerFrame = 0.0f, g_shadowPerFrame = 0.0f, g_redirPerFrame = 0.0f;
#define TRHF_COUNT(idx) (++g_callCnt[idx])
// True while our ImGui overlay initialises/renders on this thread: every hook
// then only calls the original (no fix, tracking, capture or re-arm).
static thread_local bool t_inOverlay = false;

static HookedTable* FindTable(void** vtbl)
{
    HookedTable* t = t_lastTable;
    if (t && t->vtbl == vtbl)
        return t;
    const LONG n = g_tableCount;
    for (LONG i = 0; i < n; ++i)
    {
        if (g_tables[i].vtbl == vtbl)
        {
            t_lastTable = &g_tables[i];
            return &g_tables[i];
        }
    }
    return nullptr;
}

static inline void* OrigFor(ID3D11DeviceContext* ctx, UINT idx)
{
    HookedTable* t = FindTable(*reinterpret_cast<void***>(ctx));
    void* p = t ? static_cast<void*>(t->orig[idx]) : nullptr;
    return p ? p : static_cast<void*>(g_orig[idx]);
}

// Used inside the hooks: the original of THIS context's table.
#define TRHF_ORIG(type, idx) reinterpret_cast<type>(OrigFor(This, idx))

// --- Draw re-arm ---------------------------------------------------------------------
static const UINT kDrawIdx[] = { VT_DrawIndexed, VT_Draw, VT_DrawIndexedInstanced, VT_DrawInstanced,
                                 VT_DrawIndexedInstancedIndirect, VT_DrawInstancedIndirect };
static const UINT kDrawIdxCount = static_cast<UINT>(sizeof(kDrawIdx) / sizeof(kDrawIdx[0]));
// v1.4.6: Rearm also covers Map / Unmap / UpdateSubresource (the runtime was
// seen rewriting UpdateSubresource; Map/Unmap must stay paired for the
// scratch redirect).
static const UINT kRearmIdx[] = { VT_DrawIndexed, VT_Draw, VT_DrawIndexedInstanced, VT_DrawInstanced,
                                  VT_DrawIndexedInstancedIndirect, VT_DrawInstancedIndirect,
                                  VT_Map, VT_Unmap, VT_UpdateSubresource };
static const UINT kRearmIdxCount = static_cast<UINT>(sizeof(kRearmIdx) / sizeof(kRearmIdx[0]));
static volatile LONG g_rearmCount[VT_COUNT] = {};
static volatile LONG g_rearmForeign = 0;          // v1.4.6: changed entries left alone (other module)
static volatile LONG g_rearmForeignLogged = 0;
static volatile LONG g_rearmMapLogged[3] = {};    // Map, Unmap, UpdateSubresource: logged once

static HMODULE ModuleOf(const void* p); // object-table hooks section

static const char* DrawIdxName(UINT idx)
{
    switch (idx)
    {
    case VT_Map:                          return "Map";
    case VT_Unmap:                        return "Unmap";
    case VT_UpdateSubresource:            return "UpdateSubresource";
    case VT_DrawIndexed:                  return "DrawIndexed";
    case VT_Draw:                         return "Draw";
    case VT_DrawIndexedInstanced:         return "DrawIndexedInstanced";
    case VT_DrawInstanced:                return "DrawInstanced";
    case VT_DrawIndexedInstancedIndirect: return "DrawIndexedInstancedIndirect";
    case VT_DrawInstancedIndirect:        return "DrawInstancedIndirect";
    default:                              return "?";
    }
}

// [REARM] diagnostics: fixed open-addressing set of (idx, pointer), no heap.
static const UINT kRearmLogCap = 256;
static uint64_t   g_rearmKeys[512] = {};
static UINT       g_rearmKeyCount = 0;
static bool       g_rearmCapLogged = false;
static SRWLOCK    g_rearmLock = SRWLOCK_INIT;

static void RearmNote(void** vtbl, UINT idx, void* newOrig)
{
    if (!LoggingActive() || !g_logVerbose)
        return;
    const uint64_t key =
        (static_cast<uint64_t>(idx) << 32) | static_cast<uint64_t>(reinterpret_cast<UINT_PTR>(newOrig));
    bool isNew = false, capNow = false;
    AcquireSRWLockExclusive(&g_rearmLock);
    UINT pos = static_cast<UINT>((key * 0x9E3779B97F4A7C15ull) >> 55); // 0..511
    for (;;)
    {
        if (g_rearmKeys[pos] == key)
            break;
        if (g_rearmKeys[pos] == 0)
        {
            if (g_rearmKeyCount < kRearmLogCap) // table never more than half full
            {
                g_rearmKeys[pos] = key;
                ++g_rearmKeyCount;
                isNew = true;
            }
            else if (!g_rearmCapLogged)
            {
                g_rearmCapLogged = true;
                capNow = true;
            }
            break;
        }
        pos = (pos + 1u) & 511u;
    }
    ReleaseSRWLockExclusive(&g_rearmLock);
    if (isNew)
        Log("[REARM] vtbl=%p idx=%u (%s) new orig=%p", static_cast<void*>(vtbl), idx, DrawIdxName(idx), newOrig);
    if (capNow)
        Log("[REARM] unique-entry cap (%u) reached - further new (idx, orig) pairs are not logged", kRearmLogCap);
}

// v1.4.6: "same module" test for Rearm without GetModuleHandleEx (loader lock)
// per rewrite: image ranges [base, base+SizeOfImage) of the modules that hold
// originals are cached (resolved once per module, max. 8, no heap).
struct ModRange
{
    UINT_PTR lo, hi;
};
static ModRange g_modRanges[8] = {};
static LONG     g_modRangeCount = 0;   // g_rearmLock
static bool SameModuleCached(const void* cur, const void* prev)
{
    const UINT_PTR c = reinterpret_cast<UINT_PTR>(cur), p = reinterpret_cast<UINT_PTR>(prev);
    AcquireSRWLockShared(&g_rearmLock);
    for (LONG i = 0; i < g_modRangeCount; ++i)
    {
        if (p >= g_modRanges[i].lo && p < g_modRanges[i].hi)
        {
            const bool same = c >= g_modRanges[i].lo && c < g_modRanges[i].hi;
            ReleaseSRWLockShared(&g_rearmLock);
            return same;
        }
    }
    ReleaseSRWLockShared(&g_rearmLock);
    // Not cached: resolve prev's module once and remember its image range.
    HMODULE m = ModuleOf(prev);
    if (!m)
        return ModuleOf(cur) == nullptr; // both outside any module (same rule as InstallClassHooks)
    const unsigned char* base = reinterpret_cast<const unsigned char*>(m);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const UINT_PTR lo = reinterpret_cast<UINT_PTR>(base), hi = lo + nt->OptionalHeader.SizeOfImage;
    AcquireSRWLockExclusive(&g_rearmLock);
    if (g_modRangeCount < static_cast<LONG>(ARRAYSIZE(g_modRanges)))
    {
        g_modRanges[g_modRangeCount].lo = lo;
        g_modRanges[g_modRangeCount].hi = hi;
        ++g_modRangeCount;
    }
    ReleaseSRWLockExclusive(&g_rearmLock);
    return c >= lo && c < hi;
}

// Rare path of Rearm: entry idx no longer points to our hook. v1.4.6: the new
// entry is only adopted as original if it lives in the same module as the
// previous original (runtime rewrite). A foreign hook installed after ours is
// left alone - chaining to it would recurse into ourselves. The rejected
// pointer is cached so the module test runs once per new foreign value.
static __declspec(noinline) void RearmEntry(HookedTable* t, void** vtbl, UINT idx, void* hook, void* cur)
{
    void* prev = t->orig[idx];
    if (prev && !SameModuleCached(cur, prev))
    {
        t->rejected[idx] = cur;
        InterlockedIncrement(&g_rearmForeign);
        if (LoggingActive() && InterlockedExchange(&g_rearmForeignLogged, 1) == 0)
            Log("[HOOK] context table %p [%u] %s replaced by another module (new %p, orig %p) - left alone "
                "(further cases are counted only)",
                static_cast<void*>(vtbl), idx, DrawIdxName(idx), cur, prev);
        return;
    }
    t->orig[idx] = cur; // new original first, then the hook
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vtbl[idx]), static_cast<LONG>(reinterpret_cast<LONG_PTR>(hook)));
    InterlockedIncrement(&g_rearmCount[idx]);
    RearmNote(vtbl, idx, cur);
    const int mi = (idx == VT_Map) ? 0 : (idx == VT_Unmap ? 1 : (idx == VT_UpdateSubresource ? 2 : -1));
    if (mi >= 0 && LoggingActive() && InterlockedExchange(&g_rearmMapLogged[mi], 1) == 0)
        Log("[HOOK] runtime rewrote context table %p [%u] %s (new orig %p) - re-armed (logged once per entry)",
            static_cast<void*>(vtbl), idx, DrawIdxName(idx), cur);
}

// Must only be called from inside a hook of THIS context, after the original
// call (same thread as the runtime that may rewrite the table).
static void Rearm(ID3D11DeviceContext* This)
{
    if (!g_rearmOn || g_perfMode != 0)
        return;
    void** vtbl = *reinterpret_cast<void***>(This);
    HookedTable* t = FindTable(vtbl);
    if (!t || !t->writable)
        return;
    for (UINT k = 0; k < kRearmIdxCount; ++k)
    {
        const UINT idx = kRearmIdx[k];
        void* hook = g_hookByIdx[idx];
        void* cur = vtbl[idx];
        if (cur == hook || !cur || !hook || cur == t->rejected[idx])
            continue;
        RearmEntry(t, vtbl, idx, hook, cur);
    }
}

// =============================================================================
//  Capture (diagnostics, logging on): F10 arms a capture which starts at the
//  next Present and records EVERY draw for CaptureFrames frames. Without a
//  working Present hook it falls back to a CaptureSeconds time window.
//  Outside a capture the only cost is the 50 ms-throttled hotkey poll and a
//  read of g_capActive. No heap allocation outside a capture. With logging off
//  none of this runs (all callers check LoggingActive() first).
// =============================================================================
static const UINT    kMaxCapFrames     = 60;
static volatile LONG g_capActive       = 0;
static volatile LONG g_capPending      = 0; // armed by hotkey, starts at next Present
static volatile LONG g_capLines        = 0;
static volatile LONG g_capDraws        = 0;
static volatile LONG g_capFrame        = 0; // frame index inside the capture
static volatile LONG g_capDrawInFrame  = 0;
static volatile LONG g_lastPollTick    = 0;
static volatile LONG g_frameCount      = 0; // Presents seen (whole session)
static volatile LONG g_lastPresentTick = 0;
static bool          g_capFrameMode    = false;   // protected by g_stateLock
static LONG          g_capPerFrame[kMaxCapFrames] = {};
static LONG          g_rearmAtCapStart[VT_COUNT] = {};
static UINT          g_capIndex        = 0;       // protected by g_stateLock
static DWORD         g_capStartTick    = 0;
static bool          g_keyWasDown      = false;   // only touched by the current poll winner

// Geometry watchdog ("OFF"): once every WatchSampleMs one "sample frame"
// (Present .. next Present) shadows the VB/IB contents of the learned HUD
// buffers exactly like a capture does; outside sample frames only the flag read.
static const UINT    kWatchBufs        = 16;               // learned HUD VB/IB identities (ring)
static const LONG    kWatchCopyBudget  = 8 * 1024 * 1024;  // max shadow bytes copied per sample frame
static volatile LONG g_watchSampling   = 0;   // 1 = the current frame is a sample frame
static volatile LONG g_watchSampleIdx  = 0;   // sample counter (persistence: consecutive samples)
static DWORD         g_watchSampleTick = 0;   // Present thread only
static volatile LONG g_watchCopyBytes  = 0;   // shadow bytes copied in the current sample frame
static volatile LONG g_watchBudgetLogged = 0;
static UINT          g_watchBbW = 0, g_watchBbH = 0; // back buffer at the sample start
// Last known game back-buffer size (diagnostics only: [INV] bb=WxH). Updated at
// swap-chain creation, after ResizeBuffers, at watchdog sample starts and when the
// menu/overlay creates its RTV. Packed W<<16|H so readers see a consistent pair.
static volatile LONG g_lastBbWH = 0;
static inline void BbNote(UINT w, UINT h)
{
    if (!g_logOn)
        return;
    if (w && h && w <= 0xFFFFu && h <= 0xFFFFu)
        InterlockedExchange(&g_lastBbWH, static_cast<LONG>((w << 16) | h));
}
static const void* volatile g_watchBufs[kWatchBufs] = {};
static volatile LONG g_watchBufNext    = 0;

static inline bool WatchSampling() { return g_watchSampling != 0; }

static bool WatchBufContains(const void* p)
{
    if (!p)
        return false;
    for (UINT i = 0; i < kWatchBufs; ++i)
    {
        if (g_watchBufs[i] == p)
            return true;
    }
    return false;
}

// Called from sample-frame HUD draws: remember the IA buffers of a HUD draw so
// that the next sample frame mirrors them (only these, keeps memory/copies small).
static void WatchBufLearn(const void* p)
{
    if (!p || WatchBufContains(p))
        return;
    const ULONG slot = static_cast<ULONG>(InterlockedIncrement(&g_watchBufNext) - 1) % kWatchBufs;
    g_watchBufs[slot] = p;
}

static inline bool CaptureActive() { return g_capActive != 0; }
static inline DWORD CapTime() { return GetTickCount() - g_capStartTick; }
static inline bool PresentActive()
{
    return g_frameCount > 0 && (GetTickCount() - static_cast<DWORD>(g_lastPresentTick)) < 1000u;
}

// Reserves n capture log lines; false once CaptureMaxLines is exceeded.
static bool CapTakeLines(UINT n)
{
    const LONG add = static_cast<LONG>(n);
    const LONG v = InterlockedExchangeAdd(&g_capLines, add) + add;
    return v <= static_cast<LONG>(g_cfg.captureMaxLines);
}

static bool CapSetInsertLocked(std::unordered_set<uint64_t>* s, uint64_t h) noexcept
{
    if (!s)
        return false;
    try
    {
        return s->insert(h).second;
    }
    catch (...)
    {
        return false;
    }
}

static void CapNotify(const char* msg)
{
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "TR2013 HUD Fix+: %s\n", msg);
    OutputDebugStringA(buf);
}

static void LogWindowInfo(IDXGISwapChain* sc); // DXGI section

static void StartCapture(bool frameMode, IDXGISwapChain* sc)
{
    UINT idx = 0;
    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_capActive && g_capCbSet && g_capVpSet && g_capScSet && g_shadows)
    {
        // clear() never allocates; frees the previous capture's memory.
        g_capCbSet->clear();
        g_capVpSet->clear();
        g_capScSet->clear();
        g_shadows->clear();
        if (g_vbShadows)
            g_vbShadows->clear();
        idx = ++g_capIndex;
        g_capFrameMode = frameMode;
        g_capStartTick = GetTickCount();
        for (UINT i = 0; i < kMaxCapFrames; ++i)
            g_capPerFrame[i] = 0;
        for (UINT i = 0; i < VT_COUNT; ++i)
            g_rearmAtCapStart[i] = g_rearmCount[i];
        InterlockedExchange(&g_capLines, 0);
        InterlockedExchange(&g_capDraws, 0);
        InterlockedExchange(&g_capFrame, 0);
        InterlockedExchange(&g_capDrawInFrame, 0);
        InterlockedExchange(&g_capActive, 1);
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    if (!idx)
        return;
    char msg[128];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[CAPTURE] start #%u", idx);
    CapNotify(msg);
    if (frameMode)
        Log("[CAPTURE] start #%u mode=frames (CaptureFrames=%u, CaptureMaxCBBytes=%u, CaptureMaxLines=%u, frame=%ld)",
            idx, g_cfg.captureFrames, g_cfg.captureMaxCBBytes, g_cfg.captureMaxLines, g_frameCount);
    else
        Log("[CAPTURE] start #%u mode=time (no Present hook; CaptureSeconds=%u, CaptureMaxCBBytes=%u, CaptureMaxLines=%u)",
            idx, g_cfg.captureSeconds, g_cfg.captureMaxCBBytes, g_cfg.captureMaxLines);
    if (sc)
        LogWindowInfo(sc);
}

static void EndCapture(const char* reason)
{
    UINT idx = 0, cbPrinted = 0;
    LONG draws = 0, lines = 0, frames = 0;
    bool frameMode = false;
    LONG perFrame[kMaxCapFrames] = {};
    LONG rearms[kDrawIdxCount] = {};
    AcquireSRWLockExclusive(&g_stateLock);
    if (g_capActive)
    {
        InterlockedExchange(&g_capActive, 0);
        idx       = g_capIndex;
        draws     = g_capDraws;
        lines     = g_capLines;
        frames    = g_capFrame;
        frameMode = g_capFrameMode;
        cbPrinted = static_cast<UINT>(g_capCbSet->size());
        for (UINT i = 0; i < kMaxCapFrames; ++i)
            perFrame[i] = g_capPerFrame[i];
        for (UINT k = 0; k < kDrawIdxCount; ++k)
            rearms[k] = g_rearmCount[kDrawIdx[k]] - g_rearmAtCapStart[kDrawIdx[k]];
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    if (!idx)
        return;

    char msg[128];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[CAPTURE] end #%u", idx);
    CapNotify(msg);

    LineBuf lb;
    lb.Add("[CAPTURE] end #%u mode=%s frames=%ld draws=%ld perFrame=[", idx, frameMode ? "frames" : "time", frames,
           draws);
    const LONG nf = Min<LONG>(frames, static_cast<LONG>(kMaxCapFrames));
    for (LONG i = 0; i < nf; ++i)
        lb.Add("%s%ld", i ? "," : "", perFrame[i]);
    lb.Add("] cbPrinted=%u lines=%ld rearms=[", cbPrinted, lines);
    for (UINT k = 0; k < kDrawIdxCount; ++k)
        lb.Add("%s%u:%ld", k ? "," : "", kDrawIdx[k], rearms[k]);
    lb.Add("] rearmsTotal=[");
    for (UINT k = 0; k < kDrawIdxCount; ++k)
        lb.Add("%s%u:%ld", k ? "," : "", kDrawIdx[k], static_cast<LONG>(g_rearmCount[kDrawIdx[k]]));
    lb.Add("] reason=%s", reason);
    Log("%s", lb.s);
}

// Called from Present, RSSetViewports and all draw hooks (only when logging is on).
static void PollCapture()
{
    if (!g_cfg.captureHotkey)
        return;
    const DWORD now = GetTickCount();
    const LONG last = g_lastPollTick;
    if (now - static_cast<DWORD>(last) < 50u)
        return;
    if (InterlockedCompareExchange(&g_lastPollTick, static_cast<LONG>(now), last) != last)
        return; // another thread won this poll slot

    if (g_capActive)
    {
        if (g_capLines >= static_cast<LONG>(g_cfg.captureMaxLines))
            EndCapture("CaptureMaxLines reached");
        else if (!g_capFrameMode && now - g_capStartTick >= g_cfg.captureSeconds * 1000u)
            EndCapture("CaptureSeconds elapsed");
        else if (g_capFrameMode && now - static_cast<DWORD>(g_lastPresentTick) > 2000u)
            EndCapture("no Present for 2 s");
    }

    const bool down =
        (static_cast<USHORT>(GetAsyncKeyState(static_cast<int>(g_cfg.captureHotkey))) & 0x8000u) != 0;
    if (down && !g_keyWasDown && !g_capActive && !g_capPending && !HotkeysSuppressed()) // rising edge
    {
        if (PresentActive())
        {
            InterlockedExchange(&g_capPending, 1);
            Log("[CAPTURE] hotkey - armed, starts at next Present (frame %ld)", g_frameCount);
        }
        else
        {
            StartCapture(false, nullptr);
        }
    }
    g_keyWasDown = down;
}

static void PollFix(); // fix section (F9 toggle + statistics)

// =============================================================================
//  v1.3.0 memory telemetry ([MEM], EnableLogging 1 or 2)
//  No allocations: K32GetProcessMemoryInfo (kernel32, no psapi.lib) +
//  GlobalMemoryStatusEx. v1.4.6: largest free region, walk time and region
//  count come from the last completed time-sliced address-space walk (see
//  MemWalkSlice) - no own VirtualQuery walk.
// =============================================================================
typedef BOOL (WINAPI* TRHF_PFN_K32GetProcessMemoryInfo)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
static volatile LONG g_memLastTick = 0;
static volatile LONG g_memStarted  = 0;
static volatile LONG g_memFirstTry = 0; // GetTickCount|1 of the first [MEM] attempt (waits for a walk)

// v1.4.6: result of the last completed incremental walk, published by the game
// Present thread with a sequence counter (odd = being written, 0 = nothing
// yet). Readers: [MEM] telemetry (any Present thread, process exit).
static volatile LONG      g_memPubSeq     = 0;
static volatile ULONGLONG g_memPubLargest = 0; // bytes
static volatile LONG      g_memPubWalkUs  = 0; // summed slice time of the pass
static volatile LONG      g_memPubRegions = 0;

// Bounded retries: never spins (e.g. at process exit with a killed writer).
static bool MemPubRead(ULONGLONG* largest, LONG* walkUs, LONG* regions)
{
    for (int tries = 0; tries < 8; ++tries)
    {
        const LONG s1 = g_memPubSeq;
        if (s1 == 0)
            return false;
        if (s1 & 1)
        {
            YieldProcessor();
            continue;
        }
        *largest = g_memPubLargest;
        *walkUs  = g_memPubWalkUs;
        *regions = g_memPubRegions;
        if (g_memPubSeq == s1)
            return true;
    }
    return false;
}

// Formats "[MEM] t=... private=... MB working=... MB vsize=... MB largestFree=... MB ours=... KB
// walk=... ms regions=...". Safe at process exit (kernel32 calls only, no heap, no locks).
static void FormatMemLine(char* buf, size_t cap)
{
    static TRHF_PFN_K32GetProcessMemoryInfo s_pmi = nullptr;
    static volatile LONG s_resolved = 0;
    if (!s_resolved)
    {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        s_pmi = k32 ? reinterpret_cast<TRHF_PFN_K32GetProcessMemoryInfo>(GetProcAddress(k32, "K32GetProcessMemoryInfo"))
                    : nullptr;
        InterlockedExchange(&s_resolved, 1);
    }
    double privMB = -1.0, workMB = -1.0;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (s_pmi && s_pmi(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc)))
    {
        privMB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        workMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }

    // vsize = committed + reserved = user address space minus free (current).
    double vsizeMB = -1.0;
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalVirtual >= ms.ullAvailVirtual)
        vsizeMB = static_cast<double>(ms.ullTotalVirtual - ms.ullAvailVirtual) / (1024.0 * 1024.0);

    // Largest free region: last completed incremental walk.
    char lf[32] = "n/a";
    char wk[48] = "walk=n/a";
    ULONGLONG largest = 0;
    LONG walkUs = 0, regions = 0;
    if (MemPubRead(&largest, &walkUs, &regions))
    {
        _snprintf_s(lf, sizeof(lf), _TRUNCATE, "%.1f MB", static_cast<double>(largest) / (1024.0 * 1024.0));
        _snprintf_s(wk, sizeof(wk), _TRUNCATE, "walk=%.2f ms regions=%ld", static_cast<double>(walkUs) / 1000.0,
                    regions);
    }

    const ULONGLONG t = GetTickCount64() - g_startTick;
    const LONG ours = g_oursBytes;
    _snprintf_s(buf, cap, _TRUNCATE,
                "[MEM] t=%llu.%03llu private=%.1f MB working=%.1f MB vsize=%.1f MB largestFree=%s ours=%ld KB %s",
                t / 1000ull, t % 1000ull, privMB, workMB, vsizeMB, lf, (ours > 0 ? ours : 0L) / 1024L, wk);
}

// From OnPresent: first line at the first logged Present (v1.4.6: once the
// first address-space walk completed, at most 15 s later), then every MemLogSec.
static void MemPoll()
{
    if (!g_cfg.memLogSec)
        return;
    const DWORD now = GetTickCount();
    if (!g_memStarted && g_memPubSeq == 0)
    {
        const LONG first = g_memFirstTry;
        if (!first)
        {
            InterlockedCompareExchange(&g_memFirstTry, static_cast<LONG>(now | 1u), 0);
            return;
        }
        if (now - static_cast<DWORD>(first) < 15000u)
            return;
    }
    const LONG last = g_memLastTick;
    if (g_memStarted && now - static_cast<DWORD>(last) < g_cfg.memLogSec * 1000u)
        return;
    if (InterlockedCompareExchange(&g_memLastTick, static_cast<LONG>(now), last) != last)
        return; // another thread won this slot
    InterlockedExchange(&g_memStarted, 1);
    char line[256];
    FormatMemLine(line, sizeof(line));
    Log("%s", line);
}

// =============================================================================
//  v1.4.6 low address-space warning ([Memory] MemWarning)
//  Game Present thread only (walk, check, menu and toast): no locks needed.
//  The address space is walked incrementally (time-sliced): at most
//  kMemWalkSlice VirtualQuery calls per Present, no heap. A new pass starts
//  when the last completed one is >= 10 s old (>= 1 s while the menu shows the
//  values). Nobody needs the values (MemWarning=0, no [MEM] logging, menu not
//  showing them) -> only a timer compare, no walk.
//  The full synchronous AddrSpaceWalk is used by the crash handler only.
// =============================================================================
struct MemStat
{
    bool      valid;
    ULONGLONG totalMB, availMB, largestMB;
    ULONGLONG tick; // GetTickCount64 of the last completed pass
};
static MemStat   g_memStat = {};
static ULONGLONG g_memMenuTick = 0;    // last menu frame that showed the memory values
static ULONGLONG g_memToastLast = 0;   // last time the toast was shown (0 = never)
static ULONGLONG g_memToastUntil = 0;  // toast visible until this tick
static char      g_memToastText[160] = {};

// Incremental walk state (game Present thread only).
static const UINT    kMemWalkSlice = 64; // VirtualQuery calls per Present
static bool          g_walkRunning = false;
static UINT_PTR      g_walkCursor  = 0;
static UINT_PTR      g_walkEnd     = 0;
static AddrSpaceInfo g_walkAcc     = {};
static UINT          g_walkRegions = 0;
static LONGLONG      g_walkTicks   = 0; // QPC ticks summed over the slices of the pass

// Completed pass -> g_memStat (Present thread) + sequence-published values for [MEM].
static void MemWalkPublish()
{
    const AddrSpaceInfo& as = g_walkAcc;
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    g_memStat.largestMB = as.largestFree / (1024ull * 1024ull);
    if (GlobalMemoryStatusEx(&ms))
    {
        g_memStat.totalMB = ms.ullTotalVirtual / (1024ull * 1024ull);
        g_memStat.availMB = ms.ullAvailVirtual / (1024ull * 1024ull);
    }
    else
    {
        g_memStat.totalMB = (as.committed + as.reserved + as.freeTotal) / (1024ull * 1024ull);
        g_memStat.availMB = as.freeTotal / (1024ull * 1024ull);
    }
    g_memStat.tick = GetTickCount64();
    g_memStat.valid = true;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const LONGLONG us = f.QuadPart > 0 ? g_walkTicks * 1000000ll / f.QuadPart : 0ll;
    InterlockedIncrement(&g_memPubSeq); // odd: writing
    g_memPubLargest = as.largestFree;
    g_memPubWalkUs  = static_cast<LONG>(Min<LONGLONG>(us, 0x7FFFFFFFll));
    g_memPubRegions = static_cast<LONG>(g_walkRegions);
    InterlockedIncrement(&g_memPubSeq); // even: stable
}

// One slice of the walk (starts a pass if none is running). Same accounting as
// AddrSpaceWalk. Returns true when the pass completed and was published.
static bool MemWalkSlice()
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (!g_walkRunning)
    {
        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        g_walkCursor  = reinterpret_cast<UINT_PTR>(si.lpMinimumApplicationAddress);
        g_walkEnd     = reinterpret_cast<UINT_PTR>(si.lpMaximumApplicationAddress);
        g_walkAcc     = AddrSpaceInfo{};
        g_walkRegions = 0;
        g_walkTicks   = 0;
        g_walkRunning = true;
    }
    bool done = false;
    for (UINT i = 0; i < kMemWalkSlice; ++i)
    {
        if (g_walkCursor >= g_walkEnd)
        {
            done = true;
            break;
        }
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(g_walkCursor), &mbi, sizeof(mbi)) || mbi.RegionSize == 0)
        {
            done = true;
            break;
        }
        ++g_walkRegions;
        if (mbi.State == MEM_FREE)
        {
            g_walkAcc.freeTotal += mbi.RegionSize;
            g_walkAcc.largestFree = Max<ULONGLONG>(g_walkAcc.largestFree, mbi.RegionSize);
        }
        else if (mbi.State == MEM_COMMIT)
        {
            g_walkAcc.committed += mbi.RegionSize;
        }
        else
        {
            g_walkAcc.reserved += mbi.RegionSize;
        }
        const UINT_PTR next = reinterpret_cast<UINT_PTR>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= g_walkCursor)
        {
            done = true; // wrapped at the top of the address space
            break;
        }
        g_walkCursor = next;
    }
    if (g_walkCursor >= g_walkEnd)
        done = true;
    QueryPerformanceCounter(&t1);
    g_walkTicks += t1.QuadPart - t0.QuadPart;
    if (!done)
        return false;
    g_walkRunning = false;
    MemWalkPublish();
    return true;
}

static bool MemLow()
{
    return g_memStat.valid && (g_memStat.availMB < g_cfg.memWarnFreeMB || g_memStat.largestMB < g_cfg.memWarnBlockMB);
}

// Menu frame showing the memory values: shows the last completed pass; while
// shown, MemWarnPoll starts a new pass every second.
static void MemStatNoteMenu()
{
    g_memMenuTick = GetTickCount64();
}

static inline bool MemToastActive()
{
    return g_memToastUntil != 0 && GetTickCount64() < g_memToastUntil;
}

// From the game Present hook: drives the incremental walk and evaluates the
// warning once per completed pass; toast at most every 5 min.
static void MemWarnPoll()
{
    if (!g_walkRunning)
    {
        const ULONGLONG t = GetTickCount64();
        const bool menu = g_memMenuTick != 0 && t - g_memMenuTick < 2000ull;
        if (!g_cfg.memWarning && !menu && !(LoggingActive() && g_cfg.memLogSec))
            return; // nobody needs the values
        if (g_memStat.valid && t - g_memStat.tick < (menu ? 1000ull : 10000ull))
            return;
    }
    if (!MemWalkSlice())
        return;
    if (!g_cfg.memWarning || !MemLow())
        return;
    const ULONGLONG now = g_memStat.tick;
    if (g_memToastLast && now - g_memToastLast < 300000ull)
        return;
    g_memToastLast = now;
    g_memToastUntil = now + 10000ull;
    _snprintf_s(g_memToastText, sizeof(g_memToastText), _TRUNCATE,
                "Low memory: %llu MB free (largest block %llu MB). Save your game and restart soon to avoid a crash.",
                g_memStat.availMB, g_memStat.largestMB);
    EventPush("[MEM] low address space warning: %llu MB free, largest block %llu MB (limits %u/%u MB)",
              g_memStat.availMB, g_memStat.largestMB, g_cfg.memWarnFreeMB, g_cfg.memWarnBlockMB);
    if (LoggingActive())
        Log("[MEM] low address space warning: %llu MB free of %llu MB, largest block %llu MB (limits MemWarnFreeMB=%u "
            "MemWarnBlockMB=%u)",
            g_memStat.availMB, g_memStat.totalMB, g_memStat.largestMB, g_cfg.memWarnFreeMB, g_cfg.memWarnBlockMB);
}

// Drops all VB/IB shadows (capture leftovers or the last sample frame), never
// during a capture. clear() frees the vectors; nothing is allocated.
static void WatchShadowsClear()
{
    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_capActive && g_vbShadows && !g_vbShadows->empty())
        g_vbShadows->clear();
    ReleaseSRWLockExclusive(&g_stateLock);
}

// Frame boundary for the OFF watchdog: ends a running sample frame and arms
// the next one every WatchSampleMs (never during or right before a capture).
static void WatchSamplePresent(IDXGISwapChain* sc)
{
    if (g_watchSampling)
    {
        InterlockedExchange(&g_watchSampling, 0);
        WatchShadowsClear();
    }
    if (!g_cfg.watchSampleMs || !g_fixInfra || g_capActive || g_capPending)
        return;
    const DWORD now = GetTickCount();
    if (g_watchSampleTick && now - g_watchSampleTick < g_cfg.watchSampleMs)
        return;
    g_watchSampleTick = now | 1u;
    if (sc)
    {
        DXGI_SWAP_CHAIN_DESC d = {};
        if (SUCCEEDED(sc->GetDesc(&d)))
        {
            g_watchBbW = d.BufferDesc.Width;
            g_watchBbH = d.BufferDesc.Height;
            BbNote(d.BufferDesc.Width, d.BufferDesc.Height);
        }
    }
    WatchShadowsClear(); // no stale data from a capture
    InterlockedExchange(&g_watchCopyBytes, 0);
    InterlockedIncrement(&g_watchSampleIdx);
    InterlockedExchange(&g_watchSampling, 1);
}

// Frame boundary (called after the original Present returned).
static void OnPresent(IDXGISwapChain* sc)
{
    InterlockedIncrement(&g_frameCount); // frame counter of the menu gate
    if (g_fixInfra)
        PollFix();
    if (!LoggingActive())
        return;
    InterlockedExchange(&g_lastPresentTick, static_cast<LONG>(GetTickCount())); // capture: Present alive
    PollCapture();
    MemPoll();
    WatchSamplePresent(sc); // before the capture start below (no sample frame while capturing)

    if (g_capActive)
    {
        AcquireSRWLockExclusive(&g_stateLock);
        const bool frameMode = g_capFrameMode;
        const LONG f = g_capFrame;
        if (frameMode && f >= 0 && f < static_cast<LONG>(kMaxCapFrames))
            g_capPerFrame[f] = g_capDrawInFrame;
        ReleaseSRWLockExclusive(&g_stateLock);
        if (frameMode)
        {
            InterlockedExchange(&g_capDrawInFrame, 0);
            if (InterlockedIncrement(&g_capFrame) >= static_cast<LONG>(g_cfg.captureFrames))
                EndCapture("CaptureFrames reached");
        }
    }
    else if (g_capPending)
    {
        InterlockedExchange(&g_capPending, 0);
        StartCapture(true, sc);
    }
}

// CB content during a capture: update the per-buffer shadow only. Contents are
// printed lazily when a captured draw references them (CaptureDraw).
static void CaptureCBUpdate(const void* buf, UINT bw, const unsigned char* data, UINT n)
{
    Fnv f;
    f.Val(bw);
    f.Bytes(data, n);
    const uint64_t h = f.h ? f.h : 1ull; // 0 is reserved for "unknown"

    AcquireSRWLockExclusive(&g_stateLock);
    if (g_capActive && g_shadows)
    {
        try
        {
            auto it = g_shadows->find(buf);
            if (it == g_shadows->end() && g_shadows->size() < kShadowCap)
                it = g_shadows->emplace(buf, Shadow{}).first;
            if (it != g_shadows->end())
            {
                Shadow& s = it->second;
                s.bw   = bw;
                s.hash = h;
                s.bytes.assign(data, data + n);
            }
        }
        catch (...)
        {
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

// VB/IB content during a capture or a watchdog sample frame: write
// [src, src+n) at dstOffset into the per-buffer shadow (size
// min(bw, CaptureMaxVBBytes)). Sample frames (without capture) are limited to
// kWatchCopyBudget bytes per frame.
static void VbShadowStore(const void* buf, UINT bw, UINT usage, UINT bind, const void* src, UINT dstOffset, UINT n)
{
    const UINT cap = Min<UINT>(bw, g_cfg.captureMaxVBBytes);
    if (!buf || !src || dstOffset >= cap || n == 0)
        return;
    const UINT len = Min<UINT>(n, cap - dstOffset);
    bool budgetHit = false;
    AcquireSRWLockExclusive(&g_stateLock);
    bool store = g_capActive != 0;
    if (!store && g_watchSampling)
    {
        const LONG add = static_cast<LONG>(len);
        store = InterlockedExchangeAdd(&g_watchCopyBytes, add) + add <= kWatchCopyBudget;
        budgetHit = !store;
    }
    if (store && g_vbShadows)
    {
        try
        {
            auto it = g_vbShadows->find(buf);
            if (it == g_vbShadows->end() && g_vbShadows->size() < kVbShadowCap)
                it = g_vbShadows->emplace(buf, VbShadow{}).first;
            if (it != g_vbShadows->end())
            {
                VbShadow& vs = it->second;
                if (vs.bytes.size() != cap)
                    vs.bytes.assign(cap, 0);
                SafeCopy(vs.bytes.data() + dstOffset, src, len);
                vs.bw    = bw;
                vs.usage = usage;
                vs.bind  = bind;
            }
        }
        catch (...)
        {
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    if (budgetHit && InterlockedExchange(&g_watchBudgetLogged, 1) == 0)
        Log("[WATCH] sample frame copy budget (%ld bytes) reached - remaining HUD buffers of that frame not mirrored "
            "(logged once)", kWatchCopyBudget);
}

enum DrawType : UINT
{
    DT_DrawIndexed = 1,
    DT_Draw,
    DT_DrawIndexedInstanced,
    DT_DrawInstanced,
    DT_DrawIndexedInstancedIndirect,
    DT_DrawInstancedIndirect
};
static const char* const kDrawNames[] = { "?", "DrawIndexed", "Draw", "DrawIndexedInstanced", "DrawInstanced",
                                          "DrawIndexedInstancedIndirect", "DrawInstancedIndirect" };

struct DrawInfo
{
    UINT        type;
    UINT        count;     // index or vertex count (per instance)
    UINT        instances; // 1 for non-instanced, 0 for indirect (unknown)
    UINT        start;     // start index or start vertex
    INT         base;      // base vertex (indexed)
    UINT        startInst;
    const void* args;      // indirect args buffer
    UINT        argsOff;
    UINT        fixed;     // 1 = HUD correction applied to this draw
};

struct CbPrint
{
    const void* buf;
    uint64_t    hash;
};

// Appends " tag=[s:bw#hash,...]" for all bound slots. Caller holds g_stateLock.
// Content hashes not yet printed in this capture (and bw <= CaptureMaxCBBytes)
// are queued for lazy [CAP-CB] output.
static void AppendCbListLocked(LineBuf& lb, const char* tag, const SlotState* slots, CbPrint* todo, UINT& ntodo)
{
    lb.Add(" %s=[", tag);
    bool first = true;
    for (UINT s = 0; s < kCBSlots; ++s)
    {
        const SlotState& ss = slots[s];
        if (!ss.buf)
            continue;
        uint64_t h = 0;
        if (g_shadows)
        {
            auto it = g_shadows->find(ss.buf);
            if (it != g_shadows->end())
                h = it->second.hash;
        }
        lb.Add("%ss%u:%u#%016llX", first ? "" : ",", s, ss.byteWidth, h);
        first = false;
        if (h && ss.byteWidth <= g_cfg.captureMaxCBBytes && ntodo < 2 * kCBSlots &&
            CapSetInsertLocked(g_capCbSet, h))
        {
            todo[ntodo].buf = ss.buf;
            todo[ntodo].hash = h;
            ++ntodo;
        }
    }
    lb.Add("]");
}

// --- [CAP-VTX]: POSITION decoding from VB/IB shadows ------------------------------
static const UINT kMaxVtxDecode = 4096;

static float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    uint32_t exp = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
    uint32_t man = static_cast<uint32_t>(h) & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0)
    {
        if (man == 0)
        {
            bits = sign;
        }
        else
        {
            exp = 127u - 15u + 1u; // subnormal -> normalise
            while ((man & 0x400u) == 0)
            {
                man <<= 1;
                --exp;
            }
            man &= 0x3FFu;
            bits = sign | (exp << 23) | (man << 13);
        }
    }
    else if (exp == 31)
    {
        bits = sign | 0x7F800000u | (man << 13);
    }
    else
    {
        bits = sign | ((exp + 127u - 15u) << 23) | (man << 13);
    }
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// Bytes read for a supported POSITION format, 0 = unsupported.
static UINT PosFormatBytes(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G32_FLOAT:       return 8;
    case DXGI_FORMAT_R32G32B32_FLOAT:    return 12;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R16G16_SNORM:       return 4;
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default:                             return 0;
    }
}

static void DecodePos(DXGI_FORMAT fmt, const unsigned char* p, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    switch (fmt)
    {
    case DXGI_FORMAT_R32G32_FLOAT:
        memcpy(out, p, 8);
        break;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        memcpy(out, p, 12);
        break;
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    {
        const bool two = (fmt == DXGI_FORMAT_R16G16_SINT || fmt == DXGI_FORMAT_R16G16_SNORM);
        const bool snorm = (fmt == DXGI_FORMAT_R16G16_SNORM || fmt == DXGI_FORMAT_R16G16B16A16_SNORM);
        const UINT nc = two ? 2u : 3u;
        int16_t v[3] = {};
        memcpy(v, p, nc * 2u);
        for (UINT i = 0; i < nc; ++i)
            out[i] = snorm ? Max(static_cast<float>(v[i]) / 32767.0f, -1.0f) : static_cast<float>(v[i]);
        break;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    {
        uint16_t v[3] = {};
        memcpy(v, p, 6);
        for (UINT i = 0; i < 3; ++i)
            out[i] = HalfToFloat(v[i]);
        break;
    }
    default:
        break;
    }
}

// Decodes the POSITION of every vertex of a captured draw (count <= 4096) from
// the VB/IB shadows and projects it with VS-CB s0 (rows c0..c3) in both
// conventions: row-vector clip = [x y z 1]*M, column-vector clip = M*[x y z 1]^T.
// Caller holds g_stateLock and g_logCs.
static void LogCapVtxLocked(const DrawInfo& d, LONG n, const D3D11_VIEWPORT& vp)
{
    if (d.args || d.count == 0 || d.count > kMaxVtxDecode || !g_layouts || !g_vbShadows)
        return;
    if (!CapTakeLines(1))
        return;
    const bool indexed = (d.type == DT_DrawIndexed || d.type == DT_DrawIndexedInstanced);
    LineBuf lb;
    lb.Add("[CAP-VTX] n=%ld", n);

    auto lit = g_state.il ? g_layouts->find(g_state.il) : g_layouts->end();
    if (lit == g_layouts->end())
    {
        lb.Add(" no input-layout info (il=%p)", g_state.il);
        Log("%s", lb.s);
        return;
    }
    const LayoutInfo& li = lit->second;
    lb.Add(" il=%u", li.id);
    if (!li.hasPos)
    {
        lb.Add(" no POSITION/SV_Position element");
        Log("%s", lb.s);
        return;
    }
    lb.Add(" fmt=%u", static_cast<UINT>(li.posFmt));
    const UINT need = PosFormatBytes(li.posFmt);
    if (!need)
    {
        lb.Add(" unsupported fmt");
        Log("%s", lb.s);
        return;
    }
    if (!li.posOffsetKnown || li.posSlot >= kMaxVbSlots)
    {
        lb.Add(" POSITION offset unknown or IA slot %u not tracked", li.posSlot);
        Log("%s", lb.s);
        return;
    }
    const VbBinding& vb = g_state.vb[li.posSlot];
    if (!vb.buf)
    {
        lb.Add(" no vertex buffer in IA slot %u", li.posSlot);
        Log("%s", lb.s);
        return;
    }
    auto vit = g_vbShadows->find(vb.buf);
    if (vit == g_vbShadows->end() || vit->second.bytes.empty())
    {
        lb.Add(" vb not shadowed (usage=%u, bw=%u)", vb.usage, vb.bw);
        Log("%s", lb.s);
        return;
    }
    const std::vector<unsigned char>& vbytes = vit->second.bytes;

    const std::vector<unsigned char>* ibytes = nullptr;
    UINT isz = 0;
    if (indexed)
    {
        const IbBinding& ib = g_state.ib;
        if (!ib.buf)
        {
            lb.Add(" no index buffer bound");
            Log("%s", lb.s);
            return;
        }
        isz = (ib.fmt == DXGI_FORMAT_R16_UINT) ? 2u : (ib.fmt == DXGI_FORMAT_R32_UINT) ? 4u : 0u;
        if (!isz)
        {
            lb.Add(" ib fmt=%u unsupported", static_cast<UINT>(ib.fmt));
            Log("%s", lb.s);
            return;
        }
        auto iit = g_vbShadows->find(ib.buf);
        if (iit == g_vbShadows->end() || iit->second.bytes.empty())
        {
            lb.Add(" ib not shadowed (usage=%u, bw=%u)", ib.usage, ib.bw);
            Log("%s", lb.s);
            return;
        }
        ibytes = &iit->second.bytes;
    }

    // VS-CB slot 0 matrix (c0..c3) from the CB shadow.
    float M[4][4] = {};
    bool haveM = false;
    if (g_state.vscb[0].buf && g_shadows)
    {
        auto sit = g_shadows->find(g_state.vscb[0].buf);
        if (sit != g_shadows->end() && sit->second.bytes.size() >= sizeof(M))
        {
            memcpy(M, sit->second.bytes.data(), sizeof(M));
            haveM = true;
        }
    }

    float omin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float omax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    float first[4][3] = {};
    float nmin[2][2] = { { FLT_MAX, FLT_MAX }, { FLT_MAX, FLT_MAX } };
    float nmax[2][2] = { { -FLT_MAX, -FLT_MAX }, { -FLT_MAX, -FLT_MAX } };
    UINT nproj[2] = {}, badW[2] = {};
    UINT nv = 0, oob = 0;

    for (UINT i = 0; i < d.count; ++i)
    {
        uint64_t vidx = 0;
        if (indexed)
        {
            const uint64_t ioff =
                static_cast<uint64_t>(g_state.ib.offset) + (static_cast<uint64_t>(d.start) + i) * isz;
            if (ioff + isz > ibytes->size())
            {
                ++oob;
                continue;
            }
            uint32_t iv = 0;
            memcpy(&iv, ibytes->data() + static_cast<size_t>(ioff), isz); // little endian
            const int64_t v = static_cast<int64_t>(iv) + d.base;
            if (v < 0)
            {
                ++oob;
                continue;
            }
            vidx = static_cast<uint64_t>(v);
        }
        else
        {
            vidx = static_cast<uint64_t>(d.start) + i;
        }
        const uint64_t boff = static_cast<uint64_t>(vb.offset) + vidx * vb.stride + li.posOffset;
        if (boff + need > vbytes.size())
        {
            ++oob;
            continue;
        }
        float p[3];
        DecodePos(li.posFmt, vbytes.data() + static_cast<size_t>(boff), p);
        for (UINT k = 0; k < 3; ++k)
        {
            omin[k] = Min(omin[k], p[k]);
            omax[k] = Max(omax[k], p[k]);
        }
        if (nv < 4)
            memcpy(first[nv], p, sizeof(p));
        ++nv;

        if (!haveM)
            continue;
        float clip[2][4];
        for (UINT j = 0; j < 4; ++j)
        {
            clip[0][j] = p[0] * M[0][j] + p[1] * M[1][j] + p[2] * M[2][j] + M[3][j]; // row vector
            clip[1][j] = M[j][0] * p[0] + M[j][1] * p[1] + M[j][2] * p[2] + M[j][3]; // column vector
        }
        for (UINT c = 0; c < 2; ++c)
        {
            const float w = clip[c][3];
            if (fabsf(w) < 1e-20f)
            {
                ++badW[c];
                continue;
            }
            const float nx = clip[c][0] / w;
            const float ny = clip[c][1] / w;
            nmin[c][0] = Min(nmin[c][0], nx);
            nmin[c][1] = Min(nmin[c][1], ny);
            nmax[c][0] = Max(nmax[c][0], nx);
            nmax[c][1] = Max(nmax[c][1], ny);
            ++nproj[c];
        }
    }

    lb.Add(" vcount=%u decoded=%u oob=%u", d.count, nv, oob);
    if (nv)
    {
        lb.Add(" obj min=(%.5g,%.5g,%.5g) max=(%.5g,%.5g,%.5g) first=", static_cast<double>(omin[0]),
               static_cast<double>(omin[1]), static_cast<double>(omin[2]), static_cast<double>(omax[0]),
               static_cast<double>(omax[1]), static_cast<double>(omax[2]));
        for (UINT i = 0; i < Min<UINT>(nv, 4u); ++i)
            lb.Add("(%.5g,%.5g,%.5g)", static_cast<double>(first[i][0]), static_cast<double>(first[i][1]),
                   static_cast<double>(first[i][2]));
    }
    if (!haveM)
    {
        lb.Add(" s0 not shadowed - no projection");
    }
    else
    {
        static const char* const kConv[2] = { "Row", "Col" };
        for (UINT c = 0; c < 2; ++c)
        {
            if (!nproj[c])
            {
                lb.Add(" ndc%s=n/a(w=0 x%u)", kConv[c], badW[c]);
                continue;
            }
            const double x0 = vp.TopLeftX + (nmin[c][0] + 1.0) * 0.5 * vp.Width;
            const double x1 = vp.TopLeftX + (nmax[c][0] + 1.0) * 0.5 * vp.Width;
            const double y0 = vp.TopLeftY + (1.0 - nmax[c][1]) * 0.5 * vp.Height;
            const double y1 = vp.TopLeftY + (1.0 - nmin[c][1]) * 0.5 * vp.Height;
            lb.Add(" ndc%s=[%.4f,%.4f..%.4f,%.4f] px%s=[%.1f,%.1f..%.1f,%.1f]", kConv[c],
                   static_cast<double>(nmin[c][0]), static_cast<double>(nmin[c][1]), static_cast<double>(nmax[c][0]),
                   static_cast<double>(nmax[c][1]), kConv[c], x0, y0, x1, y1);
        }
    }
    Log("%s", lb.s);
}

// Every draw during a capture -> one [CAP-DRAW] line (no dedup), followed by
// the [CAP-CB] contents it references for the first time.
static void CaptureDraw(const DrawInfo& d)
{
    InterlockedIncrement(&g_capDraws);
    const LONG n = InterlockedIncrement(&g_capDrawInFrame) - 1;
    const LONG f = g_capFrame;
    if (!CapTakeLines(1))
        return;

    // Lock order g_stateLock -> g_logCs (no path takes them the other way round).
    AcquireSRWLockExclusive(&g_stateLock);
    if (g_capActive)
    {
        CbPrint todo[2 * kCBSlots];
        UINT ntodo = 0;
        const D3D11_VIEWPORT vp = g_state.hasVp ? g_state.vp0 : D3D11_VIEWPORT{};
        const D3D11_RECT sc = g_state.hasSc ? g_state.sc0 : D3D11_RECT{};

        LineBuf lb;
        lb.Add("[CAP-DRAW] t=%lu f=%ld n=%ld type=%s idx/vtx=%u inst=%u start=%u base=%d startInst=%u", CapTime(), f, n,
               kDrawNames[d.type < 7 ? d.type : 0], d.count, d.instances, d.start, d.base, d.startInst);
        if (d.args)
            lb.Add(" args=%p+%u", d.args, d.argsOff);
        lb.Add(" vs=%p ps=%p topo=%u vp=%.1f,%.1f,%.1f,%.1f,%.3f,%.3f sc=%ld,%ld,%ld,%ld", g_state.vs, g_state.ps,
               g_state.topo, static_cast<double>(vp.TopLeftX), static_cast<double>(vp.TopLeftY),
               static_cast<double>(vp.Width), static_cast<double>(vp.Height), static_cast<double>(vp.MinDepth),
               static_cast<double>(vp.MaxDepth), sc.left, sc.top, sc.right, sc.bottom);
        uint64_t vsh = 0, psh = 0;
        UINT ilId = 0;
        if (g_shaders)
        {
            auto it = g_state.vs ? g_shaders->find(g_state.vs) : g_shaders->end();
            if (it != g_shaders->end())
                vsh = it->second.hash;
            it = g_state.ps ? g_shaders->find(g_state.ps) : g_shaders->end();
            if (it != g_shaders->end())
                psh = it->second.hash;
        }
        if (g_layouts && g_state.il)
        {
            auto it = g_layouts->find(g_state.il);
            if (it != g_layouts->end())
                ilId = it->second.id;
        }
        lb.Add(" fix=%u vsh=%016llX psh=%016llX il=%u ib=%u/%u vb0=%u/%u", d.fixed, vsh, psh, ilId,
               g_state.ib.buf ? static_cast<UINT>(g_state.ib.fmt) : 0u, g_state.ib.offset, g_state.vb[0].stride,
               g_state.vb[0].offset);
        AppendCbListLocked(lb, "vscb", g_state.vscb, todo, ntodo);
        AppendCbListLocked(lb, "pscb", g_state.pscb, todo, ntodo);

        EnterCriticalSection(&g_logCs);
        Log("%s", lb.s);
        for (UINT i = 0; i < ntodo; ++i)
        {
            auto it = g_shadows->find(todo[i].buf);
            if (it == g_shadows->end() || it->second.hash != todo[i].hash)
                continue;
            const Shadow& s = it->second;
            const UINT bytes = static_cast<UINT>(s.bytes.size());
            if (!CapTakeLines(1u + (bytes + 15u) / 16u))
                break;
            Log("[CAP-CB] buf=%p bw=%u hash=%016llX bytes=%u", todo[i].buf, s.bw, s.hash, bytes);
            LogFloatRows(s.bytes.data(), bytes);
        }
        LogCapVtxLocked(d, n, vp);
        LeaveCriticalSection(&g_logCs);
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

// =============================================================================
//  HUD correction (multiplayer-safe, non-destructive)
//
//  Draws whose vertex shader bytecode hash is listed in HudVsHashes are
//  classified by their VS-CB slot-0 matrix (192 bytes, mirrored from the
//  game's Map/Unmap/UpdateSubresource): layer w = c3.w, anchor = c3.xy / w.
//  If a fix rule applies (see the rule engine below), a corrected copy of the
//  matrix (c0..c3 and the copy in c4..c7) is written into OUR OWN dynamic
//  constant buffer, bound to VS slot 0 for exactly this one draw, and the
//  game's buffer is re-bound afterwards. Game memory is never modified.
//  Our Map/Unmap/VSSetConstantBuffers calls go straight to the originals, so
//  they never show up in our own tracking.
// =============================================================================
static const UINT kFixCBBytes   = 192;
static const UINT kFixShadows   = 64;
static const UINT kFixOpenMaps  = 16;
static const UINT kFixLogCap    = 512;

struct FixShadow
{
    const void*   buf;
    volatile LONG stamp; // LRU
    bool          valid;
    float         c[kFixCBBytes / 4];
};
struct FixOpenMap
{
    const void* res;
    const void* buf;
    const void* data;       // pointer the game writes to (our scratch if redirected)
    void*       real;       // v1.4.4: driver pointer when redirected, else nullptr
    bool        redirected; // v1.4.4: game writes into g_fixScratch[slot]
    const ID3D11DeviceContext* ctx; // v1.4.6: context of the Map (identity only, no AddRef)
    LONG        frame;      // v1.4.6: g_frameCount at Map time (a pair never spans a Present)
};

static volatile LONG g_fixRuntime   = 0; // EnableFix at start, toggled by ToggleHotkey
static SRWLOCK       g_fixLock      = SRWLOCK_INIT;
static FixShadow     g_fixShadows[kFixShadows] = {};
static FixOpenMap    g_fixMaps[kFixOpenMaps] = {};
// v1.4.4 performance fix: WRITE_DISCARD maps of a HUD-set CB get a cached scratch
// block as pData; Unmap copies it to the driver pointer (write-only) and into
// the shadow. Reading the driver pointer back (write-combined memory) cost
// ~5 us per copy, ~10 ms per frame in busy scenes. Static, never grows.
// v1.4.6: each slot is followed by a 64-byte guard (pattern checked at Unmap;
// stride 256 keeps every data start 64-byte aligned).
static const UINT  kScratchGuard   = 64;
static const DWORD kScratchGuardDw = 0xFDFDFDFDu;
alignas(64) static unsigned char g_fixScratch[kFixOpenMaps][kFixCBBytes + kScratchGuard] = {};
static volatile LONG g_fixOpenCount = 0; // open entries in g_fixMaps (lock-free Unmap early-out)
static LONG          g_redirCnt = 0;     // diagnostics: redirected maps (bench counter)
static volatile LONG g_fixStaleDrops = 0;      // v1.4.6: open-map entries dropped without copy-back
static volatile LONG g_scratchGuardHits = 0;   // v1.4.6: scratch guard found overwritten
static volatile LONG g_scratchGuardLogged = 0;

static void ScratchGuardInit()
{
    for (UINT i = 0; i < kFixOpenMaps; ++i)
        memset(&g_fixScratch[i][kFixCBBytes], 0xFD, kScratchGuard);
}
static volatile LONG g_fixStampCounter = 0;
static volatile LONG g_fixCount     = 0;  // corrected draws since the last stats line
static volatile LONG g_fixTotal     = 0;  // corrected draws (session, for the menu rate)
static volatile LONG g_backdropCount = 0; // backdrop fixes since the last stats line
static volatile LONG g_backdropTotal = 0; // backdrop fixes (session, for the menu rate)
static volatile LONG g_backdropLoggedMove = 0;
static volatile LONG g_backdropLoggedHide = 0;
static volatile LONG g_promptCount = 0;  // prompt fixes since the last stats line
static volatile LONG g_promptTotal = 0;  // prompt fixes (session, for the menu rate)
static volatile LONG g_promptLogged = 0;
static volatile LONG g_poCount = 0;   // popup-origin fixes since the last stats line
static volatile LONG g_poTotal = 0;   // popup-origin fixes (session, for the menu rate)
static volatile LONG g_poLogged = 0;
static volatile LONG g_salvageCount = 0;  // salvage fixes since the last stats line (v1.3.1)
static volatile LONG g_salvageTotal = 0;  // salvage fixes (session, for the menu rate)
static volatile LONG g_salvageLogged = 0;
static DWORD         g_fixStatTick  = 0;
static volatile LONG g_fixLastPoll  = 0;
static bool          g_fixKeyWasDown = false;
static uint64_t      g_fixLogKeys[1024] = {};
static UINT          g_fixLogKeyCount = 0;

static bool IsHudVsHash(uint64_t h)
{
    if (!h)
        return false;
    for (UINT i = 0; i < g_cfg.hudVsCount; ++i)
    {
        if (g_cfg.hudVs[i] == h)
            return true;
    }
    return false;
}

// Called with g_fixLock held exclusively.
static FixShadow* FixShadowSlotLocked(const void* buf)
{
    FixShadow* lru = &g_fixShadows[0];
    for (UINT i = 0; i < kFixShadows; ++i)
    {
        FixShadow& s = g_fixShadows[i];
        if (s.buf == buf)
            return &s;
        if (!s.buf)
            lru = &s; // prefer an empty slot
        else if (lru->buf && s.stamp < lru->stamp)
            lru = &s;
    }
    lru->buf = buf;
    lru->valid = false;
    return lru;
}

static void FixShadowStore(const void* buf, const void* src)
{
    ++g_shadowCnt;
    AcquireSRWLockExclusive(&g_fixLock);
    FixShadow* s = FixShadowSlotLocked(buf);
    s->valid = SafeCopy(s->c, src, kFixCBBytes);
    InterlockedExchange(&s->stamp, InterlockedIncrement(&g_fixStampCounter));
    ReleaseSRWLockExclusive(&g_fixLock);
}

static bool FixShadowGet(const void* buf, float* out)
{
    bool ok = false;
    AcquireSRWLockShared(&g_fixLock);
    for (UINT i = 0; i < kFixShadows; ++i)
    {
        FixShadow& s = g_fixShadows[i];
        if (s.buf == buf)
        {
            if (s.valid)
            {
                memcpy(out, s.c, kFixCBBytes);
                InterlockedExchange(&s.stamp, InterlockedIncrement(&g_fixStampCounter));
                ok = true;
            }
            break;
        }
    }
    ReleaseSRWLockShared(&g_fixLock);
    return ok;
}

// Called with g_fixLock held exclusively.
static void FixShadowInvalidateLocked(const void* buf)
{
    for (UINT i = 0; i < kFixShadows; ++i)
    {
        if (g_fixShadows[i].buf == buf)
        {
            g_fixShadows[i].valid = false;
            break;
        }
    }
}

// v1.4.6: drops all open-map entries WITHOUT copy-back (HUD set cleared, perf
// mode change) and invalidates their shadows. Map/Unmap pairs never span a
// Present, so no legitimately open map is affected.
static void FixMapsReset()
{
    AcquireSRWLockExclusive(&g_fixLock);
    for (UINT i = 0; i < kFixOpenMaps; ++i)
    {
        if (g_fixMaps[i].res)
        {
            FixShadowInvalidateLocked(g_fixMaps[i].buf);
            g_fixMaps[i] = FixOpenMap{};
        }
    }
    InterlockedExchange(&g_fixOpenCount, 0);
    ReleaseSRWLockExclusive(&g_fixLock);
}

// "HUD buffer set": buffers that were bound to VS slot 0 for a HUD draw. Only
// these are mirrored (pointer compare first - no QI for other buffers). The
// first HUD draw of a new buffer therefore stays uncorrected (no shadow yet).
static const UINT kHudSetSize = 16;
static const void* volatile g_hudSet[kHudSetSize] = {};
static volatile LONG g_hudSetNext = 0;
static volatile LONG g_hudSetUsed = 0;

static bool HudSetContains(const void* p)
{
    for (UINT i = 0; i < kHudSetSize; ++i)
    {
        if (g_hudSet[i] == p)
            return true;
    }
    return false;
}

static void HudSetAdd(const void* p)
{
    if (!p || HudSetContains(p))
        return;
    const ULONG slot = static_cast<ULONG>(InterlockedIncrement(&g_hudSetNext) - 1) % kHudSetSize;
    g_hudSet[slot] = p;
    if (g_hudSetUsed < static_cast<LONG>(kHudSetSize))
    {
        const LONG n = InterlockedIncrement(&g_hudSetUsed);
        Log("[PERF] hud buffer set: %ld entries", n);
    }
}

static void HudSetClear()
{
    for (UINT i = 0; i < kHudSetSize; ++i)
        g_hudSet[i] = nullptr;
    InterlockedExchange(&g_hudSetNext, 0);
    InterlockedExchange(&g_hudSetUsed, 0);
    FixMapsReset(); // v1.4.6
}

// True for a 192-byte constant buffer; *buf receives its identity.
static bool IsFixCB(ID3D11Resource* res, const void** buf)
{
    D3D11_BUFFER_DESC d = {};
    const void* b = QueryBuffer(res, &d);
    if (!b || d.ByteWidth != kFixCBBytes || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
        return false;
    *buf = b;
    return true;
}

// Map hook (after the original): remember mapped 192-byte CBs.
static void FixShadowInvalidate(const void* buf);
static bool PerfWantHook(LONG mode, UINT idx);

// canRedirect: our Unmap hook is installed on this context (else never redirect,
// the game's data would not reach the driver).
// v1.4.6: entries carry context + frame; an entry still open from an earlier
// frame (its Unmap bypassed us) or re-mapped without an Unmap is dropped
// without copy-back and its shadow invalidated (g_fixStaleDrops).
static void FixOnMap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP mapType,
                     D3D11_MAPPED_SUBRESOURCE* m, bool canRedirect)
{
    if (!res || sub != 0 || !m || !m->pData)
        return;
    if (!CaptureActive() && !HudSetContains(res)) // same object: ID3D11Buffer* == ID3D11Resource*
        return;
    const void* buf = nullptr;
    if (!IsFixCB(res, &buf))
        return;
    ++g_mapHitCnt;
    const LONG frame = g_frameCount;
    LONG stale = 0;
    AcquireSRWLockExclusive(&g_fixLock);
    FixOpenMap* target = nullptr;
    UINT slot = 0;
    bool same = false;
    for (UINT i = 0; i < kFixOpenMaps; ++i)
    {
        if (g_fixMaps[i].res == res)
        {
            target = &g_fixMaps[i];
            slot = i;
            same = true;
            break;
        }
        if (!target && !g_fixMaps[i].res)
        {
            target = &g_fixMaps[i];
            slot = i;
        }
    }
    if (same)
    {
        // Mapped again without an Unmap through our hook: the old entry is stale.
        FixShadowInvalidateLocked(target->buf);
        ++stale;
    }
    else if (!target)
    {
        // Table full: reclaim an entry left over from an earlier frame.
        for (UINT i = 0; i < kFixOpenMaps; ++i)
        {
            if (g_fixMaps[i].frame != frame)
            {
                target = &g_fixMaps[i];
                slot = i;
                FixShadowInvalidateLocked(target->buf);
                ++stale;
                break;
            }
        }
    }
    if (target)
    {
        if (!target->res)
            InterlockedIncrement(&g_fixOpenCount);
        target->res = res;
        target->buf = buf;
        target->real = nullptr;
        target->redirected = false;
        target->ctx = ctx;
        target->frame = frame;
        if (canRedirect && mapType == D3D11_MAP_WRITE_DISCARD && !CaptureActive())
        {
            target->real = m->pData;
            target->redirected = true;
            m->pData = g_fixScratch[slot]; // the game writes into cached memory
            ++g_redirCnt;
        }
        target->data = m->pData;
    }
    ReleaseSRWLockExclusive(&g_fixLock);
    if (stale)
        InterlockedExchangeAdd(&g_fixStaleDrops, stale);
    if (!target)
        FixShadowInvalidate(buf); // no slot: never keep a stale matrix
}

// v1.4.6: rare path - scratch guard of slot overwritten (diagnostic only).
static __declspec(noinline) void ScratchGuardReport(UINT slot)
{
    InterlockedIncrement(&g_scratchGuardHits);
    if (InterlockedExchange(&g_scratchGuardLogged, 1) != 0)
        return;
    EventPush("[GUARD] scratch overrun slot %u", slot);
    if (LoggingActive())
        Log("[GUARD] scratch overrun slot %u (game wrote past 192 bytes of a redirected map; further hits are counted "
            "only)",
            slot);
}

// Unmap hook (BEFORE the original): copy the 192 bytes into the shadow.
// v1.4.4: also the copy-back of redirected maps - must run for every Unmap
// that reaches our hook (overlay, fix off, shutdown included).
// v1.4.6: copy-back only if the entry belongs to this context and this frame.
static void FixOnUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub)
{
    if (!res || sub != 0 || g_fixOpenCount == 0)
        return;
    FixOpenMap om = {};
    bool stale = false;
    int guardSlot = -1;
    alignas(64) unsigned char tmp[kFixCBBytes];
    const LONG frame = g_frameCount;
    AcquireSRWLockExclusive(&g_fixLock);
    for (UINT i = 0; i < kFixOpenMaps; ++i)
    {
        if (g_fixMaps[i].res == res)
        {
            om = g_fixMaps[i];
            stale = (om.ctx != ctx || om.frame != frame);
            if (om.redirected)
            {
                if (!stale)
                    memcpy(tmp, g_fixScratch[i], kFixCBBytes);
                unsigned char* guard = &g_fixScratch[i][kFixCBBytes];
                if (*reinterpret_cast<const DWORD*>(guard) != kScratchGuardDw)
                {
                    memset(guard, 0xFD, kScratchGuard); // restore the pattern
                    guardSlot = static_cast<int>(i);
                }
            }
            if (stale)
                FixShadowInvalidateLocked(om.buf);
            g_fixMaps[i] = FixOpenMap{};
            InterlockedDecrement(&g_fixOpenCount);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_fixLock);
    if (!om.res)
        return;
    if (guardSlot >= 0)
        ScratchGuardReport(static_cast<UINT>(guardSlot));
    if (stale)
    {
        InterlockedIncrement(&g_fixStaleDrops); // never write into `real` of a foreign/old map
        return;
    }
    if (om.redirected)
    {
        memcpy(om.real, tmp, kFixCBBytes); // write-only into the driver's memory
        FixShadowStore(om.buf, tmp);
    }
    else
    {
        FixShadowStore(om.buf, om.data); // old path (capture, other map types)
    }
}

static void FixShadowInvalidate(const void* buf)
{
    AcquireSRWLockExclusive(&g_fixLock);
    FixShadowInvalidateLocked(buf);
    ReleaseSRWLockExclusive(&g_fixLock);
}

// v1.4.6 crash report part 2 (called from CrashVeh, no locks, no heap):
// address space at crash time and the map-redirect / re-arm counters.
static void CrashReportExtra()
{
    AddrSpaceInfo as;
    AddrSpaceWalk(&as);
    const double mb = 1024.0 * 1024.0;
    CrashLine("[CRASH] address space: committed %.0f MB, reserved %.0f MB, free %.0f MB, largest free block %.0f MB",
              static_cast<double>(as.committed) / mb, static_cast<double>(as.reserved) / mb,
              static_cast<double>(as.freeTotal) / mb, static_cast<double>(as.largestFree) / mb);
    CrashLine("[CRASH] fix: stale map drops %ld, scratch guard hits %ld, open maps %ld, frame %ld, perf mode %ld",
              static_cast<LONG>(g_fixStaleDrops), static_cast<LONG>(g_scratchGuardHits),
              static_cast<LONG>(g_fixOpenCount), static_cast<LONG>(g_frameCount), static_cast<LONG>(g_perfMode));
    CrashLine("[CRASH] re-arms: Map %ld, Unmap %ld, UpdateSubresource %ld, draw %ld/%ld/%ld/%ld, foreign left alone %ld",
              static_cast<LONG>(g_rearmCount[VT_Map]), static_cast<LONG>(g_rearmCount[VT_Unmap]),
              static_cast<LONG>(g_rearmCount[VT_UpdateSubresource]), static_cast<LONG>(g_rearmCount[VT_DrawIndexed]),
              static_cast<LONG>(g_rearmCount[VT_Draw]), static_cast<LONG>(g_rearmCount[VT_DrawIndexedInstanced]),
              static_cast<LONG>(g_rearmCount[VT_DrawInstanced]), static_cast<LONG>(g_rearmForeign));
}

// UpdateSubresource hook (BEFORE the original): whole-buffer updates only.
static void FixOnUpdate(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* src)
{
    if (!res || sub != 0 || box || !src)
        return;
    if (!CaptureActive() && !HudSetContains(res))
        return;
    const void* buf = nullptr;
    if (IsFixCB(res, &buf))
        FixShadowStore(buf, src);
}

// VSSetShader hook: remember whether the bound VS is a HUD shader.
static void FixTrackVS(ID3D11DeviceContext* ctx, const void* vs)
{
    HookedTable* t = FindTable(*reinterpret_cast<void***>(ctx));
    if (!t)
        return;
    uint64_t h = 0;
    if (vs && g_shaders)
    {
        AcquireSRWLockShared(&g_stateLock);
        auto it = g_shaders->find(vs);
        if (it != g_shaders->end())
            h = it->second.hash;
        ReleaseSRWLockShared(&g_stateLock);
    }
    t->vsHash = h;
    t->hudVs = IsHudVsHash(h);
}

// PSSetShader hook: remember the bytecode hash of the bound PS.
static void FixTrackPS(ID3D11DeviceContext* ctx, const void* ps)
{
    HookedTable* t = FindTable(*reinterpret_cast<void***>(ctx));
    if (!t)
        return;
    uint64_t h = 0;
    if (ps && g_shaders)
    {
        AcquireSRWLockShared(&g_stateLock);
        auto it = g_shaders->find(ps);
        if (it != g_shaders->end())
            h = it->second.hash;
        ReleaseSRWLockShared(&g_stateLock);
    }
    t->psHash = h;
}

// VSSetConstantBuffers hook: remember the game buffer in VS slot 0.
static void FixTrackS0(ID3D11DeviceContext* ctx, UINT startSlot, UINT num, ID3D11Buffer* const* pp)
{
    if (startSlot != 0 || num == 0)
        return;
    HookedTable* t = FindTable(*reinterpret_cast<void***>(ctx));
    if (t)
        t->vsS0 = pp ? pp[0] : nullptr;
}

// Dedup for "[FIX] applied": (vsHash, anchor rounded to 0.01). No heap.
static bool FixLogFirstTime(uint64_t vsHash, float ax, float ay)
{
    Fnv f;
    f.Val(vsHash);
    const int rx = static_cast<int>(floorf(ax * 100.0f + 0.5f));
    const int ry = static_cast<int>(floorf(ay * 100.0f + 0.5f));
    f.Val(rx);
    f.Val(ry);
    const uint64_t key = f.h | 1ull;
    bool isNew = false;
    AcquireSRWLockExclusive(&g_fixLock);
    UINT pos = static_cast<UINT>((key * 0x9E3779B97F4A7C15ull) >> 54); // 0..1023
    for (;;)
    {
        if (g_fixLogKeys[pos] == key)
            break;
        if (g_fixLogKeys[pos] == 0)
        {
            if (g_fixLogKeyCount < kFixLogCap) // table never more than half full
            {
                g_fixLogKeys[pos] = key;
                ++g_fixLogKeyCount;
                isNew = true;
            }
            break;
        }
        pos = (pos + 1u) & 1023u;
    }
    ReleaseSRWLockExclusive(&g_fixLock);
    return isNew;
}

// Column operation on a 4x4 row-vector matrix (rows c0..c3, clip = [x y z 1]*M):
// column[col] = scale*column[col] + addPerW*column[3], i.e.
// clip.col' = scale*clip.col + addPerW*clip.w for every vertex.
static void ColAffine(float* m, UINT col, float scale, float addPerW)
{
    for (UINT r = 0; r < 4; ++r)
        m[r * 4 + col] = scale * m[r * 4 + col] + addPerW * m[r * 4 + 3];
}

// Snapshot of the live fix parameters (menu writes single 32-bit floats).
struct FixParams
{
    float shiftX, shiftY, popupScale, pivotX, pivotY;
};

// Popup correction for one 16-float matrix: shift, then scale around the pivot.
// Applied to popup draws only - the HUD shaders also draw world-anchored
// markers, which must stay on their objects.
static void ApplyPopupXform(float* m, const FixParams& p)
{
    ColAffine(m, 0, 1.0f, p.shiftX);
    ColAffine(m, 1, 1.0f, p.shiftY);
    if (p.popupScale != 1.0f)
    {
        ColAffine(m, 0, p.popupScale, (1.0f - p.popupScale) * p.pivotX);
        ColAffine(m, 1, p.popupScale, (1.0f - p.popupScale) * p.pivotY);
    }
}

// Watchdog facts of the current draw, recorded by FixBeforeDraw only while
// logging is on: matrix (c0..c3) as drawn - corrected if a rule applied.
static thread_local float t_drawM[16] = {};
static thread_local bool  t_drawHave = false;
static thread_local bool  t_drawGated = false; // menu gate active for this draw
static thread_local bool  t_drawSmall = false; // small screen element

// HUD inventory (diagnostics): per-draw facts recorded by FixBeforeDraw,
// consumed by InvDraw (after the draw, where the index/vertex count is known).
struct InvDrawInfo
{
    uint64_t vsh;   // VS / PS bytecode hashes of the draw
    uint64_t psh;
    bool  have;     // gameplay HUD/prompt-layer draw with a slot-0 shadow
    char  layer;    // 'H' (HUD layer) or 'P' (prompt layer)
    bool  gated;    // menu gate active for this draw (always false: only gameplay draws are recorded)
    float w;        // layer w (c3.w) as sent by the game
    char  rule;     // 0 or the applied rule: U u O S P B
    bool  hidden;   // rule B in hide mode (not drawn)
    float ax, ay;   // anchor as sent by the game
    float sx, sy;   // scale as sent by the game
    float dax, day; // anchor as drawn (after correction)
};
static thread_local InvDrawInfo t_inv = {};

// Return values of FixBeforeDraw.
enum FixResult
{
    FIX_NONE = 0, // draw normally
    FIX_SWAP = 1, // our CB is bound to VS slot 0, call FixAfterDraw afterwards
    FIX_SKIP = 2  // do not call the original draw at all (backdrop hide mode)
};

// =============================================================================
//  Rule engine: menu gate + gameplay-only rules (spec: analysis\rulesim3.py)
//
//  Per context and frame (frame = Present counter): count draws before the
//  first HUD-VS draw, detect menus (G1/G2/G3) and keep at most one open
//  "group" (U = bottom-right popup, O = popup left at the stage origin,
//  S = salvage counter left at the top left). Rules: P prompt, U/u popup
//  (group / carry-over), O popup origin, S salvage, B backdrop.
//  Menus are never modified: while the gate is active no rule applies.
// =============================================================================
static const uint64_t kVsQuad = 0x8FE65884BC7EA6DEull; // HUD quads
static const uint64_t kVsText = 0x15A962EB1FFFCD0Dull; // non-indexed text (exempt from G3)

static volatile LONG g_gameplayFrames = 0;   // frames that passed the gate (session)
static bool          g_hudSeenGameplay = false;
static bool          g_hudWarned = false;
static bool          g_gateMenuNow = false;  // last finished frame (menu INFO)
static char          g_gateReasonNow = 0;
static DWORD         g_gateLogTick = 0;
static LONG          g_gateLogSuppressed = 0;

static void GateSetMenu(HookedTable* t, char reason)
{
    if (!t->menuThis)
        t->menuReason = reason;
    t->menuThis = true;
}

// Called at the first draw of a new frame for the previous frame of this context.
static void GateFinishFrame(HookedTable* t)
{
    if (t->gateFrame < 0)
        return; // first frame ever
    const bool menuFinal = t->menuThis || (!t->seenHud && t->drawsBefore < g_cfg.menuMinSceneDraws);
    const char reason = t->menuThis ? t->menuReason : static_cast<char>(menuFinal ? '1' : '\0');
    const bool effective = menuFinal || t->menuHold > 0; // state the finished frame was gated with
    // Hold: keep the menu state for MenuHoldFrames frames after the last menu signal.
    if (menuFinal)
        t->menuHold = Max(1, g_cfg.menuHoldFrames);
    else if (t->menuHold > 0)
        --t->menuHold;

    g_gateMenuNow = effective;
    g_gateReasonNow = effective ? (reason ? reason : t->lastReason) : '\0';
    if (menuFinal)
        t->lastReason = reason;

    // Diagnostics only from here on.
    if (!LoggingActive())
        return;
    if (!menuFinal && t->seenHud)
    {
        const LONG gf = InterlockedIncrement(&g_gameplayFrames);
        if (gf >= 600 && !g_hudSeenGameplay && !g_hudWarned)
        {
            g_hudWarned = true;
            Log("[WATCH] no gameplay draws at HudLayerW seen in 600 frames - layer constants may not match this "
                "resolution");
        }
    }

    if (effective != t->gateLogged)
    {
        t->gateLogged = effective;
        const DWORD now = GetTickCount();
        if (!g_gateLogTick || now - g_gateLogTick >= 500u)
        {
            if (effective)
                Log("[GATE] menu on (reason G%c, frame %ld)%s", g_gateReasonNow ? g_gateReasonNow : '?',
                    static_cast<LONG>(t->gateFrame), g_gateLogSuppressed ? " (+ suppressed changes)" : "");
            else
                Log("[GATE] menu off (frame %ld)%s", static_cast<LONG>(t->gateFrame),
                    g_gateLogSuppressed ? " (+ suppressed changes)" : "");
            if (g_gateLogSuppressed)
                Log("[GATE] %ld state changes suppressed (debounce 0.5 s)", g_gateLogSuppressed);
            g_gateLogTick = now | 1u;
            g_gateLogSuppressed = 0;
        }
        else
        {
            ++g_gateLogSuppressed;
        }
    }
}

// Every draw hook (before the draw): frame switch + draws-before-HUD counter.
static void GateBeginDraw(ID3D11DeviceContext* This)
{
    HookedTable* t = FindTable(*reinterpret_cast<void***>(This));
    if (!t)
        return;
    const LONG frame = g_frameCount;
    if (t->gateFrame != frame)
    {
        GateFinishFrame(t);
        t->gateFrame = frame;
        t->drawsBefore = 0;
        t->seenHud = false;
        t->menuThis = false;
        t->menuReason = 0;
        t->grp = 0;
        // v1.3.1 popup carry-over: roll the U-trigger flags (cleared in menu frames by FixBeforeDraw)
        t->uTriggerPrevFrame = t->uTriggerThisFrame;
        t->uTriggerThisFrame = false;
    }
    if (!t->seenHud && !t->hudVs)
        ++t->drawsBefore;
}

// Before a HUD-VS draw: gate, then at most one rule. Returns FIX_SWAP (our CB
// bound, call FixAfterDraw), FIX_SKIP (do not draw) or FIX_NONE. Also records
// the matrix as drawn for the watchdog (t_drawM / t_drawHave / t_drawGated) and
// the [INV] facts (t_inv) - only while logging is on (diag).
static int FixBeforeDraw(ID3D11DeviceContext* This, ID3D11Buffer** prev, UINT indexCount)
{
    const bool diag = LoggingActive();
    if (diag)
    {
        t_drawHave = false;
        t_drawGated = false;
        t_drawSmall = false;
        t_inv.have = false;
    }
    HookedTable* t = FindTable(*reinterpret_cast<void***>(This));
    if (!t || !t->hudVs || !t->vsS0)
        return FIX_NONE;
    HudSetAdd(t->vsS0); // mirror this buffer from now on

    float c[kFixCBBytes / 4];
    if (!FixShadowGet(t->vsS0, c))
        return FIX_NONE;
    const float w = c[15];
    if (!(fabsf(w) > 1e-6f))
        return FIX_NONE;
    const float ax = c[12] / w;
    const float ay = c[13] / w;
    const float sx = c[0] / w;
    const float sy = c[5] / w;
    // Separate tolerances - the HUD layer w varies with the aspect ratio
    // (16:9 24627.0, 16:10 24619.3, 5:4 24623.1, 4:3 24636.6); menu layers are >= 29 away.
    const bool onHud = fabsf(w - g_cfg.hudLayerW) <= g_cfg.hudLayerTol;
    const bool onPrompt = fabsf(w - g_cfg.promptLayerW) <= g_cfg.promptLayerTol;
    const float maxScale = Max(fabsf(sx), fabsf(sy));
    const bool isSmall = maxScale < 0.0001f; // screen element (not a full-screen image)
    if (diag)
    {
        memcpy(t_drawM, c, sizeof(t_drawM)); // as drawn unless a rule applies below
        t_drawHave = true;
        t_drawSmall = isSmall;
    }

    // ---- A. menu gate (before all rules) ----
    if (!t->seenHud)
    {
        t->seenHud = true;
        if (t->drawsBefore < g_cfg.menuMinSceneDraws)
            GateSetMenu(t, '1'); // G1: hardly any 3D scene before the first HUD draw
    }
    if (w > 1000.0f && !onHud && !onPrompt)
        GateSetMenu(t, '2'); // G2: foreign Scaleform layer
    if (t->vsHash != kVsText && w > 1000.0f && maxScale >= g_cfg.menuLargeScale)
        GateSetMenu(t, '3'); // G3: full-screen-size image element
    const bool gatedNow = t->menuThis || t->menuHold > 0;
    // HUD inventory: remember this draw (diagnostics only, no effect on the rules).
    // Only gameplay draws (menu gate not active) on the HUD layer (H) or the
    // prompt layer (P); unknown/menu layers are never recorded.
    if (diag && !gatedNow && (onHud || onPrompt) && g_cfg.inventoryEnable)
    {
        t_inv.have = true;
        t_inv.vsh = t->vsHash;
        t_inv.psh = t->psHash;
        t_inv.layer = onHud ? 'H' : 'P';
        t_inv.gated = gatedNow;
        t_inv.w = w;
        t_inv.rule = 0;
        t_inv.hidden = false;
        t_inv.ax = ax;
        t_inv.ay = ay;
        t_inv.sx = sx;
        t_inv.sy = sy;
        t_inv.dax = ax;
        t_inv.day = ay;
    }
    if (gatedNow)
    {
        t->grp = 0;
        t->uTriggerThisFrame = false;
        t->uTriggerPrevFrame = false;
        if (diag)
            t_drawGated = true;
        return FIX_NONE; // menus are never modified
    }
    if (diag && onHud)
        g_hudSeenGameplay = true; // diagnostics ([WATCH] layer warning)
    if (!g_fixRuntime || !t->fixCB)
        return FIX_NONE; // fix off: gate/watchdog only

    // ---- B. gameplay rules, at most one per draw ----
    FixParams p = {};
    p.shiftX     = g_cfg.fixShiftNdcX;
    p.shiftY     = g_cfg.fixShiftNdcY;
    p.popupScale = g_cfg.popupScale;
    p.pivotX     = g_cfg.popupPivotX;
    p.pivotY     = g_cfg.popupPivotY;
    const bool popupParams = !(p.shiftX == 0.0f && p.shiftY == 0.0f && p.popupScale == 1.0f);
    const int bdMode = g_cfg.fixBackdrop;
    const bool stain = t->psHash == g_cfg.backdropPsHash && fabsf(ax + 0.99844f) < 0.003f && fabsf(ay - 1.0f) < 0.003f;
    const bool q6 = t->vsHash == kVsQuad && indexCount == 6;
    const bool poPs = g_cfg.popupOriginPsHash != 0 && t->psHash == g_cfg.popupOriginPsHash;

    // B2. close an open group when a draw leaves its window
    // v1.2.1: member windows widened for long multi-line texts (e.g. "tomb completed":
    // first text line anchors at ax 0.60 resp. -1.63). Validated on all captures
    // (analysis\rulesim3.py): only tomb popups gain members, 0 hits in menus/hint box.
    if (t->grp == 'U' && !(onHud && ax >= 0.40f && ay >= -0.93f && ay <= -0.3f))
        t->grp = 0;
    if (t->grp == 'S' && !(onHud && ax >= -1.7f && ax <= -0.7f && ay >= 0.7f && ay <= 1.25f && !stain))
        t->grp = 0;
    if (t->grp == 'O' && !(onHud && ax >= -2.8f && ax <= -0.3f && ay >= 0.9f && ay <= 3.0f && !stain))
        t->grp = 0;

    enum { R_NONE, R_PROMPT, R_POPUP, R_ORIGIN, R_BACKDROP, R_SALVAGE } rule = R_NONE;
    // B1. prompt layer ("Press [E]") left at the stage origin
    if (onPrompt && ay >= 0.95f && ay <= 1.06f && ax >= -2.6f && ax <= -0.9f)
    {
        if (g_cfg.fixPrompt)
            rule = R_PROMPT;
    }
    else
    {
        // B3/B4. group triggers: popup background quad (6 indices, PO pixel shader)
        if (t->grp == 0 && onHud && q6 && poPs && isSmall && ax >= 0.95f && ay >= -0.9f && ay <= -0.3f)
        {
            t->grp = 'U'; // bottom-right popup
            t->uTriggerThisFrame = true;
        }
        else if (t->grp == 0 && onHud && q6 && poPs && isSmall && ax >= -1.3f && ax <= -0.8f && ay >= 1.0f &&
                 ay <= 3.0f)
        {
            t->grp = 'O'; // popup left at the stage origin (top left)
        }
        // v1.3.1: salvage counter (glow quad, BD pixel shader) left at the top left
        else if (t->grp == 0 && onHud && q6 && t->psHash == g_cfg.backdropPsHash && isSmall && !stain &&
                 ax >= -0.90f && ax <= -0.77f && ay >= 1.15f && ay <= 1.26f)
        {
            t->grp = 'S';
        }
        // B5. group members
        if (t->grp == 'U')
        {
            if (popupParams)
                rule = R_POPUP;
        }
        else if (t->grp == 'O')
        {
            if (g_cfg.fixPopupOrigin)
                rule = R_ORIGIN;
        }
        else if (t->grp == 'S')
        {
            if (g_cfg.fixSalvage)
                rule = R_SALVAGE;
        }
        // v1.3.1 popup carry-over: popup parts drawn before the U trigger of this frame
        // (the trigger fired in the previous gameplay frame) get the U treatment, no group.
        else if (t->uTriggerPrevFrame && !t->uTriggerThisFrame && onHud && ax >= 0.40f && ay >= -0.93f &&
                 ay <= -0.3f)
        {
            if (popupParams)
                rule = R_POPUP;
        }
        // B6. backdrop "black stain"
        else if (bdMode != 0 && onHud && stain && isSmall)
        {
            rule = R_BACKDROP;
        }
    }
    if (rule == R_NONE)
        return FIX_NONE;
    // v1.3.2 inventory rule tag (U = popup group, u = popup carry-over without a group)
    const char invRule = (rule == R_PROMPT)    ? 'P'
                         : (rule == R_POPUP)   ? (t->grp == 'U' ? 'U' : 'u')
                         : (rule == R_ORIGIN)  ? 'O'
                         : (rule == R_SALVAGE) ? 'S'
                                               : 'B';

    if (rule == R_BACKDROP && bdMode == 2)
    {
        if (diag && t_inv.have)
        {
            t_inv.rule = invRule;
            t_inv.hidden = true;
        }
        InterlockedIncrement(&g_backdropTotal);
        if (diag)
            InterlockedIncrement(&g_backdropCount);
        if (diag && InterlockedExchange(&g_backdropLoggedHide, 1) == 0)
            Log("[FIX] backdrop hidden (vsh=%016llX psh=%016llX anchor=(%.4f,%.4f))", t->vsHash, t->psHash,
                static_cast<double>(ax), static_cast<double>(ay));
        return FIX_SKIP;
    }
    switch (rule)
    {
    case R_PROMPT:
        c[12] += g_cfg.promptShiftX * w;     // c3.x
        c[13] += g_cfg.promptShiftY * w;     // c3.y
        c[28] += g_cfg.promptShiftX * c[31]; // c7.x (copy)
        c[29] += g_cfg.promptShiftY * c[31]; // c7.y (copy)
        break;
    case R_POPUP:
        ApplyPopupXform(c, p);      // c0..c3
        ApplyPopupXform(c + 16, p); // c4..c7 (copy)
        break;
    case R_ORIGIN:
        c[12] += g_cfg.popupOriginShiftX * w;     // final position, no popup shift afterwards
        c[13] += g_cfg.popupOriginShiftY * w;
        c[28] += g_cfg.popupOriginShiftX * c[31];
        c[29] += g_cfg.popupOriginShiftY * c[31];
        break;
    case R_SALVAGE:
        c[12] += g_cfg.salvageShiftX * w;     // c3.x
        c[13] += g_cfg.salvageShiftY * w;     // c3.y
        c[28] += g_cfg.salvageShiftX * c[31]; // c7.x (copy)
        c[29] += g_cfg.salvageShiftY * c[31]; // c7.y (copy)
        break;
    default: // R_BACKDROP, move mode
        c[12] = g_cfg.backdropX * w;
        c[13] = g_cfg.backdropY * w;
        c[28] = g_cfg.backdropX * c[31];
        c[29] = g_cfg.backdropY * c[31];
        break;
    }

    const TRHF_PFN_Map map = TRHF_ORIG(TRHF_PFN_Map, VT_Map);
    const TRHF_PFN_Unmap unmap = TRHF_ORIG(TRHF_PFN_Unmap, VT_Unmap);
    const TRHF_PFN_SetConstantBuffers setCB = TRHF_ORIG(TRHF_PFN_SetConstantBuffers, VT_VSSetConstantBuffers);
    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (FAILED(map(This, t->fixCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)) || !ms.pData)
        return FIX_NONE;
    memcpy(ms.pData, c, kFixCBBytes);
    unmap(This, t->fixCB, 0);

    ID3D11Buffer* pv = nullptr;
    This->VSGetConstantBuffers(0, 1, &pv); // AddRef'd, released in FixAfterDraw
    ID3D11Buffer* ours = t->fixCB;
    setCB(This, 0, 1, &ours);
    *prev = pv;
    if (diag)
        memcpy(t_drawM, c, sizeof(t_drawM)); // corrected matrix is what gets drawn
    if (diag && t_inv.have)
    {
        t_inv.rule = invRule;
        if (fabsf(c[15]) > 1e-6f)
        {
            t_inv.dax = c[12] / c[15];
            t_inv.day = c[13] / c[15];
        }
    }

    // Menu rate counters (*Total) always; log statistics (*Count) and
    // first-time lines only with logging.
    switch (rule)
    {
    case R_PROMPT:
        InterlockedIncrement(&g_promptTotal);
        if (diag)
            InterlockedIncrement(&g_promptCount);
        if (diag && InterlockedExchange(&g_promptLogged, 1) == 0)
            Log("[FIX] prompt moved (vsh=%016llX psh=%016llX w=%.1f anchor=(%.4f,%.4f) -> (%.4f,%.4f))", t->vsHash,
                t->psHash, static_cast<double>(w), static_cast<double>(ax), static_cast<double>(ay),
                static_cast<double>(ax + g_cfg.promptShiftX), static_cast<double>(ay + g_cfg.promptShiftY));
        break;
    case R_ORIGIN:
        InterlockedIncrement(&g_poTotal);
        if (diag)
            InterlockedIncrement(&g_poCount);
        if (diag && InterlockedExchange(&g_poLogged, 1) == 0)
            Log("[FIX] popup-origin moved (vsh=%016llX psh=%016llX anchor=(%.4f,%.4f) -> (%.4f,%.4f))", t->vsHash,
                t->psHash, static_cast<double>(ax), static_cast<double>(ay),
                static_cast<double>(ax + g_cfg.popupOriginShiftX), static_cast<double>(ay + g_cfg.popupOriginShiftY));
        break;
    case R_SALVAGE:
        InterlockedIncrement(&g_salvageTotal);
        if (diag)
            InterlockedIncrement(&g_salvageCount);
        if (diag && InterlockedExchange(&g_salvageLogged, 1) == 0)
            Log("[FIX] salvage moved (vsh=%016llX psh=%016llX anchor=(%.4f,%.4f) -> (%.4f,%.4f))", t->vsHash,
                t->psHash, static_cast<double>(ax), static_cast<double>(ay),
                static_cast<double>(ax + g_cfg.salvageShiftX), static_cast<double>(ay + g_cfg.salvageShiftY));
        break;
    case R_BACKDROP:
        InterlockedIncrement(&g_backdropTotal);
        if (diag)
            InterlockedIncrement(&g_backdropCount);
        if (diag && InterlockedExchange(&g_backdropLoggedMove, 1) == 0)
            Log("[FIX] backdrop moved (vsh=%016llX psh=%016llX anchor=(%.4f,%.4f) -> (%.4f,%.4f))", t->vsHash,
                t->psHash, static_cast<double>(ax), static_cast<double>(ay), static_cast<double>(g_cfg.backdropX),
                static_cast<double>(g_cfg.backdropY));
        break;
    default: // R_POPUP
        InterlockedIncrement(&g_fixTotal);
        if (diag)
            InterlockedIncrement(&g_fixCount);
        if (diag && g_logVerbose && FixLogFirstTime(t->vsHash, ax, ay))
        {
            const float nw = c[15];
            const float nax = (fabsf(nw) > 1e-6f) ? c[12] / nw : 0.0f;
            const float nay = (fabsf(nw) > 1e-6f) ? c[13] / nw : 0.0f;
            Log("[FIX] applied vsh=%016llX anchor=(%.2f,%.2f) -> (%.3f,%.3f)", t->vsHash, static_cast<double>(ax),
                static_cast<double>(ay), static_cast<double>(nax), static_cast<double>(nay));
        }
        break;
    }
    return FIX_SWAP;
}

// =============================================================================
//  HUD anomaly watchdog (diagnostics, EnableLogging 1 or 2)
//
//  Looks at the matrix of every HUD-VS draw as it is actually drawn (after our
//  fix rules) and reports known bad positions:
//    A "origin": anchor exactly at the stage origin (-0.99844, 1.0), small
//                screen elements, any layer
//    OFF:        element mostly off-screen (sample frames, see below)
//  Draws we corrected are no anomaly, but are reported as "fix ineffective" if
//  the signature is still present. New uncorrected finds start an automatic
//  capture.
// =============================================================================
static const UINT kWatchCap = 512;
static uint64_t      g_watchKeys[1024] = {};
static UINT          g_watchKeyCount = 0;
static bool          g_watchCapLogged = false;
static SRWLOCK       g_watchLock = SRWLOCK_INIT;
static volatile LONG g_watchATotal = 0;    // new A finds (session)
static volatile LONG g_watchASummary = 0;  // new A finds since the last summary
static volatile LONG g_autoCapCount = 0;
static DWORD         g_autoCapLastTick = 0;
static DWORD         g_watchSummaryTick = 0;

// Returns true if the key was not seen before (fixed table, no heap).
static bool WatchFirstTime(uint64_t key, bool* capNow)
{
    key |= 1ull;
    bool isNew = false;
    AcquireSRWLockExclusive(&g_watchLock);
    UINT pos = static_cast<UINT>((key * 0x9E3779B97F4A7C15ull) >> 54); // 0..1023
    for (;;)
    {
        if (g_watchKeys[pos] == key)
            break;
        if (g_watchKeys[pos] == 0)
        {
            if (g_watchKeyCount < kWatchCap) // table never more than half full
            {
                g_watchKeys[pos] = key;
                ++g_watchKeyCount;
                isNew = true;
            }
            else if (!g_watchCapLogged)
            {
                g_watchCapLogged = true;
                *capNow = true;
            }
            break;
        }
        pos = (pos + 1u) & 1023u;
    }
    ReleaseSRWLockExclusive(&g_watchLock);
    return isNew;
}

// Automatic capture for a new watchdog find (like F10), limited by
// AutoCaptureMax / AutoCaptureMinGapSec (shared by all signatures).
static void WatchAutoCapture(const char* sig, uint64_t vsh, uint64_t psh, float ax, float ay)
{
    if (g_cfg.autoCaptureMax == 0 || g_capActive || g_capPending)
        return;
    const DWORD now = GetTickCount();
    if (g_autoCapCount >= static_cast<LONG>(g_cfg.autoCaptureMax))
        return;
    if (g_autoCapLastTick && now - g_autoCapLastTick < g_cfg.autoCaptureMinGapSec * 1000u)
        return;
    const LONG k = InterlockedIncrement(&g_autoCapCount);
    g_autoCapLastTick = now | 1u;
    Log("[WATCH] auto-capture #%ld for %s vsh=%016llX psh=%016llX anchor=(%.4f,%.4f)", k, sig, vsh, psh,
        static_cast<double>(ax), static_cast<double>(ay));
    if (PresentActive())
        InterlockedExchange(&g_capPending, 1); // starts at the next Present (frame mode)
    else
        StartCapture(false, nullptr);
}

// -----------------------------------------------------------------------------
//  v1.3.0 geometry watchdog "OFF": HUD/prompt-layer elements drawn mostly
//  outside the screen (gameplay only). Evaluated only in sample frames (see
//  WatchSamplePresent); a find needs the same key in >= 2 consecutive samples.
// -----------------------------------------------------------------------------
static const uint64_t kPsGlow       = 0xEF336D585C6DE33Eull; // glow/backdrop gradients overhang legitimately
static const UINT     kOffPersist   = 64;   // persistence table (fixed, no heap)
static const UINT     kOffCap       = 256;  // reported keys per session
struct OffEntry
{
    uint64_t key;
    LONG     lastSample;
    UINT     streak;
};
static OffEntry      g_offPersist[kOffPersist] = {};
static uint64_t      g_offKeys[512] = {};
static UINT          g_offKeyCount = 0;
static bool          g_offCapLogged = false;
static volatile LONG g_watchOffTotal = 0;   // new OFF finds (session)
static volatile LONG g_watchOffSummary = 0; // new OFF finds since the last summary

// Pixel box [x0,y0,x1,y1] of the current draw: POSITION decoded from the VB/IB
// shadows (same rules as [CAP-VTX]) and projected with the drawn matrix m
// (row-vector convention, rows c0..c3) and viewport 0. Also learns the IA
// buffers of this HUD draw for the next sample frames. Caller holds g_stateLock
// (shared). False if anything is missing or incomplete.
static bool WatchPixelBoxLocked(const DrawInfo& d, const float* m, float box[4])
{
    if (d.args || d.count == 0 || d.count > kMaxVtxDecode || !g_layouts || !g_vbShadows || !g_state.il ||
        !g_state.hasVp)
        return false;
    auto lit = g_layouts->find(g_state.il);
    if (lit == g_layouts->end())
        return false;
    const LayoutInfo& li = lit->second;
    if (!li.hasPos || !li.posOffsetKnown || li.posSlot >= kMaxVbSlots)
        return false;
    const UINT need = PosFormatBytes(li.posFmt);
    if (!need)
        return false;
    const VbBinding& vb = g_state.vb[li.posSlot];
    if (!vb.buf)
        return false;
    const bool indexed = (d.type == DT_DrawIndexed || d.type == DT_DrawIndexedInstanced);
    WatchBufLearn(vb.buf);
    if (indexed)
        WatchBufLearn(g_state.ib.buf);

    auto vit = g_vbShadows->find(vb.buf);
    if (vit == g_vbShadows->end() || vit->second.bytes.empty())
        return false;
    const std::vector<unsigned char>& vbytes = vit->second.bytes;
    const std::vector<unsigned char>* ibytes = nullptr;
    UINT isz = 0;
    if (indexed)
    {
        const IbBinding& ib = g_state.ib;
        isz = (ib.fmt == DXGI_FORMAT_R16_UINT) ? 2u : (ib.fmt == DXGI_FORMAT_R32_UINT) ? 4u : 0u;
        if (!ib.buf || !isz)
            return false;
        auto iit = g_vbShadows->find(ib.buf);
        if (iit == g_vbShadows->end() || iit->second.bytes.empty())
            return false;
        ibytes = &iit->second.bytes;
    }

    float nmin[2] = { FLT_MAX, FLT_MAX };
    float nmax[2] = { -FLT_MAX, -FLT_MAX };
    UINT nproj = 0;
    for (UINT i = 0; i < d.count; ++i)
    {
        uint64_t vidx = 0;
        if (indexed)
        {
            const uint64_t ioff =
                static_cast<uint64_t>(g_state.ib.offset) + (static_cast<uint64_t>(d.start) + i) * isz;
            if (ioff + isz > ibytes->size())
                return false;
            uint32_t iv = 0;
            memcpy(&iv, ibytes->data() + static_cast<size_t>(ioff), isz); // little endian
            const int64_t v = static_cast<int64_t>(iv) + d.base;
            if (v < 0)
                return false;
            vidx = static_cast<uint64_t>(v);
        }
        else
        {
            vidx = static_cast<uint64_t>(d.start) + i;
        }
        const uint64_t boff = static_cast<uint64_t>(vb.offset) + vidx * vb.stride + li.posOffset;
        if (boff + need > vbytes.size())
            return false;
        float p[3];
        DecodePos(li.posFmt, vbytes.data() + static_cast<size_t>(boff), p);
        const float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
        const float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
        const float cw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
        if (!(fabsf(cw) >= 1e-20f))
            continue;
        const float nx = cx / cw;
        const float ny = cy / cw;
        nmin[0] = Min(nmin[0], nx);
        nmin[1] = Min(nmin[1], ny);
        nmax[0] = Max(nmax[0], nx);
        nmax[1] = Max(nmax[1], ny);
        ++nproj;
    }
    if (nproj < 3 || !(nmin[0] <= nmax[0]) || !(nmin[1] <= nmax[1]))
        return false;
    const D3D11_VIEWPORT& vp = g_state.vp0;
    box[0] = vp.TopLeftX + (nmin[0] + 1.0f) * 0.5f * vp.Width;
    box[2] = vp.TopLeftX + (nmax[0] + 1.0f) * 0.5f * vp.Width;
    box[1] = vp.TopLeftY + (1.0f - nmax[1]) * 0.5f * vp.Height;
    box[3] = vp.TopLeftY + (1.0f - nmin[1]) * 0.5f * vp.Height;
    return true;
}

// Persistence: true when key is a candidate in this AND the previous sample.
static bool OffPersistUpdate(uint64_t key, LONG sample)
{
    bool persistent = false;
    AcquireSRWLockExclusive(&g_watchLock);
    OffEntry* e = nullptr;
    OffEntry* victim = &g_offPersist[0];
    for (UINT i = 0; i < kOffPersist; ++i)
    {
        OffEntry& c = g_offPersist[i];
        if (c.key == key)
        {
            e = &c;
            break;
        }
        if (!c.key)
        {
            if (victim->key)
                victim = &c; // prefer an empty slot
        }
        else if (victim->key && c.lastSample < victim->lastSample)
        {
            victim = &c; // else the least recently seen entry
        }
    }
    if (!e)
    {
        e = victim;
        e->key = key;
        e->lastSample = sample;
        e->streak = 1;
    }
    else if (e->lastSample != sample) // several draws of one sample count once
    {
        e->streak = (e->lastSample == sample - 1) ? e->streak + 1u : 1u;
        e->lastSample = sample;
    }
    persistent = e->streak >= 2;
    ReleaseSRWLockExclusive(&g_watchLock);
    return persistent;
}

// Report dedup for OFF keys (fixed table, cap kOffCap, no heap).
static bool OffFirstTime(uint64_t key, bool* capNow)
{
    bool isNew = false;
    AcquireSRWLockExclusive(&g_watchLock);
    UINT pos = static_cast<UINT>((key * 0x9E3779B97F4A7C15ull) >> 55); // 0..511
    for (;;)
    {
        if (g_offKeys[pos] == key)
            break;
        if (g_offKeys[pos] == 0)
        {
            if (g_offKeyCount < kOffCap) // table never more than half full
            {
                g_offKeys[pos] = key;
                ++g_offKeyCount;
                isNew = true;
            }
            else if (!g_offCapLogged)
            {
                g_offCapLogged = true;
                *capNow = true;
            }
            break;
        }
        pos = (pos + 1u) & 511u;
    }
    ReleaseSRWLockExclusive(&g_watchLock);
    return isNew;
}

// One HUD-VS gameplay draw of a sample frame. m = matrix as drawn.
static void WatchOffDraw(const HookedTable* t, const float* m, int fixRes, const DrawInfo& d)
{
    const float w = m[15];
    const bool onHud = fabsf(w - g_cfg.hudLayerW) <= g_cfg.hudLayerTol;
    const bool onPrompt = fabsf(w - g_cfg.promptLayerW) <= g_cfg.promptLayerTol;
    if ((!onHud && !onPrompt) || t->psHash == kPsGlow)
        return;
    if (d.args || d.count == 0 || d.count > kMaxVtxDecode)
        return;

    float box[4] = {};
    float vpW = 0.0f, vpH = 0.0f, vpX = 0.0f, vpY = 0.0f;
    AcquireSRWLockShared(&g_stateLock);
    const bool ok = WatchPixelBoxLocked(d, m, box);
    if (ok)
    {
        vpX = g_state.vp0.TopLeftX;
        vpY = g_state.vp0.TopLeftY;
        vpW = g_state.vp0.Width;
        vpH = g_state.vp0.Height;
    }
    ReleaseSRWLockShared(&g_stateLock);
    if (!ok)
        return;

    // Screen = back buffer (fallback: viewport 0).
    float sx0 = 0.0f, sy0 = 0.0f, sx1 = static_cast<float>(g_watchBbW), sy1 = static_cast<float>(g_watchBbH);
    if (!(sx1 > 0.0f) || !(sy1 > 0.0f))
    {
        sx0 = vpX;
        sy0 = vpY;
        sx1 = vpX + vpW;
        sy1 = vpY + vpH;
    }
    const float scrW = sx1 - sx0, scrH = sy1 - sy0;
    if (!(scrW > 0.0f) || !(scrH > 0.0f))
        return;
    const float bw = box[2] - box[0], bh = box[3] - box[1];
    const float area = bw * bh;
    if (!(bw <= 640.0f * (scrW / 2560.0f)) || !(bh <= 360.0f * (scrH / 1440.0f)) || !(area > 4.0f))
        return; // large elements (backgrounds, full-screen images) or tiny/degenerate
    const float ix = Max(0.0f, Min(box[2], sx1) - Max(box[0], sx0));
    const float iy = Max(0.0f, Min(box[3], sy1) - Max(box[1], sy0));
    const float out = 1.0f - (ix * iy) / area;
    if (!(out >= 0.5f))
        return;

    // Key: shaders, layer, count, box rounded to 32 px.
    Fnv f;
    f.Val(t->vsHash);
    f.Val(t->psHash);
    const UINT layer = onHud ? 1u : 2u;
    f.Val(layer);
    f.Val(d.count);
    for (UINT k = 0; k < 4; ++k)
    {
        const int r = static_cast<int>(floorf(box[k] / 32.0f + 0.5f));
        f.Val(r);
    }
    const uint64_t key = f.h | 1ull;
    if (!OffPersistUpdate(key, g_watchSampleIdx))
        return;
    bool capNow = false;
    const bool isNew = OffFirstTime(key, &capNow);
    if (capNow)
        Log("[WATCH] OFF unique-entry cap (%u) reached - further new OFF finds are not logged", kOffCap);
    if (!isNew)
        return;

    InterlockedIncrement(&g_watchOffTotal);
    InterlockedIncrement(&g_watchOffSummary);
    const bool corrected = (fixRes == FIX_SWAP);
    const float ax = m[12] / w;
    const float ay = m[13] / w;
    const double tSec = static_cast<double>(GetTickCount64() - g_startTick) / 1000.0;
    Log("[WATCH] OFF new: t=%.3f vsh=%016llX psh=%016llX w=%.1f cnt=%u box=[%.0f,%.0f..%.0f,%.0f] out=%d%% "
        "corrected=%d anchor=(%.4f,%.4f)%s",
        tSec, t->vsHash, t->psHash, static_cast<double>(w), d.count, static_cast<double>(box[0]),
        static_cast<double>(box[1]), static_cast<double>(box[2]), static_cast<double>(box[3]),
        static_cast<int>(floorf(out * 100.0f + 0.5f)), corrected ? 1 : 0, static_cast<double>(ax),
        static_cast<double>(ay), corrected ? " (fix incomplete)" : "");
    WatchAutoCapture("OFF", t->vsHash, t->psHash, ax, ay);
}

// -----------------------------------------------------------------------------
//  v1.3.2 HUD inventory ([INV], EnableLogging 1 or 2, InventoryEnable=1)
//
//  Records every distinct gameplay HUD/prompt-layer element once, corrected or
//  not, as the basis for a later per-element HUD customization. v1.3.4: only
//  gameplay draws (menu gate not active) on the HUD layer (H) or the prompt
//  layer (P) within HudLayerTol/PromptLayerTol; other layers are never recorded.
//  Key: base key (VS, PS, layer, index/vertex count) + anchor rounded to 0.05.
//  v1.3.3: at most kInvPerBase entries per base key (animated elements produce a
//  new key per anchor step), counted in a second fixed table. Fixed open-
//  addressing tables (no heap); nothing is added once the entry table is full.
// -----------------------------------------------------------------------------
static const UINT    kInvSlots   = 8192;
static const UINT    kInvCap     = 6144;  // max. entries (75 % load keeps the probe chains short)
static const UINT    kInvPerBase = 3;     // max. entries per base key
static uint64_t      g_invKeys[kInvSlots] = {};
static uint64_t      g_invBaseKeys[kInvSlots] = {}; // base keys; never more than entries (<= kInvCap)
static uint8_t       g_invBaseCnt[kInvSlots] = {};
static UINT          g_invCount = 0;
static volatile LONG g_invFull = 0;
static volatile LONG g_invNewSummary = 0; // new entries since the last summary
static DWORD         g_invSummaryTick = 0;
static SRWLOCK       g_invLock = SRWLOCK_INIT;

static UINT InvSlot(uint64_t key)
{
    return static_cast<UINT>((key * 0x9E3779B97F4A7C15ull) >> 51); // 0..8191
}

// 1 = new key added, 0 = known or per-base limit reached, -1 = table just became full.
static int InvInsert(uint64_t key, uint64_t baseKey)
{
    int res = 0;
    AcquireSRWLockExclusive(&g_invLock);
    UINT pos = InvSlot(key);
    for (;;)
    {
        if (g_invKeys[pos] == key)
            break;
        if (g_invKeys[pos] == 0)
        {
            if (g_invCount >= kInvCap)
            {
                if (InterlockedExchange(&g_invFull, 1) == 0)
                    res = -1;
                break;
            }
            // Per-base limit (base count <= entry count < kInvSlots, so a free slot always exists).
            UINT bpos = InvSlot(baseKey ^ 0x5BD1E9955BD1E995ull);
            while (g_invBaseKeys[bpos] != 0 && g_invBaseKeys[bpos] != baseKey)
                bpos = (bpos + 1u) & (kInvSlots - 1u);
            if (g_invBaseKeys[bpos] == baseKey && static_cast<UINT>(g_invBaseCnt[bpos]) >= kInvPerBase)
                break; // limited: not stored, so the next draw probes again (cheap, no logging)
            g_invBaseKeys[bpos] = baseKey;
            ++g_invBaseCnt[bpos];
            g_invKeys[pos] = key;
            ++g_invCount;
            res = 1;
            break;
        }
        pos = (pos + 1u) & (kInvSlots - 1u);
    }
    ReleaseSRWLockExclusive(&g_invLock);
    return res;
}

// After the draw (logging active), uses t_inv from FixBeforeDraw.
static void InvDraw(const DrawInfo& d)
{
    if (!t_inv.have)
        return;
    t_inv.have = false;
    if (g_invFull)
        return; // stop adding
    const InvDrawInfo& e = t_inv;
    // Base key: shaders, layer, count.
    Fnv f;
    f.Val(e.vsh);
    f.Val(e.psh);
    f.Val(e.layer);
    f.Val(d.count);
    const double wR = floor(static_cast<double>(e.w) * 10.0 + 0.5) / 10.0; // logged value (0.1)
    const uint64_t baseKey = f.h | 1ull;
    // Entry key: base key + anchor rounded to 0.05.
    const int rx = static_cast<int>(floorf(e.ax / 0.05f + 0.5f));
    const int ry = static_cast<int>(floorf(e.ay / 0.05f + 0.5f));
    f.Val(rx);
    f.Val(ry);
    const int res = InvInsert(f.h | 1ull, baseKey);
    if (res < 0)
    {
        Log("[INV] table full (%u entries) - further new HUD elements are not recorded", kInvCap);
        return;
    }
    if (res == 0)
        return;

    InterlockedIncrement(&g_invNewSummary);
    char ruleStr[8] = "none";
    if (e.rule)
    {
        ruleStr[0] = e.rule;
        ruleStr[1] = 0;
    }
    char drawnStr[64] = "hidden";
    if (!e.hidden)
        _snprintf_s(drawnStr, sizeof(drawnStr), _TRUNCATE, "(%.4f,%.4f)", static_cast<double>(e.dax),
                    static_cast<double>(e.day));
    const double tSec = static_cast<double>(GetTickCount64() - g_startTick) / 1000.0;
    const ULONG bb = static_cast<ULONG>(g_lastBbWH);
    Log("[INV] new t=%.3f layer=%c w=%.1f gate=%s bb=%lux%lu vsh=%016llX psh=%016llX cnt=%u anchor=(%.4f,%.4f) "
        "scale=(%.6f,%.6f) rule=%s drawn=%s",
        tSec, e.layer, wR, e.gated ? "menu" : "game", bb >> 16, bb & 0xFFFFul, e.vsh, e.psh, d.count,
        static_cast<double>(e.ax), static_cast<double>(e.ay), static_cast<double>(e.sx), static_cast<double>(e.sy),
        ruleStr, drawnStr);
}

// Called from PollFix: summary every 10 minutes if new entries were added.
static void InvSummary(DWORD now)
{
    if (!LoggingActive() || !g_cfg.inventoryEnable)
        return;
    if (!g_invSummaryTick)
        g_invSummaryTick = now;
    if (now - g_invSummaryTick < 600000u)
        return;
    g_invSummaryTick = now;
    const LONG n = InterlockedExchange(&g_invNewSummary, 0);
    if (n <= 0)
        return;
    AcquireSRWLockShared(&g_invLock);
    const UINT total = g_invCount;
    ReleaseSRWLockShared(&g_invLock);
    Log("[INV] summary: total=%u new=%ld", total, n);
}

// After the original draw (logging active). fixRes = result of FixBeforeDraw.
static void WatchDraw(ID3D11DeviceContext* This, int fixRes, const DrawInfo& d)
{
    if (t_inv.have)
        InvDraw(d); // v1.3.2 HUD inventory (independent of the watchdog below)
    const bool haveDrawn = t_drawHave;
    const bool gated = t_drawGated;
    const bool isSmall = t_drawSmall;
    t_drawHave = false;
    if (fixRes == FIX_SKIP || !haveDrawn || gated)
        return; // hidden by us, no matrix or menu (never checked)
    HookedTable* t = FindTable(*reinterpret_cast<void***>(This));
    if (!t)
        return;
    float m[16];
    memcpy(m, t_drawM, sizeof(m));
    const float w = m[15];
    if (!(fabsf(w) > 1e-6f))
        return;
    if (g_watchSampling && !g_capActive)
        WatchOffDraw(t, m, fixRes, d); // sample frame only
    if (!isSmall)
        return; // signature A: small screen elements only
    const float ax = m[12] / w;
    const float ay = m[13] / w;
    // Signature A: anchor exactly at the stage origin.
    const bool sigA = fabsf(ax + 0.99844f) < 0.002f && fabsf(ay - 1.0f) < 0.002f;
    if (!sigA)
        return;

    const bool fixed = (fixRes == FIX_SWAP);
    Fnv f;
    const UINT sig = 1u; // signature id (A), part of the dedup key
    f.Val(sig);
    f.Val(fixed);
    f.Val(t->vsHash);
    f.Val(t->psHash);
    const int rw = static_cast<int>(floorf(w + 0.5f));
    const int rx = static_cast<int>(floorf(ax / 0.05f + 0.5f));
    const int ry = static_cast<int>(floorf(ay / 0.05f + 0.5f));
    f.Val(rw);
    f.Val(rx);
    f.Val(ry);
    bool capNow = false;
    const bool isNew = WatchFirstTime(f.h, &capNow);
    if (capNow)
        Log("[WATCH] unique-entry cap (%u) reached - further new finds are not logged", kWatchCap);
    if (!isNew)
        return;

    InterlockedIncrement(&g_watchATotal);
    InterlockedIncrement(&g_watchASummary);
    const double tSec = static_cast<double>(GetTickCount64() - g_startTick) / 1000.0;
    Log("[WATCH] A new: t=%.3f vsh=%016llX psh=%016llX w=%.1f anchor=(%.4f,%.4f) scale=(%.6f,%.6f) frame=%ld%s",
        tSec, t->vsHash, t->psHash, static_cast<double>(w), static_cast<double>(ax), static_cast<double>(ay),
        static_cast<double>(m[0] / w), static_cast<double>(m[5] / w), static_cast<LONG>(g_frameCount),
        fixed ? " (fix ineffective: corrected draw still matches)" : "");

    // Automatic capture for new, uncorrected A finds (like F10, limited).
    if (fixed)
        return;
    WatchAutoCapture("A", t->vsHash, t->psHash, ax, ay);
}

// Called from PollFix (every 50 ms at most): summary every 60 s if there were new finds.
static void WatchSummary(DWORD now)
{
    if (!LoggingActive())
        return;
    if (!g_watchSummaryTick)
        g_watchSummaryTick = now;
    if (now - g_watchSummaryTick < 60000u)
        return;
    g_watchSummaryTick = now;
    const LONG a = InterlockedExchange(&g_watchASummary, 0);
    const LONG off = InterlockedExchange(&g_watchOffSummary, 0);
    if (a > 0 || off > 0)
        Log("[WATCH] summary: A=%ld OFF=%ld (session A=%ld OFF=%ld, auto-captures %ld)", a, off,
            static_cast<LONG>(g_watchATotal), static_cast<LONG>(g_watchOffTotal), static_cast<LONG>(g_autoCapCount));
}

// After the draw: re-bind the game's buffer via the original and release it.
static void FixAfterDraw(ID3D11DeviceContext* This, ID3D11Buffer* prev)
{
    const TRHF_PFN_SetConstantBuffers setCB = TRHF_ORIG(TRHF_PFN_SetConstantBuffers, VT_VSSetConstantBuffers);
    setCB(This, 0, 1, &prev);
    if (prev)
        prev->Release();
}

// Draw function to call. After FixBeforeDraw changed state through the
// originals, the runtime may have rewritten the draw entry - re-arm first and
// call whatever the runtime installed.
static void* DrawFn(ID3D11DeviceContext* This, UINT idx, bool fixed)
{
    if (!fixed)
        return OrigFor(This, idx);
    Rearm(This);
    void** vtbl = *reinterpret_cast<void***>(This);
    void* cur = vtbl[idx];
    return (cur && cur != g_hookByIdx[idx]) ? cur : OrigFor(This, idx);
}

// F9 toggle + (logging only) 10 s statistics and watchdog/inventory summaries.
// Called from Present and RSSetViewports, throttled to 50 ms.
static void PollFix()
{
    const DWORD now = GetTickCount();
    const LONG last = g_fixLastPoll;
    if (now - static_cast<DWORD>(last) < 50u)
        return;
    if (InterlockedCompareExchange(&g_fixLastPoll, static_cast<LONG>(now), last) != last)
        return;

    if (g_cfg.fixToggleHotkey)
    {
        const bool down =
            (static_cast<USHORT>(GetAsyncKeyState(static_cast<int>(g_cfg.fixToggleHotkey))) & 0x8000u) != 0;
        if (down && !g_fixKeyWasDown && !HotkeysSuppressed())
        {
            const LONG on = g_fixRuntime ? 0 : 1;
            InterlockedExchange(&g_fixRuntime, on);
            Log("[FIX] toggled %s (hotkey 0x%02X)", on ? "ON" : "OFF", g_cfg.fixToggleHotkey);
            char msg[64];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "TR2013 HUD Fix+: fix %s\n", on ? "ON" : "OFF");
            OutputDebugStringA(msg);
        }
        g_fixKeyWasDown = down;
    }

    if (!LoggingActive())
        return; // statistics and summaries are diagnostics
    if (!g_fixStatTick)
        g_fixStatTick = now;
    WatchSummary(now);
    InvSummary(now);
    if (now - g_fixStatTick >= 10000u)
    {
        const double secs = (now - g_fixStatTick) / 1000.0;
        g_fixStatTick = now;
        // v1.4.6 map-redirect safety counters (session totals, logged when they change)
        static LONG s_staleSeen = 0, s_guardSeen = 0;
        const LONG stale = g_fixStaleDrops, guard = g_scratchGuardHits;
        if (stale != s_staleSeen || guard != s_guardSeen)
        {
            s_staleSeen = stale;
            s_guardSeen = guard;
            Log("[FIX] map safety: stale map drops %ld, scratch guard hits %ld (session), re-arms Map %ld Unmap %ld "
                "UpdateSubresource %ld",
                stale, guard, static_cast<LONG>(g_rearmCount[VT_Map]), static_cast<LONG>(g_rearmCount[VT_Unmap]),
                static_cast<LONG>(g_rearmCount[VT_UpdateSubresource]));
        }
        const LONG npo = InterlockedExchange(&g_poCount, 0);
        if (npo > 0)
            Log("[FIX] %ld popup-origin fixes in last %.1f s", npo, secs);
        const LONG nsv = InterlockedExchange(&g_salvageCount, 0);
        if (nsv > 0)
            Log("[FIX] %ld salvage fixes in last %.1f s", nsv, secs);
        const LONG np = InterlockedExchange(&g_promptCount, 0);
        if (np > 0)
            Log("[FIX] %ld prompt fixes in last %.1f s", np, secs);
        const LONG nb = InterlockedExchange(&g_backdropCount, 0);
        if (nb > 0)
            Log("[FIX] %ld backdrop fixes in last %.1f s (mode %s)", nb, secs,
                g_cfg.fixBackdrop == 2 ? "hide" : "move");
        const LONG n = InterlockedExchange(&g_fixCount, 0);
        if (n > 0)
            Log("[FIX] %ld draws corrected in the last %.1f s (%.1f/s), fix %s", n, secs, n / secs,
                g_fixRuntime ? "ON" : "OFF");
    }
}

// =============================================================================
//  Hook implementations
// =============================================================================

// Reads GetDesc of the bound buffers into info[]; returns the clamped count.
static UINT ReadCBSlots(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* pp, SlotState* info, Fnv& f)
{
    const UINT n = (StartSlot < kCBSlots) ? Min<UINT>(NumBuffers, kCBSlots - StartSlot) : 0u;
    f.Val(StartSlot);
    f.Val(NumBuffers);
    for (UINT i = 0; i < n; ++i)
    {
        ID3D11Buffer* b = pp ? pp[i] : nullptr;
        if (b)
        {
            D3D11_BUFFER_DESC d = {};
            b->GetDesc(&d);
            info[i].buf       = b;
            info[i].byteWidth = d.ByteWidth;
            info[i].usage     = static_cast<UINT>(d.Usage);
            info[i].cpuAccess = d.CPUAccessFlags;
            info[i].bind      = d.BindFlags;
            info[i].misc      = d.MiscFlags;
            const UINT one = 1;
            f.Val(one);
            f.Val(info[i].byteWidth);
            f.Val(info[i].usage);
            f.Val(info[i].cpuAccess);
            f.Val(info[i].bind);
            f.Val(info[i].misc);
        }
        else
        {
            info[i] = SlotState{};
            const UINT zero = 0;
            f.Val(zero);
        }
    }
    return n;
}

// --- [7] VSSetConstantBuffers -------------------------------------------------
static void STDMETHODCALLTYPE Hook_VSSetConstantBuffers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumBuffers,
                                                        ID3D11Buffer* const* ppConstantBuffers)
{
    TRHF_COUNT(VT_VSSetConstantBuffers);
    const TRHF_PFN_SetConstantBuffers o = TRHF_ORIG(TRHF_PFN_SetConstantBuffers, VT_VSSetConstantBuffers);
    if (t_inOverlay)
    {
        o(This, StartSlot, NumBuffers, ppConstantBuffers);
        return;
    }
    o(This, StartSlot, NumBuffers, ppConstantBuffers);
    Rearm(This);
    if (g_fixInfra)
        FixTrackS0(This, StartSlot, NumBuffers, ppConstantBuffers);
    if (!LoggingActive())
        return;

    SlotState info[kCBSlots] = {};
    Fnv f;
    const UINT n = ReadCBSlots(StartSlot, NumBuffers, ppConstantBuffers, info, f);

    bool cap = false;
    AcquireSRWLockExclusive(&g_stateLock);
    for (UINT i = 0; i < n; ++i)
    {
        g_state.vscb[StartSlot + i] = info[i];
        if (info[i].buf)
            SlotMapSetLocked(info[i].buf, static_cast<int>(StartSlot + i));
    }
    const bool isNew = DedupInsertLocked(CAT_VSCB, f.h, cap);
    ReleaseSRWLockExclusive(&g_stateLock);

    if (cap)
        LogCapReached(CAT_VSCB);
    if (!isNew)
        return;
    LineBuf lb;
    lb.Add("[VSCB] VSSetConstantBuffers start=%u n=%u", StartSlot, NumBuffers);
    for (UINT i = 0; i < n; ++i)
    {
        if (info[i].buf)
            lb.Add(" | s%u: bw=%u usage=%u cpu=0x%X bind=0x%X misc=0x%X", StartSlot + i, info[i].byteWidth,
                   info[i].usage, info[i].cpuAccess, info[i].bind, info[i].misc);
        else
            lb.Add(" | s%u: null", StartSlot + i);
    }
    Log("%s", lb.s);
}

// --- [16] PSSetConstantBuffers (state tracking only) ----------------------------
static void STDMETHODCALLTYPE Hook_PSSetConstantBuffers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumBuffers,
                                                        ID3D11Buffer* const* ppConstantBuffers)
{
    TRHF_COUNT(VT_PSSetConstantBuffers);
    const TRHF_PFN_SetConstantBuffers o = TRHF_ORIG(TRHF_PFN_SetConstantBuffers, VT_PSSetConstantBuffers);
    if (t_inOverlay)
    {
        o(This, StartSlot, NumBuffers, ppConstantBuffers);
        return;
    }
    o(This, StartSlot, NumBuffers, ppConstantBuffers);
    Rearm(This);
    if (!LoggingActive())
        return;

    SlotState info[kCBSlots] = {};
    Fnv f;
    const UINT n = ReadCBSlots(StartSlot, NumBuffers, ppConstantBuffers, info, f);
    AcquireSRWLockExclusive(&g_stateLock);
    for (UINT i = 0; i < n; ++i)
        g_state.pscb[StartSlot + i] = info[i];
    ReleaseSRWLockExclusive(&g_stateLock);
}

// --- [11] VSSetShader -----------------------------------------------------------
static void STDMETHODCALLTYPE Hook_VSSetShader(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader,
                                               ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances)
{
    TRHF_COUNT(VT_VSSetShader);
    const TRHF_PFN_VSSetShader o = TRHF_ORIG(TRHF_PFN_VSSetShader, VT_VSSetShader);
    if (t_inOverlay)
    {
        o(This, pVertexShader, ppClassInstances, NumClassInstances);
        return;
    }
    o(This, pVertexShader, ppClassInstances, NumClassInstances);
    Rearm(This);
    if (g_fixInfra)
        FixTrackVS(This, pVertexShader);
    if (!LoggingActive())
        return;

    Fnv f;
    const void* vs = pVertexShader; // identity only, no AddRef
    f.Val(vs);
    bool cap = false;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.vs = vs;
    const bool isNew = DedupInsertLocked(CAT_VS, f.h, cap);
    ReleaseSRWLockExclusive(&g_stateLock);

    if (cap)
        LogCapReached(CAT_VS);
    if (isNew)
        Log("[VS] new vertex shader %p (classInstances=%u)", vs, NumClassInstances);
}

// --- [9] PSSetShader / [24] IASetPrimitiveTopology (state tracking only) ---------
static void STDMETHODCALLTYPE Hook_PSSetShader(ID3D11DeviceContext* This, ID3D11PixelShader* pPixelShader,
                                               ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances)
{
    TRHF_COUNT(VT_PSSetShader);
    const TRHF_PFN_PSSetShader o = TRHF_ORIG(TRHF_PFN_PSSetShader, VT_PSSetShader);
    if (t_inOverlay)
    {
        o(This, pPixelShader, ppClassInstances, NumClassInstances);
        return;
    }
    o(This, pPixelShader, ppClassInstances, NumClassInstances);
    Rearm(This);
    if (g_fixInfra)
        FixTrackPS(This, pPixelShader);
    if (!LoggingActive())
        return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.ps = pPixelShader; // identity only, no AddRef
    ReleaseSRWLockExclusive(&g_stateLock);
}

static void STDMETHODCALLTYPE Hook_IASetPrimitiveTopology(ID3D11DeviceContext* This, D3D11_PRIMITIVE_TOPOLOGY Topology)
{
    TRHF_COUNT(VT_IASetPrimitiveTopology);
    const TRHF_PFN_IASetPrimitiveTopology o = TRHF_ORIG(TRHF_PFN_IASetPrimitiveTopology, VT_IASetPrimitiveTopology);
    if (t_inOverlay)
    {
        o(This, Topology);
        return;
    }
    o(This, Topology);
    Rearm(This);
    if (!LoggingActive())
        return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.topo = static_cast<UINT>(Topology);
    ReleaseSRWLockExclusive(&g_stateLock);
}

// --- Draw variants ------------------------------------------------------------------
static void TrackDraw(const DrawInfo& d)
{
    const void* vs = nullptr;
    const void* ps = nullptr;
    UINT bw[4] = {};
    float vpW = 0.0f, vpH = 0.0f;
    bool cap = false;

    AcquireSRWLockExclusive(&g_stateLock);
    vs = g_state.vs;
    ps = g_state.ps;
    for (UINT i = 0; i < 4; ++i)
        bw[i] = g_state.vscb[i].buf ? g_state.vscb[i].byteWidth : 0u;
    if (g_state.hasVp)
    {
        vpW = g_state.vp0.Width;
        vpH = g_state.vp0.Height;
    }
    Fnv f;
    f.Val(d.type);
    f.Val(d.count);
    f.Val(d.instances);
    f.Val(vs);
    f.Val(ps);
    f.Bytes(bw, sizeof(bw));
    f.Val(vpW);
    f.Val(vpH);
    const bool isNew = DedupInsertLocked(CAT_DRAW, f.h, cap);
    ReleaseSRWLockExclusive(&g_stateLock);

    if (cap)
        LogCapReached(CAT_DRAW);
    if (!isNew)
        return;
    LineBuf lb;
    lb.Add("[DRAW] %s count=%u inst=%u start=%u baseVertex=%d startInst=%u", kDrawNames[d.type < 7 ? d.type : 0],
           d.count, d.instances, d.start, d.base, d.startInst);
    if (d.args)
        lb.Add(" args=%p+%u", d.args, d.argsOff);
    lb.Add(" vs=%p ps=%p cbBW0..3=%u,%u,%u,%u vp0=%.1fx%.1f", vs, ps, bw[0], bw[1], bw[2], bw[3],
           static_cast<double>(vpW), static_cast<double>(vpH));
    Log("%s", lb.s);
}

static void OnDraw(const DrawInfo& d)
{
    PollCapture();
    if (g_logVerbose)
        TrackDraw(d);
    if (CaptureActive())
        CaptureDraw(d);
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation,
                                               INT BaseVertexLocation)
{
    TRHF_COUNT(VT_DrawIndexed);
    if (t_inOverlay)
    {
        reinterpret_cast<TRHF_PFN_DrawIndexed>(OrigFor(This, VT_DrawIndexed))(This, IndexCount, StartIndexLocation, BaseVertexLocation);
        return;
    }
    ID3D11Buffer* prev = nullptr;
    if (g_fixInfra)
        GateBeginDraw(This);
    const int fixRes = g_fixInfra ? FixBeforeDraw(This, &prev, IndexCount) : FIX_NONE;
    const bool fixed = (fixRes == FIX_SWAP);
    if (fixRes != FIX_SKIP)
    {
        const TRHF_PFN_DrawIndexed o = reinterpret_cast<TRHF_PFN_DrawIndexed>(DrawFn(This, VT_DrawIndexed, fixed));
        o(This, IndexCount, StartIndexLocation, BaseVertexLocation);
    }
    if (fixed)
        FixAfterDraw(This, prev);
    Rearm(This); // the runtime may switch the draw entry during the draw itself
    if (!LoggingActive())
        return;
    const DrawInfo d = { DT_DrawIndexed, IndexCount, 1u, StartIndexLocation, BaseVertexLocation, 0u, nullptr, 0u,
                         static_cast<UINT>(fixRes) };
    WatchDraw(This, fixRes, d);
    OnDraw(d);
}

static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
{
    TRHF_COUNT(VT_Draw);
    if (t_inOverlay)
    {
        reinterpret_cast<TRHF_PFN_Draw>(OrigFor(This, VT_Draw))(This, VertexCount, StartVertexLocation);
        return;
    }
    ID3D11Buffer* prev = nullptr;
    if (g_fixInfra)
        GateBeginDraw(This);
    const int fixRes = g_fixInfra ? FixBeforeDraw(This, &prev, 0u) : FIX_NONE;
    const bool fixed = (fixRes == FIX_SWAP);
    if (fixRes != FIX_SKIP)
    {
        const TRHF_PFN_Draw o = reinterpret_cast<TRHF_PFN_Draw>(DrawFn(This, VT_Draw, fixed));
        o(This, VertexCount, StartVertexLocation);
    }
    if (fixed)
        FixAfterDraw(This, prev);
    Rearm(This);
    if (!LoggingActive())
        return;
    const DrawInfo d = { DT_Draw, VertexCount, 1u, StartVertexLocation, 0, 0u, nullptr, 0u, static_cast<UINT>(fixRes) };
    WatchDraw(This, fixRes, d);
    OnDraw(d);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* This, UINT IndexCountPerInstance,
                                                        UINT InstanceCount, UINT StartIndexLocation,
                                                        INT BaseVertexLocation, UINT StartInstanceLocation)
{
    TRHF_COUNT(VT_DrawIndexedInstanced);
    if (t_inOverlay)
    {
        reinterpret_cast<TRHF_PFN_DrawIndexedInstanced>(OrigFor(This, VT_DrawIndexedInstanced))(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
        return;
    }
    ID3D11Buffer* prev = nullptr;
    if (g_fixInfra)
        GateBeginDraw(This);
    const int fixRes = g_fixInfra ? FixBeforeDraw(This, &prev, IndexCountPerInstance) : FIX_NONE;
    const bool fixed = (fixRes == FIX_SWAP);
    if (fixRes != FIX_SKIP)
    {
        const TRHF_PFN_DrawIndexedInstanced o =
            reinterpret_cast<TRHF_PFN_DrawIndexedInstanced>(DrawFn(This, VT_DrawIndexedInstanced, fixed));
        o(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }
    if (fixed)
        FixAfterDraw(This, prev);
    Rearm(This);
    if (!LoggingActive())
        return;
    const DrawInfo d = { DT_DrawIndexedInstanced, IndexCountPerInstance, InstanceCount, StartIndexLocation,
                         BaseVertexLocation, StartInstanceLocation, nullptr, 0u, static_cast<UINT>(fixRes) };
    WatchDraw(This, fixRes, d);
    OnDraw(d);
}

static void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* This, UINT VertexCountPerInstance,
                                                 UINT InstanceCount, UINT StartVertexLocation,
                                                 UINT StartInstanceLocation)
{
    TRHF_COUNT(VT_DrawInstanced);
    if (t_inOverlay)
    {
        reinterpret_cast<TRHF_PFN_DrawInstanced>(OrigFor(This, VT_DrawInstanced))(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
        return;
    }
    ID3D11Buffer* prev = nullptr;
    if (g_fixInfra)
        GateBeginDraw(This);
    const int fixRes = g_fixInfra ? FixBeforeDraw(This, &prev, 0u) : FIX_NONE;
    const bool fixed = (fixRes == FIX_SWAP);
    if (fixRes != FIX_SKIP)
    {
        const TRHF_PFN_DrawInstanced o =
            reinterpret_cast<TRHF_PFN_DrawInstanced>(DrawFn(This, VT_DrawInstanced, fixed));
        o(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
    if (fixed)
        FixAfterDraw(This, prev);
    Rearm(This);
    if (!LoggingActive())
        return;
    const DrawInfo d = { DT_DrawInstanced, VertexCountPerInstance, InstanceCount, StartVertexLocation, 0,
                         StartInstanceLocation, nullptr, 0u, static_cast<UINT>(fixRes) };
    WatchDraw(This, fixRes, d);
    OnDraw(d);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                                UINT AlignedByteOffsetForArgs)
{
    TRHF_COUNT(VT_DrawIndexedInstancedIndirect);
    if (t_inOverlay)
    {
        reinterpret_cast<TRHF_PFN_DrawIndirect>(OrigFor(This, VT_DrawIndexedInstancedIndirect))(This, pBufferForArgs, AlignedByteOffsetForArgs);
        return;
    }
    ID3D11Buffer* prev = nullptr;
    if (g_fixInfra)
        GateBeginDraw(This);
    const int fixRes = g_fixInfra ? FixBeforeDraw(This, &prev, 0u) : FIX_NONE;
    const bool fixed = (fixRes == FIX_SWAP);
    if (fixRes != FIX_SKIP)
    {
        const TRHF_PFN_DrawIndirect o =
            reinterpret_cast<TRHF_PFN_DrawIndirect>(DrawFn(This, VT_DrawIndexedInstancedIndirect, fixed));
        o(This, pBufferForArgs, AlignedByteOffsetForArgs);
    }
    if (fixed)
        FixAfterDraw(This, prev);
    Rearm(This);
    if (!LoggingActive())
        return;
    // Counts live in a GPU buffer and are unknown here (reported as 0).
    const DrawInfo d = { DT_DrawIndexedInstancedIndirect, 0u, 0u, 0u, 0, 0u, pBufferForArgs, AlignedByteOffsetForArgs,
                         static_cast<UINT>(fixRes) };
    WatchDraw(This, fixRes, d);
    OnDraw(d);
}

static void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                         UINT AlignedByteOffsetForArgs)
{
    TRHF_COUNT(VT_DrawInstancedIndirect);
    if (t_inOverlay)
    {
        reinterpret_cast<TRHF_PFN_DrawIndirect>(OrigFor(This, VT_DrawInstancedIndirect))(This, pBufferForArgs, AlignedByteOffsetForArgs);
        return;
    }
    ID3D11Buffer* prev = nullptr;
    if (g_fixInfra)
        GateBeginDraw(This);
    const int fixRes = g_fixInfra ? FixBeforeDraw(This, &prev, 0u) : FIX_NONE;
    const bool fixed = (fixRes == FIX_SWAP);
    if (fixRes != FIX_SKIP)
    {
        const TRHF_PFN_DrawIndirect o =
            reinterpret_cast<TRHF_PFN_DrawIndirect>(DrawFn(This, VT_DrawInstancedIndirect, fixed));
        o(This, pBufferForArgs, AlignedByteOffsetForArgs);
    }
    if (fixed)
        FixAfterDraw(This, prev);
    Rearm(This);
    if (!LoggingActive())
        return;
    const DrawInfo d = { DT_DrawInstancedIndirect, 0u, 0u, 0u, 0, 0u, pBufferForArgs, AlignedByteOffsetForArgs,
                         static_cast<UINT>(fixRes) };
    WatchDraw(This, fixRes, d);
    OnDraw(d);
}

// --- [17] IASetInputLayout / [18] IASetVertexBuffers / [19] IASetIndexBuffer --------
static void STDMETHODCALLTYPE Hook_IASetInputLayout(ID3D11DeviceContext* This, ID3D11InputLayout* pInputLayout)
{
    TRHF_COUNT(VT_IASetInputLayout);
    const TRHF_PFN_IASetInputLayout o = TRHF_ORIG(TRHF_PFN_IASetInputLayout, VT_IASetInputLayout);
    if (t_inOverlay)
    {
        o(This, pInputLayout);
        return;
    }
    o(This, pInputLayout);
    Rearm(This);
    if (!LoggingActive())
        return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.il = pInputLayout; // identity only
    ReleaseSRWLockExclusive(&g_stateLock);
}

static void STDMETHODCALLTYPE Hook_IASetVertexBuffers(ID3D11DeviceContext* This, UINT StartSlot, UINT NumBuffers,
                                                      ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides,
                                                      const UINT* pOffsets)
{
    TRHF_COUNT(VT_IASetVertexBuffers);
    const TRHF_PFN_IASetVertexBuffers o = TRHF_ORIG(TRHF_PFN_IASetVertexBuffers, VT_IASetVertexBuffers);
    if (t_inOverlay)
    {
        o(This, StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
        return;
    }
    o(This, StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
    Rearm(This);
    if (!LoggingActive())
        return;

    const UINT n = (StartSlot < kMaxVbSlots) ? Min<UINT>(NumBuffers, kMaxVbSlots - StartSlot) : 0u;
    VbBinding nb[kMaxVbSlots] = {};
    for (UINT i = 0; i < n; ++i)
    {
        ID3D11Buffer* b = ppVertexBuffers ? ppVertexBuffers[i] : nullptr;
        nb[i].buf    = b;
        nb[i].stride = pStrides ? pStrides[i] : 0u;
        nb[i].offset = pOffsets ? pOffsets[i] : 0u;
        if (b)
        {
            D3D11_BUFFER_DESC d = {};
            b->GetDesc(&d);
            nb[i].bw    = d.ByteWidth;
            nb[i].usage = static_cast<UINT>(d.Usage);
        }
    }
    AcquireSRWLockExclusive(&g_stateLock);
    for (UINT i = 0; i < n; ++i)
        g_state.vb[StartSlot + i] = nb[i];
    ReleaseSRWLockExclusive(&g_stateLock);
}

static void STDMETHODCALLTYPE Hook_IASetIndexBuffer(ID3D11DeviceContext* This, ID3D11Buffer* pIndexBuffer,
                                                    DXGI_FORMAT Format, UINT Offset)
{
    TRHF_COUNT(VT_IASetIndexBuffer);
    const TRHF_PFN_IASetIndexBuffer o = TRHF_ORIG(TRHF_PFN_IASetIndexBuffer, VT_IASetIndexBuffer);
    if (t_inOverlay)
    {
        o(This, pIndexBuffer, Format, Offset);
        return;
    }
    o(This, pIndexBuffer, Format, Offset);
    Rearm(This);
    if (!LoggingActive())
        return;

    IbBinding ib = {};
    ib.buf    = pIndexBuffer;
    ib.fmt    = Format;
    ib.offset = Offset;
    if (pIndexBuffer)
    {
        D3D11_BUFFER_DESC d = {};
        pIndexBuffer->GetDesc(&d);
        ib.bw    = d.ByteWidth;
        ib.usage = static_cast<UINT>(d.Usage);
    }
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.ib = ib;
    ReleaseSRWLockExclusive(&g_stateLock);
}

// --- [14] Map ---------------------------------------------------------------------
// Tracks mapped constant buffers (CBDATA samples / capture shadows) and, during a
// capture, mapped vertex/index buffers.
static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource,
                                          D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
{
    TRHF_COUNT(VT_Map);
    const TRHF_PFN_Map o = TRHF_ORIG(TRHF_PFN_Map, VT_Map);
    if (t_inOverlay)
        return o(This, pResource, Subresource, MapType, MapFlags, pMappedResource);
    const HRESULT hr = o(This, pResource, Subresource, MapType, MapFlags, pMappedResource);
    Rearm(This);
    if (g_fixInfra && SUCCEEDED(hr))
    {
        // v1.4.6: redirect only if THIS context's Unmap entry is our hook right
        // now (read after Rearm) - else the copy-back would never run.
        const bool unmapOurs = (*reinterpret_cast<void* const* const*>(This))[VT_Unmap] == g_hookByIdx[VT_Unmap] &&
                               g_hookByIdx[VT_Unmap] != nullptr;
        FixOnMap(This, pResource, Subresource, MapType, pMappedResource,
                 unmapOurs && (g_perfMode == 0 || PerfWantHook(g_perfMode, VT_Unmap)));
    }
    if (FAILED(hr) || !LoggingActive() || !pResource || Subresource != 0 || !pMappedResource ||
        !pMappedResource->pData)
        return hr;
    const bool cap = CaptureActive();
    // Watchdog sample frame: learned HUD VB/IB only (pointer compare, no QI otherwise)
    const bool samp = WatchSampling() && WatchBufContains(pResource);
    // [CBDATA] samples exist only with EnableLogging=1 (CbNeedsDumpLocked); with
    // EnableLogging=2 only a capture or a watchdog sample frame needs the rest.
    if ((!g_logVerbose || g_cfg.cbDumpSamples == 0) && !cap && !samp)
        return hr;

    D3D11_BUFFER_DESC d = {};
    const void* buf = QueryBuffer(pResource, &d);
    if (!buf)
        return hr;
    const bool isCB = (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
    const bool isVB = (d.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) != 0;
    if (!isCB && !(isVB && (cap || samp)))
        return hr;

    AcquireSRWLockExclusive(&g_stateLock);
    bool want = false;
    if (isCB)
        want = g_capActive || CbNeedsDumpLocked(CbSignature(d.ByteWidth, SlotMapGetLocked(buf)));
    else
        want = g_capActive != 0 || samp;
    if (want)
    {
        OpenMap* target = nullptr;
        for (UINT i = 0; i < kMaxOpenMaps; ++i)
        {
            if (g_openMaps[i].res == pResource)
            {
                target = &g_openMaps[i];
                break;
            }
        }
        if (!target && static_cast<UINT>(g_openMapCount) < kMaxOpenMaps)
        {
            for (UINT i = 0; i < kMaxOpenMaps; ++i)
            {
                if (!g_openMaps[i].res)
                {
                    target = &g_openMaps[i];
                    InterlockedIncrement(&g_openMapCount);
                    break;
                }
            }
        }
        if (target)
        {
            target->res   = pResource;
            target->buf   = buf;
            target->data  = pMappedResource->pData;
            target->bw    = d.ByteWidth;
            target->kind  = isCB ? 0u : 1u;
            target->bind  = d.BindFlags;
            target->usage = static_cast<UINT>(d.Usage);
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    return hr;
}

// --- [15] Unmap -------------------------------------------------------------------
static __declspec(noinline) void UnmapDiag(ID3D11DeviceContext* This, TRHF_PFN_Unmap o, ID3D11Resource* pResource,
                                           UINT Subresource);

static void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource)
{
    TRHF_COUNT(VT_Unmap);
    const TRHF_PFN_Unmap o = TRHF_ORIG(TRHF_PFN_Unmap, VT_Unmap);
    // v1.4.4: first, ungated - copies redirected maps back before the original Unmap
    FixOnUnmap(This, pResource, Subresource);
    if (t_inOverlay)
    {
        o(This, pResource, Subresource);
        return;
    }
    // Lock-free early-out (like FixOnUnmap): no tracked diagnostic map open and
    // no capture -> UnmapDiag would only call the original + Rearm anyway.
    if (!LoggingActive() || !pResource || Subresource != 0 || (g_openMapCount == 0 && !CaptureActive()))
    {
        o(This, pResource, Subresource);
        Rearm(This);
        return;
    }
    UnmapDiag(This, o, pResource, Subresource);
}

// v1.4.6: diagnostics part of Hook_Unmap (logging on). Separate, non-inlined,
// so the 4 KB copy buffer (__chkstk) is not in the hot hook's frame. Calls the
// original Unmap and Rearm itself (same order as before).
static __declspec(noinline) void UnmapDiag(ID3D11DeviceContext* This, TRHF_PFN_Unmap o, ID3D11Resource* pResource,
                                           UINT Subresource)
{
    OpenMap om = {};
    int slot = -1;
    UINT sample = 0;
    AcquireSRWLockExclusive(&g_stateLock);
    if (g_openMapCount > 0)
    {
        for (UINT i = 0; i < kMaxOpenMaps; ++i)
        {
            if (g_openMaps[i].res == pResource)
            {
                om = g_openMaps[i];
                g_openMaps[i] = OpenMap{};
                InterlockedDecrement(&g_openMapCount);
                break;
            }
        }
        if (om.res && om.kind == 0)
        {
            slot = SlotMapGetLocked(om.buf);
            sample = CbTakeDumpLocked(CbSignature(om.bw, slot));
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);

    if (om.res && om.kind == 1)
    {
        // Vertex/index buffer: copy from pData (= buffer start, also for
        // NO_OVERWRITE) BEFORE the original Unmap invalidates the pointer.
        if (CaptureActive() || WatchSampling())
            VbShadowStore(om.buf, om.bw, om.usage, om.bind, om.data, 0u, Min<UINT>(om.bw, g_cfg.captureMaxVBBytes));
        o(This, pResource, Subresource);
        Rearm(This);
        return;
    }

    const bool capture = om.res && CaptureActive();

    // Copy BEFORE the original Unmap invalidates the mapped pointer.
    unsigned char tmp[kMaxCBDumpBytes];
    const UINT nDump = sample ? Min<UINT>(om.bw, g_cfg.cbDumpMaxBytes) : 0u;
    const UINT nCap = capture ? Min<UINT>(om.bw, g_cfg.captureMaxCBBytes) : 0u;
    const UINT n = Max(nDump, nCap);
    const bool ok = n > 0 && SafeCopy(tmp, om.data, n);

    o(This, pResource, Subresource);
    Rearm(This);

    if (ok && nDump)
        LogCBData("Unmap", om.buf, om.bw, slot, sample, tmp, nDump);
    if (ok && nCap)
        CaptureCBUpdate(om.buf, om.bw, tmp, nCap);
}

// --- [44] RSSetViewports ------------------------------------------------------------
static void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext* This, UINT NumViewports,
                                                  const D3D11_VIEWPORT* pViewports)
{
    TRHF_COUNT(VT_RSSetViewports);
    const TRHF_PFN_RSSetViewports o = TRHF_ORIG(TRHF_PFN_RSSetViewports, VT_RSSetViewports);
    if (t_inOverlay)
    {
        o(This, NumViewports, pViewports);
        return;
    }
    o(This, NumViewports, pViewports);
    Rearm(This);
    if (g_fixInfra)
        PollFix();
    if (!LoggingActive())
        return;
    PollCapture();

    const UINT n = pViewports ? Min<UINT>(NumViewports, kMaxVpSc) : 0u;
    Fnv f;
    f.Val(NumViewports);
    for (UINT i = 0; i < n; ++i)
        f.Val(pViewports[i]);

    bool cap = false, capNew = false;
    AcquireSRWLockExclusive(&g_stateLock);
    if (n > 0)
    {
        g_state.vp0 = pViewports[0];
        g_state.hasVp = true;
    }
    const bool isNew = DedupInsertLocked(CAT_VP, f.h, cap);
    if (g_capActive)
        capNew = CapSetInsertLocked(g_capVpSet, f.h);
    ReleaseSRWLockExclusive(&g_stateLock);

    if (cap)
        LogCapReached(CAT_VP);
    if (!isNew && !capNew)
        return;
    LineBuf lb;
    lb.Add(" n=%u", NumViewports);
    for (UINT i = 0; i < n; ++i)
    {
        const D3D11_VIEWPORT& v = pViewports[i];
        lb.Add(" | %u: x=%.3f y=%.3f w=%.3f h=%.3f z=%.3f..%.3f", i, static_cast<double>(v.TopLeftX),
               static_cast<double>(v.TopLeftY), static_cast<double>(v.Width), static_cast<double>(v.Height),
               static_cast<double>(v.MinDepth), static_cast<double>(v.MaxDepth));
    }
    if (isNew)
        Log("[VP] RSSetViewports%s", lb.s);
    if (capNew && CapTakeLines(1))
        Log("[CAP-VP] t=%lu f=%ld%s", CapTime(), static_cast<LONG>(g_capFrame), lb.s);
}

// --- [45] RSSetScissorRects ---------------------------------------------------------
static void STDMETHODCALLTYPE Hook_RSSetScissorRects(ID3D11DeviceContext* This, UINT NumRects, const D3D11_RECT* pRects)
{
    TRHF_COUNT(VT_RSSetScissorRects);
    const TRHF_PFN_RSSetScissorRects o = TRHF_ORIG(TRHF_PFN_RSSetScissorRects, VT_RSSetScissorRects);
    if (t_inOverlay)
    {
        o(This, NumRects, pRects);
        return;
    }
    o(This, NumRects, pRects);
    Rearm(This);
    if (!LoggingActive())
        return;

    const UINT n = pRects ? Min<UINT>(NumRects, kMaxVpSc) : 0u;
    Fnv f;
    f.Val(NumRects);
    for (UINT i = 0; i < n; ++i)
        f.Val(pRects[i]);

    bool cap = false, capNew = false;
    AcquireSRWLockExclusive(&g_stateLock);
    if (n > 0)
    {
        g_state.sc0 = pRects[0];
        g_state.hasSc = true;
    }
    const bool isNew = DedupInsertLocked(CAT_SC, f.h, cap);
    if (g_capActive)
        capNew = CapSetInsertLocked(g_capScSet, f.h);
    ReleaseSRWLockExclusive(&g_stateLock);

    if (cap)
        LogCapReached(CAT_SC);
    if (!isNew && !capNew)
        return;
    LineBuf lb;
    lb.Add(" n=%u", NumRects);
    for (UINT i = 0; i < n; ++i)
        lb.Add(" | %u: l=%ld t=%ld r=%ld b=%ld", i, pRects[i].left, pRects[i].top, pRects[i].right, pRects[i].bottom);
    if (isNew)
        Log("[SC] RSSetScissorRects%s", lb.s);
    if (capNew && CapTakeLines(1))
        Log("[CAP-SC] t=%lu f=%ld%s", CapTime(), static_cast<LONG>(g_capFrame), lb.s);
}

// --- [48] UpdateSubresource ---------------------------------------------------------
static __declspec(noinline) void UpdateDiag(ID3D11DeviceContext* This, TRHF_PFN_UpdateSubresource o,
                                            ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
                                            const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, bool cap,
                                            bool samp);

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext* This, ID3D11Resource* pDstResource,
                                                     UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData,
                                                     UINT SrcRowPitch, UINT SrcDepthPitch)
{
    TRHF_COUNT(VT_UpdateSubresource);
    const TRHF_PFN_UpdateSubresource o = TRHF_ORIG(TRHF_PFN_UpdateSubresource, VT_UpdateSubresource);
    if (t_inOverlay)
    {
        o(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
        return;
    }
    if (g_fixInfra)
        FixOnUpdate(pDstResource, DstSubresource, pDstBox, pSrcData);
    const bool diag = LoggingActive() && pDstResource && DstSubresource == 0 && pSrcData;
    const bool cap = diag && CaptureActive();
    // Watchdog sample frame: learned HUD VB/IB only (pointer compare, no QI otherwise)
    const bool samp = diag && WatchSampling() && WatchBufContains(pDstResource);
    if (!diag || (g_cfg.cbDumpSamples == 0 && !cap && !samp))
    {
        o(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
        Rearm(This);
        return;
    }
    UpdateDiag(This, o, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, cap, samp);
}

// v1.4.6: diagnostics part of Hook_UpdateSubresource (logging on). Separate,
// non-inlined, so the 4 KB copy buffer (__chkstk) is not in the hot hook's
// frame. Calls the original and Rearm itself (same order as before).
static __declspec(noinline) void UpdateDiag(ID3D11DeviceContext* This, TRHF_PFN_UpdateSubresource o,
                                            ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
                                            const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, bool cap,
                                            bool samp)
{
    D3D11_BUFFER_DESC d = {};
    const void* buf = QueryBuffer(pDstResource, &d);
    const bool isCB = buf && (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
    const bool isVB = buf && (d.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) != 0;

    if (isVB && (cap || samp))
    {
        // Buffers: the box is a byte range [left, right).
        const UINT off = pDstBox ? pDstBox->left : 0u;
        const UINT len = pDstBox ? (pDstBox->right > pDstBox->left ? pDstBox->right - pDstBox->left : 0u) : d.ByteWidth;
        VbShadowStore(buf, d.ByteWidth, static_cast<UINT>(d.Usage), d.BindFlags, pSrcData, off, len);
    }
    if (!isCB || pDstBox)
    {
        o(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
        Rearm(This);
        return;
    }

    int slot = -1;
    UINT sample = 0;
    AcquireSRWLockExclusive(&g_stateLock);
    slot = SlotMapGetLocked(buf);
    sample = CbTakeDumpLocked(CbSignature(d.ByteWidth, slot));
    ReleaseSRWLockExclusive(&g_stateLock);

    unsigned char tmp[kMaxCBDumpBytes];
    const UINT nDump = sample ? Min<UINT>(d.ByteWidth, g_cfg.cbDumpMaxBytes) : 0u;
    const UINT nCap = cap ? Min<UINT>(d.ByteWidth, g_cfg.captureMaxCBBytes) : 0u;
    const UINT n = Max(nDump, nCap);
    const bool ok = n > 0 && SafeCopy(tmp, pSrcData, n);

    o(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    Rearm(This);

    if (ok && nDump)
        LogCBData("UpdateSubresource", buf, d.ByteWidth, slot, sample, tmp, nDump);
    if (ok && nCap)
        CaptureCBUpdate(buf, d.ByteWidth, tmp, nCap);
}

// =============================================================================
//  Context table installation
// =============================================================================
struct HookDef
{
    UINT        index;
    void*       hook;
    const char* name;
};

static const HookDef g_hookDefs[] = {
    { VT_VSSetConstantBuffers,         reinterpret_cast<void*>(&Hook_VSSetConstantBuffers),         "VSSetConstantBuffers" },
    { VT_PSSetConstantBuffers,         reinterpret_cast<void*>(&Hook_PSSetConstantBuffers),         "PSSetConstantBuffers" },
    { VT_IASetInputLayout,             reinterpret_cast<void*>(&Hook_IASetInputLayout),             "IASetInputLayout" },
    { VT_IASetVertexBuffers,           reinterpret_cast<void*>(&Hook_IASetVertexBuffers),           "IASetVertexBuffers" },
    { VT_IASetIndexBuffer,             reinterpret_cast<void*>(&Hook_IASetIndexBuffer),             "IASetIndexBuffer" },
    { VT_VSSetShader,                  reinterpret_cast<void*>(&Hook_VSSetShader),                  "VSSetShader" },
    { VT_PSSetShader,                  reinterpret_cast<void*>(&Hook_PSSetShader),                  "PSSetShader" },
    { VT_IASetPrimitiveTopology,       reinterpret_cast<void*>(&Hook_IASetPrimitiveTopology),       "IASetPrimitiveTopology" },
    { VT_DrawIndexed,                  reinterpret_cast<void*>(&Hook_DrawIndexed),                  "DrawIndexed" },
    { VT_Draw,                         reinterpret_cast<void*>(&Hook_Draw),                         "Draw" },
    { VT_DrawIndexedInstanced,         reinterpret_cast<void*>(&Hook_DrawIndexedInstanced),         "DrawIndexedInstanced" },
    { VT_DrawInstanced,                reinterpret_cast<void*>(&Hook_DrawInstanced),                "DrawInstanced" },
    { VT_DrawIndexedInstancedIndirect, reinterpret_cast<void*>(&Hook_DrawIndexedInstancedIndirect), "DrawIndexedInstancedIndirect" },
    { VT_DrawInstancedIndirect,        reinterpret_cast<void*>(&Hook_DrawInstancedIndirect),        "DrawInstancedIndirect" },
    { VT_Map,                          reinterpret_cast<void*>(&Hook_Map),                          "Map" },
    { VT_Unmap,                        reinterpret_cast<void*>(&Hook_Unmap),                        "Unmap" },
    { VT_RSSetViewports,               reinterpret_cast<void*>(&Hook_RSSetViewports),               "RSSetViewports" },
    { VT_RSSetScissorRects,            reinterpret_cast<void*>(&Hook_RSSetScissorRects),            "RSSetScissorRects" },
    { VT_UpdateSubresource,            reinterpret_cast<void*>(&Hook_UpdateSubresource),            "UpdateSubresource" },
};
static const UINT kHookCount = static_cast<UINT>(sizeof(g_hookDefs) / sizeof(g_hookDefs[0]));

static SRWLOCK g_hookLock = SRWLOCK_INIT;

// True if [p, p+size) is committed RW / RWX memory within one region.
static bool IsWritableRange(const void* p, SIZE_T size)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD prot = mbi.Protect & 0xFFu;
    if (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE)
        return false;
    const UINT_PTR end = reinterpret_cast<UINT_PTR>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<UINT_PTR>(p) + size <= end;
}

static DWORD WritableProtFor(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) &&
        (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return PAGE_EXECUTE_READWRITE;
    return PAGE_READWRITE;
}

static void InstallContextHooks(ID3D11DeviceContext* ctx, const char* origin)
{
    if (!ctx || !g_active || g_shutdown)
        return;
    if (!g_fixInfra && !g_cfg.enableLogging)
    {
        Log("[HOOK] EnableFix=0 and EnableLogging=0 - context hooks not installed (%s)", origin);
        return;
    }

    void** vtbl = *reinterpret_cast<void***>(ctx);
    if (!vtbl)
        return;

    const SIZE_T size = VT_COUNT * sizeof(void*);
    UINT patched = 0;
    bool reused = false, evicted = false, writable = false, ok = true;
    DWORD protErr = 0;
    HookedTable* t = nullptr;

    AcquireSRWLockExclusive(&g_hookLock);
    for (UINT k = 0; k < kHookCount; ++k)
        g_hookByIdx[g_hookDefs[k].index] = g_hookDefs[k].hook;

    const LONG cnt = g_tableCount;
    for (LONG i = 0; i < cnt; ++i)
    {
        if (g_tables[i].vtbl == vtbl)
        {
            t = &g_tables[i]; // same address again (e.g. new context reusing freed memory)
            reused = true;
            break;
        }
    }
    if (!t)
    {
        if (cnt < static_cast<LONG>(kMaxTables))
        {
            t = &g_tables[cnt];
        }
        else
        {
            t = &g_tables[g_tableEvict++ % kMaxTables];
            t->vtbl = nullptr;
            evicted = true;
        }
        for (UINT i = 0; i < VT_COUNT; ++i)
            t->orig[i] = nullptr;
    }

    writable = IsWritableRange(vtbl, size);

    // Fix state belongs to the (new) context living at this table address.
    if (t->fixCB)
    {
        t->fixCB->Release();
        t->fixCB = nullptr;
    }
    t->vsS0 = nullptr;
    t->vsHash = 0;
    t->hudVs = false;
    t->psHash = 0;
    t->gateFrame = -1;
    t->drawsBefore = 0;
    t->seenHud = false;
    t->menuThis = false;
    t->menuReason = 0;
    t->lastReason = 0;
    t->menuHold = 0;
    t->grp = 0;
    t->uTriggerThisFrame = false;
    t->uTriggerPrevFrame = false;
    t->gateLogged = false;
    for (UINT i = 0; i < VT_COUNT; ++i)
        t->rejected[i] = nullptr; // v1.4.6

    // Pass 1: originals of this table (published before any entry is swapped).
    for (UINT k = 0; k < kHookCount; ++k)
    {
        const HookDef& hd = g_hookDefs[k];
        void* cur = vtbl[hd.index];
        if (cur == hd.hook)
        {
            if (!t->orig[hd.index])
                t->orig[hd.index] = g_orig[hd.index];
            continue;
        }
        t->orig[hd.index] = cur;
        if (!g_orig[hd.index])
            g_orig[hd.index] = cur;
    }
    t->writable = writable;
    t->vtbl = vtbl;
    if (!reused && !evicted)
        InterlockedIncrement(&g_tableCount);

    // Pass 2: swap the entries.
    DWORD oldProt = 0;
    bool protChanged = false;
    if (!writable)
    {
        protChanged = VirtualProtect(vtbl, size, WritableProtFor(vtbl), &oldProt) != FALSE;
        if (!protChanged)
        {
            ok = false;
            protErr = GetLastError();
        }
    }
    if (ok)
    {
        for (UINT k = 0; k < kHookCount; ++k)
        {
            const HookDef& hd = g_hookDefs[k];
            if (vtbl[hd.index] == hd.hook)
                continue;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&vtbl[hd.index]),
                                static_cast<LONG>(reinterpret_cast<LONG_PTR>(hd.hook)));
            ++patched;
        }
        if (protChanged)
        {
            DWORD dummy = 0;
            VirtualProtect(vtbl, size, oldProt, &dummy);
        }
    }
    ReleaseSRWLockExclusive(&g_hookLock);

    if (!LoggingActive())
        return;
    if (!ok)
    {
        Log("[HOOK] context %p table %p NOT patched: VirtualProtect failed (err %lu) (%s)", static_cast<void*>(ctx),
            static_cast<void*>(vtbl), protErr, origin);
        return;
    }
    Log("[HOOK] context %p table %p (%s%s, %s): %u/%u entries patched, draw re-arm %s (%s)", static_cast<void*>(ctx),
        static_cast<void*>(vtbl), reused ? "existing table address" : "new table", evicted ? ", evicted oldest" : "",
        writable ? "RW memory" : "read-only, VirtualProtect", patched, kHookCount,
        !g_rearmOn ? "disabled" : (writable ? "on" : "OFF (table not writable)"), origin);
    for (UINT k = 0; k < kHookCount; ++k)
        Log("[HOOK]   [%2u] %-28s orig=%p", g_hookDefs[k].index, g_hookDefs[k].name,
            static_cast<void*>(t->orig[g_hookDefs[k].index]));
}

// v1.4.3 performance A/B test: (un)patch the context hooks of the game's
// immediate context live. Called from the Present hook (render thread).
static bool IsDrawIdx(UINT idx)
{
    for (UINT k = 0; k < kDrawIdxCount; ++k)
    {
        if (kDrawIdx[k] == idx)
            return true;
    }
    return false;
}

static int PerfGroupOf(UINT idx)
{
    if (IsDrawIdx(idx))
        return PG_DRAW;
    switch (idx)
    {
    case VT_VSSetConstantBuffers: case VT_PSSetConstantBuffers: return PG_CB;
    case VT_VSSetShader: case VT_PSSetShader:                   return PG_SHADER;
    case VT_IASetInputLayout: case VT_IASetVertexBuffers:
    case VT_IASetIndexBuffer: case VT_IASetPrimitiveTopology:   return PG_IA;
    case VT_Map: case VT_Unmap: case VT_UpdateSubresource:      return PG_MAP;
    default:                                                    return PG_RS; // RSSetViewports / RSSetScissorRects
    }
}

// Mode 0 all hooks, 1 all but draw, 2 none, 3..7 only group PG_CB..PG_RS.
static bool PerfWantHook(LONG mode, UINT idx)
{
    if (mode == 0)
        return true;
    if (mode == 1)
        return !IsDrawIdx(idx);
    if (mode == 2)
        return false;
    if (mode == 8)
        return idx == VT_Map;
    if (mode == 9)
        return idx == VT_Unmap;
    if (mode == 10)
        return idx == VT_UpdateSubresource;
    return PerfGroupOf(idx) == static_cast<int>(mode - 3 + PG_CB);
}

static void PerfModeLabel(LONG m, char* out, size_t cap)
{
    if (m == 0)
        _snprintf_s(out, cap, _TRUNCATE, "Normal: all hooks");
    else if (m == 1)
        _snprintf_s(out, cap, _TRUNCATE, "Test B: draw hooks off");
    else if (m == 2)
        _snprintf_s(out, cap, _TRUNCATE, "Test A: all hooks off");
    else if (m >= 8)
        _snprintf_s(out, cap, _TRUNCATE, "Test %ld: only '%s' hook on", m,
                    m == 8 ? "Map" : (m == 9 ? "Unmap" : "UpdateSubresource"));
    else
        _snprintf_s(out, cap, _TRUNCATE, "Test %ld: only '%s' hooks on", m, kPerfGroupNames[m - 3 + PG_CB]);
}

// v1.4.3 bench diagnostics: vtable entries seen at mode switches (written to
// tr_hud_fix_perf.txt). Slots: Map, Unmap, UpdateSubresource.
static const UINT kTraceIdx[3] = { VT_Map, VT_Unmap, VT_UpdateSubresource };
struct PerfTrace { LONG from, to; void* vtbl; void* cur[3]; void* orig[3]; };
static PerfTrace g_perfTrace[32] = {};
static UINT      g_perfTraceCount = 0;

static void PerfModeApply(IDXGISwapChain* sc, LONG mode)
{
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev)
        return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx); // AddRefs
    dev->Release();
    if (!ctx)
        return;
    void** vtbl = *reinterpret_cast<void***>(ctx);
    const SIZE_T size = VT_COUNT * sizeof(void*);
    UINT changed = 0, foreign = 0;
    bool found = false, ok = false;
    const bool modeChanged = (mode != g_perfMode);
    AcquireSRWLockExclusive(&g_hookLock);
    HookedTable* t = vtbl ? FindTable(vtbl) : nullptr;
    if (t)
    {
        found = true;
        DWORD oldProt = 0;
        bool prot = false;
        ok = t->writable;
        if (!ok)
            ok = prot = VirtualProtect(vtbl, size, WritableProtFor(vtbl), &oldProt) != FALSE;
        if (g_perfTraceCount < 32)
        {
            PerfTrace& tr = g_perfTrace[g_perfTraceCount++];
            tr.from = g_perfMode;
            tr.to = mode;
            tr.vtbl = vtbl;
            for (UINT j = 0; j < 3; ++j)
            {
                tr.cur[j] = vtbl[kTraceIdx[j]];
                tr.orig[j] = t->orig[kTraceIdx[j]];
            }
        }
        if (ok)
        {
            InterlockedExchange(&g_perfMode, mode); // first: stops Rearm in modes 1/2
            for (UINT k = 0; k < kHookCount; ++k)
            {
                const HookDef& hd = g_hookDefs[k];
                const bool want = PerfWantHook(mode, hd.index);
                void* cur = vtbl[hd.index];
                void* put = nullptr;
                if (want && cur && cur != hd.hook)
                {
                    // v1.4.6: adopt only a same-module entry (runtime rewrite), never a foreign hook.
                    if (t->orig[hd.index] && ModuleOf(cur) != ModuleOf(t->orig[hd.index]))
                    {
                        ++foreign;
                    }
                    else
                    {
                        t->orig[hd.index] = cur; // runtime's current entry becomes the original
                        put = hd.hook;
                    }
                }
                else if (!want && cur == hd.hook && t->orig[hd.index])
                {
                    put = t->orig[hd.index];
                }
                if (put)
                {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vtbl[hd.index]),
                                        static_cast<LONG>(reinterpret_cast<LONG_PTR>(put)));
                    ++changed;
                }
            }
            if (prot)
            {
                DWORD dummy = 0;
                VirtualProtect(vtbl, size, oldProt, &dummy);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    ctx->Release();
    if (modeChanged)
        FixMapsReset(); // v1.4.6: no open redirected map survives a hook-set change
    if (foreign)
    {
        InterlockedExchangeAdd(&g_rearmForeign, static_cast<LONG>(foreign));
        static volatile LONG s_logged = 0;
        if (LoggingActive() && InterlockedExchange(&s_logged, 1) == 0)
            Log("[PERF] %u context entry/entries replaced by another module - left alone", foreign);
    }
    char label[64];
    PerfModeLabel(mode, label, sizeof(label));
    Log("[PERF] hook test mode %ld (%s): %u entries changed%s", mode, label, changed,
        !found ? " - game context table not found" : (!ok ? " - VirtualProtect failed" : ""));
}

// =============================================================================
//  Object-table hooks: IDXGIFactory::CreateSwapChain (10), IDXGISwapChain
//  Present (8) / SetFullscreenState (10) / ResizeBuffers (13) / ResizeTarget
//  (14), ID3D11Device CreateInputLayout (11) / CreateVertexShader (12) /
//  CreatePixelShader (15). One original per table. Writable (per-instance,
//  heap) tables are patched directly, read-only class vtables via VirtualProtect.
// =============================================================================
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_SetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_GetFullscreenState)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_CreateInputLayout)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_CreateVertexShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
typedef HRESULT (STDMETHODCALLTYPE* TRHF_PFN_CreatePixelShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

static const UINT kMaxClassIdx    = 5;
static const UINT kMaxClassTables = 16;

struct ClassTable
{
    void** vtbl;
    void*  orig[kMaxClassIdx];
};
struct ClassHookSet
{
    const char*   name;
    UINT          n;
    UINT          idx[kMaxClassIdx];
    const char*   fn[kMaxClassIdx];
    ClassTable    tables[kMaxClassTables];
    volatile LONG count;
};
enum { SW_Present, SW_SetFullscreenState, SW_GetFullscreenState, SW_ResizeBuffers, SW_ResizeTarget };
enum { DV_CreateInputLayout, DV_CreateVertexShader, DV_CreatePixelShader };

static ClassHookSet g_factoryHooks = { "IDXGIFactory", 1, { 10, 0, 0, 0, 0 },
                                       { "CreateSwapChain", nullptr, nullptr, nullptr, nullptr }, {}, 0 };
static ClassHookSet g_swapHooks    = { "IDXGISwapChain", 5, { 8, 10, 11, 13, 14 },
                                       { "Present", "SetFullscreenState", "GetFullscreenState", "ResizeBuffers",
                                         "ResizeTarget" }, {}, 0 };
static ClassHookSet g_deviceHooks  = { "ID3D11Device", 3, { 11, 12, 15, 0, 0 },
                                       { "CreateInputLayout", "CreateVertexShader", "CreatePixelShader", nullptr,
                                         nullptr }, {}, 0 };

static void* ClassOrig(ClassHookSet& s, void* obj, UINT k)
{
    void** vtbl = *reinterpret_cast<void***>(obj);
    const LONG n = s.count;
    for (LONG i = 0; i < n; ++i)
    {
        if (s.tables[i].vtbl == vtbl)
            return s.tables[i].orig[k];
    }
    return n > 0 ? s.tables[0].orig[k] : nullptr;
}

static HMODULE ModuleOf(const void* p)
{
    HMODULE h = nullptr;
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(p), &h))
        return nullptr;
    return h;
}

static void InstallClassHooks(ClassHookSet& s, void* const* hooks, void* obj, const char* origin)
{
    if (!obj || !g_active || g_shutdown)
        return;
    void** vtbl = *reinterpret_cast<void***>(obj);
    if (!vtbl)
        return;
    UINT lo = s.idx[0], hi = s.idx[0];
    for (UINT k = 0; k < s.n; ++k)
    {
        lo = Min(lo, s.idx[k]);
        hi = Max(hi, s.idx[k]);
    }
    void** base = vtbl + lo;
    const SIZE_T size = (hi - lo + 1u) * sizeof(void*);

    bool isNew = false, full = false, ok = true, writable = false;
    DWORD err = 0;
    UINT foreign = 0;
    bool patchedK[kMaxClassIdx] = {};
    void* origLog[kMaxClassIdx] = {};
    ClassTable* t = nullptr;

    AcquireSRWLockExclusive(&g_hookLock);
    const LONG n = s.count;
    for (LONG i = 0; i < n; ++i)
    {
        if (s.tables[i].vtbl == vtbl)
        {
            t = &s.tables[i];
            break;
        }
    }
    if (!t)
    {
        if (n >= static_cast<LONG>(kMaxClassTables))
        {
            full = true;
        }
        else
        {
            t = &s.tables[n];
            isNew = true;
        }
    }
    if (t)
    {
        bool want[kMaxClassIdx] = {};
        bool any = false;
        for (UINT k = 0; k < s.n; ++k)
        {
            void* cur = vtbl[s.idx[k]];
            if (cur == hooks[k])
            {
                if (isNew)
                    t->orig[k] = n > 0 ? s.tables[0].orig[k] : nullptr;
                continue;
            }
            // Known table whose entry changed: re-patch only if the new entry
            // lives in the same module as the old original (fresh object at a
            // reused address). A foreign hook installed after ours is left
            // alone - chaining to it would recurse into ourselves.
            if (!isNew && t->orig[k] && ModuleOf(cur) != ModuleOf(t->orig[k]))
            {
                ++foreign;
                continue;
            }
            t->orig[k] = cur;
            want[k] = true;
            any = true;
        }
        t->vtbl = vtbl;
        if (isNew)
            InterlockedIncrement(&s.count); // publish originals before swapping entries

        if (any)
        {
            writable = IsWritableRange(base, size);
            DWORD oldProt = 0;
            bool changed = false;
            if (!writable)
            {
                changed = VirtualProtect(base, size, WritableProtFor(base), &oldProt) != FALSE;
                if (!changed)
                {
                    ok = false;
                    err = GetLastError();
                }
            }
            if (ok)
            {
                for (UINT k = 0; k < s.n; ++k)
                {
                    if (!want[k])
                        continue;
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vtbl[s.idx[k]]),
                                        static_cast<LONG>(reinterpret_cast<LONG_PTR>(hooks[k])));
                    patchedK[k] = true;
                }
                if (changed)
                {
                    DWORD dummy = 0;
                    VirtualProtect(base, size, oldProt, &dummy);
                }
            }
        }
        for (UINT k = 0; k < s.n; ++k)
            origLog[k] = t->orig[k];
    }
    ReleaseSRWLockExclusive(&g_hookLock);

    if (!LoggingActive())
        return;
    if (full)
    {
        Log("[HOOK] %s table %p NOT hooked: too many distinct tables (%s)", s.name, static_cast<void*>(vtbl), origin);
        return;
    }
    if (!ok)
    {
        Log("[HOOK] %s table %p NOT hooked: VirtualProtect failed (err %lu) (%s)", s.name, static_cast<void*>(vtbl),
            err, origin);
        return;
    }
    if (foreign)
        Log("[HOOK] %s table %p: %u entry/entries replaced by another module - left alone (%s)", s.name,
            static_cast<void*>(vtbl), foreign, origin);
    for (UINT k = 0; k < s.n; ++k)
    {
        if (patchedK[k])
            Log("[HOOK] %s table %p [%u] %s hooked (%s, orig=%p) (%s)", s.name, static_cast<void*>(vtbl), s.idx[k],
                s.fn[k], writable ? "RW table" : "VirtualProtect", origLog[k], origin);
    }
}

// --- Shader / input-layout registry ---------------------------------------------------
static uint64_t HashBytesSafe(const void* p, size_t n)
{
    __try
    {
        Fnv f;
        if (p && n)
            f.Bytes(p, n);
        const uint64_t len = n;
        f.Val(len);
        return f.h;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void RegisterShader(const char* kind, const void* shader, const void* code, SIZE_T len)
{
    const bool diag = LoggingActive();
    if (!shader || !g_shaders || !(diag || g_fixInfra))
        return;
    const uint64_t h = HashBytesSafe(code, len);
    const UINT size = static_cast<UINT>(len);
    bool doLog = false, capNow = false;
    AcquireSRWLockExclusive(&g_stateLock);
    try
    {
        (*g_shaders)[shader] = ShaderInfo{ h, size };
    }
    catch (...)
    {
    }
    if (diag)
    {
        if (g_shaderLogCount < kShaderLogCap)
        {
            ++g_shaderLogCount;
            doLog = true;
        }
        else if (!g_shaderCapLogged)
        {
            g_shaderCapLogged = true;
            capNow = true;
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    if (doLog)
        Log("[SHADER] %s ptr=%p hash=%016llX size=%u%s", kind, shader, h, size, IsHudVsHash(h) ? " (HUD VS)" : "");
    if (capNow)
        Log("[SHADER] log cap (%u) reached - further shaders are hashed but not logged", kShaderLogCap);
}

// Byte size of common vertex formats (for D3D11_APPEND_ALIGNED_ELEMENT), 0 = unknown.
static UINT FormatBytes(DXGI_FORMAT fmt)
{
    const UINT f = static_cast<UINT>(fmt);
    if (f >= 1u && f <= 4u)   return 16; // R32G32B32A32
    if (f >= 5u && f <= 8u)   return 12; // R32G32B32
    if (f >= 9u && f <= 22u)  return 8;  // R16G16B16A16, R32G32, R32G8X24
    if (f >= 23u && f <= 47u) return 4;  // R10G10B10A2, R11G11B10, R8G8B8A8, R16G16, R32, R24G8
    if (f >= 48u && f <= 59u) return 2;  // R8G8, R16
    if (f >= 60u && f <= 65u) return 1;  // R8, A8
    if (f >= 87u && f <= 93u) return 4;  // B8G8R8A8 / B8G8R8X8 variants
    return 0;
}

static void RegisterLayout(const void* layout, const D3D11_INPUT_ELEMENT_DESC* e, UINT num)
{
    if (!layout || !LoggingActive() || !g_layouts)
        return;
    LayoutInfo li = {};
    UINT slotOff[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    bool slotUnknown[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    const UINT kSlots = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    Fnv f;
    LineBuf lb;
    const UINT cnt = e ? num : 0u;
    for (UINT i = 0; i < cnt; ++i)
    {
        const D3D11_INPUT_ELEMENT_DESC& el = e[i];
        const char* name = el.SemanticName ? el.SemanticName : "";
        const size_t nameLen = strnlen(name, 64);
        const UINT slot = el.InputSlot;
        const bool append = (el.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT);
        UINT off = el.AlignedByteOffset;
        bool offKnown = true;
        if (append)
        {
            if (slot < kSlots && !slotUnknown[slot])
                off = slotOff[slot];
            else
                offKnown = false;
        }
        if (slot < kSlots)
        {
            const UINT sz = FormatBytes(el.Format);
            if (offKnown && sz)
            {
                slotOff[slot] = off + sz;
                slotUnknown[slot] = false;
            }
            else
            {
                slotUnknown[slot] = true;
            }
        }
        f.Bytes(name, nameLen);
        f.Val(el.SemanticIndex);
        f.Val(el.Format);
        f.Val(slot);
        f.Val(off);
        f.Val(el.InputSlotClass);
        f.Val(el.InstanceDataStepRate);
        if (!li.hasPos && el.SemanticIndex == 0 && (_stricmp(name, "POSITION") == 0 || _stricmp(name, "SV_Position") == 0))
        {
            li.hasPos = true;
            li.posFmt = el.Format;
            li.posSlot = slot;
            li.posOffset = off;
            li.posOffsetKnown = offKnown;
        }
        if (offKnown)
            lb.Add(" | %.63s%u fmt=%u slot=%u off=%u%s cls=%u step=%u", name, el.SemanticIndex,
                   static_cast<UINT>(el.Format), slot, off, append ? "(append)" : "",
                   static_cast<UINT>(el.InputSlotClass), el.InstanceDataStepRate);
        else
            lb.Add(" | %.63s%u fmt=%u slot=%u off=?(append) cls=%u step=%u", name, el.SemanticIndex,
                   static_cast<UINT>(el.Format), slot, static_cast<UINT>(el.InputSlotClass), el.InstanceDataStepRate);
    }
    li.hash = f.h;
    li.numElems = cnt;

    AcquireSRWLockExclusive(&g_stateLock);
    li.id = ++g_layoutNextId;
    try
    {
        (*g_layouts)[layout] = li;
    }
    catch (...)
    {
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    Log("[LAYOUT] id=%u ptr=%p hash=%016llX elems=%u%s", li.id, layout, li.hash, cnt, lb.s);
}

// --- ID3D11Device hooks (pass-through) ------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_CreateInputLayout(ID3D11Device* This, const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs,
                                                        UINT NumElements, const void* pShaderBytecodeWithInputSignature,
                                                        SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout)
{
    const TRHF_PFN_CreateInputLayout o =
        reinterpret_cast<TRHF_PFN_CreateInputLayout>(ClassOrig(g_deviceHooks, This, DV_CreateInputLayout));
    if (!o)
        return E_FAIL; // v1.4.6: no original known (should not happen)
    if (t_inOverlay)
        return o(This, pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);
    const HRESULT hr =
        o(This, pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);
    if (SUCCEEDED(hr) && ppInputLayout && *ppInputLayout)
        RegisterLayout(*ppInputLayout, pInputElementDescs, NumElements);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateVertexShader(ID3D11Device* This, const void* pShaderBytecode,
                                                         SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage,
                                                         ID3D11VertexShader** ppVertexShader)
{
    const TRHF_PFN_CreateVertexShader o =
        reinterpret_cast<TRHF_PFN_CreateVertexShader>(ClassOrig(g_deviceHooks, This, DV_CreateVertexShader));
    if (!o)
        return E_FAIL;
    if (t_inOverlay)
        return o(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
    const HRESULT hr = o(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
    if (SUCCEEDED(hr) && ppVertexShader && *ppVertexShader)
        RegisterShader("vs", *ppVertexShader, pShaderBytecode, BytecodeLength);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePixelShader(ID3D11Device* This, const void* pShaderBytecode,
                                                        SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage,
                                                        ID3D11PixelShader** ppPixelShader)
{
    const TRHF_PFN_CreatePixelShader o =
        reinterpret_cast<TRHF_PFN_CreatePixelShader>(ClassOrig(g_deviceHooks, This, DV_CreatePixelShader));
    if (!o)
        return E_FAIL;
    if (t_inOverlay)
        return o(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
    const HRESULT hr = o(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
    if (SUCCEEDED(hr) && ppPixelShader && *ppPixelShader)
        RegisterShader("ps", *ppPixelShader, pShaderBytecode, BytecodeLength);
    return hr;
}

// --- Swap chain ---------------------------------------------------------------------------
static void LogSwapChainDesc(const char* tag, const DXGI_SWAP_CHAIN_DESC* d)
{
    if (!LoggingActive())
        return;
    RECT rc = {};
    const BOOL haveRc = d->OutputWindow ? GetClientRect(d->OutputWindow, &rc) : FALSE;
    Log("[DEVICE] %s swapchain: %ux%u fmt=%d refresh=%u/%u buffers=%u msaa=%u/%u windowed=%d swapEffect=%d "
        "flags=0x%X hwnd=%p client=%s%ldx%ld",
        tag, d->BufferDesc.Width, d->BufferDesc.Height, static_cast<int>(d->BufferDesc.Format),
        d->BufferDesc.RefreshRate.Numerator, d->BufferDesc.RefreshRate.Denominator, d->BufferCount,
        d->SampleDesc.Count, d->SampleDesc.Quality, d->Windowed ? 1 : 0, static_cast<int>(d->SwapEffect), d->Flags,
        static_cast<void*>(d->OutputWindow), haveRc ? "" : "n/a:", static_cast<long>(rc.right - rc.left),
        static_cast<long>(rc.bottom - rc.top));
}

// Window / fullscreen / monitor state (logged at capture start).
static void LogWindowInfo(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC d = {};
    HWND hwnd = nullptr;
    if (SUCCEEDED(sc->GetDesc(&d)))
    {
        LogSwapChainDesc("capture", &d);
        hwnd = d.OutputWindow;
    }

    BOOL fs = FALSE;
    IDXGIOutput* out = nullptr;
    const HRESULT hr = sc->GetFullscreenState(&fs, &out);
    Log("[WINDOW] GetFullscreenState hr=0x%08lX fullscreen=%d output=%p", static_cast<unsigned long>(hr), fs ? 1 : 0,
        static_cast<void*>(out));
    if (out)
    {
        DXGI_OUTPUT_DESC od = {};
        if (SUCCEEDED(out->GetDesc(&od)))
        {
            char u8[128];
            Log("[WINDOW] output %s desktop=(%ld,%ld)-(%ld,%ld) attached=%d rotation=%d",
                ToUtf8(od.DeviceName, u8, sizeof(u8)), od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, od.AttachedToDesktop ? 1 : 0,
                static_cast<int>(od.Rotation));
        }
        out->Release();
    }

    if (!hwnd)
        return;
    RECT cr = {}, wr = {};
    POINT org = { 0, 0 };
    GetClientRect(hwnd, &cr);
    GetWindowRect(hwnd, &wr);
    ClientToScreen(hwnd, &org);
    Log("[WINDOW] hwnd=%p client=%ldx%ld at screen (%ld,%ld) window=(%ld,%ld)-(%ld,%ld) style=0x%08lX exstyle=0x%08lX",
        static_cast<void*>(hwnd), cr.right - cr.left, cr.bottom - cr.top, org.x, org.y, wr.left, wr.top, wr.right,
        wr.bottom, static_cast<unsigned long>(GetWindowLongW(hwnd, GWL_STYLE)),
        static_cast<unsigned long>(GetWindowLongW(hwnd, GWL_EXSTYLE)));

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
    {
        char u8[128];
        Log("[WINDOW] monitor %s rect=(%ld,%ld)-(%ld,%ld) work=(%ld,%ld)-(%ld,%ld) primary=%d",
            ToUtf8(mi.szDevice, u8, sizeof(u8)), mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right,
            mi.rcMonitor.bottom, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom,
            (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
    }
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    TRHF_PFN_GetDpiForWindow pGetDpiForWindow =
        u32 ? reinterpret_cast<TRHF_PFN_GetDpiForWindow>(GetProcAddress(u32, "GetDpiForWindow")) : nullptr;
    if (pGetDpiForWindow)
        Log("[WINDOW] GetDpiForWindow=%u", pGetDpiForWindow(hwnd));
}

static volatile LONG g_firstPresentLogged = 0;
static IDXGISwapChain* volatile g_gameSc = nullptr; // identity of the game swap chain (menu target)
static void OverlayBeforePresent(IDXGISwapChain* sc); // overlay section
static void OverlayReleaseRTV();
static volatile LONG g_dxgiLogCount = 0;
static volatile LONG g_firstSwapChain = 0;

static bool DxgiLogAllowed() { return LoggingActive() && InterlockedIncrement(&g_dxgiLogCount) <= 256; }

// =============================================================================
//  v1.1 performance: frame-time statistics and FPS limiter (Present on the
//  game swap chain). Fixed buffers only - no allocation per frame.
// =============================================================================
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static const UINT kFtRing = 240;
static float    g_ftRing[kFtRing] = {};
static UINT     g_ftPos = 0;
static UINT     g_ftCount = 0;
static LONGLONG g_qpcFreq = 0;
static LONGLONG g_ftLastQpc = 0;
static UINT     g_perfFramesSince = 0;
static LONGLONG g_perfLastCalc = 0;
static float    g_perfFps = 0.0f;
static float    g_perfMs = 0.0f;
static float    g_perfLow1 = 0.0f;
static float    g_perfPlot[kFtRing] = {}; // chronological copy for the menu graph
static UINT     g_perfPlotCount = 0;
// v1.4.6: set each Present by OverlayBeforePresent (menu, FPS overlay or
// benchmark shown); the frame-time statistics (240-float sort) only run then.
static bool     g_perfStatsWanted = false;
static bool     g_perfStatsStale  = true;  // recompute at once when they become wanted

static LONGLONG QpcNow()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

static LONGLONG QpcFreq()
{
    if (!g_qpcFreq)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = (f.QuadPart > 0) ? f.QuadPart : 1;
    }
    return g_qpcFreq;
}

// After the original Present of the game swap chain.
static void PerfOnPresent()
{
    const LONGLONG now = QpcNow();
    const LONGLONG freq = QpcFreq();
    if (g_ftLastQpc)
    {
        const double ms = static_cast<double>(now - g_ftLastQpc) * 1000.0 / static_cast<double>(freq);
        g_ftRing[g_ftPos] = static_cast<float>(ms);
        g_ftPos = (g_ftPos + 1u) % kFtRing;
        if (g_ftCount < kFtRing)
            ++g_ftCount;
    }
    g_ftLastQpc = now;
    ++g_perfFramesSince;
    if (g_ftCount == 0)
        return;
    const bool due = now - g_perfLastCalc >= freq / 2; // recompute every 0.5 s
    const bool want = g_perfStatsWanted;
    if (!due && !(want && g_perfStatsStale))
        return;
    if (due)
    {
        g_perfLastCalc = now;
        LONG sum[PG_COUNT] = {};
        for (UINT i = 0; i < VT_COUNT; ++i)
        {
            if (g_hookByIdx[i])
                sum[PerfGroupOf(i)] += g_callCnt[i];
            g_callCntSnap[i] = g_callCnt[i];
            g_callCnt[i] = 0;
        }
        const float fr = static_cast<float>(g_perfFramesSince ? g_perfFramesSince : 1u);
        for (int gI = 0; gI < PG_COUNT; ++gI)
            g_callPerFrame[gI] = static_cast<float>(sum[gI]) / fr;
        g_callPerFrameMap[0] = static_cast<float>(g_callCntSnap[VT_Map]) / fr;
        g_callPerFrameMap[1] = static_cast<float>(g_callCntSnap[VT_Unmap]) / fr;
        g_callPerFrameMap[2] = static_cast<float>(g_callCntSnap[VT_UpdateSubresource]) / fr;
        g_mapHitPerFrame = static_cast<float>(g_mapHitCnt) / fr;
        g_shadowPerFrame = static_cast<float>(g_shadowCnt) / fr;
        g_redirPerFrame = static_cast<float>(g_redirCnt) / fr;
        g_mapHitCnt = 0;
        g_shadowCnt = 0;
        g_redirCnt = 0;
        g_perfFramesSince = 0;
    }
    if (!want)
    {
        g_perfStatsStale = true;
        return;
    }
    g_perfStatsStale = false;

    float sorted[kFtRing];
    double sum = 0.0;
    const UINT n = g_ftCount;
    const UINT start = (g_ftPos + kFtRing - n) % kFtRing;
    for (UINT i = 0; i < n; ++i)
    {
        const float v = g_ftRing[(start + i) % kFtRing];
        g_perfPlot[i] = v;
        sorted[i] = v;
        sum += v;
    }
    g_perfPlotCount = n;
    std::sort(sorted, sorted + n);
    const double avg = sum / static_cast<double>(n);
    g_perfMs = static_cast<float>(avg);
    g_perfFps = (avg > 0.0) ? static_cast<float>(1000.0 / avg) : 0.0f;
    // 1% low = FPS of the 99th-percentile frame time.
    UINT idx = static_cast<UINT>(ceil(0.99 * static_cast<double>(n)));
    idx = idx ? idx - 1u : 0u;
    g_perfLow1 = (sorted[idx] > 0.0f) ? 1000.0f / sorted[idx] : 0.0f;
}

static HANDLE   g_limTimer = nullptr;
static bool     g_limTimerTried = false;
static bool     g_limTimerHiRes = false;
static LONGLONG g_limNext = 0;
static int      g_limLogged = 0;

static void LimiterInitTimer()
{
    if (g_limTimerTried)
        return;
    g_limTimerTried = true;
    g_limTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    g_limTimerHiRes = (g_limTimer != nullptr);
    if (!g_limTimer)
        g_limTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS); // no timeBeginPeriod
}

// Before the original Present of the game swap chain. FpsLimit 0 = off.
static void LimiterWait()
{
    const int fps = g_cfg.fpsLimit;
    if (fps != g_limLogged)
    {
        g_limLogged = fps;
        g_limNext = 0;
        if (fps > 0)
        {
            LimiterInitTimer();
            Log("[PERF] limiter %d fps (%s timer)", fps,
                g_limTimerHiRes ? "high-resolution" : (g_limTimer ? "standard" : "no"));
        }
        else
        {
            Log("[PERF] limiter off");
        }
    }
    if (fps <= 0)
        return;

    const LONGLONG freq = QpcFreq();
    const LONGLONG period = freq / fps;
    LONGLONG now = QpcNow();
    if (!g_limNext || now - g_limNext > period)
    {
        g_limNext = now + period; // first frame or more than one frame behind: re-sync
        return;
    }
    if (now < g_limNext)
    {
        // Sleep until ~1.5 ms (2 ms with a standard timer) before the target, then spin.
        const LONGLONG margin = g_limTimerHiRes ? (freq * 15) / 10000 : (freq * 2) / 1000;
        const LONGLONG sleepTicks = g_limNext - now - margin;
        if (g_limTimer && sleepTicks > 0)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(static_cast<double>(sleepTicks) * 1.0e7 / static_cast<double>(freq));
            if (due.QuadPart < 0 && SetWaitableTimer(g_limTimer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(g_limTimer, 1000);
        }
        while ((now = QpcNow()) < g_limNext)
            YieldProcessor();
    }
    g_limNext += period;
}

// =============================================================================
//  v1.1 display: borderless fullscreen (fake exclusive fullscreen) and flip
//  model. Both are applied at start ([Display] values read at load).
// =============================================================================
static volatile LONG g_fakeFs = 0;   // borderless: game believes it is fullscreen
static bool g_flipActive    = false; // game swap chain created with FLIP_DISCARD
static bool g_tearingActive = false; // ... and DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING

// Borderless: window style WS_POPUP, covering the monitor of the output window.
static void BorderlessApply(IDXGISwapChain* sc, const char* why)
{
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow)
    {
        Log("[DISPLAY] borderless (%s): no output window", why);
        return;
    }
    const HWND hwnd = d.OutputWindow;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi))
    {
        Log("[DISPLAY] borderless (%s): GetMonitorInfo failed", why);
        return;
    }
    const RECT r = mi.rcMonitor;
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) &
                             ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                                                    WS_EX_STATICEDGE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    const bool sameSize = d.BufferDesc.Width == static_cast<UINT>(r.right - r.left) &&
                          d.BufferDesc.Height == static_cast<UINT>(r.bottom - r.top);
    Log("[DISPLAY] borderless (%s): hwnd=%p monitor (%ld,%ld)-(%ld,%ld), back buffer %ux%u%s", why,
        static_cast<void*>(hwnd), r.left, r.top, r.right, r.bottom, d.BufferDesc.Width, d.BufferDesc.Height,
        sameSize ? "" : " (scaled by DXGI)");
}

// True if the factory supports DXGI_FEATURE_PRESENT_ALLOW_TEARING. factory may
// be null (then a temporary factory from the loaded dxgi.dll is used).
static bool TearingSupported(IDXGIFactory* factory)
{
    typedef HRESULT (WINAPI* PFN_CreateFactory1)(REFIID, void**);
    IDXGIFactory* tmp = nullptr;
    if (!factory)
    {
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        PFN_CreateFactory1 create =
            dxgi ? reinterpret_cast<PFN_CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&tmp))))
            factory = tmp;
    }
    BOOL allow = FALSE;
    if (factory)
    {
        IDXGIFactory5* f5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5))) && f5)
        {
            if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                allow = FALSE;
            f5->Release();
        }
    }
    if (tmp)
        tmp->Release();
    return allow != FALSE;
}

// FlipModel=1 (applied at start): copy of the game's desc with FLIP_DISCARD.
// Returns true if `out` should be used; logs the reason otherwise.
static bool PrepareFlipDesc(const DXGI_SWAP_CHAIN_DESC* in, DXGI_SWAP_CHAIN_DESC* out, IDXGIFactory* factory,
                            bool* tearing)
{
    *tearing = false;
    if (!in)
        return false;
    if (!g_startFlip)
    {
        Log("[DXGI] flip model off (FlipModel=0)");
        return false;
    }
    if (in->SampleDesc.Count != 1)
    {
        Log("[DXGI] flip model off: MSAA %u not supported by flip", in->SampleDesc.Count);
        return false;
    }
    switch (in->BufferDesc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        break;
    default:
        Log("[DXGI] flip model off: format %d not supported (sRGB/other)", static_cast<int>(in->BufferDesc.Format));
        return false;
    }
    *out = *in;
    out->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    out->BufferCount = Max<UINT>(2u, in->BufferCount);
    if (g_startTearing)
    {
        if (TearingSupported(factory))
        {
            out->Flags |= static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
            *tearing = true;
        }
        else
        {
            Log("[DXGI] allow tearing requested but not supported by this system");
        }
    }
    return true;
}

// Real (not faked) fullscreen state of the swap chain, via the original method.
static bool RealFullscreen(IDXGISwapChain* sc);


// v1.4.6: once per session - Present reported device removed/reset: record the
// removal reason for the crash report. Rare path only (no per-frame cost).
static volatile LONG g_devRemovedNoted = 0;
static __declspec(noinline) void DeviceRemovedNote(IDXGISwapChain* sc, HRESULT hr)
{
    if (InterlockedExchange(&g_devRemovedNoted, 1) != 0)
        return;
    HRESULT reason = E_FAIL;
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) // AddRef'd
    {
        reason = dev->GetDeviceRemovedReason();
        dev->Release();
    }
    EventPush("[DEVICE] device removed hr=0x%08lX reason=0x%08lX (swapchain %p)", static_cast<unsigned long>(hr),
              static_cast<unsigned long>(reason), static_cast<void*>(sc));
    if (LoggingActive())
        Log("[DEVICE] Present: device removed hr=0x%08lX reason=0x%08lX (swapchain %p)", static_cast<unsigned long>(hr),
            static_cast<unsigned long>(reason), static_cast<void*>(sc));
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    const TRHF_PFN_Present o = reinterpret_cast<TRHF_PFN_Present>(ClassOrig(g_swapHooks, This, SW_Present));
    if (!o)
        return DXGI_ERROR_INVALID_CALL; // v1.4.6: no original known (should not happen)
    const bool real = !(Flags & DXGI_PRESENT_TEST) && g_active && !g_shutdown;
    const bool game = real && This == g_gameSc;
    if (game)
    {
        MemWarnPoll();              // v1.4.6: time-sliced address-space walk + low-memory check
        OverlayBeforePresent(This); // hotkeys; menu/FPS overlay only do work while shown
        LimiterWait();              // FpsLimit (0 = off)
        if (g_tearingActive && SyncInterval == 0 && !RealFullscreen(This))
            Flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    const HRESULT hr = o(This, SyncInterval, Flags);
    if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && g_active && !g_shutdown)
        DeviceRemovedNote(This, hr);
    if (real)
    {
        if (game)
            PerfOnPresent();
        if (g_logOn && InterlockedExchange(&g_firstPresentLogged, 1) == 0)
            Log("[DXGI] first Present on swapchain %p (sync=%u flags=0x%X) - frame-based capture available",
                static_cast<void*>(This), SyncInterval, Flags);
        OnPresent(This);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_SetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget)
{
    const TRHF_PFN_SetFullscreenState o =
        reinterpret_cast<TRHF_PFN_SetFullscreenState>(ClassOrig(g_swapHooks, This, SW_SetFullscreenState));
    if (!o)
        return DXGI_ERROR_INVALID_CALL; // v1.4.6
    if (g_startBorderless && This == g_gameSc && g_active && !g_shutdown)
    {
        // Borderless: never enter exclusive fullscreen; fake the state instead.
        if (Fullscreen)
        {
            BorderlessApply(This, "SetFullscreenState(TRUE)");
            InterlockedExchange(&g_fakeFs, 1);
        }
        else
        {
            InterlockedExchange(&g_fakeFs, 0);
            Log("[DISPLAY] borderless: SetFullscreenState(FALSE) - window stays borderless");
        }
        return S_OK;
    }
    const HRESULT hr = o(This, Fullscreen, pTarget);
    if (DxgiLogAllowed())
        Log("[DXGI] SetFullscreenState swapchain=%p fullscreen=%d target=%p hr=0x%08lX", static_cast<void*>(This),
            Fullscreen ? 1 : 0, static_cast<void*>(pTarget), static_cast<unsigned long>(hr));
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_GetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    const TRHF_PFN_GetFullscreenState o =
        reinterpret_cast<TRHF_PFN_GetFullscreenState>(ClassOrig(g_swapHooks, This, SW_GetFullscreenState));
    if (g_startBorderless && g_fakeFs && This == g_gameSc)
    {
        if (pFullscreen)
            *pFullscreen = TRUE;
        if (ppTarget)
        {
            *ppTarget = nullptr;
            if (FAILED(This->GetContainingOutput(ppTarget))) // AddRef'd for the caller
                *ppTarget = nullptr;
        }
        return S_OK;
    }
    if (!o)
        return DXGI_ERROR_INVALID_CALL; // v1.4.6
    return o(This, pFullscreen, ppTarget);
}

static bool RealFullscreen(IDXGISwapChain* sc)
{
    const TRHF_PFN_GetFullscreenState o =
        reinterpret_cast<TRHF_PFN_GetFullscreenState>(ClassOrig(g_swapHooks, sc, SW_GetFullscreenState));
    BOOL fs = FALSE;
    return o && SUCCEEDED(o(sc, &fs, nullptr)) && fs != FALSE;
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height,
                                                   DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    const TRHF_PFN_ResizeBuffers o =
        reinterpret_cast<TRHF_PFN_ResizeBuffers>(ClassOrig(g_swapHooks, This, SW_ResizeBuffers));
    if (!o)
        return DXGI_ERROR_INVALID_CALL; // v1.4.6
    if (This == g_gameSc)
    {
        OverlayReleaseRTV(); // must not hold back-buffer references across ResizeBuffers
        if (g_tearingActive)
            SwapChainFlags |= static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING); // must match creation flags
    }
    const HRESULT hr = o(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    if (DxgiLogAllowed())
        Log("[DXGI] ResizeBuffers swapchain=%p count=%u %ux%u fmt=%d flags=0x%X hr=0x%08lX", static_cast<void*>(This),
            BufferCount, Width, Height, static_cast<int>(NewFormat), SwapChainFlags, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && This == g_gameSc && LoggingActive())
    {
        // Diagnostics ([INV] bb): Width/Height 0 = window client size, read the actual size back.
        DXGI_SWAP_CHAIN_DESC d = {};
        if (SUCCEEDED(This->GetDesc(&d)))
            BbNote(d.BufferDesc.Width, d.BufferDesc.Height);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters)
{
    const TRHF_PFN_ResizeTarget o = reinterpret_cast<TRHF_PFN_ResizeTarget>(ClassOrig(g_swapHooks, This, SW_ResizeTarget));
    if (!o)
        return DXGI_ERROR_INVALID_CALL; // v1.4.6
    const HRESULT hr = o(This, pNewTargetParameters);
    if (DxgiLogAllowed())
    {
        if (pNewTargetParameters)
            Log("[DXGI] ResizeTarget swapchain=%p %ux%u refresh=%u/%u fmt=%d scaling=%d hr=0x%08lX",
                static_cast<void*>(This), pNewTargetParameters->Width, pNewTargetParameters->Height,
                pNewTargetParameters->RefreshRate.Numerator, pNewTargetParameters->RefreshRate.Denominator,
                static_cast<int>(pNewTargetParameters->Format), static_cast<int>(pNewTargetParameters->Scaling),
                static_cast<unsigned long>(hr));
        else
            Log("[DXGI] ResizeTarget swapchain=%p (null desc) hr=0x%08lX", static_cast<void*>(This),
                static_cast<unsigned long>(hr));
    }
    // Borderless: a windowed ResizeTarget changes the window size - restore the monitor rect.
    if (g_startBorderless && g_fakeFs && This == g_gameSc)
        BorderlessApply(This, "after ResizeTarget");
    return hr;
}

// Creates our dynamic 192-byte CB once for the game device (the one that owns
// the swap chain) and attaches it to its immediate context's table.
static void FixSetupForDevice(ID3D11Device* dev)
{
    if (!g_fixInfra || !dev)
        return;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = kFixCBBytes;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr;
    const HRESULT hr = dev->CreateBuffer(&bd, nullptr, &cb);
    if (FAILED(hr) || !cb)
    {
        Log("[FIX] CreateBuffer(192, DYNAMIC) failed hr=0x%08lX - correction unavailable", static_cast<unsigned long>(hr));
        return;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    bool attached = false;
    if (ctx)
    {
        void** vtbl = *reinterpret_cast<void***>(ctx);
        AcquireSRWLockExclusive(&g_hookLock);
        const LONG n = g_tableCount;
        for (LONG i = 0; i < n; ++i)
        {
            if (g_tables[i].vtbl == vtbl)
            {
                ID3D11Buffer* old = g_tables[i].fixCB;
                g_tables[i].fixCB = cb;
                if (old && old != cb)
                    old->Release();
                attached = true;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_hookLock);
    }
    Log("[FIX] own CB %p for device %p / context %p: %s (fix %s, %u HUD VS hashes, shiftNdcX=%.4f, toggle 0x%02X)",
        static_cast<void*>(cb), static_cast<void*>(dev), static_cast<void*>(ctx),
        attached ? "attached" : "context not hooked - unused", g_fixRuntime ? "ON" : "OFF", g_cfg.hudVsCount,
        static_cast<double>(g_cfg.fixShiftNdcX), g_cfg.fixToggleHotkey);
    if (!attached)
        cb->Release();
    if (ctx)
        ctx->Release();
}

static void OnSwapChainCreated(const char* api, HRESULT hr, const DXGI_SWAP_CHAIN_DESC* pReqDesc,
                               IDXGISwapChain* sc)
{
    Log("[DXGI] %s hr=0x%08lX swapchain=%p", api, static_cast<unsigned long>(hr), static_cast<void*>(sc));
    if (pReqDesc)
        LogSwapChainDesc("requested", pReqDesc);
    if (FAILED(hr) || !sc)
        return;
    g_gameSc = sc; // newest swap chain = game swap chain (menu target)
    HWND hwnd = pReqDesc ? pReqDesc->OutputWindow : nullptr;
    DXGI_SWAP_CHAIN_DESC actual = {};
    if (SUCCEEDED(sc->GetDesc(&actual)))
    {
        LogSwapChainDesc("actual", &actual);
        BbNote(actual.BufferDesc.Width, actual.BufferDesc.Height);
        if (actual.OutputWindow)
            hwnd = actual.OutputWindow;
    }
    if (hwnd && InterlockedExchange(&g_firstSwapChain, 1) == 0)
        QueryAndLogDpi("first swapchain", hwnd);

    void* const hooks[kMaxClassIdx] = { reinterpret_cast<void*>(&Hook_Present),
                                        reinterpret_cast<void*>(&Hook_SetFullscreenState),
                                        reinterpret_cast<void*>(&Hook_GetFullscreenState),
                                        reinterpret_cast<void*>(&Hook_ResizeBuffers),
                                        reinterpret_cast<void*>(&Hook_ResizeTarget) };
    InstallClassHooks(g_swapHooks, hooks, sc, api);
    HudSetClear(); // new swap chain/device: HUD buffers are re-learned

    if (g_startBorderless && hwnd)
    {
        // DXGI's own Alt+Enter would bypass our SetFullscreenState hook.
        IDXGIFactory* f = nullptr;
        if (SUCCEEDED(sc->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&f))) && f)
        {
            const HRESULT mh = f->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            Log("[DISPLAY] borderless active: Alt+Enter disabled (hr=0x%08lX)", static_cast<unsigned long>(mh));
            f->Release();
        }
    }

    // The device owning the swap chain is the game device: create our fix CB.
    if (g_fixInfra)
    {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev)
        {
            FixSetupForDevice(dev);
            dev->Release();
        }
        else
        {
            Log("[FIX] swapchain %p: GetDevice(ID3D11Device) failed - correction unavailable", static_cast<void*>(sc));
        }
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                      IDXGISwapChain** ppSwapChain)
{
    const TRHF_PFN_CreateSwapChain o = reinterpret_cast<TRHF_PFN_CreateSwapChain>(ClassOrig(g_factoryHooks, This, 0));
    if (!o)
        return DXGI_ERROR_INVALID_CALL; // v1.4.6
    DXGI_SWAP_CHAIN_DESC flipDesc = {};
    bool tearing = false;
    const bool flip = g_active && !g_shutdown && PrepareFlipDesc(pDesc, &flipDesc, This, &tearing);
    HRESULT hr = o(This, pDevice, flip ? &flipDesc : pDesc, ppSwapChain);
    const bool flipOk = flip && SUCCEEDED(hr);
    if (flip && FAILED(hr))
    {
        Log("[DXGI] flip model failed hr=0x%08lX, fallback legacy", static_cast<unsigned long>(hr));
        hr = o(This, pDevice, pDesc, ppSwapChain);
    }
    if (flip)
    {
        g_flipActive = flipOk;
        g_tearingActive = flipOk && tearing;
        if (flipOk)
            Log("[DXGI] flip model on: FLIP_DISCARD buffers=%u tearing=%d", flipDesc.BufferCount, tearing ? 1 : 0);
    }
    if (g_active && !g_shutdown)
        OnSwapChainCreated("IDXGIFactory::CreateSwapChain", hr, flipOk ? &flipDesc : pDesc,
                           (SUCCEEDED(hr) && ppSwapChain) ? *ppSwapChain : nullptr);
    return hr;
}

// Game device -> IDXGIDevice -> adapter -> parent factory -> hook CreateSwapChain.
static void HookFactoryFromDevice(ID3D11Device* dev)
{
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) || !dxgiDev)
    {
        Log("[DXGI] device %p: no IDXGIDevice - Present hook unavailable", static_cast<void*>(dev));
        return;
    }
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter)
    {
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) && factory)
        {
            void* const hooks[kMaxClassIdx] = { reinterpret_cast<void*>(&Hook_CreateSwapChain), nullptr, nullptr, nullptr };
            InstallClassHooks(g_factoryHooks, hooks, factory, "factory of game device");
            factory->Release();
        }
        else
        {
            Log("[DXGI] adapter->GetParent(IDXGIFactory) failed - Present hook unavailable");
        }
        adapter->Release();
    }
    else
    {
        Log("[DXGI] IDXGIDevice::GetAdapter failed - Present hook unavailable");
    }
    dxgiDev->Release();
}

// =============================================================================
//  In-game configuration menu (Dear ImGui 1.93, DX11 + Win32 backends)
//
//  * Initialised lazily the first time the menu is opened (MenuHotkey, polled
//    in Present on the game swap chain). Until then: no ImGui, no subclass.
//  * Rendered only while the menu is open, before the original Present, into
//    our own back-buffer RTV. All D3D11 calls made by ImGui run with
//    t_inOverlay = true, so every context/device hook passes straight through
//    (no fix, no tracking, no capture, no re-arm).
//  * Window subclass: while the menu is open, mouse/keyboard/char/WM_INPUT
//    messages go to ImGui only. Everything else always reaches the game.
//  * Visual style modelled on the game's HUD (square, dark, white highlight
//    bars). No game assets; optional Windows system font "Bahnschrift".
// =============================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct OverlayState
{
    bool                    initTried;
    bool                    ok;
    bool                    menuKeyWasDown;
    bool                    rtvFailLogged;
    HWND                    hwnd;
    WNDPROC                 origProc;
    ID3D11Device*           dev;
    ID3D11DeviceContext*    ctx;
    ID3D11RenderTargetView* rtv;
    UINT                    bbW, bbH;
    float                   uiScale;
    bool                    firstLayout;
    DWORD                   saveMsgTick;
    bool                    saveOk;
    DWORD                   saveErr;
    LONG                    rateLastTotal;
    DWORD                   rateLastTick;
    float                   rate;
    // v1.1
    bool                    overlayKeyWasDown;
    bool                    perfKeyWasDown;
    bool                    limitCustom;
    bool                    menuClearConfirm;
    int                     bindRow = -1; // hotkey row being bound, -1 = none
    bool                    bindPrev[256];
    char                    bindMsg[96];
    DWORD                   bindMsgTick;
    // v1.1.1
    int                     tab;          // selected menu tab (kept for the session)
    bool                    clampPending; // clamp the window into the screen on (re)open
    bool                    havePos;
    ImVec2                  winPos;
    ImVec2                  winSize;
    float                   bdRate;
    LONG                    bdLastTotal;
    float                   prRate;      // prompt fixes per second (v1.1.2)
    LONG                    prLastTotal;
    float                   poRate;      // popup-origin fixes per second (v1.1.3)
    LONG                    poLastTotal;
    float                   svRate;      // salvage fixes per second (v1.3.1)
    LONG                    svLastTotal;
};
static OverlayState     g_ov = {};
static CRITICAL_SECTION g_imguiCs; // serialises ImGui between WndProc and render thread
static volatile LONG    g_menuOpen = 0;

static float OverlayScaleFor(UINT bbHeight)
{
    if (g_cfg.menuScale > 0.0f)
        return g_cfg.menuScale;
    return Max(1.0f, static_cast<float>(bbHeight) / 1080.0f);
}

// TR-2013-HUD-like style: square, dark translucent panels, off-white text,
// white highlight bars. Caller holds g_imguiCs with a current ImGui context.
static void OverlayApplyStyle(float scale)
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);

    st.WindowRounding    = 0.0f;
    st.FrameRounding     = 0.0f;
    st.GrabRounding      = 0.0f;
    st.ChildRounding     = 0.0f;
    st.PopupRounding     = 0.0f;
    st.ScrollbarRounding = 0.0f;
    st.TabRounding       = 0.0f;
    st.WindowBorderSize  = 1.0f;
    st.FrameBorderSize   = 0.0f;
    st.WindowPadding     = ImVec2(16.0f, 14.0f);
    st.FramePadding      = ImVec2(8.0f, 5.0f);
    st.ItemSpacing       = ImVec2(10.0f, 8.0f);
    st.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    st.FontSizeBase      = 18.0f;
    st.GrabMinSize       = 4.0f; // narrow grab, does not cover the value (scaled below)

    const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 offWhite(235.0f / 255.0f, 235.0f / 255.0f, 230.0f / 255.0f, 1.0f);
    const ImVec4 grabWhite(240.0f / 255.0f, 240.0f / 255.0f, 235.0f / 255.0f, 1.0f);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text]                 = offWhite;
    c[ImGuiCol_TextDisabled]         = ImVec4(140.0f / 255.0f, 140.0f / 255.0f, 135.0f / 255.0f, 1.0f);
    c[ImGuiCol_WindowBg]             = ImVec4(12.0f / 255.0f, 12.0f / 255.0f, 12.0f / 255.0f, 0.82f);
    c[ImGuiCol_ChildBg]              = ImVec4(20.0f / 255.0f, 20.0f / 255.0f, 20.0f / 255.0f, 0.60f);
    // v1.4.1 contrast rule: every standard ImGui widget draws off-white text, so
    // its hover/active/selected fills are translucent light grey over the dark
    // panel - never (near-)opaque white. Only the custom Tr* widgets and the
    // selected tab use a white fill, and those draw black text.
    c[ImGuiCol_PopupBg]              = ImVec4(10.0f / 255.0f, 10.0f / 255.0f, 10.0f / 255.0f, 0.97f); // combo lists + tooltips
    c[ImGuiCol_Border]               = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.85f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.0f, 0.0f, 0.0f, 0.85f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.0f, 0.0f, 0.0f, 0.85f);
    c[ImGuiCol_FrameBg]              = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(1.0f, 1.0f, 1.0f, 0.18f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(1.0f, 1.0f, 1.0f, 0.30f);
    c[ImGuiCol_SliderGrab]           = ImVec4(grabWhite.x, grabWhite.y, grabWhite.z, 0.55f); // value text stays readable
    c[ImGuiCol_SliderGrabActive]     = ImVec4(1.0f, 1.0f, 1.0f, 0.90f);
    c[ImGuiCol_CheckMark]            = white;
    c[ImGuiCol_Button]               = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(1.0f, 1.0f, 1.0f, 0.18f); // close "X", combo arrow box
    c[ImGuiCol_ButtonActive]         = ImVec4(1.0f, 1.0f, 1.0f, 0.30f);
    c[ImGuiCol_Header]               = ImVec4(1.0f, 1.0f, 1.0f, 0.22f); // selected combo item
    c[ImGuiCol_HeaderHovered]        = ImVec4(1.0f, 1.0f, 1.0f, 0.26f);
    c[ImGuiCol_HeaderActive]         = ImVec4(1.0f, 1.0f, 1.0f, 0.32f);
    c[ImGuiCol_Separator]            = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(1.0f, 1.0f, 1.0f, 0.35f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(1.0f, 1.0f, 1.0f, 0.50f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(1.0f, 1.0f, 1.0f, 0.30f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(1.0f, 1.0f, 1.0f, 0.50f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.30f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.40f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.0f, 1.0f, 1.0f, 0.60f);
    c[ImGuiCol_PlotLines]            = ImVec4(1.0f, 1.0f, 1.0f, 0.80f);
    c[ImGuiCol_PlotLinesHovered]     = white;
    c[ImGuiCol_InputTextCursor]      = offWhite;
    c[ImGuiCol_TextSelectedBg]       = ImVec4(1.0f, 1.0f, 1.0f, 0.30f);
    c[ImGuiCol_TextLink]             = white;
    c[ImGuiCol_NavCursor]            = ImVec4(1.0f, 1.0f, 1.0f, 0.80f); // outline only, no fill
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
    // Tabs like the game's message box: selected = white (text drawn black).
    c[ImGuiCol_Tab]                       = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_TabHovered]                = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    c[ImGuiCol_TabSelected]               = white;
    c[ImGuiCol_TabSelectedOverline]       = white;
    c[ImGuiCol_TabDimmed]                 = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_TabDimmedSelected]         = white;
    c[ImGuiCol_TabDimmedSelectedOverline] = white;

    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
}

// "Bahnschrift" (Windows 10+ system font) if present - never shipped/embedded.
static void OverlayLoadFont()
{
    ImGuiIO& io = ImGui::GetIO();
    wchar_t path[MAX_PATH + 64] = {};
    const UINT n = GetWindowsDirectoryW(path, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        wcscat_s(path, L"\\Fonts\\bahnschrift.ttf");
        if (FileExists(path))
        {
            char u8[1024];
            // 1.93: glyphs (incl. Latin-1 umlauts) are rasterised on demand, no glyph ranges needed.
            if (io.Fonts->AddFontFromFileTTF(ToUtf8(path, u8, sizeof(u8)), 18.0f))
            {
                Log("[MENU] font: %s", u8);
                return;
            }
        }
    }
    io.Fonts->AddFontDefault();
    Log("[MENU] font: Bahnschrift not found - ImGui default font");
}

static bool IsBlockedInputMsg(UINT msg)
{
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
        return true;
    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_DEADCHAR:
    case WM_UNICHAR:
    case WM_INPUT:
    case WM_MOUSEHOVER:
    case WM_MOUSELEAVE:
        return true;
    default:
        return false; // incl. WM_SYSKEY* (Alt+F4/Alt+Tab keep working), activation, size, focus, WM_SETCURSOR
    }
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_menuOpen && g_ov.ok)
    {
        EnterCriticalSection(&g_imguiCs);
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        LeaveCriticalSection(&g_imguiCs);
        if (IsBlockedInputMsg(msg))
            return (msg == WM_INPUT) ? DefWindowProcW(hwnd, msg, wp, lp) : 0; // DefWindowProc frees the raw-input data
    }
    return CallWindowProcW(g_ov.origProc, hwnd, msg, wp, lp);
}

static bool OverlayInit(IDXGISwapChain* sc)
{
    g_ov.initTried = true;
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow)
    {
        Log("[MENU] init failed: swap chain has no OutputWindow - menu disabled");
        return false;
    }
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev)
    {
        Log("[MENU] init failed: GetDevice - menu disabled");
        return false;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx)
    {
        dev->Release();
        Log("[MENU] init failed: no immediate context - menu disabled");
        return false;
    }

    bool ok = false;
    const float scale = OverlayScaleFor(d.BufferDesc.Height);
    t_inOverlay = true;
    EnterCriticalSection(&g_imguiCs);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (ImGui_ImplWin32_Init(d.OutputWindow))
    {
        if (ImGui_ImplDX11_Init(dev, ctx))
            ok = true;
        else
            ImGui_ImplWin32_Shutdown();
    }
    if (ok)
    {
        OverlayLoadFont();
        OverlayApplyStyle(scale);
    }
    else
    {
        ImGui::DestroyContext();
    }
    LeaveCriticalSection(&g_imguiCs);
    t_inOverlay = false;

    if (!ok)
    {
        ctx->Release();
        dev->Release();
        Log("[MENU] init failed: ImGui Win32/DX11 backend init - menu disabled, game unaffected");
        return false;
    }
    // Device/context references are kept for the process lifetime.
    g_ov.dev = dev;
    g_ov.ctx = ctx;
    g_ov.hwnd = d.OutputWindow;
    g_ov.uiScale = scale;
    g_ov.firstLayout = true;
    g_ov.ok = true;

    // Subclass: store the original procedure BEFORE ours can be called.
    g_ov.origProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(d.OutputWindow, GWLP_WNDPROC));
    MemoryBarrier();
    SetLastError(0);
    const LONG_PTR prev =
        SetWindowLongPtrW(d.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc));
    const DWORD err = GetLastError();
    if (prev)
        g_ov.origProc = reinterpret_cast<WNDPROC>(prev);
    if (!prev && err)
        Log("[MENU] window subclass failed (err %lu) - menu visible but without mouse/keyboard input", err);

    Log("[MENU] init ok: hwnd=%p device=%p backbuffer=%ux%u uiScale=%.2f ImGui %s", static_cast<void*>(d.OutputWindow),
        static_cast<void*>(dev), d.BufferDesc.Width, d.BufferDesc.Height, static_cast<double>(scale), IMGUI_VERSION);
    return true;
}

static bool OverlayCreateRTV(IDXGISwapChain* sc)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
    {
        if (!g_ov.rtvFailLogged)
            Log("[MENU] GetBuffer(0) failed - menu not drawn");
        g_ov.rtvFailLogged = true;
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    bb->GetDesc(&td);
    ID3D11RenderTargetView* rtv = nullptr;
    t_inOverlay = true;
    const HRESULT hr = g_ov.dev->CreateRenderTargetView(bb, nullptr, &rtv);
    t_inOverlay = false;
    bb->Release();
    if (FAILED(hr) || !rtv)
    {
        if (!g_ov.rtvFailLogged)
            Log("[MENU] CreateRenderTargetView failed hr=0x%08lX (fmt=%d) - menu not drawn",
                static_cast<unsigned long>(hr), static_cast<int>(td.Format));
        g_ov.rtvFailLogged = true;
        return false;
    }
    g_ov.rtv = rtv;
    g_ov.bbW = td.Width;
    g_ov.bbH = td.Height;
    BbNote(td.Width, td.Height);
    const float scale = OverlayScaleFor(td.Height);
    if (scale != g_ov.uiScale)
    {
        EnterCriticalSection(&g_imguiCs);
        g_ov.uiScale = scale;
        OverlayApplyStyle(scale);
        LeaveCriticalSection(&g_imguiCs);
    }
    Log("[MENU] back-buffer RTV %ux%u fmt=%d uiScale=%.2f", td.Width, td.Height, static_cast<int>(td.Format),
        static_cast<double>(g_ov.uiScale));
    return true;
}

// ResizeBuffers hook (BEFORE the original): our RTV must not reference the old buffers.
static void OverlayReleaseRTV()
{
    EnterCriticalSection(&g_imguiCs);
    if (g_ov.rtv)
    {
        g_ov.rtv->Release();
        g_ov.rtv = nullptr;
    }
    LeaveCriticalSection(&g_imguiCs);
}

// --- Hotkey binding (menu section HOTKEYS) -----------------------------------------
struct HotkeyRow
{
    const char* name;
    UINT*       vk;
};
static HotkeyRow g_hkRows[] = {
    { "Menu", &g_cfg.menuHotkey },
    { "Toggle fix", &g_cfg.fixToggleHotkey },
    { "FPS overlay", &g_cfg.overlayHotkey },
    { "Capture (debug)", &g_cfg.captureHotkey },
};
static const UINT kHkRows = static_cast<UINT>(sizeof(g_hkRows) / sizeof(g_hkRows[0]));

// Readable key name: own table for common keys, GetKeyNameTextW as fallback.
static const char* VkName(UINT vk, char* buf, size_t cap)
{
    if (const char* n = VkNameTable(vk, buf, cap))
        return n;
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t w[64] = {};
    if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), w, 64) > 0)
    {
        ToUtf8(w, buf, static_cast<int>(cap));
        if (buf[0])
            return buf;
    }
    _snprintf_s(buf, cap, _TRUNCATE, "VK 0x%02X", vk);
    return buf;
}

static bool IsModifierVk(UINT vk)
{
    switch (vk)
    {
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_MENU:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

static void HotkeyEndBind()
{
    g_ov.bindRow = -1;
    InterlockedExchange(&g_bindActive, 0);
    g_suppressTick = GetTickCount() | 1u; // keep hotkey edges suppressed briefly (key still held)
}

static void HotkeyBeginBind(int row)
{
    g_ov.bindRow = row;
    g_ov.menuClearConfirm = false;
    g_ov.bindMsg[0] = 0;
    for (UINT vk = 0; vk < 256u; ++vk)
        g_ov.bindPrev[vk] = vk >= 0x08u && vk <= 0xFEu &&
                            (static_cast<USHORT>(GetAsyncKeyState(static_cast<int>(vk))) & 0x8000u) != 0;
    InterlockedExchange(&g_bindActive, 1);
}

// Once per menu frame while binding: first newly pressed key (VK 0x08..0xFE,
// mouse buttons excluded). ESC cancels.
static void HotkeyPollBind()
{
    if (g_ov.bindRow < 0)
        return;
    for (UINT vk = 0x08u; vk <= 0xFEu; ++vk)
    {
        const bool down = (static_cast<USHORT>(GetAsyncKeyState(static_cast<int>(vk))) & 0x8000u) != 0;
        const bool edge = down && !g_ov.bindPrev[vk];
        g_ov.bindPrev[vk] = down;
        if (!edge)
            continue;
        if (vk == VK_ESCAPE)
        {
            HotkeyEndBind();
            return;
        }
        if (IsModifierVk(vk))
        {
            _snprintf_s(g_ov.bindMsg, sizeof(g_ov.bindMsg), _TRUNCATE, "Modifier keys alone are not allowed");
            g_ov.bindMsgTick = GetTickCount();
            continue;
        }
        const char* usedBy = nullptr;
        for (UINT r = 0; r < kHkRows; ++r)
        {
            if (static_cast<int>(r) != g_ov.bindRow && *g_hkRows[r].vk == vk)
                usedBy = g_hkRows[r].name;
        }
        if (usedBy)
        {
            _snprintf_s(g_ov.bindMsg, sizeof(g_ov.bindMsg), _TRUNCATE, "Already used by %s", usedBy);
            g_ov.bindMsgTick = GetTickCount();
            continue;
        }
        char kn[64];
        *g_hkRows[g_ov.bindRow].vk = vk;
        Log("[MENU] hotkey %s = %s (0x%02X)", g_hkRows[g_ov.bindRow].name, VkName(vk, kn, sizeof(kn)), vk);
        HotkeyEndBind();
        return;
    }
}

static void OverlaySetOpen(bool open, IDXGISwapChain* sc)
{
    if (!open && g_ov.bindRow >= 0)
        HotkeyEndBind();
    if (open && !g_ov.initTried)
        OverlayInit(sc);
    if (open && !g_ov.ok)
        return;
    if (open == (g_menuOpen != 0))
        return;
    if (open)
    {
        EnterCriticalSection(&g_imguiCs);
        ImGuiIO& io = ImGui::GetIO();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.MouseDrawCursor = true; // software cursor - reliable in exclusive fullscreen
        LeaveCriticalSection(&g_imguiCs);
    }
    if (open)
        g_ov.clampPending = true;
    InterlockedExchange(&g_menuOpen, open ? 1 : 0);
    Log("[MENU] %s", open ? "opened" : "closed");
}

// --- Widgets in the game-HUD style ---------------------------------------------------
// Section title: full-width white bar with black upper-case text.
static void TrSectionHeader(const char* label)
{
    const float s = g_ov.uiScale;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight() + 4.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(240, 240, 235, 255));
    dl->AddText(ImVec2(p.x + 6.0f * s, p.y + 2.0f * s), IM_COL32(0, 0, 0, 255), label);
    ImGui::Dummy(ImVec2(w, h));
}

// Button: dark with thin white frame; hovered/active = white bar with black text.
static bool TrButton(const char* label, float width = 0.0f)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 size(width > 0.0f ? width : ts.x + st.FramePadding.x * 2.0f, ts.y + st.FramePadding.y * 2.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    if (hot)
    {
        dl->AddRectFilled(p, q, ImGui::IsItemActive() ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 230));
    }
    else
    {
        dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 20));
        dl->AddRect(p, q, IM_COL32(255, 255, 255, 140));
    }
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + st.FramePadding.y),
                hot ? IM_COL32(0, 0, 0, 255) : IM_COL32(235, 235, 230, 255), label);
    return pressed;
}

// Text with a fake-bold effect (extra 1 px passes to the right).
static void BoldText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text)
{
    const int passes = static_cast<int>(Max(1.0f, floorf(size * 0.045f)));
    for (int o = 0; o <= passes; ++o)
        dl->AddText(font, size, ImVec2(pos.x + static_cast<float>(o), pos.y), col, text);
}

// Vector logo header (no image assets), matching the release branding
// (branding/graphics, word mark of the icon): red accent bar, white box with a
// black bold "HUD FIX+", the version in grey at the right. Everything scales
// with the font size, so it follows the UI scale and the loaded font.
static void TrLogoHeader()
{
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float w = ImGui::GetContentRegionAvail().x;
    const auto F = [](float v) { return floorf(v + 0.5f); };
    const float H = F(fs * 2.1f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 white = IM_COL32(240, 240, 235, 255);
    const ImU32 red = IM_COL32(158, 28, 58, 255); // #9e1c3a, branding accent

    const float bar = Max(3.0f, F(fs * 0.22f));
    const float pad = F(fs * 0.45f);
    const float bigSz = F(H * 0.86f);
    const ImVec2 ts = font->CalcTextSizeA(bigSz, FLT_MAX, 0.0f, "HUD FIX+");
    const float bx = p.x + bar;
    const float boxX1 = bx + ts.x + 2.0f * pad + 2.0f;
    dl->AddRectFilled(p, ImVec2(bx, p.y + H), red);
    dl->AddRectFilled(ImVec2(bx, p.y), ImVec2(boxX1, p.y + H), white);
    BoldText(dl, font, bigSz, ImVec2(bx + pad, F(p.y + (H - ts.y) * 0.5f)), IM_COL32(0, 0, 0, 255), "HUD FIX+");

    char ver[24];
    _snprintf_s(ver, sizeof(ver), _TRUNCATE, "v%s", kVersion);
    const float verSz = F(fs * 0.80f);
    const ImVec2 vs = font->CalcTextSizeA(verSz, FLT_MAX, 0.0f, ver);
    if (p.x + w - vs.x >= boxX1 + fs)
        dl->AddText(font, verSz, ImVec2(F(p.x + w - vs.x), F(p.y + H - vs.y)), IM_COL32(150, 150, 145, 255), ver);

    ImGui::Dummy(ImVec2(w, H));
}

static bool IniWriteFloat(const wchar_t* section, const wchar_t* key, float v)
{
    wchar_t buf[32] = {};
    swprintf_s(buf, ARRAYSIZE(buf), L"%.4f", static_cast<double>(v)); // our static CRT: "C" locale
    return WritePrivateProfileStringW(section, key, buf, g_iniPath) != FALSE;
}

static bool IniWriteInt(const wchar_t* section, const wchar_t* key, int v)
{
    wchar_t buf[16] = {};
    swprintf_s(buf, ARRAYSIZE(buf), L"%d", v);
    return WritePrivateProfileStringW(section, key, buf, g_iniPath) != FALSE;
}

// v1.4.5: hotkeys are saved as key names (F9, INSERT ...); keys without a
// table name as virtual-key code; 0 = none.
static bool IniWriteKey(const wchar_t* section, const wchar_t* key, UINT v)
{
    wchar_t buf[32] = {};
    char nb[32];
    const char* name = v ? VkNameTable(v, nb, sizeof(nb)) : nullptr;
    if (!v)
        swprintf_s(buf, ARRAYSIZE(buf), L"0");
    else if (name)
        swprintf_s(buf, ARRAYSIZE(buf), L"%hs", name);
    else
        swprintf_s(buf, ARRAYSIZE(buf), L"0x%02X", v);
    return WritePrivateProfileStringW(section, key, buf, g_iniPath) != FALSE;
}

static void MenuSaveIni()
{
    bool ok = true;
    ok = IniWriteInt(L"General", L"EnableFix", g_fixRuntime ? 1 : 0) && ok;
    ok = IniWriteFloat(L"Fix", L"ShiftNdcX", g_cfg.fixShiftNdcX) && ok;
    ok = IniWriteFloat(L"Fix", L"ShiftNdcY", g_cfg.fixShiftNdcY) && ok;
    ok = IniWriteFloat(L"Fix", L"PopupScale", g_cfg.popupScale) && ok;
    ok = IniWriteInt(L"Fix", L"FixBackdrop", g_cfg.fixBackdrop) && ok;
    ok = IniWriteInt(L"Fix", L"FixPrompt", g_cfg.fixPrompt ? 1 : 0) && ok;
    ok = IniWriteInt(L"Fix", L"FixPopupOrigin", g_cfg.fixPopupOrigin ? 1 : 0) && ok;
    ok = IniWriteInt(L"Fix", L"FixSalvage", g_cfg.fixSalvage ? 1 : 0) && ok;
    ok = IniWriteKey(L"Fix", L"ToggleHotkey", g_cfg.fixToggleHotkey) && ok;
    ok = IniWriteKey(L"Menu", L"MenuHotkey", g_cfg.menuHotkey) && ok;
    ok = IniWriteInt(L"Perf", L"FpsLimit", g_cfg.fpsLimit) && ok;
    ok = IniWriteInt(L"Perf", L"ShowFpsOverlay", g_cfg.showFpsOverlay ? 1 : 0) && ok;
    ok = IniWriteKey(L"Perf", L"OverlayHotkey", g_cfg.overlayHotkey) && ok;
    ok = IniWriteInt(L"Display", L"Borderless", g_cfg.borderless ? 1 : 0) && ok;
    ok = IniWriteInt(L"Display", L"FlipModel", g_cfg.flipModel ? 1 : 0) && ok;
    ok = IniWriteInt(L"Display", L"AllowTearing", g_cfg.allowTearing ? 1 : 0) && ok;
    ok = IniWriteKey(L"Capture", L"CaptureHotkey", g_cfg.captureHotkey) && ok;
    ok = IniWriteInt(L"Memory", L"MemWarning", g_cfg.memWarning ? 1 : 0) && ok;
    g_ov.saveOk = ok;
    g_ov.saveErr = ok ? 0 : GetLastError();
    g_ov.saveMsgTick = GetTickCount();
    char u8[2048];
    if (ok)
        Log("[MENU] saved ini: %s (EnableFix=%ld ShiftNdcX=%.4f ShiftNdcY=%.4f PopupScale=%.4f Backdrop=%d FpsLimit=%d "
            "Osd=%d Borderless=%d Flip=%d Tearing=%d Keys=0x%02X/0x%02X/0x%02X/0x%02X)",
            ToUtf8(g_iniPath, u8, sizeof(u8)), static_cast<LONG>(g_fixRuntime), static_cast<double>(g_cfg.fixShiftNdcX),
            static_cast<double>(g_cfg.fixShiftNdcY), static_cast<double>(g_cfg.popupScale), g_cfg.fixBackdrop,
            g_cfg.fpsLimit, g_cfg.showFpsOverlay ? 1 : 0, g_cfg.borderless ? 1 : 0, g_cfg.flipModel ? 1 : 0,
            g_cfg.allowTearing ? 1 : 0, g_cfg.menuHotkey, g_cfg.fixToggleHotkey, g_cfg.overlayHotkey,
            g_cfg.captureHotkey);
    else
        Log("[MENU] saving ini FAILED (err %lu): %s", g_ov.saveErr, ToUtf8(g_iniPath, u8, sizeof(u8)));
}

static void MenuResetDefaults()
{
    InterlockedExchange(&g_fixRuntime, 1);
    g_cfg.fixShiftNdcX    = -0.5200f;
    g_cfg.fixShiftNdcY    = 0.0f;
    g_cfg.popupScale      = 1.0f;
    g_cfg.fixBackdrop     = 1;
    g_cfg.fixPrompt       = true;
    g_cfg.fixPopupOrigin  = true;
    g_cfg.fixSalvage      = true;
    g_cfg.fpsLimit        = 0;
    g_cfg.showFpsOverlay  = false;
    g_cfg.borderless      = false;
    g_cfg.flipModel       = false;
    g_cfg.allowTearing    = false;
    g_cfg.menuHotkey      = 0x2D; // INSERT
    g_cfg.fixToggleHotkey = 0x78; // F9
    g_cfg.overlayHotkey   = 0;    // none
    g_cfg.captureHotkey   = 0x79; // F10
    g_cfg.memWarning      = true;
    g_ov.limitCustom = false;
    g_ov.menuClearConfirm = false;
    Log("[MENU] reset to defaults");
}

// --- Menu tabs (v1.1.1) ---------------------------------------------------------------
enum MenuTab
{
    TAB_FIX,
    TAB_PERF,
    TAB_DISPLAY,
    TAB_HOTKEYS,
    TAB_INFO,
    TAB_COUNT
};
static const char* const kTabNames[TAB_COUNT] = { "FIX", "PERFORMANCE", "DISPLAY", "HOTKEYS", "INFO" };

static ImVec4 MenuWarnColor() { return ImVec4(150.0f / 255.0f, 40.0f / 255.0f, 30.0f / 255.0f, 1.0f); }

static void MenuTabFix()
{
    bool fixOn = g_fixRuntime != 0;
    if (ImGui::Checkbox("Enable fix", &fixOn))
    {
        InterlockedExchange(&g_fixRuntime, fixOn ? 1 : 0);
        Log("[FIX] toggled %s (menu)", fixOn ? "ON" : "OFF");
    }
    ImGui::SetItemTooltip("Toggle the HUD correction.");
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::Combo("Backdrop", &g_cfg.fixBackdrop, "Off\0" "Move\0" "Hide\0"))
        Log("[FIX] backdrop mode %d (menu)", g_cfg.fixBackdrop);
    ImGui::SetItemTooltip("Fixes the dark stain at the top when picking up items.");
    if (ImGui::Checkbox("Fix prompt position", &g_cfg.fixPrompt))
        Log("[FIX] prompt fix %s (menu)", g_cfg.fixPrompt ? "on" : "off");
    ImGui::SetItemTooltip("Fixes 'Press [E]' prompts appearing in the top-left corner.");
    if (ImGui::Checkbox("Fix popup in top-left", &g_cfg.fixPopupOrigin))
        Log("[FIX] popup-origin fix %s (menu)", g_cfg.fixPopupOrigin ? "on" : "off");
    ImGui::SetItemTooltip("Moves notifications that appear in the top-left corner back to the bottom right.");
    if (ImGui::Checkbox("Salvage counter top-right", &g_cfg.fixSalvage))
        Log("[FIX] salvage fix %s (menu)", g_cfg.fixSalvage ? "on" : "off");
    ImGui::SetItemTooltip("Moves the salvage counter to its original top-right position (pre-2021 layout).");
    ImGui::PopItemWidth();
    if (!g_cfg.showAdvanced)
        return; // v1.4.5: calibrated values; sliders only with [Menu] ShowAdvanced=1
    ImGui::Spacing();

    TrSectionHeader("POPUP (ADVANCED)");
    ImGui::TextDisabled("Only affects bottom-right notifications");
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::SliderFloat("Offset X", &g_cfg.fixShiftNdcX, -1.0f, 0.5f, "%.4f");
    ImGui::SetItemTooltip("Horizontal shift in NDC (2.0 = full width).");
    ImGui::TextDisabled("= %.0f px @ %u", static_cast<double>(g_cfg.fixShiftNdcX) * 0.5 * g_ov.bbW, g_ov.bbW);
    ImGui::SliderFloat("Offset Y", &g_cfg.fixShiftNdcY, -0.5f, 0.5f, "%.4f");
    ImGui::SetItemTooltip("Vertical shift in NDC (positive = up).");
    ImGui::SliderFloat("Scale", &g_cfg.popupScale, 0.5f, 1.5f, "%.2fx");
    ImGui::SetItemTooltip("Scale around the notification's right edge.");
    ImGui::PopItemWidth();
}

static void MenuTabPerf(float s)
{
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    static const int kLimitPresets[] = { 0, 30, 60, 120, 144 };
    int sel = 5;
    if (g_cfg.fpsLimit == 0)
    {
        sel = 0;
    }
    else if (!g_ov.limitCustom)
    {
        for (int i = 1; i < 5; ++i)
        {
            if (g_cfg.fpsLimit == kLimitPresets[i])
                sel = i;
        }
    }
    if (ImGui::Combo("FPS limit", &sel, "Off\0" "30\0" "60\0" "120\0" "144\0" "Custom\0"))
    {
        g_ov.limitCustom = (sel == 5);
        if (sel < 5)
            g_cfg.fpsLimit = kLimitPresets[sel];
        else if (g_cfg.fpsLimit < 20)
            g_cfg.fpsLimit = 90;
    }
    ImGui::SetItemTooltip("Caps the frame rate. Best with in-game VSync off.");
    if (sel == 5)
    {
        ImGui::SliderInt("Custom FPS", &g_cfg.fpsLimit, 20, 360);
        ImGui::SetItemTooltip("Target frame rate (20..360).");
    }
    ImGui::PopItemWidth();
    ImGui::Checkbox("FPS overlay", &g_cfg.showFpsOverlay);
    ImGui::SetItemTooltip("Small FPS counter in the top-right corner.");
    ImGui::Spacing();
    ImGui::Text("FPS %.1f | %.2f ms | 1%% low %.1f", static_cast<double>(g_perfFps), static_cast<double>(g_perfMs),
                static_cast<double>(g_perfLow1));
    // Fixed Y range so a steady frame time is a steady line: 0..2x target, else 0..50 ms.
    const float yMax = (g_cfg.fpsLimit > 0) ? 2000.0f / static_cast<float>(g_cfg.fpsLimit) : 50.0f;
    if (g_perfPlotCount > 1)
        ImGui::PlotLines("##frametimes", g_perfPlot, static_cast<int>(g_perfPlotCount), 0, nullptr, 0.0f, yMax,
                         ImVec2(ImGui::GetContentRegionAvail().x, 60.0f * s));
    ImGui::TextDisabled("Frame time graph: 0..%.0f ms", static_cast<double>(yMax));

    // v1.4.6 low address-space warning + address-space bar (cached values, max. 1 refresh/s)
    ImGui::Spacing();
    if (ImGui::Checkbox("Low memory warning", &g_cfg.memWarning))
        Log("[MEM] low memory warning %s (menu)", g_cfg.memWarning ? "on" : "off");
    ImGui::SetItemTooltip("Shows a notice when the game's 32-bit address space runs low,\n"
                          "so you can save and restart before an out-of-memory crash.");
    MemStatNoteMenu();
    if (g_memStat.valid && g_memStat.totalMB > 0)
    {
        const ULONGLONG usedMB = g_memStat.totalMB > g_memStat.availMB ? g_memStat.totalMB - g_memStat.availMB : 0ull;
        const float frac = static_cast<float>(static_cast<double>(usedMB) / static_cast<double>(g_memStat.totalMB));
        char ov[96];
        _snprintf_s(ov, sizeof(ov), _TRUNCATE, "%.2f / %.2f GB used - largest free block %llu MB",
                    static_cast<double>(usedMB) / 1024.0, static_cast<double>(g_memStat.totalMB) / 1024.0,
                    g_memStat.largestMB);
        int pushed = 0;
        if (MemLow())
        {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.80f, 0.12f, 0.10f, 1.0f)); // red
            pushed = 1;
        }
        else if (g_memStat.availMB < 2ull * g_cfg.memWarnFreeMB)
        {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.90f, 0.75f, 0.10f, 1.0f)); // yellow
            pushed = 1;
        }
        ImGui::TextUnformatted("Address space used");
        ImGui::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), ov);
        if (pushed)
            ImGui::PopStyleColor(pushed);
        ImGui::SetItemTooltip("32-bit game process: about 4 GB in total. Warning below %u MB free\n"
                              "or a largest free block below %u MB ([Memory] in the ini).",
                              g_cfg.memWarnFreeMB, g_cfg.memWarnBlockMB);
    }
}

static void MenuTabDisplay()
{
    ImGui::TextDisabled("Changes apply after a game restart.");
    ImGui::Checkbox("Borderless (experimental)", &g_cfg.borderless);
    ImGui::SetItemTooltip("Borderless window instead of exclusive fullscreen.\n"
                          "Known issue: the game still minimizes on Alt-Tab.\n"
                          "Alternative: launcher Fullscreen on, Exclusive off.");
    ImGui::Checkbox("Flip model", &g_cfg.flipModel);
    ImGui::SetItemTooltip("Modern flip presentation (advanced).");
    ImGui::BeginDisabled(!g_cfg.flipModel);
    ImGui::Checkbox("Allow tearing", &g_cfg.allowTearing);
    ImGui::SetItemTooltip("Tearing/VRR with VSync off (needs flip model).");
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("Active: %s, %s%s", g_startBorderless ? "borderless" : "fullscreen",
                        g_flipActive ? "flip" : "legacy", g_tearingActive ? ", tearing" : "");
    const bool wantTearing = g_cfg.flipModel && g_cfg.allowTearing;
    if (g_cfg.borderless != g_startBorderless || g_cfg.flipModel != g_startFlip || wantTearing != g_startTearing)
        ImGui::TextColored(MenuWarnColor(), "Restart the game to apply.");
}

static void MenuTabHotkeys(float s)
{
    HotkeyPollBind();
    const UINT rows = g_cfg.enableLogging ? kHkRows : kHkRows - 1u; // capture only with logging
    for (UINT r = 0; r < rows; ++r)
    {
        char kn[64];
        ImGui::PushID(static_cast<int>(r));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(g_hkRows[r].name);
        ImGui::SameLine(135.0f * s);
        if (g_ov.bindRow == static_cast<int>(r))
            ImGui::TextUnformatted("...");
        else
            ImGui::TextUnformatted(VkName(*g_hkRows[r].vk, kn, sizeof(kn)));
        ImGui::SameLine(235.0f * s);
        if (TrButton("Change"))
        {
            if (g_ov.bindRow == static_cast<int>(r))
                HotkeyEndBind();
            else
                HotkeyBeginBind(static_cast<int>(r));
        }
        ImGui::SetItemTooltip("Press the new key afterwards.");
        ImGui::SameLine();
        if (TrButton("Clear"))
        {
            if (r == 0 && !g_ov.menuClearConfirm)
            {
                g_ov.menuClearConfirm = true; // safety net: without a menu key only the ini can bring it back
            }
            else
            {
                *g_hkRows[r].vk = 0;
                g_ov.menuClearConfirm = false;
                Log("[MENU] hotkey %s cleared", g_hkRows[r].name);
            }
        }
        ImGui::SetItemTooltip("Remove this key.");
        ImGui::PopID();
        if (g_ov.bindRow == static_cast<int>(r))
            ImGui::TextDisabled("Press a key... (ESC = cancel)");
        if (r == 0 && g_ov.menuClearConfirm)
            ImGui::TextColored(MenuWarnColor(), "Click again to confirm");
    }
    if (g_ov.bindMsg[0] && GetTickCount() - g_ov.bindMsgTick < 3000u)
        ImGui::TextColored(MenuWarnColor(), "%s", g_ov.bindMsg);
}

static void MenuTabInfo()
{
    ImGui::Text("TR2013 HUD Fix+ v%s", kVersion);
    ImGui::Text("Resolution %ux%u", g_ov.bbW, g_ov.bbH);
    ImGui::Text("Corrected draws/s: %.0f", static_cast<double>(g_ov.rate));
    ImGui::Text("Backdrop fixes/s: %.0f", static_cast<double>(g_ov.bdRate));
    ImGui::Text("Prompt fixes/s: %.0f", static_cast<double>(g_ov.prRate));
    ImGui::Text("Popup-origin fixes/s: %.0f", static_cast<double>(g_ov.poRate));
    ImGui::Text("Salvage fixes/s: %.0f", static_cast<double>(g_ov.svRate));
    if (LoggingActive())
        ImGui::Text("Watchdog: A %ld / OFF %ld", static_cast<LONG>(g_watchATotal), static_cast<LONG>(g_watchOffTotal));
    // v1.4.6
    MemStatNoteMenu();
    if (g_memStat.valid)
        ImGui::Text("Address space: %llu MB free of %llu MB, largest block %llu MB", g_memStat.availMB,
                    g_memStat.totalMB, g_memStat.largestMB);
    ImGui::TextDisabled("Map safety: stale drops %ld, guard hits %ld, re-arms Map/Unmap/Upd %ld/%ld/%ld",
                        static_cast<LONG>(g_fixStaleDrops), static_cast<LONG>(g_scratchGuardHits),
                        static_cast<LONG>(g_rearmCount[VT_Map]), static_cast<LONG>(g_rearmCount[VT_Unmap]),
                        static_cast<LONG>(g_rearmCount[VT_UpdateSubresource]));
    if (g_gateMenuNow)
        ImGui::Text("Context: MENU (G%c)", g_gateReasonNow ? g_gateReasonNow : '?');
    else
        ImGui::Text("Context: GAMEPLAY");
    char k1[64], k2[64];
    ImGui::TextDisabled("%s menu | %s fix | ESC close", VkName(g_cfg.menuHotkey, k1, sizeof(k1)),
                        VkName(g_cfg.fixToggleHotkey, k2, sizeof(k2)));
}

// Builds the menu window. Returns true if the user closed it.
static bool OverlayBuildMenu()
{
    const float s = g_ov.uiScale;
    const DWORD now = GetTickCount();
    const ImGuiIO& io = ImGui::GetIO();

    // Corrected draws / backdrop fixes per second (sampled once per second).
    if (!g_ov.rateLastTick)
    {
        g_ov.rateLastTick = now;
        g_ov.rateLastTotal = g_fixTotal;
        g_ov.bdLastTotal = g_backdropTotal;
        g_ov.prLastTotal = g_promptTotal;
        g_ov.poLastTotal = g_poTotal;
        g_ov.svLastTotal = g_salvageTotal;
    }
    else if (now - g_ov.rateLastTick >= 1000u)
    {
        const LONG total = g_fixTotal;
        const LONG bdTotal = g_backdropTotal;
        const LONG prTotal = g_promptTotal;
        const LONG poTotal = g_poTotal;
        const float dt = static_cast<float>(now - g_ov.rateLastTick);
        g_ov.rate = static_cast<float>(total - g_ov.rateLastTotal) * 1000.0f / dt;
        g_ov.bdRate = static_cast<float>(bdTotal - g_ov.bdLastTotal) * 1000.0f / dt;
        g_ov.rateLastTotal = total;
        g_ov.bdLastTotal = bdTotal;
        g_ov.prRate = static_cast<float>(prTotal - g_ov.prLastTotal) * 1000.0f / dt;
        g_ov.prLastTotal = prTotal;
        g_ov.poRate = static_cast<float>(poTotal - g_ov.poLastTotal) * 1000.0f / dt;
        g_ov.poLastTotal = poTotal;
        const LONG svTotal = g_salvageTotal;
        g_ov.svRate = static_cast<float>(svTotal - g_ov.svLastTotal) * 1000.0f / dt;
        g_ov.svLastTotal = svTotal;
        g_ov.rateLastTick = now;
    }

    // Fixed width, automatic height up to the display height minus a margin.
    const float winW = 420.0f * s;
    const float maxH = Max(200.0f * s, io.DisplaySize.y - 80.0f * s);
    if (g_ov.firstLayout)
    {
        ImGui::SetNextWindowPos(ImVec2(40.0f * s, 40.0f * s), ImGuiCond_FirstUseEver);
        g_ov.firstLayout = false;
    }
    else if (g_ov.clampPending && g_ov.havePos)
    {
        // Keep the whole window on screen when (re)opening.
        ImVec2 p = g_ov.winPos;
        p.x = Max(0.0f, Min(p.x, io.DisplaySize.x - g_ov.winSize.x));
        p.y = Max(0.0f, Min(p.y, io.DisplaySize.y - g_ov.winSize.y));
        ImGui::SetNextWindowPos(p, ImGuiCond_Always);
    }
    g_ov.clampPending = false;
    ImGui::SetNextWindowSizeConstraints(ImVec2(winW, 0.0f), ImVec2(winW, maxH));

    bool open = true;
    if (!ImGui::Begin("TR2013 HUD FIX+###trhf_menu", &open,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return !open;
    }

    TrLogoHeader();
    ImGui::Spacing();

    // Tabs in the TR style: the selected tab is white with black text.
    int newTab = g_ov.tab;
    if (ImGui::BeginTabBar("##trhf_tabs", ImGuiTabBarFlags_FittingPolicyShrink | ImGuiTabBarFlags_NoTooltip))
    {
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            const bool current = (i == g_ov.tab);
            if (current)
            {
                // ImGui draws a hovered/held tab with TabHovered even if it is the
                // selected one: keep it white so the black text stays readable.
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            }
            const bool selected = ImGui::BeginTabItem(kTabNames[i]);
            if (current)
                ImGui::PopStyleColor(2);
            if (selected)
            {
                newTab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    g_ov.tab = newTab;

    // Tab content in a child that scrolls if the display is too small.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                          st.ItemSpacing.y * 3.0f + st.WindowPadding.y;
    const float childMax = Max(80.0f * s, maxH - ImGui::GetCursorPosY() - footerH);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, childMax));
    if (ImGui::BeginChild("##trhf_tab", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY))
    {
        ImGui::Spacing();
        switch (g_ov.tab)
        {
        case TAB_PERF:    MenuTabPerf(s); break;
        case TAB_DISPLAY: MenuTabDisplay(); break;
        case TAB_HOTKEYS: MenuTabHotkeys(s); break;
        case TAB_INFO:    MenuTabInfo(); break;
        default:          MenuTabFix(); break;
        }
        ImGui::Spacing();
    }
    ImGui::EndChild();

    // Footer, always visible.
    ImGui::Separator();
    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
    if (TrButton("Reset to defaults", half))
        MenuResetDefaults();
    ImGui::SetItemTooltip("Restore defaults (not saved).");
    ImGui::SameLine();
    if (TrButton("Save to ini", half))
        MenuSaveIni();
    ImGui::SetItemTooltip("Write the current values to tr_hud_fix.ini.");
    if (g_ov.saveMsgTick && now - g_ov.saveMsgTick < 2000u)
    {
        if (g_ov.saveOk)
            ImGui::Text("Saved");
        else
            ImGui::TextColored(MenuWarnColor(), "Save failed (%lu)", g_ov.saveErr);
    }
    else
    {
        ImGui::TextDisabled(" "); // keep the height stable
    }

    if (!HotkeysSuppressed() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        open = false;
    g_ov.winPos = ImGui::GetWindowPos();
    g_ov.winSize = ImGui::GetWindowSize();
    g_ov.havePos = true;
    ImGui::End();
    return !open;
}

// v1.4.3 automatic hook benchmark (BenchHotkey): runs every test mode for
// kBenchSettleSec + kBenchMeasureSec, then writes tr_hud_fix_perf.txt next to
// the ini and shows the table in the FPS overlay. Render thread only.
static const LONG   kBenchSeq[]      = { 0, 2, 6, 0, 2 };
static const UINT   kBenchSteps      = static_cast<UINT>(sizeof(kBenchSeq) / sizeof(kBenchSeq[0]));
static const double kBenchSettleSec  = 2.0;
static const double kBenchMeasureSec = 6.0;
static const UINT   kBenchMaxFrames  = 4096;
struct BenchResult { LONG mode; float fps, ms, low1; UINT frames; };
static int         g_benchStep = -1;     // -1 = idle
static LONGLONG    g_benchStepStart = 0;
static LONGLONG    g_benchLastQpc = 0;
static float       g_benchFt[kBenchMaxFrames] = {};
static UINT        g_benchN = 0;
static BenchResult g_benchRes[kBenchSteps] = {};
static UINT        g_benchResCount = 0;
static bool        g_benchKeyWasDown = false;
static float       g_benchCalls[PG_COUNT] = {}; // calls/frame snapshot from the first "Normal" step
static float       g_benchCallsMap[3] = {};
static float       g_benchHits[3] = {}; // CB-shadow map hits, shadow copies, redirected maps per frame (Normal)
static LONG        g_benchHudSet = 0;
static bool        g_benchDone = false;

// Small FPS counter (top-right, no input), also while the menu is closed.
static void OverlayBuildFpsOsd()
{
    const float s = g_ov.uiScale;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 6.0f * s, 6.0f * s), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove;
    // v1.4.1: small white notification-style bar with black text (like the menu header).
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(240.0f / 255.0f, 240.0f / 255.0f, 235.0f / 255.0f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 3.0f * s));
    if (ImGui::Begin("##trhf_fps", nullptr, flags))
    {
        ImGui::Text("%.0f FPS  %.1f ms  1%% %.0f", static_cast<double>(g_perfFps), static_cast<double>(g_perfMs),
                    static_cast<double>(g_perfLow1));
        if (g_cfg.perfTestHotkey || g_cfg.benchHotkey)
        {
            const LONG m = g_perfMode;
            char label[64];
            PerfModeLabel(m, label, sizeof(label));
            ImGui::Text("%s | fix %s", label, (m == 0 && g_fixRuntime) ? "ON" : "OFF");
            if (g_benchStep >= 0)
                ImGui::Text("BENCHMARK step %d/%u - stand still! (F8 = abort)", g_benchStep + 1, kBenchSteps);
            else if (g_benchDone)
            {
                ImGui::Text("Benchmark done -> tr_hud_fix_perf.txt (F8 = again)");
                for (UINT i = 0; i < g_benchResCount; ++i)
                {
                    char bl[64];
                    PerfModeLabel(g_benchRes[i].mode, bl, sizeof(bl));
                    ImGui::Text("%-30s %5.1f FPS  1%% %5.1f", bl, static_cast<double>(g_benchRes[i].fps),
                                static_cast<double>(g_benchRes[i].low1));
                }
            }
            ImGui::Text("calls/frame: draw %.0f cb %.0f sh %.0f ia %.0f map %.0f rs %.0f",
                        static_cast<double>(g_callPerFrame[PG_DRAW]), static_cast<double>(g_callPerFrame[PG_CB]),
                        static_cast<double>(g_callPerFrame[PG_SHADER]), static_cast<double>(g_callPerFrame[PG_IA]),
                        static_cast<double>(g_callPerFrame[PG_MAP]), static_cast<double>(g_callPerFrame[PG_RS]));
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// v1.4.6 low-memory toast: top centre, no input, opaque dark red with white
// text (readable on any background), also while the menu is closed.
static void OverlayBuildMemToast()
{
    const float s = g_ov.uiScale;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f * s), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.45f, 0.06f, 0.04f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 8.0f * s));
    if (ImGui::Begin("##trhf_memtoast", nullptr, flags))
        ImGui::TextUnformatted(g_memToastText);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}

static void BenchWriteFile()
{
    wchar_t path[1024];
    wcsncpy_s(path, g_iniPath, _TRUNCATE);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    slash[1] = 0;
    wcsncat_s(path, L"tr_hud_fix_perf.txt", _TRUNCATE);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "==== TR2013 HUD Fix+ v%s hook benchmark %04u-%02u-%02u %02u:%02u:%02u ====\r\n", kVersion, st.wYear,
            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fprintf(f, "calls/frame (Normal): draw %.0f cb %.0f shader %.0f ia %.0f map %.0f rs %.0f\r\n",
            static_cast<double>(g_benchCalls[PG_DRAW]), static_cast<double>(g_benchCalls[PG_CB]),
            static_cast<double>(g_benchCalls[PG_SHADER]), static_cast<double>(g_benchCalls[PG_IA]),
            static_cast<double>(g_benchCalls[PG_MAP]), static_cast<double>(g_benchCalls[PG_RS]));
    fprintf(f, "per frame (Normal): HUD-CB map hits %.0f, shadow copies %.0f, redirected %.0f, HUD buffer set %ld entries\r\n",
            static_cast<double>(g_benchHits[0]), static_cast<double>(g_benchHits[1]),
            static_cast<double>(g_benchHits[2]), g_benchHudSet);
    fprintf(f, "calls/frame (Normal): Map %.0f Unmap %.0f UpdateSubresource %.0f\r\n",
            static_cast<double>(g_benchCallsMap[0]), static_cast<double>(g_benchCallsMap[1]),
            static_cast<double>(g_benchCallsMap[2]));
    fprintf(f, "map safety (session): stale map drops %ld, scratch guard hits %ld, re-arms Map %ld Unmap %ld "
               "UpdateSubresource %ld, foreign entries left alone %ld\r\n",
            static_cast<LONG>(g_fixStaleDrops), static_cast<LONG>(g_scratchGuardHits),
            static_cast<LONG>(g_rearmCount[VT_Map]), static_cast<LONG>(g_rearmCount[VT_Unmap]),
            static_cast<LONG>(g_rearmCount[VT_UpdateSubresource]), static_cast<LONG>(g_rearmForeign));
    for (UINT i = 0; i < g_perfTraceCount; ++i)
    {
        const PerfTrace& tr = g_perfTrace[i];
        fprintf(f, "switch %ld->%ld vtbl=%p", tr.from, tr.to, tr.vtbl);
        static const char* const kN[3] = { "Map", "Unmap", "UpdSub" };
        for (UINT j = 0; j < 3; ++j)
        {
            char mod[64] = "?";
            HMODULE hm = nullptr;
            if (tr.cur[j] && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                static_cast<LPCSTR>(tr.cur[j]), &hm) && hm)
            {
                char full[MAX_PATH];
                if (GetModuleFileNameA(hm, full, MAX_PATH))
                {
                    const char* b = strrchr(full, '\\');
                    strncpy_s(mod, b ? b + 1 : full, _TRUNCATE);
                }
            }
            fprintf(f, " | %s cur=%p(%s) orig=%p", kN[j], tr.cur[j], mod, tr.orig[j]);
        }
        fprintf(f, "\r\n");
    }
    for (UINT i = 0; i < g_benchResCount; ++i)
    {
        char label[64];
        PerfModeLabel(g_benchRes[i].mode, label, sizeof(label));
        fprintf(f, "%-34s %6.1f FPS  %6.2f ms  1%%low %6.1f  (%u frames)\r\n", label,
                static_cast<double>(g_benchRes[i].fps), static_cast<double>(g_benchRes[i].ms),
                static_cast<double>(g_benchRes[i].low1), g_benchRes[i].frames);
    }
    fprintf(f, "\r\n");
    fclose(f);
}

static void BenchFinishStep()
{
    BenchResult r = {};
    r.mode = kBenchSeq[g_benchStep];
    r.frames = g_benchN;
    if (g_benchN > 0)
    {
        double sum = 0.0;
        for (UINT i = 0; i < g_benchN; ++i)
            sum += g_benchFt[i];
        std::sort(g_benchFt, g_benchFt + g_benchN);
        UINT idx = static_cast<UINT>(ceil(0.99 * static_cast<double>(g_benchN)));
        idx = idx ? idx - 1u : 0u;
        r.ms = static_cast<float>(sum / g_benchN);
        r.fps = r.ms > 0.0f ? 1000.0f / r.ms : 0.0f;
        r.low1 = g_benchFt[idx] > 0.0f ? 1000.0f / g_benchFt[idx] : 0.0f;
    }
    if (r.mode == 0 && g_benchResCount == 0)
    {
        memcpy(g_benchCalls, g_callPerFrame, sizeof(g_benchCalls));
        memcpy(g_benchCallsMap, g_callPerFrameMap, sizeof(g_benchCallsMap));
        g_benchHits[0] = g_mapHitPerFrame;
        g_benchHits[1] = g_shadowPerFrame;
        g_benchHits[2] = g_redirPerFrame;
        g_benchHudSet = g_hudSetUsed;
    }
    if (g_benchResCount < kBenchSteps)
        g_benchRes[g_benchResCount++] = r;
}

static void BenchStartStep(IDXGISwapChain* sc, int step)
{
    g_benchStep = step;
    g_benchN = 0;
    g_benchStepStart = QpcNow();
    if (g_perfMode != kBenchSeq[step])
        PerfModeApply(sc, kBenchSeq[step]);
}

// v1.4.6: overlay/perf hotkeys are sampled every 50 ms (not every Present).
// Besides the "down now" bit, bit 0 ("pressed since the previous
// GetAsyncKeyState call") counts as down, so a tap shorter than the poll
// interval is not lost. tapBit is false for the very first sample (stale bit).
static const DWORD kHotkeyPollMs = 50;
static DWORD g_hotkeyPollTick = 0;
static bool  g_hotkeyPolled   = false; // at least one sample taken

static inline bool HotkeySample(UINT vk, bool tapBit)
{
    const USHORT s = static_cast<USHORT>(GetAsyncKeyState(static_cast<int>(vk)));
    return (s & 0x8000u) != 0 || (tapBit && (s & 1u) != 0);
}

static void BenchTick(IDXGISwapChain* sc, bool suppressed, bool pollKeys, bool tapBit)
{
    if (g_cfg.benchHotkey && pollKeys)
    {
        const bool down = HotkeySample(g_cfg.benchHotkey, tapBit);
        if (down && !g_benchKeyWasDown && !suppressed)
        {
            if (g_benchStep < 0)
            {
                g_benchResCount = 0;
                g_benchDone = false;
                g_benchLastQpc = 0;
                g_perfTraceCount = 0;
                Log("[PERF] benchmark started");
                BenchStartStep(sc, 0);
            }
            else
            {
                g_benchStep = -1; // abort
                PerfModeApply(sc, 0);
                Log("[PERF] benchmark aborted");
            }
        }
        g_benchKeyWasDown = down;
    }
    if (g_benchStep < 0)
        return;
    const LONGLONG now = QpcNow();
    const LONGLONG freq = QpcFreq();
    const double inStep = static_cast<double>(now - g_benchStepStart) / static_cast<double>(freq);
    if (g_benchLastQpc && inStep >= kBenchSettleSec && g_benchN < kBenchMaxFrames)
        g_benchFt[g_benchN++] = static_cast<float>(static_cast<double>(now - g_benchLastQpc) * 1000.0 / freq);
    g_benchLastQpc = now;
    if (inStep < kBenchSettleSec + kBenchMeasureSec)
        return;
    BenchFinishStep();
    if (g_benchStep + 1 < static_cast<int>(kBenchSteps))
    {
        BenchStartStep(sc, g_benchStep + 1);
        return;
    }
    g_benchStep = -1;
    g_benchDone = true;
    if (g_perfMode != 0)
        PerfModeApply(sc, 0);
    BenchWriteFile();
    Log("[PERF] benchmark finished");
}

// Present hook on the game swap chain (before the original Present).
static void OverlayBeforePresent(IDXGISwapChain* sc)
{
    const DWORD keyNow = GetTickCount();
    const bool pollKeys = !g_hotkeyPolled || keyNow - g_hotkeyPollTick >= kHotkeyPollMs;
    const bool tapBit = g_hotkeyPolled;
    const bool suppressed = pollKeys && HotkeysSuppressed();
    if (pollKeys)
    {
        g_hotkeyPollTick = keyNow;
        g_hotkeyPolled = true;
        if (g_cfg.menuHotkey)
        {
            const bool down = HotkeySample(g_cfg.menuHotkey, tapBit);
            if (down && !g_ov.menuKeyWasDown && !suppressed)
                OverlaySetOpen(g_menuOpen == 0, sc);
            g_ov.menuKeyWasDown = down;
        }
        if (g_cfg.overlayHotkey)
        {
            const bool down = HotkeySample(g_cfg.overlayHotkey, tapBit);
            if (down && !g_ov.overlayKeyWasDown && !suppressed)
            {
                g_cfg.showFpsOverlay = !g_cfg.showFpsOverlay;
                Log("[PERF] FPS overlay %s (hotkey)", g_cfg.showFpsOverlay ? "on" : "off");
            }
            g_ov.overlayKeyWasDown = down;
        }
        if (g_cfg.perfTestHotkey)
        {
            const bool down = HotkeySample(g_cfg.perfTestHotkey, tapBit);
            if (down && !g_ov.perfKeyWasDown && !suppressed)
                PerfModeApply(sc, (g_perfMode + 1) % kPerfModes);
            g_ov.perfKeyWasDown = down;
        }
    }
    BenchTick(sc, suppressed, pollKeys, tapBit);
    const bool menu = g_menuOpen != 0;
    const bool other = menu || g_cfg.showFpsOverlay || g_benchStep >= 0 || g_benchDone;
    g_perfStatsWanted = other; // PerfOnPresent: frame-time statistics only while displayed
    const bool toast = MemToastActive(); // v1.4.6
    // v1.4.6: with MemWarning on, initialise the overlay early in the session
    // (~10 s of frames), while address space is still plentiful, so the
    // low-memory toast can always be drawn later without loading d3dcompiler /
    // building the font atlas at the worst possible moment. MenuHotkey=0 (no
    // overlay wanted) keeps the old behaviour: log / event ring only.
    if (!g_ov.initTried && g_cfg.memWarning && g_cfg.menuHotkey && g_frameCount > 600)
        OverlayInit(sc);
    if (!other && !toast)
        return; // nothing shown: no ImGui work at all
    if (!g_ov.initTried)
    {
        // v1.4.6: the low-memory toast alone never initialises ImGui (d3dcompiler,
        // shaders, font atlas = address space) - it is only drawn if the overlay
        // already exists; otherwise the warning stays in the log / event ring.
        if (!other)
            return;
        OverlayInit(sc);
    }
    if (!g_ov.ok)
        return;

    bool close = false;
    // v1.4.6: RTV check/creation inside g_imguiCs (ResizeBuffers releases it under the same lock).
    EnterCriticalSection(&g_imguiCs);
    if (!g_ov.rtv && !OverlayCreateRTV(sc))
    {
        LeaveCriticalSection(&g_imguiCs);
        return;
    }
    t_inOverlay = true;
    ImGuiIO& io = ImGui::GetIO();
    // Cursor only while the menu is open; the FPS overlay must not touch it.
    io.MouseDrawCursor = menu;
    if (menu)
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Exclusive fullscreen: client rect may differ from the back buffer. Keep
    // ImGui (and mouse) in client coordinates and stretch to the back buffer.
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f && g_ov.bbW && g_ov.bbH)
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(g_ov.bbW) / io.DisplaySize.x,
                                            static_cast<float>(g_ov.bbH) / io.DisplaySize.y);
    ImGui::NewFrame();
    if (menu)
        close = OverlayBuildMenu();
    if (g_cfg.showFpsOverlay || g_benchStep >= 0 || g_benchDone)
        OverlayBuildFpsOsd();
    if (toast)
        OverlayBuildMemToast();
    ImGui::Render();

    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_ov.ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv); // AddRef'd
    g_ov.ctx->OMSetRenderTargets(1, &g_ov.rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); // backend saves/restores the remaining state
    g_ov.ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (oldRtv[i])
            oldRtv[i]->Release();
    }
    if (oldDsv)
        oldDsv->Release();
    LeaveCriticalSection(&g_imguiCs);
    t_inOverlay = false;

    if (close)
        OverlaySetOpen(false, sc);
}

// Device-level hooks (shader/layout registry) on the game device.
static void HookDevice(ID3D11Device* dev, const char* origin)
{
    void* const hooks[kMaxClassIdx] = { reinterpret_cast<void*>(&Hook_CreateInputLayout),
                                        reinterpret_cast<void*>(&Hook_CreateVertexShader),
                                        reinterpret_cast<void*>(&Hook_CreatePixelShader), nullptr };
    InstallClassHooks(g_deviceHooks, hooks, dev, origin);
}

// =============================================================================
//  Device creation (called from the proxy exports)
// =============================================================================
static thread_local int t_createDepth = 0; // suppresses nested/internal device-creation calls
static LONG g_firstDevice = 0;

static void OnDeviceCreated(const char* api, HRESULT hr, IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE driverType,
                            UINT flags, const D3D_FEATURE_LEVEL* pLevels, UINT numLevels, UINT sdkVersion,
                            const DXGI_SWAP_CHAIN_DESC* pReqDesc, IDXGISwapChain** ppSwapChain,
                            ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                            ID3D11DeviceContext** ppImmediateContext)
{
    ID3D11Device* dev = (SUCCEEDED(hr) && ppDevice) ? *ppDevice : nullptr;

    if (LoggingActive())
    {
        LineBuf lvl;
        if (pLevels && numLevels)
        {
            for (UINT i = 0; i < numLevels && i < 16; ++i)
                lvl.Add("%s0x%X", i ? "," : "", static_cast<unsigned>(pLevels[i]));
        }
        else
        {
            lvl.Add("default");
        }
        D3D_FEATURE_LEVEL got = static_cast<D3D_FEATURE_LEVEL>(0);
        if (SUCCEEDED(hr))
        {
            if (pFeatureLevel)
                got = *pFeatureLevel;
            else if (dev)
                got = dev->GetFeatureLevel();
        }
        Log("[DEVICE] %s hr=0x%08lX adapter=%p driverType=%d flags=0x%X sdk=%u requestedFL=[%s] obtainedFL=0x%X "
            "device=%p",
            api, static_cast<unsigned long>(hr), static_cast<void*>(pAdapter), static_cast<int>(driverType), flags,
            sdkVersion, lvl.s, static_cast<unsigned>(got), static_cast<void*>(dev));
    }

    if (!dev)
    {
        if (pReqDesc)
            LogSwapChainDesc("requested", pReqDesc);
        return;
    }

    if (InterlockedExchange(&g_firstDevice, 1) == 0)
    {
        LateInit(); // DPI report (logging only)
        QueryAndLogDpi("first D3D11 device", pReqDesc ? pReqDesc->OutputWindow : nullptr);
    }

    ID3D11DeviceContext* ctx = (ppImmediateContext && *ppImmediateContext) ? *ppImmediateContext : nullptr;
    if (ctx)
    {
        InstallContextHooks(ctx, api);
    }
    else
    {
        ID3D11DeviceContext* tmp = nullptr;
        dev->GetImmediateContext(&tmp); // AddRefs
        if (tmp)
        {
            InstallContextHooks(tmp, api);
            tmp->Release();
        }
    }

    // Frame boundaries: swap chain created together with the device, or later
    // via the factory (IDXGIFactory::CreateSwapChain hook).
    if (pReqDesc)
        OnSwapChainCreated(api, hr, pReqDesc, ppSwapChain ? *ppSwapChain : nullptr);
    HookFactoryFromDevice(dev);
    HookDevice(dev, api); // shader bytecode hashes + input layouts
}

static HRESULT DoCreateDevice(PFN_D3D11_CREATE_DEVICE real, const char* api, IDXGIAdapter* pAdapter,
                              D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                              const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
                              ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                              ID3D11DeviceContext** ppImmediateContext)
{
    if (!real)
    {
        Log("[DEVICE] %s: real function not available -> E_FAIL", api);
        return E_FAIL;
    }
    const bool outer = (t_createDepth++ == 0);
    const HRESULT hr = real(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice,
                            pFeatureLevel, ppImmediateContext);
    --t_createDepth;
    if (outer && g_active && !g_shutdown)
        OnDeviceCreated(api, hr, pAdapter, DriverType, Flags, pFeatureLevels, FeatureLevels, SDKVersion, nullptr,
                        nullptr, ppDevice, pFeatureLevel, ppImmediateContext);
    return hr;
}

static HRESULT DoCreateDeviceAndSwapChain(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN real, const char* api,
                                          IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                          UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                          UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                          IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                          D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    if (!real)
    {
        Log("[DEVICE] %s: real function not available -> E_FAIL", api);
        return E_FAIL;
    }
    const bool outer = (t_createDepth++ == 0);
    DXGI_SWAP_CHAIN_DESC flipDesc = {};
    bool tearing = false;
    const bool flip = outer && g_active && !g_shutdown && PrepareFlipDesc(pSwapChainDesc, &flipDesc, nullptr, &tearing);
    HRESULT hr = real(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                      flip ? &flipDesc : pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    const bool flipOk = flip && SUCCEEDED(hr);
    if (flip && FAILED(hr))
    {
        Log("[DXGI] flip model failed hr=0x%08lX, fallback legacy", static_cast<unsigned long>(hr));
        hr = real(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
                  ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    }
    if (flip)
    {
        g_flipActive = flipOk;
        g_tearingActive = flipOk && tearing;
        if (flipOk)
            Log("[DXGI] flip model on: FLIP_DISCARD buffers=%u tearing=%d", flipDesc.BufferCount, tearing ? 1 : 0);
    }
    --t_createDepth;
    if (outer && g_active && !g_shutdown)
        OnDeviceCreated(api, hr, pAdapter, DriverType, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                        flipOk ? &flipDesc : pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    return hr;
}

// =============================================================================
//  PROXY (d3d11.dll): chain loading + exports
// =============================================================================

static HMODULE g_hTarget = nullptr;
static PFN_D3D11_CREATE_DEVICE g_realCreateDevice = nullptr;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_realCreateDeviceAndSwapChain = nullptr;
static INIT_ONCE g_resolveOnce = INIT_ONCE_STATIC_INIT;

extern "C" void __cdecl TRHF_ResolveAll(void);

// --- Fallback stubs for pass-through exports ---------------------------------
// Every pass-through pointer starts out pointing at one of these. If the
// target DLL does not export a function (e.g. D3D11On12CreateDevice on Win7),
// the call returns STATUS_NOT_SUPPORTED (0xC00000BB, a failure code both as
// NTSTATUS and as HRESULT) and pops exactly the stdcall argument bytes, so the
// caller's stack stays balanced. Hence every forwarded export below must have
// a known signature (byte count).
#pragma warning(push)
#pragma warning(disable : 4731 4740)
extern "C" __declspec(naked) void __cdecl TRHF_Fail_4(void)
{
    __asm {
        mov eax, 0xC00000BB
        ret 4
    }
}
extern "C" __declspec(naked) void __cdecl TRHF_Fail_8(void)
{
    __asm {
        mov eax, 0xC00000BB
        ret 8
    }
}
extern "C" __declspec(naked) void __cdecl TRHF_Fail_20(void)
{
    __asm {
        mov eax, 0xC00000BB
        ret 20
    }
}
extern "C" __declspec(naked) void __cdecl TRHF_Fail_24(void)
{
    __asm {
        mov eax, 0xC00000BB
        ret 24
    }
}
extern "C" __declspec(naked) void __cdecl TRHF_Fail_40(void)
{
    __asm {
        mov eax, 0xC00000BB
        ret 40
    }
}

// --- Pass-through export list: X(name, stdcall argument bytes) ----------------
// Must match src\exports.def. Only functions with a well-known signature:
//   D3D11CoreCreateDevice(IDXGIFactory*, IDXGIAdapter*, UINT, const D3D_FEATURE_LEVEL*, UINT, ID3D11Device**) = 24
//   D3D11CoreCreateLayeredDevice(const void*, DWORD, const void*, REFIID, void**)                              = 20
//   D3D11CoreGetLayeredDeviceSize(const void*, DWORD)                                                         = 8
//   D3D11CoreRegisterLayers(const void*, DWORD)                                                               = 8
//   CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*, IInspectable**)                         (Win8.1+)      = 8
//   CreateDirect3D11SurfaceFromDXGISurface(IDXGISurface*, IInspectable**)                      (Win8.1+)      = 8
//   D3D11On12CreateDevice(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*,
//                         UINT, UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*) (Win10)    = 40
//   OpenAdapter10 / OpenAdapter10_2(D3D10DDIARG_OPENADAPTER*)                                                 = 4
//   D3DKMT*(const D3DKMT_xxx*) - every D3DKMT thunk takes exactly one pointer                                 = 4
#define TRHF_FORWARD_LIST(X)                          \
    X(D3D11CoreCreateDevice, 24)                      \
    X(D3D11CoreCreateLayeredDevice, 20)               \
    X(D3D11CoreGetLayeredDeviceSize, 8)               \
    X(D3D11CoreRegisterLayers, 8)                     \
    X(CreateDirect3D11DeviceFromDXGIDevice, 8)        \
    X(CreateDirect3D11SurfaceFromDXGISurface, 8)      \
    X(D3D11On12CreateDevice, 40)                      \
    X(OpenAdapter10, 4)                               \
    X(OpenAdapter10_2, 4)                             \
    X(D3DKMTCloseAdapter, 4)                          \
    X(D3DKMTCreateAllocation, 4)                      \
    X(D3DKMTCreateContext, 4)                         \
    X(D3DKMTCreateDevice, 4)                          \
    X(D3DKMTCreateSynchronizationObject, 4)           \
    X(D3DKMTDestroyAllocation, 4)                     \
    X(D3DKMTDestroyContext, 4)                        \
    X(D3DKMTDestroyDevice, 4)                         \
    X(D3DKMTDestroySynchronizationObject, 4)          \
    X(D3DKMTEscape, 4)                                \
    X(D3DKMTGetContextSchedulingPriority, 4)          \
    X(D3DKMTGetDeviceState, 4)                        \
    X(D3DKMTGetDisplayModeList, 4)                    \
    X(D3DKMTGetMultisampleMethodList, 4)              \
    X(D3DKMTGetRuntimeData, 4)                        \
    X(D3DKMTGetSharedPrimaryHandle, 4)                \
    X(D3DKMTLock, 4)                                  \
    X(D3DKMTOpenAdapterFromHdc, 4)                    \
    X(D3DKMTOpenResource, 4)                          \
    X(D3DKMTPresent, 4)                               \
    X(D3DKMTQueryAdapterInfo, 4)                      \
    X(D3DKMTQueryAllocationResidency, 4)              \
    X(D3DKMTQueryResourceInfo, 4)                     \
    X(D3DKMTRender, 4)                                \
    X(D3DKMTSetAllocationPriority, 4)                 \
    X(D3DKMTSetContextSchedulingPriority, 4)          \
    X(D3DKMTSetDisplayMode, 4)                        \
    X(D3DKMTSetDisplayPrivateDriverFormat, 4)         \
    X(D3DKMTSetGammaRamp, 4)                          \
    X(D3DKMTSetVidPnSourceOwner, 4)                   \
    X(D3DKMTSignalSynchronizationObject, 4)           \
    X(D3DKMTUnlock, 4)                                \
    X(D3DKMTWaitForSynchronizationObject, 4)          \
    X(D3DKMTWaitForVerticalBlankEvent, 4)

// Naked stub: preserve all registers + flags, resolve the target lazily
// (thread-safe InitOnce), then tail-jump to the real export (or the fail stub)
// with the caller's untouched stack frame.
#define TRHF_DECLARE_FWD(name, bytes)                                           \
    static void* g_pfn_##name = reinterpret_cast<void*>(&TRHF_Fail_##bytes);    \
    extern "C" __declspec(naked) void __cdecl Fwd_##name(void)                  \
    {                                                                           \
        __asm {                                                                 \
            __asm pushad                                                        \
            __asm pushfd                                                        \
            __asm call TRHF_ResolveAll                                          \
            __asm popfd                                                         \
            __asm popad                                                         \
            __asm jmp dword ptr [g_pfn_##name]                                  \
        }                                                                       \
    }

TRHF_FORWARD_LIST(TRHF_DECLARE_FWD)
#pragma warning(pop)

#define TRHF_FWD_ENTRY(name, bytes) { #name, &g_pfn_##name },
struct FwdEntry
{
    const char* name;
    void**      slot;
};
static const FwdEntry kForwards[] = { TRHF_FORWARD_LIST(TRHF_FWD_ENTRY) };

static bool IsAbsolutePath(const wchar_t* p)
{
    return (p[0] && p[1] == L':') || p[0] == L'\\' || p[0] == L'/';
}

static HMODULE TryLoadTarget(const wchar_t* path, const char* how)
{
    char u8[4096];
    HMODULE h = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h)
    {
        const DWORD err = GetLastError();
        Log("[PROXY] %s: LoadLibrary failed (err %lu): %s", how, err, ToUtf8(path, u8, sizeof(u8)));
        return nullptr;
    }
    if (h == g_hSelf)
    {
        // Our own module - never chain to ourselves (no FreeLibrary: the
        // extra reference is harmless, the module is pinned anyway).
        Log("[PROXY] %s: resolves to this proxy itself - skipped: %s", how, ToUtf8(path, u8, sizeof(u8)));
        return nullptr;
    }
    Log("[PROXY] %s: loaded %s (module %p)", how, ToUtf8(path, u8, sizeof(u8)), static_cast<void*>(h));
    return h;
}

static BOOL CALLBACK ResolveCb(PINIT_ONCE, PVOID, PVOID*)
{
    HMODULE h = nullptr;
    wchar_t path[2048] = {};

    // 1. Explicit ProxyDll from the ini (relative to our folder or absolute).
    if (g_cfg.proxyDll[0])
    {
        if (IsAbsolutePath(g_cfg.proxyDll))
        {
            wcscpy_s(path, g_cfg.proxyDll);
        }
        else
        {
            wcscpy_s(path, g_selfDir);
            wcscat_s(path, g_cfg.proxyDll);
        }
        h = TryLoadTarget(path, "ini ProxyDll");
    }

    // 2. Auto-detected chain targets next to us (only if the file exists).
    static const wchar_t* const kAuto[] = { L"d3d11_orig.dll", L"dxvk_d3d11.dll" };
    for (size_t i = 0; !h && i < ARRAYSIZE(kAuto); ++i)
    {
        wcscpy_s(path, g_selfDir);
        wcscat_s(path, kAuto[i]);
        if (FileExists(path))
            h = TryLoadTarget(path, "auto chain");
    }

    // 3. System d3d11.dll (SysWOW64 on 64-bit Windows, System32 on 32-bit).
    if (!h)
    {
        wchar_t sys[MAX_PATH + 32] = {};
        UINT n = GetSystemWow64DirectoryW(sys, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            wcscat_s(sys, L"\\d3d11.dll");
            h = TryLoadTarget(sys, "system");
        }
    }

    g_hTarget = h;
    if (h)
    {
        g_realCreateDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(h, "D3D11CreateDevice"));
        g_realCreateDeviceAndSwapChain =
            reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(h, "D3D11CreateDeviceAndSwapChain"));
        Log("[PROXY] D3D11CreateDevice=%p D3D11CreateDeviceAndSwapChain=%p",
            reinterpret_cast<void*>(g_realCreateDevice), reinterpret_cast<void*>(g_realCreateDeviceAndSwapChain));

        UINT missing = 0;
        LineBuf lb;
        for (size_t i = 0; i < ARRAYSIZE(kForwards); ++i)
        {
            FARPROC fp = GetProcAddress(h, kForwards[i].name);
            if (fp)
            {
                *kForwards[i].slot = reinterpret_cast<void*>(fp);
            }
            else
            {
                ++missing;
                lb.Add(" %s", kForwards[i].name);
            }
        }
        Log("[PROXY] pass-through exports resolved: %u/%u", static_cast<UINT>(ARRAYSIZE(kForwards)) - missing,
            static_cast<UINT>(ARRAYSIZE(kForwards)));
        if (missing)
            Log("[PROXY] not exported by target (stubbed, return 0xC00000BB):%s", lb.s);
    }
    else
    {
        Log("[PROXY] FATAL: no target d3d11.dll could be loaded - all D3D11 calls will fail");
    }

    LateInit();
    return TRUE;
}

extern "C" void __cdecl TRHF_ResolveAll(void)
{
    InitOnceExecuteOnce(&g_resolveOnce, ResolveCb, nullptr, nullptr);
}

extern "C" HRESULT WINAPI Proxy_D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                  UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                                  UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice,
                                                  D3D_FEATURE_LEVEL* pFeatureLevel,
                                                  ID3D11DeviceContext** ppImmediateContext)
{
    TRHF_ResolveAll();
    return DoCreateDevice(g_realCreateDevice, "D3D11CreateDevice", pAdapter, DriverType, Software, Flags,
                          pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
}

extern "C" HRESULT WINAPI Proxy_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    TRHF_ResolveAll();
    return DoCreateDeviceAndSwapChain(g_realCreateDeviceAndSwapChain, "D3D11CreateDeviceAndSwapChain", pAdapter,
                                      DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                                      pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
}

// =============================================================================
//  Log header
// =============================================================================
typedef LONG (WINAPI* TRHF_PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);

static void WriteHeader()
{
    if (!g_logOn)
        return;
    char u8[4096];
    Log("==== TR2013 HUD Fix+ v%s - %s ====", kVersion, TRHF_Marker);
    Log("Build target : %s, compiled %s %s", kBuildTarget, __DATE__, __TIME__);
    Log("Module       : %s", ToUtf8(g_selfPath, u8, sizeof(u8)));
    Log("Mode         : PROXY (d3d11.dll exports)");

    wchar_t exe[1024] = {};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    Log("Process      : %s (PID %lu, WOW64=%d)", ToUtf8(exe, u8, sizeof(u8)), GetCurrentProcessId(), wow64 ? 1 : 0);

    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    TRHF_PFN_RtlGetVersion pRtlGetVersion =
        ntdll ? reinterpret_cast<TRHF_PFN_RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (pRtlGetVersion && pRtlGetVersion(&vi) == 0)
        Log("OS           : Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    else
        Log("OS           : unknown (RtlGetVersion unavailable)");

    Log("Ini          : %s (%s)", ToUtf8(g_iniPath, u8, sizeof(u8)), FileExists(g_iniPath) ? "found" : "NOT found, defaults used");
    Log("  EnableFix=%d (HUD correction)  EnableLogging=%d", g_cfg.enableFix, g_cfg.enableLogging);
    Log("  ProxyDll=\"%s\"", ToUtf8(g_cfg.proxyDll, u8, sizeof(u8)));
    Log("  MaxUniqueEntries=%u  CBDumpMaxBytes=%u  CBDumpSamplesPerSig=%u", g_cfg.maxUniqueEntries,
        g_cfg.cbDumpMaxBytes, g_cfg.cbDumpSamples);
    Log("  RearmDrawHooks=%d (effective: %s)", g_cfg.rearmDrawHooks, g_rearmOn ? "on" : "off");
    Log("  CaptureHotkey=0x%02X%s  CaptureFrames=%u  CaptureSeconds=%u (fallback)  CaptureMaxCBBytes=%u  CaptureMaxLines=%u",
        g_cfg.captureHotkey, g_cfg.captureHotkey ? "" : " (off)", g_cfg.captureFrames, g_cfg.captureSeconds,
        g_cfg.captureMaxCBBytes, g_cfg.captureMaxLines);
    Log("  CaptureMaxVBBytes=%u", g_cfg.captureMaxVBBytes);
    Log("  [Fix] FixPopupOrigin=%d PopupOriginShift=(%.4f,%.4f) PopupOriginTriggerPsHash=%016llX",
        g_cfg.fixPopupOrigin ? 1 : 0, static_cast<double>(g_cfg.popupOriginShiftX),
        static_cast<double>(g_cfg.popupOriginShiftY), g_cfg.popupOriginPsHash);
    Log("  [Fix] FixSalvage=%d SalvageShift=(%.4f,%.4f)", g_cfg.fixSalvage ? 1 : 0,
        static_cast<double>(g_cfg.salvageShiftX), static_cast<double>(g_cfg.salvageShiftY));
    Log("  [Fix] HudLayerTol=%.2f PromptLayerTol=%.2f MenuMinSceneDraws=%u MenuLargeScale=%.6f MenuHoldFrames=%d "
        "(menus are never modified)",
        static_cast<double>(g_cfg.hudLayerTol), static_cast<double>(g_cfg.promptLayerTol), g_cfg.menuMinSceneDraws,
        static_cast<double>(g_cfg.menuLargeScale), g_cfg.menuHoldFrames);
    Log("  [Fix] FixPrompt=%d PromptLayerW=%.1f PromptShift=(%.5f,%.5f) HudLayerW=%.1f", g_cfg.fixPrompt ? 1 : 0,
        static_cast<double>(g_cfg.promptLayerW), static_cast<double>(g_cfg.promptShiftX),
        static_cast<double>(g_cfg.promptShiftY), static_cast<double>(g_cfg.hudLayerW));
    Log("  [Fix] FixBackdrop=%d (0=off 1=move 2=hide) BackdropPsHash=%016llX BackdropTarget=(%.4f,%.4f)",
        g_cfg.fixBackdrop, g_cfg.backdropPsHash, static_cast<double>(g_cfg.backdropX),
        static_cast<double>(g_cfg.backdropY));
    Log("  [Fix] ShiftNdcY=%.4f PopupScale=%.2f Pivot=(%.3f,%.3f)", static_cast<double>(g_cfg.fixShiftNdcY),
        static_cast<double>(g_cfg.popupScale), static_cast<double>(g_cfg.popupPivotX),
        static_cast<double>(g_cfg.popupPivotY));
    Log("  [Menu] MenuHotkey=0x%02X MenuScale=%.2f (0=auto)  fix infrastructure: %s", g_cfg.menuHotkey,
        static_cast<double>(g_cfg.menuScale), g_fixInfra ? "on" : "off");
    Log("  [Perf] FpsLimit=%d ShowFpsOverlay=%d OverlayHotkey=0x%02X", g_cfg.fpsLimit, g_cfg.showFpsOverlay ? 1 : 0,
        g_cfg.overlayHotkey);
    Log("  [Display] Borderless=%d FlipModel=%d AllowTearing=%d (applied at start)", g_cfg.borderless ? 1 : 0,
        g_cfg.flipModel ? 1 : 0, g_cfg.allowTearing ? 1 : 0);
    Log("  [Memory] MemWarning=%d MemWarnFreeMB=%u MemWarnBlockMB=%u", g_cfg.memWarning ? 1 : 0, g_cfg.memWarnFreeMB,
        g_cfg.memWarnBlockMB);
    {
        LineBuf hl;
        for (UINT i = 0; i < g_cfg.hudVsCount; ++i)
            hl.Add("%s%016llX", i ? "," : "", g_cfg.hudVs[i]);
        Log("  [Fix] HudVsHashes=%s ShiftNdcX=%.4f ToggleHotkey=0x%02X", hl.s,
            static_cast<double>(g_cfg.fixShiftNdcX), g_cfg.fixToggleHotkey);
    }
    Log("  EnableLogging=%d (%s) MaxLogMB=%u AutoCaptureMax=%u AutoCaptureMinGapSec=%u", g_cfg.enableLogging,
        g_logVerbose ? "full" : "watchdog/essential only", g_cfg.maxLogMB, g_cfg.autoCaptureMax,
        g_cfg.autoCaptureMinGapSec);
    Log("  WatchSampleMs=%u (OFF watchdog sample frame, 0=off) MemLogSec=%u (0=off) InventoryEnable=%d (0=off)",
        g_cfg.watchSampleMs, g_cfg.memLogSec, g_cfg.inventoryEnable);
    Log("Log          : %s", ToUtf8(g_logPath, u8, sizeof(u8)));
    Log("DPI          : queried at first D3D11 activity (not inside DllMain)");
    Log("Legend       : [VP] viewports [SC] scissors [VSCB] VS constant buffers [VS] vertex shaders "
        "[DRAW] draw signatures [CBDATA] constant-buffer contents (float4 rows)");
    Log("Legend       : hotkey capture -> [CAPTURE] armed/start/end, [CAP-DRAW] EVERY draw (f=frame n=draw index) "
        "with vscb/pscb content hashes, [CAP-CB] referenced CB content (hash + float4 rows, printed once), "
        "[CAP-VP]/[CAP-SC]; [REARM] draw-entry re-arm, [DXGI] factory/swapchain hooks");
    Log("Legend       : [SHADER] vs/ps bytecode hash (stable across runs), [LAYOUT] input layouts (il=id), "
        "[CAP-DRAW] vsh/psh/il/ib=fmt/offset/vb0=stride/offset, [CAP-VTX] decoded POSITION bounds + NDC/pixel box "
        "(row- and column-vector convention), [WINDOW] window/monitor/fullscreen state at capture start");
    Log("Legend       : [WATCH] A = anchor at the stage origin, OFF = HUD/prompt element >= 50 %% off-screen in "
        "2 consecutive sample frames (box in pixels, matrix as drawn); [MEM] private/working set, address space "
        "(vsize = committed+reserved), largest free region, ours = our C++ heap (excl. ImGui), walk/regions = "
        "time (sum of the per-frame slices) and region count of the last address-space walk");
    Log("Legend       : [INV] HUD inventory: each new element once; gameplay draws only (menu gate off), "
        "layer H=HUD P=prompt (within HudLayerTol/PromptLayerTol), max. 3 entries per VS/PS/layer/cnt, "
        "w = layer w (c3.w, 0.1), gate = menu gate state at the draw (always game), bb = last known back buffer "
        "WxH, cnt = index/vertex count, anchor/scale as sent by the game, rule = fix rule applied to that draw "
        "(U popup, u popup carry-over, O popup at origin, S salvage, P prompt, B backdrop), drawn = anchor after "
        "correction; summary every 10 min");
}

// =============================================================================
//  DllMain - kept slim: paths, ini, (logging on) log, crash handler (always)
// =============================================================================
extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_imguiCs);
        g_startTick = GetTickCount64();

        // Pin the module: our vtable hooks must never point into unloaded code.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

        InitPaths();
        ReadIni();
        AcquireInstance();
        g_fixInfra = g_cfg.enableFix != 0 || g_cfg.menuHotkey != 0 || g_cfg.enableLogging != 0; // watchdog needs shadows
        g_rearmOn = g_cfg.rearmDrawHooks != 0 && (g_cfg.enableLogging != 0 || g_fixInfra);
        g_fixRuntime = g_cfg.enableFix ? 1 : 0;

        if (g_active)
        {
            // [MEM] "ours" counter: enabled before our first allocation (InitState).
            g_heapCount = g_cfg.enableLogging != 0 && g_cfg.memLogSec != 0;
            ScratchGuardInit(); // v1.4.6: scratch guard pattern (16 x 64 bytes)
            InitState();
            if (g_cfg.enableLogging)
                OpenLog();
            WriteHeader();
            // v1.4.6: always installed (log-only, never handles anything). Without a
            // log file it writes tr_hud_fix_crash.txt only if a qualifying exception occurs.
            const PVOID veh = AddVectoredExceptionHandler(0, CrashVeh);
            if (g_logOn)
                Log("[DIAG] crash diagnostics %s (log rotated to tr_hud_fix.prev.log)", veh ? "installed" : "FAILED");
        }
        // Inactive instance (another TR_HUD_Fix already loaded): the proxy
        // still forwards all exports, but installs no hooks and writes no log.
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        // Process exit or (impossible, pinned) unload: stop logging, touch
        // nothing else. Containers are intentionally leaked.
        if (g_logOn)
        {
            if (g_cfg.memLogSec)
            {
                char memLine[256];
                FormatMemLine(memLine, sizeof(memLine)); // kernel32 only, no heap
                LogUnsafe("%s", memLine);
            }
            LogUnsafe("[EXIT] process exit (reserved=%p)", lpvReserved); // no lock at exit
        }
        g_shutdown = true;
    }
    return TRUE;
}
