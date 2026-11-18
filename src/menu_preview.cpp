// =============================================================================
//  TR2013 HUD Fix+ - offline menu preview (build\menu_preview.exe)
//
//  Development tool only (never shipped, not part of the release package).
//  Renders the REAL in-game settings overlay (the Dear ImGui menu of
//  dllmain.cpp: same style, font loading, layout and render path) into an
//  offscreen 2560x1440 D3D11 swap chain and writes the frames as 32-bit BMPs,
//  e.g. to build an animated GIF of the menu.
//
//  Build:  build.bat preview      -> build\menu_preview.exe (x86 console, /MT)
//  Run:    build\menu_preview.exe [outdir]     (default: build\preview)
//
//  This file includes dllmain.cpp as a whole with TRHF_MENU_PREVIEW defined, so
//  every static of the overlay is reachable. The only TRHF_MENU_PREVIEW blocks in
//  dllmain.cpp are: a virtual clock in QpcNow() (fake frame times) and a forced
//  tab selection (g_previewTab). Without the define, dllmain.cpp is unchanged.
//
//  No hooks are installed, no log is written, DllMain never runs (EXE). The
//  init the menu needs (critical sections, paths, ini) is done in wmain.
// =============================================================================
#ifndef TRHF_MENU_PREVIEW
#define TRHF_MENU_PREVIEW
#endif

#include "dllmain.cpp"
#include "imgui_internal.h" // ImGui::FindWindowByName (FPS overlay rectangle)

static const UINT kPvW          = 2560; // same as the user's game (uiScale 1.33)
static const UINT kPvH          = 1440;
static const UINT kPvWarmup     = 10;   // rendered frames before each still
static const UINT kPvPerfFrames = 24;   // extra PERFORMANCE captures
static const UINT kPvPerfStep   = 3;    // rendered frames per extra capture
static const UINT kPvSimPerDraw = 4;    // simulated game frames per rendered frame (~38 ms)

static const char* const kPvTabFile[TAB_COUNT] = { "fix", "performance", "display", "hotkeys", "info" };

struct PvState
{
    HWND                    hwnd;
    IDXGISwapChain*         sc;
    ID3D11Device*           dev;
    ID3D11DeviceContext*    ctx;
    ID3D11RenderTargetView* rtv;     // our own view (clear only)
    ID3D11Texture2D*        staging; // CPU read-back, created once
    std::vector<unsigned char> pixels; // BMP pixel data (bottom-up BGRA), allocated once
    wchar_t                 outDir[1024];
    UINT                    captures;
    UINT                    failures;
    bool                    haveUnion;
    float                   ux0, uy0, ux1, uy1; // union of all menu rectangles
};

// --- Window ---------------------------------------------------------------------------
static LRESULT CALLBACK PvWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_GETMINMAXINFO)
    {
        // Allow a 2560x1440 client area even on smaller monitors (window stays hidden).
        MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMaxSize.x = mm->ptMaxTrackSize.x = 16384;
        mm->ptMaxSize.y = mm->ptMaxTrackSize.y = 16384;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void PvPump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Same DPI state as the game process (per-monitor aware, see test run 1).
static void PvSetDpiAware()
{
    typedef BOOL (WINAPI* PFN_SetCtx)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_SetCtx set = u32 ? reinterpret_cast<PFN_SetCtx>(GetProcAddress(u32, "SetProcessDpiAwarenessContext")) : nullptr;
    if (set && !set(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) // PER_MONITOR_AWARE_V2
        set(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-3)));         // PER_MONITOR_AWARE
}

