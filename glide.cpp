// Frame limiter DLL: hooks IDXGISwapChain::Present / Present1 (DX11, DX12), SwapBuffers (OpenGL) and vkQueuePresentKHR (Vulkan) and
// waits for an exact deadline before each present. Config: glide.ini next to this DLL.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <set>
#include <tlhelp32.h>

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

// Sleep on a high-resolution timer until `g_margin` before the deadline, then spin. The margin follows how late the timer really wakes
// (99th percentile of the last 256 waits + 10%, between 0.5 and 2.5 ms): a fixed 1.5 ms spin costs ~25% of a core at 177 FPS, most of it wasted.
static LONGLONG g_margin, g_over[256]; static int g_overN;
static void WaitUntil(LONGLONG deadline) {
    if (!g_margin) g_margin = g_freq * 3 / 2000;
    LONGLONG left = deadline - Now();
    if (left > g_margin && g_timer) {
        LONGLONG wake = deadline - g_margin;
        LARGE_INTEGER due; due.QuadPart = -((left - g_margin) * 10000000LL / g_freq);
        SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(g_timer, 100);
        LONGLONG over = Now() - wake; g_over[g_overN++] = over < 0 ? 0 : over;
        if (g_overN == 256) {
            std::sort(g_over, g_over + 256);
            LONGLONG want = g_over[253] * 11 / 10 + g_freq / 10000, lo = g_freq / 2000, hi = g_freq * 5 / 2000;
            g_margin = want < lo ? lo : want > hi ? hi : want; g_overN = 0;
        }
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

// One API owns the limiter at a time: DXVK-style stacks present through DXGI and Vulkan for the same frame and must not be limited twice.
static int g_owner; static LONGLONG g_ownerSeen;
static bool Claim(int api, LONGLONG now) {
    if (g_owner == api) { g_ownerSeen = now; return true; }
    if (g_owner && now - g_ownerSeen < g_freq) return false;
    g_owner = api; g_ownerSeen = now; g_next = g_lastPresent = 0; return true;
}

static void Limit(int api) {
    LONGLONG now = Now();
    if (!Claim(api, now)) return;
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
    t_in = true; Limit(1);
    HRESULT hr = g_present(sc, si, fl);
    t_in = false; return hr;
}

static HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_in) return g_present1(sc, si, fl, p);
    t_in = true; Limit(1);
    HRESULT hr = g_present1(sc, si, fl, p);
    t_in = false; return hr;
}

