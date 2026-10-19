// Glide installer / uninstaller in one exe. Per-user install (no admin): %LOCALAPPDATA%\Programs\Glide.
// glide.dll and glide-ui.exe are embedded as resources (setup.rc). Flags: /uninstall, /S (silent).
#define UNICODE
#define NOMINMAX
#include <algorithm>
using std::min; using std::max;
#include <windows.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
using namespace Gdiplus;
#include "logo.h"
#include "lang.h"

// Installer text follows the Windows language (same order as lang.h: en es pt ja zh ko).
enum { I_DESK, I_OPEN, I_INSTALL, I_FINISH, I_RETRY, I_CANCEL, I_LBL, I_LBLDONE, I_READY, I_NOTE, I_TITLE, I_ERRDLL, I_ERRUI, I_KEEP, I_UNTITLE, I_UNDONE, I_COUNT };
static const wchar_t* const kSet[I_COUNT][NLANG] = {
    { L"Create a desktop shortcut", L"Crear acceso directo en el escritorio", L"Criar atalho na área de trabalho", L"デスクトップにショートカットを作成", L"创建桌面快捷方式", L"바탕 화면에 바로 가기 만들기" },
    { L"Open Glide when finished", L"Abrir Glide al terminar", L"Abrir o Glide ao concluir", L"完了後に Glide を開く", L"完成后打开 Glide", L"완료 후 Glide 열기" },
    { L"Install", L"Instalar", L"Instalar", L"インストール", L"安装", L"설치" },
    { L"Finish", L"Finalizar", L"Concluir", L"完了", L"完成", L"마침" },
    { L"Retry", L"Reintentar", L"Tentar de novo", L"再試行", L"重试", L"다시 시도" },
    { L"Cancel", L"Cancelar", L"Cancelar", L"キャンセル", L"取消", L"취소" },
    { L"INSTALLATION", L"INSTALACIÓN", L"INSTALAÇÃO", L"インストール", L"安装", L"설치" },
    { L"INSTALLED", L"INSTALADO", L"INSTALADO", L"インストール完了", L"已安装", L"설치됨" },
    { L"Glide is ready. Open it from the Start menu.", L"Glide está listo. Ábrelo desde el menú Inicio.", L"O Glide está pronto. Abra-o pelo menu Iniciar.", L"Glide の準備ができました。スタートメニューから開けます。", L"Glide 已就绪。可从开始菜单打开。", L"Glide가 준비되었습니다. 시작 메뉴에서 여세요." },
    { L"Glide automatically blocks games with anti-cheat.", L"Glide bloquea automáticamente los juegos con anti-cheat.", L"O Glide bloqueia automaticamente jogos com anti-cheat.", L"Glide はアンチチート搭載ゲームを自動的にブロックします。", L"Glide 会自动屏蔽带反作弊的游戏。", L"Glide는 안티치트가 있는 게임을 자동으로 차단합니다." },
    { L"Install Glide", L"Instalar Glide", L"Instalar o Glide", L"Glide のインストール", L"安装 Glide", L"Glide 설치" },
    { L"Could not write glide.dll. If a game with Glide injected is open, close it and retry.", L"No se pudo escribir glide.dll. Si hay un juego abierto con Glide inyectado, ciérralo y reintenta.", L"Não foi possível gravar glide.dll. Se há um jogo aberto com o Glide injetado, feche-o e tente de novo.", L"glide.dll を書き込めません。Glide が注入されたゲームを閉じて再試行してください。", L"无法写入 glide.dll。如果有已注入 Glide 的游戏正在运行，请关闭后重试。", L"glide.dll을 쓸 수 없습니다. Glide가 주입된 게임이 열려 있으면 닫고 다시 시도하세요." },
    { L"Could not write glide-ui.exe.", L"No se pudo escribir glide-ui.exe.", L"Não foi possível gravar glide-ui.exe.", L"glide-ui.exe を書き込めません。", L"无法写入 glide-ui.exe。", L"glide-ui.exe를 쓸 수 없습니다." },
    { L"Keep your profiles and settings (glide.ini)?", L"¿Quieres conservar tus perfiles y ajustes (glide.ini)?", L"Manter seus perfis e ajustes (glide.ini)?", L"プロファイルと設定（glide.ini）を残しますか？", L"是否保留你的配置和设置（glide.ini）？", L"프로필과 설정(glide.ini)을 유지할까요?" },
    { L"Uninstall Glide", L"Desinstalar Glide", L"Desinstalar o Glide", L"Glide のアンインストール", L"卸载 Glide", L"Glide 제거" },
    { L"Glide has been uninstalled.", L"Glide se ha desinstalado.", L"O Glide foi desinstalado.", L"Glide をアンインストールしました。", L"Glide 已卸载。", L"Glide가 제거되었습니다." },
};
static int g_lang;
static const wchar_t* I(int id) { return kSet[id][g_lang]; }

