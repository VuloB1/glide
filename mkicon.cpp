// Build tool: renders the logo at several sizes and writes glide.ico (PNG-compressed entries).
#define NOMINMAX
#include <algorithm>
using std::min; using std::max;
#include <windows.h>
#include <gdiplus.h>
#include <objidl.h>
#include <cstdio>
#include <vector>
using namespace Gdiplus;
#include "logo.h"

static bool Png(int size, std::vector<BYTE>& out) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    { Graphics g(&bmp); g.Clear(Color(0, 0, 0, 0)); DrawLogo(g, 0, 0, (float)size); }
    UINT n = 0, bytes = 0; GetImageEncodersSize(&n, &bytes);
    std::vector<BYTE> buf(bytes); ImageCodecInfo* ci = (ImageCodecInfo*)buf.data(); GetImageEncoders(n, bytes, ci);
    CLSID png = {}; for (UINT i = 0; i < n; i++) if (!wcscmp(ci[i].MimeType, L"image/png")) png = ci[i].Clsid;
    IStream* s = nullptr; CreateStreamOnHGlobal(nullptr, TRUE, &s);
    bool ok = bmp.Save(s, &png, nullptr) == Ok;
    if (ok) { HGLOBAL h; GetHGlobalFromStream(s, &h); SIZE_T sz = GlobalSize(h); out.assign((BYTE*)GlobalLock(h), (BYTE*)GlobalLock(h) + sz); GlobalUnlock(h); }
    s->Release(); return ok;
}

int main() {
    ULONG_PTR tok; GdiplusStartupInput in; GdiplusStartup(&tok, &in, nullptr);
    const int sizes[] = { 16, 24, 32, 48, 64, 256 }; const int N = 6;
    std::vector<BYTE> png[N];
    for (int i = 0; i < N; i++) if (!Png(sizes[i], png[i])) { puts("png failed"); return 1; }
    FILE* f = fopen("glide.ico", "wb"); if (!f) return 1;
    WORD hdr[3] = { 0, 1, N }; fwrite(hdr, 6, 1, f);
    DWORD off = 6 + 16 * N;
    for (int i = 0; i < N; i++) {
        BYTE e[16] = { (BYTE)(sizes[i] == 256 ? 0 : sizes[i]), (BYTE)(sizes[i] == 256 ? 0 : sizes[i]), 0, 0, 1, 0, 32, 0 };
        DWORD len = (DWORD)png[i].size(); memcpy(e + 8, &len, 4); memcpy(e + 12, &off, 4); fwrite(e, 16, 1, f); off += len;
    }
    for (int i = 0; i < N; i++) fwrite(png[i].data(), png[i].size(), 1, f);
    fclose(f); GdiplusShutdown(tok); return 0;
}
