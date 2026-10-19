// Frame limiter DLL: hooks IDXGISwapChain::Present / Present1 (covers DX11 and DX12) and
// waits for an exact deadline before each present. Config: glide.ini next to this DLL.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>

static HMODULE g_mod;
static LONGLONG g_freq, g_next, g_interval, g_lastCfg;
static bool g_calReq; // set from the ini by LoadConfig, consumed in Limit
static HANDLE g_timer;
static wchar_t g_ini[MAX_PATH], g_exe[MAX_PATH];

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PresentFn g_present;
static Present1Fn g_present1;

static LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static void LoadConfig(LONGLONG now) {
    g_lastCfg = now;
    // per-game profile: section named after the exe, falling back to [glide] fps
    UINT fps = GetPrivateProfileIntW(g_exe, L"fps", GetPrivateProfileIntW(L"glide", L"fps", 0, g_ini), g_ini);
    LONGLONG iv = fps ? g_freq / fps : 0;
    if (iv != g_interval) { g_interval = iv; g_next = 0; }
    g_calReq = GetPrivateProfileIntW(g_exe, L"calibrate", 0, g_ini) != 0;
}

// Sleep on a high-resolution timer until ~1.5 ms before the deadline, then spin.
static void WaitUntil(LONGLONG deadline) {
    LONGLONG margin = g_freq * 3 / 2000;
    LONGLONG left = deadline - Now();
    if (left > margin && g_timer) {
        LARGE_INTEGER due; due.QuadPart = -((left - margin) * 10000000LL / g_freq);
        SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(g_timer, 100);
    }
    while (Now() < deadline) YieldProcessor();
}

static thread_local bool t_in;

// Ring of recent frame times (ms) shared with the UI; layout must match ui.cpp.
struct Stats { volatile LONG head; float ms[240]; volatile LONG calState, suggest, capable, calLeft; }; // calState: 0 idle, 1 measuring, 2 done, 3 failed
static Stats* g_stats;
static LONGLONG g_lastPresent;

// Calibration: run uncapped for ~10 s (first second skipped), then suggest a cap the game holds without fluctuating:
// 95th-percentile frametime plus 10% headroom. Triggered by "calibrate=1" in the game's ini section.
static const int kCalMax = 16384;
static bool g_calib;
static LONGLONG g_calBeg, g_calEnd;
static int g_calN;
static float g_cal[kCalMax];

static void Calib(LONGLONG now, float ms) {
    if (now >= g_calBeg && g_calN < kCalMax) g_cal[g_calN++] = ms;
    if (g_stats) g_stats->calLeft = now < g_calEnd ? (LONG)((g_calEnd - now) / g_freq + 1) : 0;
    if (now < g_calEnd) return;
    g_calib = g_calReq = false;
    WritePrivateProfileStringW(g_exe, L"calibrate", L"0", g_ini);
    if (!g_stats) return;
    if (g_calN < 60) { g_stats->calState = 3; return; }
    std::sort(g_cal, g_cal + g_calN);
    float p95 = g_cal[(int)(g_calN * 0.95f)];
    g_stats->capable = (LONG)(1000 / p95);
    g_stats->suggest = (LONG)(1000 / (p95 * 1.1f));
    g_stats->calState = 2;
}

static void Limit() {
    LONGLONG now = Now();
    if (now - g_lastCfg > g_freq) LoadConfig(now);
    if (g_calReq && !g_calib) {
        g_calib = true; g_calN = 0; g_calBeg = now + g_freq; g_calEnd = now + 11 * g_freq;
        if (g_stats) g_stats->calState = 1;
    }
    if (g_interval && !g_calib) {
        if (!g_next) g_next = now;
        WaitUntil(g_next);
        g_next += g_interval;
        now = Now();
        if (now > g_next) g_next = now; // fell behind: resync instead of bursting to catch up
    }
    if (g_lastPresent) {
        float ms = (now - g_lastPresent) * 1000.0f / g_freq;
        if (g_stats) { LONG h = g_stats->head; g_stats->ms[h % 240] = ms; g_stats->head = h + 1; }
        if (g_calib) Calib(now, ms);
    }
    g_lastPresent = now;
}

static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT si, UINT fl) {
    if (t_in) return g_present(sc, si, fl);
    t_in = true; Limit();
    HRESULT hr = g_present(sc, si, fl);
    t_in = false; return hr;
}

static HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_in) return g_present1(sc, si, fl, p);
    t_in = true; Limit();
    HRESULT hr = g_present1(sc, si, fl, p);
    t_in = false; return hr;
}

static void Patch(void** slot, void* hook, void** orig) {
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *orig = *slot; *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

static DWORD WINAPI Init(LPVOID) {
    QueryPerformanceFrequency((LARGE_INTEGER*)&g_freq);
    GetModuleFileNameW(g_mod, g_ini, MAX_PATH);
    wcscpy_s(wcsrchr(g_ini, L'\\') + 1, 16, L"glide.ini");
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(g_exe, wcsrchr(path, L'\\') + 1);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LoadConfig(Now());
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Stats), L"Local\\glide_stats");
    if (m) g_stats = (Stats*)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, sizeof(Stats));

    // Dummy swapchain only to read the vtable; all swapchains share dxgi.dll's vtable.
    WNDCLASSW wc = { 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), 0, 0, 0, 0, L"glide_dummy" };
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"glide_dummy", L"", WS_OVERLAPPED, 0, 0, 8, 8, 0, 0, wc.hInstance, 0);
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 1; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = w;
    d.SampleDesc.Count = 1; d.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, nullptr))) {
        void** vt = *(void***)sc;
        Patch(&vt[8], HkPresent, (void**)&g_present);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) { // vtable index 22 = Present1
            Patch(&vt[22], HkPresent1, (void**)&g_present1);
            sc1->Release();
        }
        sc->Release(); dev->Release();
    }
    DestroyWindow(w); UnregisterClassW(L"glide_dummy", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_mod = m; DisableThreadLibraryCalls(m);
        CloseHandle(CreateThread(nullptr, 0, Init, nullptr, 0, nullptr));
    }
    return TRUE;
}
// shortcut: no unload (DLL stays pinned, hooks never removed), upgrade if you need hot-eject.
// shortcut: one global deadline, so a process with several swapchains is limited as one stream.