static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Glide";
static wchar_t g_dir[MAX_PATH], g_self[MAX_PATH];

static std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring s; if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { s = p; CoTaskMemFree(p); } return s;
}
static std::wstring In(const wchar_t* dir, const wchar_t* f) { return std::wstring(dir) + L"\\" + f; }

static void KillProc(const wchar_t* exe) {
    PROCESSENTRY32W e = { sizeof(e) };
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e))
        if (!_wcsicmp(e.szExeFile, exe))
            if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, e.th32ProcessID)) { TerminateProcess(p, 0); WaitForSingleObject(p, 3000); CloseHandle(p); }
    CloseHandle(s);
}

static bool Extract(int id, const std::wstring& path) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA); if (!r) return false;
    HGLOBAL h = LoadResource(nullptr, r); DWORD n = SizeofResource(nullptr, r);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = WriteFile(f, LockResource(h), n, &w, nullptr) && w == n;
    CloseHandle(f); return ok;
}

static void Shortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    IShellLinkW* l = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)))) return;
    l->SetPath(target.c_str()); l->SetWorkingDirectory(dir.c_str()); l->SetDescription(L"Glide - frame pacing"); l->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    l->Release();
}

static void RegStr(HKEY k, const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2)); }

// Returns an empty string on success, otherwise a message for the user.
static std::wstring Install(bool desktop) {
    SHCreateDirectoryExW(nullptr, g_dir, nullptr);
    KillProc(L"glide-ui.exe");
    if (!Extract(101, In(g_dir, L"glide.dll"))) return I(I_ERRDLL);
    if (!Extract(102, In(g_dir, L"glide-ui.exe"))) return I(I_ERRUI);
    std::wstring un = In(g_dir, L"uninstall.exe");
    if (_wcsicmp(g_self, un.c_str())) CopyFileW(g_self, un.c_str(), FALSE);

    std::wstring exe = In(g_dir, L"glide-ui.exe");
    Shortcut(In(Known(FOLDERID_Programs).c_str(), L"Glide.lnk"), exe, g_dir);
    if (desktop) Shortcut(In(Known(FOLDERID_Desktop).c_str(), L"Glide.lnk"), exe, g_dir);

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegStr(k, L"DisplayName", L"Glide"); RegStr(k, L"DisplayVersion", L"1.0"); RegStr(k, L"Publisher", L"Glide");
        RegStr(k, L"InstallLocation", g_dir); RegStr(k, L"DisplayIcon", exe);
        RegStr(k, L"UninstallString", L"\"" + un + L"\" /uninstall");
        DWORD one = 1, sz = 300; RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (BYTE*)&one, 4); RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (BYTE*)&one, 4);
        RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (BYTE*)&sz, 4); RegCloseKey(k);
    }
    return L"";
}