// --- Paths -----------------------------------------------------------------------------
// tr_hud_fix.ini of the repository root (exe is build\menu_preview.exe), else the
// current directory, else next to the exe (missing file = built-in defaults).
static void PvSelectIni()
{
    wchar_t root[1024] = {};
    wcscpy_s(root, g_selfDir); // "...\build\"
    size_t n = wcslen(root);
    if (n > 0 && root[n - 1] == L'\\')
        root[--n] = 0;
    wchar_t* slash = wcsrchr(root, L'\\');
    if (slash)
        slash[1] = 0;
    else
        root[0] = 0;
    wchar_t cand[1024] = {};
    wcscpy_s(cand, root);
    wcscat_s(cand, L"tr_hud_fix.ini");
    if (root[0] && FileExists(cand))
    {
        wcscpy_s(g_iniPath, cand);
        return;
    }
    const DWORD c = GetCurrentDirectoryW(ARRAYSIZE(cand), cand);
    if (c > 0 && c + 16 < ARRAYSIZE(cand))
    {
        wcscat_s(cand, L"\\tr_hud_fix.ini");
        if (FileExists(cand))
            wcscpy_s(g_iniPath, cand);
    }
}

static bool PvMakeDirs(const wchar_t* dir)
{
    wchar_t p[1024] = {};
    wcscpy_s(p, dir);
    const size_t n = wcslen(p);
    for (size_t i = 1; i < n; ++i)
    {
        if ((p[i] == L'\\' || p[i] == L'/') && p[i - 1] != L':' && p[i - 1] != L'\\' && p[i - 1] != L'/')
        {
            const wchar_t keep = p[i];
            p[i] = 0;
            CreateDirectoryW(p, nullptr); // existing / root parts fail harmlessly
            p[i] = keep;
        }
    }
    CreateDirectoryW(p, nullptr);
    const DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// --- Fake session data ------------------------------------------------------------------
// ~104 fps (9.0..10.1 ms) with a slow drift, noise and a small hitch every 97 frames.
static uint32_t g_pvRng = 0x20132013u;
static UINT     g_pvSimFrame = 0;

static float PvFrameMs()
{
    const UINT i = g_pvSimFrame++;
    g_pvRng = g_pvRng * 1664525u + 1013904223u;
    const float noise = static_cast<float>((g_pvRng >> 8) & 0xFFFFu) / 65535.0f - 0.5f;
    const float fi = static_cast<float>(i);
    float ms = 9.55f + 0.30f * sinf(fi * 0.021f) + 0.15f * sinf(fi * 0.13f) + 0.45f * noise;
    if (i % 97u == 41u)
        ms += 2.0f;
    return ms;
}

// Advances the virtual clock and feeds the real frame-time statistics
// (PerfOnPresent: ring, 0.5 s recompute, plot copy, 1 % low) - same code as in game.
static void PvSimulateGameFrames(UINT n)
{
    const double freq = static_cast<double>(QpcFreq());
    for (UINT k = 0; k < n; ++k)
    {
        g_previewQpc += static_cast<LONGLONG>(static_cast<double>(PvFrameMs()) * freq / 1000.0);
        PerfOnPresent();
    }
}

// Values the game would produce: address space of a long session, fix rates.
static void PvFakeSession()
{
    g_memStat.valid     = true;
    g_memStat.totalMB   = 4095;            // 32-bit LAA process on 64-bit Windows: "4.00 GB"
    g_memStat.availMB   = 1996;            // 2099 MB used = "2.05 GB"
    g_memStat.largestMB = 1900;
    g_memStat.tick      = GetTickCount64();
    // INFO rates: OverlayBuildMenu re-samples only after 1000 ms since rateLastTick.
    g_ov.rateLastTick = GetTickCount();
    g_ov.rate   = 1248.0f;
    g_ov.bdRate = 104.0f;
    g_ov.prRate = 208.0f;
    g_ov.poRate = 104.0f;
    g_ov.svRate = 312.0f;
}

// --- Rendering / capture ------------------------------------------------------------------
// One frame through the game's code path: OverlayBeforePresent draws on top of
// the (cleared) back buffer, exactly like in the Present hook.
static void PvRender(PvState& s)
{
    PvPump();
    PvSimulateGameFrames(kPvSimPerDraw);
    PvFakeSession();
    const float clear[4] = { 30.0f / 255.0f, 32.0f / 255.0f, 28.0f / 255.0f, 1.0f }; // #1E201C
    s.ctx->ClearRenderTargetView(s.rtv, clear);
    if (g_ov.ok)
    {
        // No mouse (no hover, no software cursor): -FLT_MAX = "mouse unavailable".
        EnterCriticalSection(&g_imguiCs);
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        LeaveCriticalSection(&g_imguiCs);
    }
    OverlayBeforePresent(s.sc);
}

static void PvPresent(PvState& s)
{
    s.sc->Present(0, 0); // hidden window: S_OK or DXGI_STATUS_OCCLUDED, both fine
}

static bool PvWriteBmp(const wchar_t* path, const unsigned char* data, UINT w, UINT h)
{
    const DWORD imgBytes = w * h * 4u;
    BITMAPFILEHEADER fh = {};
    fh.bfType    = 0x4D42; // "BM"
    fh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    fh.bfSize    = fh.bfOffBits + imgBytes;
    BITMAPINFOHEADER ih = {};
    ih.biSize        = sizeof(BITMAPINFOHEADER);
    ih.biWidth       = static_cast<LONG>(w);
    ih.biHeight      = static_cast<LONG>(h); // bottom-up (most compatible)
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = imgBytes;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD wr1 = 0, wr2 = 0, wr3 = 0;
    const bool ok = WriteFile(f, &fh, sizeof(fh), &wr1, nullptr) && wr1 == sizeof(fh) &&
                    WriteFile(f, &ih, sizeof(ih), &wr2, nullptr) && wr2 == sizeof(ih) &&
                    WriteFile(f, data, imgBytes, &wr3, nullptr) && wr3 == imgBytes;
    CloseHandle(f);
    return ok;
}

// Back buffer -> staging -> BMP (after rendering, before Present).
static bool PvCapture(PvState& s, const char* name)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(s.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
        return false;
    s.ctx->CopyResource(s.staging, bb);
    bb->Release();
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(s.ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m)) || !m.pData)
        return false;
    const unsigned char* src = static_cast<const unsigned char*>(m.pData);
    for (UINT y = 0; y < kPvH; ++y)
    {
        const unsigned char* sr = src + static_cast<size_t>(y) * m.RowPitch;
        unsigned char* dr = s.pixels.data() + static_cast<size_t>(kPvH - 1u - y) * kPvW * 4u;
        for (UINT x = 0; x < kPvW; ++x, sr += 4, dr += 4)
        {
            dr[0] = sr[2]; // RGBA -> BGRA
            dr[1] = sr[1];
            dr[2] = sr[0];
            dr[3] = 255;   // opaque (blended alpha is meaningless here)
        }
    }
    s.ctx->Unmap(s.staging, 0);
    wchar_t path[1200] = {};
    swprintf_s(path, ARRAYSIZE(path), L"%s\\%hs.bmp", s.outDir, name);
    return PvWriteBmp(path, s.pixels.data(), kPvW, kPvH);
}

