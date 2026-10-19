// Self-check: D3D11 window presenting uncapped; optionally LoadLibrary glide.dll, then prints frametime stats.
// Usage: test.exe [load]
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#pragma comment(lib, "d3d11.lib")

int main(int argc, char** argv) {
    WNDCLASSW wc = { 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), 0, 0, 0, 0, L"t" };
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"t", L"t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 320, 240, 0, 0, wc.hInstance, 0);
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 2; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = w; d.SampleDesc.Count = 1; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc; ID3D11Device* dev; ID3D11DeviceContext* ctx;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx))) return 2;
    if (argc > 1 && !strcmp(argv[1], "load")) { LoadLibraryW(L"glide.dll"); Sleep(500); } // let hook install

    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    const int N = argc > 2 ? atoi(argv[2]) : 600; double* ms = new double[N];
    QueryPerformanceCounter(&a);
    for (int i = 0; i < N; i++) {
        MSG m; while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        sc->Present(0, 0);
        QueryPerformanceCounter(&b);
        ms[i] = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; a = b;
    }
    double mean = 0, var = 0, mx = 0;
    for (int i = 10; i < N; i++) { mean += ms[i]; if (ms[i] > mx) mx = ms[i]; }
    mean /= N - 10;
    for (int i = 10; i < N; i++) var += (ms[i] - mean) * (ms[i] - mean);
    printf("mean %.3f ms (%.1f fps)  stddev %.3f ms  max %.3f ms\n", mean, 1000 / mean, sqrt(var / (N - 10)), mx);
    return 0;
}