static void Uninstall(bool keepConfig) {
    KillProc(L"glide-ui.exe");
    for (const wchar_t* f : { L"glide.dll", L"glide-ui.exe" }) {
        std::wstring p = In(g_dir, f);
        if (!DeleteFileW(p.c_str()) && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT); // still loaded by a game
    }
    if (!keepConfig) DeleteFileW(In(g_dir, L"glide.ini").c_str());
    DeleteFileW(In(Known(FOLDERID_Programs).c_str(), L"Glide.lnk").c_str());
    DeleteFileW(In(Known(FOLDERID_Desktop).c_str(), L"Glide.lnk").c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstKey);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Glide"); // "Start with Windows" entry of the app
    wchar_t cmd[2 * MAX_PATH + 100]; // this exe cannot delete itself while running
    swprintf_s(cmd, L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"%s\\uninstall.exe\" & rmdir \"%s\"", g_dir, g_dir);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

// ---------- window ----------
enum { B_MAIN = 1, B_CANCEL, C_DESK, C_OPEN };
struct Btn { int id; float x, y, w, h; };
static std::vector<Btn> g_btns = { { B_MAIN, 300, 288, 136, 38 }, { B_CANCEL, 176, 288, 112, 38 }, { C_DESK, 40, 176, 380, 24 }, { C_OPEN, 40, 206, 380, 24 } };
static int g_state, g_hot, g_down; // state: 0 ready, 1 done, 2 error
static bool g_desk = true, g_open = true;
static std::wstring g_err;
static float g_k = 1;
static HWND g_wnd;
static Font *fTitle, *fSub, *fLabel, *fBtn, *fSmall;
static const Color cBg(18, 20, 24), cCard(27, 30, 37), cBorder(44, 48, 58), cInput(38, 42, 51), cText(230, 232, 238), cMuted(139, 145, 161), cAccent(91, 140, 255), cBad(240, 90, 80), cGood(61, 220, 151);

static int S(int v) { return (int)(v * g_k + .5f); }
static Color Mix(Color c, int d) { auto cl = [](int v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); }; return Color(c.GetA(), cl(c.GetR() + d), cl(c.GetG() + d), cl(c.GetB() + d)); }
static void Round(Graphics& g, Color fill, float x, float y, float w, float h, float r, const Color* stroke = nullptr) {
    GraphicsPath p; float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90); p.AddArc(x + w - d, y, d, d, 270, 90); p.AddArc(x + w - d, y + h - d, d, d, 0, 90); p.AddArc(x, y + h - d, d, d, 90, 90); p.CloseFigure();
    SolidBrush b(fill); g.FillPath(&b, &p); if (stroke) { Pen pen(*stroke, 1); g.DrawPath(&pen, &p); }
}
static void Txt(Graphics& g, const wchar_t* s, Font* f, Color c, float x, float y, float w = 400, float h = 0, StringAlignment al = StringAlignmentNear) {
    StringFormat sf; sf.SetAlignment(al); sf.SetFormatFlags(StringFormatFlagsNoWrap); sf.SetTrimming(StringTrimmingEllipsisCharacter);
    if (h > 0) sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(c); RectF r(x, y, w, h > 0 ? h : 100); g.DrawString(s, -1, f, r, &sf, &b);
}
static bool Shown(int id) { return id == B_CANCEL ? g_state != 1 : id == C_DESK ? g_state != 1 : true; }
static RECT BtnRect(const Btn& b) { return { S((int)b.x), S((int)b.y), S((int)(b.x + b.w)), S((int)(b.y + b.h)) }; }
static void Inv(int id) { for (auto& b : g_btns) if (b.id == id) { RECT r = BtnRect(b); InvalidateRect(g_wnd, &r, FALSE); } }
static int Hit(LPARAM lp) {
    float x = (short)LOWORD(lp) / g_k, y = (short)HIWORD(lp) / g_k;
    for (auto& b : g_btns) if (Shown(b.id) && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return b.id;
    return 0;
}

static void DrawBtn(Graphics& g, const Btn& b) {
    GraphicsState st = g.Save(); g.TranslateTransform(b.x, b.y);
    int shade = b.id == g_down && b.id == g_hot ? -18 : b.id == g_hot ? 14 : 0;
    if (b.id == C_DESK || b.id == C_OPEN) { // checkbox
        bool on = b.id == C_DESK ? g_desk : g_open;
        Round(g, on ? Mix(cAccent, shade) : Mix(cInput, shade), 0, (b.h - 20) / 2, 20, 20, 6, on ? nullptr : &cBorder);
        if (on) { Pen p(Color(255, 255, 255), 2.2f); p.SetLineCap(LineCapRound, LineCapRound, DashCapRound); g.DrawLine(&p, 5.f, b.h / 2, 8.5f, b.h / 2 + 3.5f); g.DrawLine(&p, 8.5f, b.h / 2 + 3.5f, 15.f, b.h / 2 - 3.5f); }
        Txt(g, b.id == C_DESK ? I(I_DESK) : I(I_OPEN), fBtn, cText, 30, 0, b.w - 30, b.h);
    } else if (b.id == B_MAIN) {
        Round(g, Mix(cAccent, shade), 0, 0, b.w, b.h, 9);
        Txt(g, g_state == 0 ? I(I_INSTALL) : g_state == 1 ? I(I_FINISH) : I(I_RETRY), fBtn, Color(255, 255, 255), 0, 0, b.w, b.h, StringAlignmentCenter);
    } else {
        Round(g, Mix(cInput, shade), 0, 0, b.w, b.h, 9, &cBorder);
        Txt(g, I(I_CANCEL), fBtn, cText, 0, 0, b.w, b.h, StringAlignmentCenter);
    }
    g.Restore(st);
}

static void Paint(HDC dc) {
    RECT rc; GetClientRect(g_wnd, &rc);
    HDC m = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); SelectObject(m, bm);
    {
        Graphics g(m); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(cBg); g.ScaleTransform(g_k, g_k);
        DrawLogo(g, 28, 24, 68);
        Txt(g, L"Glide", fTitle, cText, 112, 26);
        Txt(g, kStr[S_SUB][g_lang], fSub, cMuted, 114, 66);
        Round(g, cCard, 24, 112, 412, 150, 14, &cBorder);
        Txt(g, g_state == 1 ? I(I_LBLDONE) : I(I_LBL), fLabel, cMuted, 40, 126);
        if (g_state == 2) { Txt(g, g_err.c_str(), fSmall, cBad, 40, 148, 380, 20); }
        else if (g_state == 1) { Txt(g, I(I_READY), fBtn, cGood, 40, 148, 380, 20); }
        else Txt(g, g_dir, fSmall, cMuted, 40, 148, 380, 20);
        for (auto& b : g_btns) if (Shown(b.id)) DrawBtn(g, b);
        Txt(g, I(I_NOTE), fSmall, cMuted, 28, 270, 410);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
    DeleteObject(bm); DeleteDC(m);
}

static void Click(int id) {
    if (id == C_DESK) g_desk = !g_desk;
    else if (id == C_OPEN) g_open = !g_open;
    else if (id == B_CANCEL) DestroyWindow(g_wnd);
    else if (id == B_MAIN) {
        if (g_state == 1) {
            if (g_open) ShellExecuteW(nullptr, L"open", In(g_dir, L"glide-ui.exe").c_str(), nullptr, g_dir, SW_SHOWNORMAL);
            DestroyWindow(g_wnd); return;
        }
        g_err = Install(g_desk); g_state = g_err.empty() ? 1 : 2;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps); Paint(dc); EndPaint(w, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int h = Hit(lp);
        if (h != g_hot) { int o = g_hot; g_hot = h; Inv(o); Inv(h); TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, w, 0 }; TrackMouseEvent(&t); }
        return 0;
    }
    case WM_MOUSELEAVE: { int o = g_hot; g_hot = 0; Inv(o); return 0; }
    case WM_SETCURSOR: if (g_hot && LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; } break;
    case WM_LBUTTONDOWN: g_down = Hit(lp); SetCapture(w); Inv(g_down); return 0;
    case WM_LBUTTONUP: { int d = g_down; g_down = 0; ReleaseCapture(); Inv(d); if (d && d == Hit(lp)) Click(d); return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    std::wstring cmd = GetCommandLineW();
    bool uninst = cmd.find(L"/uninstall") != std::wstring::npos, silent = cmd.find(L"/S") != std::wstring::npos;
    GetModuleFileNameW(nullptr, g_self, MAX_PATH);
    std::wstring local = Known(FOLDERID_LocalAppData);
    swprintf_s(g_dir, L"%s\\Programs\\Glide", local.c_str());
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WORD pl = PRIMARYLANGID(GetUserDefaultUILanguage());
    g_lang = pl == LANG_SPANISH ? 1 : pl == LANG_PORTUGUESE ? 2 : pl == LANG_JAPANESE ? 3 : pl == LANG_CHINESE ? 4 : pl == LANG_KOREAN ? 5 : 0;

    if (uninst) {
        bool keep = !silent && MessageBoxW(nullptr, I(I_KEEP), I(I_UNTITLE), MB_YESNO | MB_ICONQUESTION) == IDYES;
        Uninstall(keep);
        if (!silent) MessageBoxW(nullptr, I(I_UNDONE), L"Glide", MB_ICONINFORMATION);
        return 0;
    }
    if (silent) return Install(false).empty() ? 0 : 1;

    SetProcessDPIAware();
    HDC sdc = GetDC(nullptr); g_k = GetDeviceCaps(sdc, LOGPIXELSX) / 96.f; ReleaseDC(nullptr, sdc);
    ULONG_PTR gdip; GdiplusStartupInput gin; GdiplusStartup(&gdip, &gin, nullptr);
    const wchar_t* fam = g_lang == 3 ? L"Yu Gothic UI" : g_lang == 4 ? L"Microsoft YaHei UI" : g_lang == 5 ? L"Malgun Gothic" : L"Segoe UI";
    fTitle = new Font(L"Segoe UI", 30, FontStyleBold, UnitPixel); fSub = new Font(fam, 14, FontStyleRegular, UnitPixel);
    fLabel = new Font(fam, 11, FontStyleBold, UnitPixel); fBtn = new Font(fam, 13, FontStyleBold, UnitPixel);
    fSmall = new Font(fam, 12, FontStyleRegular, UnitPixel);

    WNDCLASSW wc = { 0, Proc, 0, 0, hi, LoadIconW(hi, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, 0, L"GlideSetup" };
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, S(460), S(346) }; AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g_wnd = CreateWindowW(L"GlideSetup", I(I_TITLE), style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, 0, 0, hi, 0);
    BOOL dark = TRUE; DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));
    COLORREF cap = RGB(18, 20, 24); DwmSetWindowAttribute(g_wnd, 35, &cap, sizeof(cap));
    ShowWindow(g_wnd, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, 0, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    GdiplusShutdown(gdip);
    return 0;
}
// shortcut: per-user install only, no version upgrade logic beyond overwriting files; upgrade if you need Program Files / machine-wide installs.