static void PvPrintRect(const char* file, const char* what, float px, float py, float sx, float sy)
{
    const int x0 = static_cast<int>(floorf(px));
    const int y0 = static_cast<int>(floorf(py));
    const int x1 = static_cast<int>(ceilf(px + sx));
    const int y1 = static_cast<int>(ceilf(py + sy));
    printf("  %-24s %-5s x=%d y=%d w=%d h=%d\n", file, what, x0, y0, x1 - x0, y1 - y0);
}

// Captures the current frame and prints the menu window rectangle (recorded
// by OverlayBuildMenu inside Begin/End of this very frame).
static void PvCaptureMenu(PvState& s, const char* name)
{
    if (!PvCapture(s, name))
    {
        printf("  %-24s FAILED\n", name);
        ++s.failures;
        return;
    }
    ++s.captures;
    const ImVec2 p = g_ov.winPos;
    const ImVec2 z = g_ov.winSize;
    PvPrintRect(name, "menu", p.x, p.y, z.x, z.y);
    if (!s.haveUnion)
    {
        s.ux0 = p.x;
        s.uy0 = p.y;
        s.ux1 = p.x + z.x;
        s.uy1 = p.y + z.y;
        s.haveUnion = true;
    }
    else
    {
        s.ux0 = Min(s.ux0, p.x);
        s.uy0 = Min(s.uy0, p.y);
        s.ux1 = Max(s.ux1, p.x + z.x);
        s.uy1 = Max(s.uy1, p.y + z.y);
    }
}