static void Patch(void** slot, void* hook, void** orig) {
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *orig = *slot; *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

// ---- OpenGL: SwapBuffers / wglSwapBuffers, hooked by rewriting the import tables of loaded modules (GLFW/LWJGL call SwapBuffers from gdi32) ----
typedef BOOL(WINAPI* SwapFn)(HDC);
static SwapFn g_swapGdi, g_swapWgl;
static BOOL WINAPI HkSwapGdi(HDC dc) {
    if (t_in) return g_swapGdi(dc);
    t_in = true; Limit(2); BOOL r = g_swapGdi(dc); t_in = false; return r;
}
static BOOL WINAPI HkSwapWgl(HDC dc) {
    if (t_in) return g_swapWgl(dc);
    t_in = true; Limit(2); BOOL r = g_swapWgl(dc); t_in = false; return r;
}

static void PatchIat(HMODULE m) {
    BYTE* base = (BYTE*)m;
    __try {
        auto dos = (IMAGE_DOS_HEADER*)base; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew); if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return;
        for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++)
            for (auto t = (IMAGE_THUNK_DATA*)(base + d->FirstThunk); t->u1.Function; t++) {
                void** slot = (void**)&t->u1.Function; void* dummy;
                if (g_swapGdi && *slot == (void*)g_swapGdi) Patch(slot, HkSwapGdi, &dummy);
                else if (g_swapWgl && *slot == (void*)g_swapWgl) Patch(slot, HkSwapWgl, &dummy);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ScanGl() {
    if (!g_swapGdi) g_swapGdi = (SwapFn)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "SwapBuffers");
    if (!g_swapWgl) g_swapWgl = (SwapFn)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglSwapBuffers");
    static std::set<HMODULE> done;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (s == INVALID_HANDLE_VALUE) return; // modules are loading right now: try again next tick
    MODULEENTRY32W e = { sizeof(e) };
    for (BOOL ok = Module32FirstW(s, &e); ok; ok = Module32NextW(s, &e))
        if (e.hModule != g_mod && done.insert(e.hModule).second) PatchIat(e.hModule);
    CloseHandle(s);
}

// ---- Vulkan: vkQueuePresentKHR. The app has usually cached the driver's function pointer already, so the function body itself is
// patched (5-byte jump written atomically into one aligned 8-byte block, to a stub near the target). To call the original we put the
// bytes back for the duration of the call. Pointers hooked: the loader's export plus whatever vkGetDeviceProcAddr returns for a dummy device. ----
typedef int(__stdcall* PresentVk)(void*, const void*);
struct VkHook { volatile LONG64* blk; int off; void* fn; BYTE orig[5], jmp[5]; };
static VkHook g_vk[4]; static int g_nvk;

static void SetBytes(VkHook& h, const BYTE* src) {
    for (;;) {
        LONG64 cur = *h.blk, nw = cur; memcpy((BYTE*)&nw + h.off, src, 5);
        if (InterlockedCompareExchange64(h.blk, nw, cur) == cur) break;
    }
}
static int VkCall(int i, void* q, const void* info) {
    VkHook& h = g_vk[i]; bool top = !t_in;
    if (top) { t_in = true; Limit(3); }
    SetBytes(h, h.orig); int r = ((PresentVk)h.fn)(q, info); SetBytes(h, h.jmp);
    if (top) t_in = false;
    return r;
}
template <int I> static int __stdcall VkThunk(void* q, const void* info) { return VkCall(I, q, info); }
static void* const kVkThunk[4] = { (void*)VkThunk<0>, (void*)VkThunk<1>, (void*)VkThunk<2>, (void*)VkThunk<3> };

static void InstallVk(void* fn) {
    if (!fn || g_nvk >= 4) return;
    for (int i = 0; i < g_nvk; i++) if (g_vk[i].fn == fn) return;
    uintptr_t a = (uintptr_t)fn, b = a & ~(uintptr_t)7; int off = (int)(a - b);
    if (off + 5 > 8) return; // the 5 bytes must sit inside one aligned qword to be swapped atomically
    BYTE* stub = nullptr;
    for (uintptr_t d = 0x10000; d < 0x70000000 && !stub; d += 0x10000) {
        stub = (BYTE*)VirtualAlloc((void*)((a & ~(uintptr_t)0xFFFF) - d), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!stub) stub = (BYTE*)VirtualAlloc((void*)((a & ~(uintptr_t)0xFFFF) + d), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!stub) return;
    void* hook = kVkThunk[g_nvk];
    BYTE j[14] = { 0xFF, 0x25, 0, 0, 0, 0 }; memcpy(j + 6, &hook, 8); memcpy(stub, j, 14); // jmp qword ptr [rip+0]; .quad hook
    LONG64 rel = (LONG64)stub - (LONG64)(a + 5);
    if (rel != (LONG32)rel) return;
    DWORD old; if (!VirtualProtect((void*)b, 8, PAGE_EXECUTE_READWRITE, &old)) return; // blocked by dynamic-code mitigation: skip
    VkHook& h = g_vk[g_nvk]; h.blk = (volatile LONG64*)b; h.off = off; h.fn = fn;
    memcpy(h.orig, fn, 5); h.jmp[0] = 0xE9; *(LONG32*)(h.jmp + 1) = (LONG32)rel;
    g_nvk++; SetBytes(h, h.jmp);
}

// Minimal Vulkan declarations so the build does not need the SDK.
struct VkAppInfo { int sType; const void* pNext; const char* app; unsigned appVer; const char* eng; unsigned engVer, api; };
struct VkInstInfo { int sType; const void* pNext; unsigned flags; const VkAppInfo* app; unsigned nLayers; const char* const* layers; unsigned nExt; const char* const* ext; };
struct VkQueueInfo { int sType; const void* pNext; unsigned flags, family, count; const float* prio; };
struct VkDevInfo { int sType; const void* pNext; unsigned flags, nQueue; const VkQueueInfo* queues; unsigned nLayers; const char* const* layers; unsigned nExt; const char* const* ext; const void* features; };

static void HookVulkan() {
    HMODULE m = GetModuleHandleW(L"vulkan-1.dll");
    auto fn = [&](const char* n) { return (void*)GetProcAddress(m, n); };
    InstallVk(fn("vkQueuePresentKHR"));
    auto create = (int(__stdcall*)(const VkInstInfo*, const void*, void**))fn("vkCreateInstance");
    auto enumDev = (int(__stdcall*)(void*, unsigned*, void**))fn("vkEnumeratePhysicalDevices");
    auto mkDev = (int(__stdcall*)(void*, const VkDevInfo*, const void*, void**))fn("vkCreateDevice");
    auto getProc = (void*(__stdcall*)(void*, const char*))fn("vkGetDeviceProcAddr");
    auto delDev = (void(__stdcall*)(void*, const void*))fn("vkDestroyDevice");
    auto delInst = (void(__stdcall*)(void*, const void*))fn("vkDestroyInstance");
    if (!create || !enumDev || !mkDev || !getProc || !delDev || !delInst) return;
    VkAppInfo ai = { 0, nullptr, "glide", 1, nullptr, 0, 1 << 22 }; // Vulkan 1.0
    VkInstInfo ii = { 1, nullptr, 0, &ai };
    void* inst = nullptr; if (create(&ii, nullptr, &inst) != 0 || !inst) return;
    void* devs[8]; unsigned n = 8;
    if (enumDev(inst, &n, devs) >= 0) for (unsigned i = 0; i < n && i < 8; i++) {
        float prio = 1; VkQueueInfo qi = { 2, nullptr, 0, 0, 1, &prio }; const char* ext[1] = { "VK_KHR_swapchain" };
        VkDevInfo di = { 3, nullptr, 0, 1, &qi, 0, nullptr, 1, ext, nullptr };
        void* dev = nullptr;
        if (mkDev(devs[i], &di, nullptr, &dev) == 0 && dev) { InstallVk(getProc(dev, "vkQueuePresentKHR")); delDev(dev, nullptr); }
    }
    delInst(inst, nullptr);
}

static void HookDxgi();

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
    bool dx = false, vk = false; int ticks = 0;
    for (;;) { // DX games: hook once dxgi.dll is there. OpenGL games (e.g. Minecraft Java): patch SwapBuffers imports as modules appear.
        if (!dx && GetModuleHandleW(L"dxgi.dll")) { dx = true; HookDxgi(); }
        if (!vk && GetModuleHandleW(L"vulkan-1.dll")) { vk = true; HookVulkan(); }
        if (GetModuleHandleW(L"opengl32.dll")) ScanGl();
        Sleep(++ticks < 40 ? 500 : 3000); // modules load mostly at startup: poll fast for 20 s, then rarely
    }
}

// Dummy swapchain only to read the vtable; all swapchains share dxgi.dll's vtable.
static void HookDxgi() {
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
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_mod = m; DisableThreadLibraryCalls(m);
        CloseHandle(CreateThread(nullptr, 0, Init, nullptr, 0, nullptr));
    }
    return TRUE;
}
// shortcut: no unload (DLL stays pinned, hooks never removed), upgrade if you need hot-eject.
// shortcut: the Vulkan inline patch is not safe against another thread executing the first bytes of vkQueuePresentKHR at the exact moment of patching; use a real detour library if that ever shows up.
// shortcut: one global deadline, so a process with several swapchains is limited as one stream.