// --- Setup / teardown ----------------------------------------------------------------------
static bool PvCreateDevice(PvState& s, bool* warp)
{
    wchar_t sys[MAX_PATH + 32] = {};
    UINT n = GetSystemWow64DirectoryW(sys, MAX_PATH); // same order as the proxy (SysWOW64, else System32)
    if (n == 0 || n >= MAX_PATH)
        n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    wcscat_s(sys, L"\\d3d11.dll");
    HMODULE d3d = LoadLibraryW(sys); // full path: never the proxy in build\ (kept loaded until exit)
    if (!d3d)
    {
        printf("[ERROR] LoadLibrary failed (%lu): %ls\n", GetLastError(), sys);
        return false;
    }
    const PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN create =
        reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain"));
    if (!create)
    {
        printf("[ERROR] D3D11CreateDeviceAndSwapChain not exported by %ls\n", sys);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width                   = kPvW;
    sd.BufferDesc.Height                  = kPvH;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM; // game back buffer: fmt=28
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count                   = 1;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount                        = 1;
    sd.OutputWindow                       = s.hwnd;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD; // legacy blt model like the game default

    const D3D_DRIVER_TYPE types[2] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    for (int i = 0; i < 2; ++i)
    {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const HRESULT hr = create(nullptr, types[i], nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &s.sc, &s.dev,
                                  &fl, &s.ctx);
        if (SUCCEEDED(hr) && s.sc && s.dev && s.ctx)
        {
            *warp = (i == 1);
            return true;
        }
        if (s.sc)
            s.sc->Release();
        if (s.ctx)
            s.ctx->Release();
        if (s.dev)
            s.dev->Release();
        s.sc = nullptr;
        s.ctx = nullptr;
        s.dev = nullptr;
        printf("[WARN] D3D11CreateDeviceAndSwapChain(%s) failed hr=0x%08lX\n", i ? "WARP" : "HARDWARE",
               static_cast<unsigned long>(hr));
    }
    return false;
}

static bool PvCreateTargets(PvState& s)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(s.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
        return false;
    D3D11_TEXTURE2D_DESC td = {};
    bb->GetDesc(&td);
    HRESULT hr = s.dev->CreateRenderTargetView(bb, nullptr, &s.rtv);
    bb->Release();
    if (FAILED(hr) || !s.rtv || td.Width != kPvW || td.Height != kPvH)
        return false;
    td.Usage          = D3D11_USAGE_STAGING;
    td.BindFlags      = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags      = 0;
    hr = s.dev->CreateTexture2D(&td, nullptr, &s.staging);
    if (FAILED(hr) || !s.staging)
        return false;
    s.pixels.resize(static_cast<size_t>(kPvW) * kPvH * 4u);
    return true;
}

static void PvShutdown(PvState& s, HINSTANCE inst, const wchar_t* cls)
{
    if (g_menuOpen)
        OverlaySetOpen(false, s.sc);
    EnterCriticalSection(&g_imguiCs);
    if (g_ov.ok)
    {
        g_ov.ok = false; // OverlayWndProc: no more ImGui calls
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    LeaveCriticalSection(&g_imguiCs);
    if (s.hwnd && g_ov.origProc)
        SetWindowLongPtrW(s.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_ov.origProc)); // undo the subclass
    OverlayReleaseRTV();
    if (g_ov.ctx)
    {
        g_ov.ctx->Release(); // OverlayInit keeps these for the process lifetime in game
        g_ov.ctx = nullptr;
    }
    if (g_ov.dev)
    {
        g_ov.dev->Release();
        g_ov.dev = nullptr;
    }
    if (s.staging)
        s.staging->Release();
    if (s.rtv)
        s.rtv->Release();
    if (s.ctx)
    {
        s.ctx->ClearState();
        s.ctx->Flush();
    }
    if (s.sc)
        s.sc->Release();
    if (s.ctx)
        s.ctx->Release();
    if (s.dev)
        s.dev->Release();
    s.staging = nullptr;
    s.rtv = nullptr;
    s.sc = nullptr;
    s.ctx = nullptr;
    s.dev = nullptr;
    if (s.hwnd)
        DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    UnregisterClassW(cls, inst);
}

// --- Entry point ----------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv)
{
    PvSetDpiAware();

    // Replaces the DllMain init the menu depends on (no hooks, no VEH, no log).
    InitializeCriticalSection(&g_logCs);
    InitializeCriticalSection(&g_imguiCs);
    g_startTick = GetTickCount64();
    InitPaths(); // g_hSelf == nullptr -> paths of the exe (build\)
    PvSelectIni();
    ReadIni();
    g_cfg.enableLogging  = 0;     // end-user default: logging off (also hides the capture hotkey row)
    g_logVerbose         = false;
    g_cfg.showFpsOverlay = false; // menu captures: menu only
    g_cfg.perfTestHotkey = 0;     // no diagnostic lines in the FPS overlay
    g_cfg.benchHotkey    = 0;
    g_fixInfra   = g_cfg.enableFix != 0 || g_cfg.menuHotkey != 0;
    g_rearmOn    = g_cfg.rearmDrawHooks != 0 && g_fixInfra;
    g_fixRuntime = g_cfg.enableFix ? 1 : 0;
    ScratchGuardInit();

    PvState s = {};
    if (argc > 1 && argv[1] && argv[1][0])
    {
        wcsncpy_s(s.outDir, argv[1], _TRUNCATE);
    }
    else
    {
        wcscpy_s(s.outDir, g_selfDir);
        wcscat_s(s.outDir, L"preview");
    }
    for (size_t n = wcslen(s.outDir); n > 3 && (s.outDir[n - 1] == L'\\' || s.outDir[n - 1] == L'/'); --n)
        s.outDir[n - 1] = 0;
    if (!PvMakeDirs(s.outDir))
    {
        printf("[ERROR] cannot create output directory: %ls\n", s.outDir);
        return 1;
    }

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* const cls = L"TRHF_MenuPreview";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PvWndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = cls;
    if (!RegisterClassExW(&wc))
    {
        printf("[ERROR] RegisterClassEx failed (%lu)\n", GetLastError());
        return 1;
    }
    // WS_POPUP: client area == window size. Never shown (offscreen rendering).
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"TR2013 HUD Fix+ menu preview", WS_POPUP, 0, 0,
                             static_cast<int>(kPvW), static_cast<int>(kPvH), nullptr, nullptr, inst, nullptr);
    if (!s.hwnd)
    {
        printf("[ERROR] CreateWindowEx failed (%lu)\n", GetLastError());
        UnregisterClassW(cls, inst);
        return 1;
    }
    RECT rc = {};
    GetClientRect(s.hwnd, &rc);
    if (rc.right - rc.left != static_cast<LONG>(kPvW) || rc.bottom - rc.top != static_cast<LONG>(kPvH))
    {
        SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(kPvW), static_cast<int>(kPvH),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetClientRect(s.hwnd, &rc);
        if (rc.right - rc.left != static_cast<LONG>(kPvW) || rc.bottom - rc.top != static_cast<LONG>(kPvH))
            printf("[WARN] client area is %ldx%ld instead of %ux%u - layout may differ from in-game\n",
                   rc.right - rc.left, rc.bottom - rc.top, kPvW, kPvH);
    }

    bool warp = false;
    if (!PvCreateDevice(s, &warp) || !PvCreateTargets(s))
    {
        printf("[ERROR] D3D11 device / swap chain / staging setup failed\n");
        PvShutdown(s, inst, cls);
        return 1;
    }

    // Prefill the frame-time ring (240 frames) on the virtual clock.
    g_previewQpc = QpcNow(); // real QPC as the start value (g_previewQpc was 0)
    g_perfStatsWanted = true;
    PvSimulateGameFrames(kFtRing + 8u);

    // Open the menu exactly like the menu hotkey does (lazy OverlayInit inside).
    OverlaySetOpen(true, s.sc);
    if (!g_ov.ok)
    {
        printf("[ERROR] OverlayInit failed (ImGui Win32/DX11 backend)\n");
        PvShutdown(s, inst, cls);
        return 1;
    }

    char iniU8[1100], outU8[1100];
    printf("TR2013 HUD Fix+ v%s menu preview - %ux%u %s, uiScale %.2f, ImGui %s\n", kVersion, kPvW, kPvH,
           warp ? "WARP" : "hardware", static_cast<double>(g_ov.uiScale), IMGUI_VERSION);
    printf("  ini: %s%s\n", ToUtf8(g_iniPath, iniU8, sizeof(iniU8)), FileExists(g_iniPath) ? "" : " (missing, defaults)");
    printf("  out: %s\n", ToUtf8(s.outDir, outU8, sizeof(outU8)));

    // a/b: one still per tab (after warm-up), PERFORMANCE additionally 24 frames.
    for (int t = 0; t < TAB_COUNT; ++t)
    {
        g_previewTab = t;
        for (UINT i = 0; i < kPvWarmup; ++i)
        {
            PvRender(s);
            PvPresent(s);
        }
        char name[64];
        PvRender(s);
        _snprintf_s(name, sizeof(name), _TRUNCATE, "%02d_%s_00", t + 1, kPvTabFile[t]);
        PvCaptureMenu(s, name);
        PvPresent(s);
        if (t != TAB_PERF)
            continue;
        for (UINT k = 1; k <= kPvPerfFrames; ++k)
        {
            for (UINT j = 0; j < kPvPerfStep; ++j)
            {
                PvRender(s);
                if (j + 1u == kPvPerfStep)
                {
                    _snprintf_s(name, sizeof(name), _TRUNCATE, "%02d_%s_%02u", t + 1, kPvTabFile[t], k);
                    PvCaptureMenu(s, name);
                }
                PvPresent(s);
            }
        }
    }
    g_previewTab = -1;

    // c: menu closed, FPS overlay only.
    OverlaySetOpen(false, s.sc);
    g_cfg.showFpsOverlay = true;
    for (UINT i = 0; i <= kPvWarmup; ++i)
    {
        PvRender(s);
        if (i == kPvWarmup)
        {
            if (PvCapture(s, "90_fpsoverlay"))
            {
                ++s.captures;
                EnterCriticalSection(&g_imguiCs);
                const ImGuiWindow* w = ImGui::FindWindowByName("##trhf_fps");
                if (w)
                    PvPrintRect("90_fpsoverlay", "fps", w->Pos.x, w->Pos.y, w->Size.x, w->Size.y);
                else
                    printf("  %-24s fps   (window not found)\n", "90_fpsoverlay");
                LeaveCriticalSection(&g_imguiCs);
            }
            else
            {
                printf("  %-24s FAILED\n", "90_fpsoverlay");
                ++s.failures;
            }
        }
        PvPresent(s);
    }
    g_cfg.showFpsOverlay = false;

    if (s.haveUnion)
        PvPrintRect("menu union (crop)", "menu", s.ux0, s.uy0, s.ux1 - s.ux0, s.uy1 - s.uy0);
    printf("%u frame(s) written, %u failure(s)\n", s.captures, s.failures);

    PvShutdown(s, inst, cls);
    DeleteCriticalSection(&g_imguiCs);
    DeleteCriticalSection(&g_logCs);
    return s.failures ? 1 : 0;
}
