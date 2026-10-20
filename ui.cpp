// Glide UI: pick a process, per-game profile (FPS cap + auto-inject) stored in glide.ini, inject glide.dll, see live frametimes.
// Games with known anti-cheat are refused (name blocklist + anti-cheat modules in the target); extend it with a [block] section in glide.ini.
#define UNICODE
#define NOMINMAX
#include <algorithm>
using std::min; using std::max;
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wincrypt.h>
#include "version.h"
#include <gdiplus.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

struct Stats { volatile LONG head; float ms[240]; volatile LONG calState, suggest, capable, calLeft; }; // must match glide.cpp

#include "lang.h"
enum { ID_PROC = 1, ID_FPS, ID_APPLY, ID_INJECT, ID_AUTO, ID_TIMER, ID_PICK, ID_CALIB, ID_USE, ID_PRE = 10, ID_LANG = 20, ID_STARTUP, ID_BG, ID_GEAR, ID_UPCHK, ID_AUTOUP, ID_GITHUB, ID_ISSUES };
static HWND hProc, hFps, g_wnd;
static bool g_auto, g_startup, g_bg, g_quit; // g_bg: closing the window hides it to the tray instead of exiting
static int g_lang, g_page; // g_page: 0 main, 1 settings
static bool g_autoUp = true;
enum { U_IDLE, U_CHECKING, U_LATEST, U_AVAIL, U_ERR, U_DL, U_INST, U_ERR2 };
static volatile LONG g_up; // update state; worker threads fill the strings below, then publish the state
static std::wstring g_upTag, g_upUrl; static std::string g_upSha;
static ULONGLONG g_nextCheck;
static const UINT WM_UPDATE = WM_APP + 3;
static const wchar_t* T(int id) { return kStr[id][g_lang]; }
static std::wstring g_status; // empty = default footer hint
static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH];
static const UINT WM_TRAY = WM_APP + 1, WM_SHOWME = WM_APP + 2;
static UINT g_tbCreated; static HICON g_icon;
static HANDLE g_map;
static Stats* g_st;
static LONG g_lastHead; static DWORD g_lastChange;
static std::set<DWORD> g_tried; // pids auto-inject already attempted (one shot each)
static int g_sugg;                       // FPS suggested by the last calibration (0 = none)
static bool g_calWait, g_calSeen; static DWORD g_calT0;
struct Msg { int id, arg; }; // calibration message: string id + optional %d argument, so it re-renders when the language changes
static Msg g_c1 = { S_CAL_IDLE1, 0 }, g_c2 = { S_CAL_IDLE2, 0 };

// Not exhaustive: games whose anti-cheat bans for injected DLLs or that are widely reported to.
static const wchar_t* kBlock[] = {
    L"VALORANT-Win64-Shipping.exe", L"VALORANT.exe", L"Overwatch.exe", L"FortniteClient-Win64-Shipping.exe",
    L"r5apex.exe", L"r5apex_dx12.exe", L"cs2.exe", L"csgo.exe", L"dota2.exe", L"hl2.exe", L"Deadlock.exe", L"project8.exe",
    L"destiny2.exe", L"RainbowSix.exe", L"RainbowSix_Vulkan.exe", L"RainbowSix_BE.exe", L"TslGame.exe",
    L"EscapeFromTarkov.exe", L"League of Legends.exe", L"LeagueClient.exe", L"GTA5.exe", L"GTA5_Enhanced.exe", L"RDR2.exe",
    L"cod.exe", L"ModernWarfare.exe", L"BlackOps6.exe", L"bf6.exe", L"bf2042.exe", L"BF1.exe", L"bfv.exe", L"BF4.exe", L"THE FINALS.exe",
    L"Discovery.exe", L"Marvel-Win64-Shipping.exe", L"RustClient.exe", L"DayZ_x64.exe", L"FC24.exe", L"FC25.exe", L"FC26.exe",
    L"FIFA23.exe", L"ZenlessZoneZero.exe", L"GenshinImpact.exe", L"YuanShen.exe", L"StarRail.exe", L"eldenring.exe",
    L"nightreign.exe", L"DeadByDaylight-Win64-Shipping.exe", L"HaloInfinite.exe", L"RobloxPlayerBeta.exe", L"FallGuys_client_game.exe",
    L"RocketLeague.exe", L"LostArk.exe", L"BlackDesert64.exe", L"NarakaBladepoint.exe", L"Hunt.exe", L"ReadyOrNot-Win64-Shipping.exe",
    L"VRChat.exe", L"Paladins.exe", L"Smite.exe", L"PathOfExile_x64.exe", L"Warzone.exe", L"MarvelRivals.exe", L"Lineage2.exe",
    L"NBA2K25.exe", L"NBA2K26.exe", L"WWE2K24.exe", L"MK1.exe", L"Wow.exe", L"Diablo IV.exe", L"Battle.net.exe",
};
// Substrings of module names that mean an anti-cheat is loaded in the target.
static const wchar_t* kAcMods[] = { L"easyanticheat", L"beclient", L"battleye", L"gameguard", L"npggnt", L"xigncode", L"vgk", L"mhyprot", L"faceit", L"ricochet", L"eaanticheat", L"javelin" };

static bool Blocked(const wchar_t* name) {
    for (auto b : kBlock) if (!_wcsicmp(b, name)) return true;
    return GetPrivateProfileIntW(L"block", name, 0, g_ini) != 0;
}

struct Mods { bool dxgi, gl, vk, glide, ac; }; // gl: OpenGL (e.g. Minecraft Java), vk: Vulkan
static Mods ScanModules(DWORD pid) {
    Mods r = {};
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return r; // protected/exited: treated as "nothing found"
    MODULEENTRY32W e = { sizeof(e) };
    for (BOOL ok = Module32FirstW(s, &e); ok; ok = Module32NextW(s, &e)) {
        wchar_t m[MAX_MODULE_NAME32 + 1]; wcscpy_s(m, e.szModule); _wcslwr_s(m);
        if (!wcscmp(m, L"dxgi.dll")) r.dxgi = true;
        if (!wcscmp(m, L"opengl32.dll")) r.gl = true;
        if (!wcscmp(m, L"vulkan-1.dll")) r.vk = true;
        if (!wcscmp(m, L"glide.dll")) r.glide = true;
        for (auto k : kAcMods) if (wcsstr(m, k)) r.ac = true;
    }
    CloseHandle(s);
    return r;
}

static std::vector<std::pair<DWORD, std::wstring>> Procs() {
    std::vector<std::pair<DWORD, std::wstring>> v;
    PROCESSENTRY32W e = { sizeof(e) };
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e)) v.push_back({ e.th32ProcessID, e.szExeFile });
    CloseHandle(s);
    return v;
}

static BOOL CALLBACK EnumWin(HWND h, LPARAM lp) {
    if (IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && GetWindowTextLengthW(h)) {
        DWORD pid; GetWindowThreadProcessId(h, &pid);
        auto& m = *(std::map<DWORD, std::wstring>*)lp;
        if (!m.count(pid)) { wchar_t t[128]; GetWindowTextW(h, t, 128); m[pid] = t; }
    }
    return TRUE;
}
static std::map<DWORD, std::wstring> WindowPids() { std::map<DWORD, std::wstring> s; EnumWindows(EnumWin, (LPARAM)&s); return s; }

static void LoadProfile() {
    wchar_t n[64]; GetWindowTextW(hProc, n, 64);
    wchar_t t[16];
    swprintf_s(t, L"%u", GetPrivateProfileIntW(n, L"fps", GetPrivateProfileIntW(L"glide", L"fps", 177, g_ini), g_ini));
    SetWindowTextW(hFps, t);
    g_auto = GetPrivateProfileIntW(n, L"auto", 0, g_ini) != 0; InvalidateRect(g_wnd, nullptr, FALSE);
}

static void SaveProfile() {
    wchar_t n[64], t[16];
    GetWindowTextW(hProc, n, 64);
    if (!*n) return;
    GetWindowTextW(hFps, t, 16);  WritePrivateProfileStringW(n, L"fps", t, g_ini);
    WritePrivateProfileStringW(n, L"auto", g_auto ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"glide", L"process", n, g_ini);
}

enum { R_OK, R_BLOCKED, R_NOOPEN, R_FAILED };
static int InjectPid(DWORD pid, const wchar_t* name) {
    if (Blocked(name) || ScanModules(pid).ac) return R_BLOCKED;
    wchar_t dll[MAX_PATH];
    swprintf_s(dll, L"%s\\glide.dll", g_dir);
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!p) return R_NOOPEN;
    SIZE_T n = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD ret = 0;
    if (mem && WriteProcessMemory(p, mem, dll, n, nullptr)) {
        HANDLE t = CreateRemoteThread(p, nullptr, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32"), "LoadLibraryW"), mem, 0, nullptr);
        if (t) { WaitForSingleObject(t, 5000); GetExitCodeThread(t, &ret); CloseHandle(t); }
    }
    CloseHandle(p);
    return ret ? R_OK : R_FAILED;
}

static void Inject() {
    wchar_t name[64];
    GetWindowTextW(hProc, name, 64);
    DWORD pid = 0;
    for (auto& p : Procs()) if (!_wcsicmp(p.second.c_str(), name)) { pid = p.first; break; }
    if (!pid) { MessageBoxW(g_wnd, T(S_MB_NOTFOUND), L"Glide", MB_ICONWARNING); return; }
    if (ScanModules(pid).glide) { MessageBoxW(g_wnd, T(S_MB_ALREADY), L"Glide", MB_ICONINFORMATION); return; }
    switch (InjectPid(pid, name)) {
    case R_BLOCKED: MessageBoxW(g_wnd, T(S_MB_BLOCKED), L"Glide", MB_ICONSTOP); break;
    case R_NOOPEN:  MessageBoxW(g_wnd, T(S_MB_NOOPEN), L"Glide", MB_ICONERROR); break;
    case R_FAILED:  MessageBoxW(g_wnd, T(S_MB_FAILED), L"Glide", MB_ICONERROR); break;
    }
}

// Every ~2 s: inject into windowed processes whose profile has auto=1.
static void AutoScan() {
    wchar_t names[4096], msg[160];
    GetPrivateProfileSectionNamesW(names, 4096, g_ini);
    auto wins = WindowPids(); auto procs = Procs();
    for (wchar_t* n = names; *n; n += wcslen(n) + 1) {
        if (!GetPrivateProfileIntW(n, L"auto", 0, g_ini)) continue;
        for (auto& p : procs) {
            if (_wcsicmp(p.second.c_str(), n) || !wins.count(p.first) || !g_tried.insert(p.first).second) continue;
            if (ScanModules(p.first).glide) continue;
            switch (InjectPid(p.first, n)) {
            case R_OK:      swprintf_s(msg, T(S_AUTO_OK), n); break;
            case R_BLOCKED: swprintf_s(msg, T(S_AUTO_BLK), n); break;
            default:        swprintf_s(msg, T(S_AUTO_FAIL), n);
            }
            g_status = msg;
        }
    }
}

// ---------- look & feel (GDI+, everything laid out in 96-dpi units and scaled by g_k) ----------
using namespace Gdiplus;
#include "logo.h"
static const Color cBg(18, 20, 24), cCard(27, 30, 37), cBorder(44, 48, 58), cInput(38, 42, 51), cText(230, 232, 238),
                   cMuted(139, 145, 161), cAccent(91, 140, 255), cGood(61, 220, 151), cBad(240, 90, 80);
static Font *fTitle, *fSub, *fLabel, *fBig, *fSmall, *fBtn;
static float g_k = 1;
static const int kPre[6] = { 60, 90, 120, 144, 180, 0 };
static HBRUSH g_brIn, g_brCard;
static const RECT kStatsCard = { 24, 384, 536, 572 }, kPill = { 312, 22, 488, 54 };

static int S(int v) { return (int)(v * g_k + .5f); }
static Color Mix(Color c, int d) { auto cl = [](int v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); }; return Color(c.GetA(), cl(c.GetR() + d), cl(c.GetG() + d), cl(c.GetB() + d)); }

static void RoundPath(GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90); p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90); p.AddArc(x, y + h - d, d, d, 90, 90); p.CloseFigure();
}
static void Round(Graphics& g, Color fill, float x, float y, float w, float h, float r, const Color* stroke = nullptr) {
    GraphicsPath p; RoundPath(p, x, y, w, h, r);
    SolidBrush b(fill); g.FillPath(&b, &p);
    if (stroke) { Pen pen(*stroke, 1); g.DrawPath(&pen, &p); }
}
static void Txt(Graphics& g, const wchar_t* s, Font* f, Color c, float x, float y, float w = 400, float h = 0, StringAlignment al = StringAlignmentNear) {
    StringFormat sf; sf.SetAlignment(al); sf.SetFormatFlags(StringFormatFlagsNoWrap); sf.SetTrimming(StringTrimmingEllipsisCharacter);
    if (h > 0) sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(c); RectF r(x, y, w, h > 0 ? h : 100); g.DrawString(s, -1, f, r, &sf, &b);
}

static int CurFps() { wchar_t t[16]; GetWindowTextW(hFps, t, 16); return _wtoi(t); }

// Buttons are painted and hit-tested by the main window (native child buttons leave a stray 1px edge).
struct Btn { int id; float x, y, w, h; int str; const wchar_t* lit; int page; }; // label = lit if set, else the translated string `str`
static const wchar_t* Label(const Btn& b) { return b.id == ID_UPCHK ? T(g_up == U_AVAIL ? S_UP_NOW : g_up == U_CHECKING ? S_UP_CHECKING : g_up == U_DL ? S_UP_DL : g_up == U_INST ? S_UP_INST : S_UP_CHECK) : b.lit ? b.lit : T(b.str); }
static bool On(int id) { return id == ID_AUTO ? g_auto : id == ID_STARTUP ? g_startup : id == ID_AUTOUP ? g_autoUp : g_bg; }
static bool Vis(const Btn& b) { return b.page < 0 || b.page == g_page; }
static std::vector<Btn> g_btns;
static int g_hot, g_down;

static RECT BtnRect(const Btn& b) { return { S((int)b.x), S((int)b.y), S((int)(b.x + b.w)), S((int)(b.y + b.h)) }; }
static void InvBtn(int id) { for (auto& b : g_btns) if (b.id == id) { RECT r = BtnRect(b); InvalidateRect(g_wnd, &r, FALSE); } }
static void InvPills() { for (int i = 0; i < 6; i++) InvBtn(ID_PRE + i); }
static int HitBtn(LPARAM lp) {
    float x = (short)LOWORD(lp) / g_k, y = (short)HIWORD(lp) / g_k;
    for (auto& b : g_btns) if (Vis(b) && (b.id != ID_USE || g_sugg) && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return b.id;
    return 0;
}

static void DrawBtn(Graphics& g, const Btn& b) {
    GraphicsState st = g.Save(); g.TranslateTransform(b.x, b.y);
    float w = b.w, h = b.h; int id = b.id;
    int shade = id == g_down && id == g_hot ? -18 : id == g_hot ? 14 : 0;
    if (id == ID_PICK || id == ID_LANG) { // whole input box is the click target; chevron on the right
        if (id == g_hot) { GraphicsPath gp; RoundPath(gp, 0.5f, 0.5f, w - 1, h - 1, 8); Pen hp(cAccent, 1.2f); g.DrawPath(&hp, &gp); }
        Pen p(shade ? cText : cMuted, 2); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound); p.SetLineJoin(LineJoinRound);
        PointF c[3] = { PointF(w - 25, h / 2 - 2), PointF(w - 20, h / 2 + 3), PointF(w - 15, h / 2 - 2) }; g.DrawLines(&p, c, 3);
    } else if (id == ID_GEAR) {
        Round(g, Mix(cCard, shade), 0, 0, w, h, 10, &cBorder);
        Color ic = shade ? cText : cMuted;
        if (g_page) { Pen p(ic, 2); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound); p.SetLineJoin(LineJoinRound);
            PointF a[3] = { PointF(w / 2 + 3, h / 2 - 7), PointF(w / 2 - 4, h / 2), PointF(w / 2 + 3, h / 2 + 7) }; g.DrawLines(&p, a, 3); }
        else {
            GraphicsPath gp(FillModeAlternate); PointF pts[32]; float cx = w / 2, cy = h / 2;
            const float da[4] = { -0.30f, -0.17f, 0.17f, 0.30f }, rr[4] = { 7.5f, 10.5f, 10.5f, 7.5f };
            for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) { float a = i * 0.785398f + da[k]; pts[i * 4 + k] = PointF(cx + cosf(a) * rr[k], cy + sinf(a) * rr[k]); }
            gp.AddPolygon(pts, 32); gp.AddEllipse(cx - 3.5f, cy - 3.5f, 7.f, 7.f);
            SolidBrush gb(ic); g.FillPath(&gb, &gp);
            if (g_up == U_AVAIL) { SolidBrush dot(cAccent); g.FillEllipse(&dot, w - 12, 4.f, 8.f, 8.f); }
        }
    } else if (id == ID_AUTO || id == ID_STARTUP || id == ID_BG || id == ID_AUTOUP) { // toggle switch + label
        bool on = On(id);
        Round(g, on ? Mix(cAccent, shade) : Mix(Color(66, 71, 84), shade), 0, (h - 20) / 2, 38, 20, 10);
        SolidBrush k(Color(255, 255, 255)); g.FillEllipse(&k, on ? 20.f : 2.f, (h - 16) / 2, 16.f, 16.f);
        Txt(g, Label(b), fBtn, cText, 48, 0, w - 48, h);
    } else if (id >= ID_PRE && id < ID_PRE + 6) { // fps preset pill
        bool sel = kPre[id - ID_PRE] == CurFps();
        Round(g, sel ? Mix(cAccent, shade) : Mix(cInput, shade), 0, 0, w, h, h / 2);
        Txt(g, Label(b), fBtn, sel ? Color(255, 255, 255) : cText, 0, 0, w, h, StringAlignmentCenter);
    } else if (id == ID_INJECT || id == ID_USE || (id == ID_UPCHK && g_up == U_AVAIL)) {
        Round(g, Mix(cAccent, shade), 0, 0, w, h, 8);
        Txt(g, Label(b), fBtn, Color(255, 255, 255), 0, 0, w, h, StringAlignmentCenter);
    } else { // secondary
        Round(g, Mix(cInput, shade), 0, 0, w, h, 8, &cBorder);
        Txt(g, Label(b), fBtn, cText, 0, 0, w, h, StringAlignmentCenter);
    }
    g.Restore(st);
}

// Apps that load dxgi.dll but are not games: hidden from the picker (anything under the Windows folder is hidden too).
static const wchar_t* kNoise[] = { L"claude.exe", L"discord.exe", L"chrome.exe", L"msedge.exe", L"firefox.exe", L"waterfox.exe", L"brave.exe", L"opera.exe",
    L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"galaxyclient.exe", L"battle.net.exe", L"code.exe", L"spotify.exe", L"obs64.exe",
    L"slack.exe", L"teams.exe", L"ms-teams.exe", L"notepad.exe", L"wallpaper64.exe", L"powershell.exe", L"windowsterminal.exe", L"glide-ui.exe" };
static bool IsNoise(DWORD pid, const wchar_t* name) {
    for (auto n : kNoise) if (!_wcsicmp(n, name)) return true;
    wchar_t path[MAX_PATH] = L"", win[MAX_PATH]; DWORD sz = MAX_PATH;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { QueryFullProcessImageNameW(p, 0, path, &sz); CloseHandle(p); }
    GetWindowsDirectoryW(win, MAX_PATH);
    return !_wcsnicmp(path, win, wcslen(win));
}

// Process picker: custom popup (layered, anti-aliased) listing graphics apps; blocked games are shown dimmed and cannot be picked.
struct PItem { std::wstring name, title; bool blocked, sel; };
static struct { HWND h; std::vector<PItem> it; int hover, top, pick; bool done; int w, row; bool proc; } g_pk; // proc: process list (vs. language list)
static const int PK_M = 14, PK_PAD = 6, PK_MAX = 6;

static int PkRows() { int n = (int)g_pk.it.size(); return n < 1 ? 1 : n > PK_MAX ? PK_MAX : n; }

static void PkPaint() {
    int rows = PkRows(); float H = (float)(PK_PAD * 2 + rows * g_pk.row), W = (float)g_pk.w, M = (float)PK_M;
    int pw = S(g_pk.w + 2 * PK_M), ph = S((int)H + 2 * PK_M);
    Bitmap bmp(pw, ph, PixelFormat32bppPARGB);
    {
        Graphics g(&bmp); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit); g.ScaleTransform(g_k, g_k);
        for (int i = 0; i < 7; i++) { float e = (float)(7 - i); Round(g, Color(9, 0, 0, 0), M - e, M - e + 4, W + 2 * e, H + 2 * e, 12 + e); } // soft shadow
        Round(g, Color(31, 34, 42), M, M, W, H, 12, &cBorder);
        if (g_pk.it.empty()) {
            Txt(g, T(S_NOGAMES), fBtn, cText, M + 16, M + PK_PAD + 5, W - 32);
            Txt(g, T(S_NOGAMES2), fSmall, cMuted, M + 16, M + PK_PAD + 24, W - 32);
        }
        for (int r = 0; r < rows && g_pk.top + r < (int)g_pk.it.size(); r++) {
            int i = g_pk.top + r; const PItem& it = g_pk.it[i];
            float x = M + PK_PAD, y = M + PK_PAD + r * g_pk.row, w = W - 2 * PK_PAD, rh = (float)g_pk.row - 2;
            if (i == g_pk.hover && !it.blocked) Round(g, cInput, x, y, w, rh, 8);
            if (it.title.empty()) Txt(g, it.name.c_str(), fBtn, cText, x + 12, y, w - 44, rh);
            else {
                Txt(g, it.name.c_str(), fBtn, it.blocked ? cMuted : cText, x + 12, y + 6, w - (it.blocked ? 110.f : 44.f));
                Txt(g, it.title.c_str(), fSmall, Color(110, 116, 132), x + 12, y + 24, w - 44);
            }
            if (it.blocked) {
                Round(g, Color(50, 240, 90, 80), x + w - 96, y + 8, 84, 20, 10);
                Txt(g, T(S_BLOCKED), fLabel, cBad, x + w - 96, y + 8, 84, 20, StringAlignmentCenter);
            } else if (it.sel) {
                SolidBrush d(cAccent); g.FillEllipse(&d, x + w - 24, y + rh / 2 - 4, 8.f, 8.f);
            }
        }
        if ((int)g_pk.it.size() > PK_MAX) { // scrollbar thumb
            float th = (H - 16) * PK_MAX / g_pk.it.size(), ty = M + 8 + (H - 16 - th) * g_pk.top / (g_pk.it.size() - PK_MAX);
            Round(g, Color(70, 255, 255, 255), M + W - 7, ty, 3, th, 1.5f);
        }
    }
    HBITMAP hb; bmp.GetHBITMAP(Color(0, 0, 0, 0), &hb);
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc); HGDIOBJ old = SelectObject(mdc, hb);
    SIZE sz = { pw, ph }; POINT src = { 0, 0 }; BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_pk.h, sdc, nullptr, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mdc, old); DeleteObject(hb); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
}

static int PkHit(LPARAM lp) {
    float x = (short)LOWORD(lp) / g_k - PK_M, y = (short)HIWORD(lp) / g_k - PK_M;
    if (x < PK_PAD || x > g_pk.w - PK_PAD || y < PK_PAD) return -1;
    int r = (int)(y - PK_PAD) / g_pk.row, i = g_pk.top + r;
    return r < PkRows() && i < (int)g_pk.it.size() ? i : -1;
}

static void PkMove(int dir) { // keyboard: next selectable row in direction
    int n = (int)g_pk.it.size();
    for (int i = g_pk.hover + dir; i >= 0 && i < n; i += dir)
        if (!g_pk.it[i].blocked) {
            g_pk.hover = i;
            if (i < g_pk.top) g_pk.top = i; else if (i >= g_pk.top + PK_MAX) g_pk.top = i - PK_MAX + 1;
            break;
        }
    PkPaint();
}

static LRESULT CALLBACK PkProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEMOVE: {
        int i = PkHit(lp);
        if (i != g_pk.hover) { g_pk.hover = i; PkPaint(); TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); }
        return 0;
    }
    case WM_MOUSELEAVE: g_pk.hover = -1; PkPaint(); return 0;
    case WM_SETCURSOR:
        if (g_pk.hover >= 0 && !g_pk.it[g_pk.hover].blocked) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONUP: { int i = PkHit(lp); if (i >= 0 && !g_pk.it[i].blocked) { g_pk.pick = i; g_pk.done = true; } return 0; }
    case WM_MOUSEWHEEL: {
        int max = (int)g_pk.it.size() - PK_MAX; if (max < 0) max = 0;
        g_pk.top -= GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        g_pk.top = g_pk.top < 0 ? 0 : g_pk.top > max ? max : g_pk.top; PkPaint(); return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) g_pk.done = true;
        else if (wp == VK_DOWN) PkMove(1);
        else if (wp == VK_UP) PkMove(-1);
        else if (wp == VK_RETURN && g_pk.hover >= 0 && !g_pk.it[g_pk.hover].blocked) { g_pk.pick = g_pk.hover; g_pk.done = true; }
        return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) g_pk.done = true; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// Shows g_pk.it under (or above) button `btn`; returns the picked index or -1.
static int ShowPick(int btn, int w, int row, bool proc, bool up) {
    g_pk.w = w; g_pk.row = row; g_pk.proc = proc;
    g_pk.hover = -1; g_pk.top = 0; g_pk.pick = -1; g_pk.done = false;
    static bool reg;
    if (!reg) { WNDCLASSW wc = { 0, PkProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, 0, L"GlidePick" }; RegisterClassW(&wc); reg = true; }
    int m = S(PK_M), pw = S(w + 2 * PK_M), inner = S(PK_PAD * 2 + PkRows() * row), ph = inner + 2 * m;
    POINT pt = { 0, 0 };
    for (auto& b : g_btns) if (b.id == btn) { RECT r = BtnRect(b); pt = { r.left, up ? r.top - S(4) - inner : r.bottom + S(4) }; }
    ClientToScreen(g_wnd, &pt);
    g_pk.h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"GlidePick", L"", WS_POPUP, pt.x - m, pt.y - m, pw, ph, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    PkPaint(); ShowWindow(g_pk.h, SW_SHOW); SetForegroundWindow(g_pk.h);
    MSG msg;
    while (!g_pk.done && GetMessageW(&msg, 0, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DestroyWindow(g_pk.h);
    return g_pk.pick;
}

static void PickProc() {
    std::vector<PItem> v; std::set<std::wstring> seen;
    auto wins = WindowPids();
    wchar_t cur[64]; GetWindowTextW(hProc, cur, 64);
    for (auto& p : Procs()) {
        if (p.first == GetCurrentProcessId() || !wins.count(p.first) || seen.count(p.second) || IsNoise(p.first, p.second.c_str())) continue;
        Mods md = ScanModules(p.first);
        if (!md.dxgi && !md.gl && !md.vk) continue;
        seen.insert(p.second); v.push_back({ p.second, wins[p.first], Blocked(p.second.c_str()) || md.ac, !_wcsicmp(p.second.c_str(), cur) });
    }
    std::sort(v.begin(), v.end(), [](const PItem& a, const PItem& b) { return a.blocked != b.blocked ? !a.blocked : _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    g_pk.it = v;
    int i = ShowPick(ID_PICK, 330, 46, true, false);
    if (i >= 0) { SetWindowTextW(hProc, v[i].name.c_str()); LoadProfile(); }
}

// Calibration: the DLL runs the game uncapped for ~10 s (see glide.cpp) and publishes a suggestion in the shared stats.
static void CalibTick() {
    if (!g_calWait || !g_st) return;
    LONG s = g_st->calState;
    if (s == 1) {
        g_calSeen = true;
        g_c1 = { S_CAL_MEAS, (int)g_st->calLeft }; g_c2 = { S_CAL_PLAY, 0 };
    } else if (g_calSeen && (s == 2 || s == 3)) {
        g_calWait = false;
        if (s == 3) { g_c1 = { S_CAL_FAIL, 0 }; g_c2 = { S_CAL_FEW, 0 }; return; }
        DEVMODEW dm = {}; dm.dmSize = sizeof(dm); EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
        int hz = dm.dmDisplayFrequency, sug = g_st->suggest; bool mon = hz > 30 && sug > hz - 3;
        g_sugg = mon ? hz - 3 : sug;
        g_c1 = { S_CAL_SUG, g_sugg };
        g_c2 = mon ? Msg{ S_CAL_MON, hz } : Msg{ S_CAL_HOLD, (int)g_st->capable };
    } else if (!g_calSeen && GetTickCount() - g_calT0 > 5000) {
        g_calWait = false; g_c1 = { S_CAL_NOSTART, 0 }; g_c2 = { S_CAL_ISINJ, 0 };
    }
}

// ---------- settings: language, start with Windows, tray ----------
static const wchar_t* kRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool StartupGet() { return RegGetValueW(HKEY_CURRENT_USER, kRun, L"Glide", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS; }
static void StartupSet(bool on) {
    if (!on) { RegDeleteKeyValueW(HKEY_CURRENT_USER, kRun, L"Glide"); return; }
    wchar_t exe[MAX_PATH], v[MAX_PATH + 16]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    swprintf_s(v, L"\"%s\" /tray", exe);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRun, L"Glide", REG_SZ, v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

static void Tray(DWORD op) {
    NOTIFYICONDATAW n = { sizeof(n) }; n.hWnd = g_wnd; n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; n.uCallbackMessage = WM_TRAY; n.hIcon = g_icon; wcscpy_s(n.szTip, L"Glide");
    Shell_NotifyIconW(op, &n);
}
static void ShowMain() { ShowWindow(g_wnd, SW_SHOW); ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); }

static void MakeFonts() { // CJK needs its own family; title/numbers are Latin-only so they stay Segoe UI
    const wchar_t* fam = g_lang == 3 ? L"Yu Gothic UI" : g_lang == 4 ? L"Microsoft YaHei UI" : g_lang == 5 ? L"Malgun Gothic" : L"Segoe UI";
    delete fSub; delete fLabel; delete fSmall; delete fBtn;
    fSub = new Font(fam, 13, FontStyleRegular, UnitPixel); fLabel = new Font(fam, 11, FontStyleBold, UnitPixel);
    fSmall = new Font(fam, 12, FontStyleRegular, UnitPixel); fBtn = new Font(fam, 13, FontStyleBold, UnitPixel);
}
static void SetLang(int l) {
    g_lang = l; MakeFonts();
    wchar_t t[8]; swprintf_s(t, L"%d", l); WritePrivateProfileStringW(L"glide", L"lang", t, g_ini);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// ---------- updates: GitHub releases API over HTTPS; the installer is only run if its SHA-256 matches the release's digest ----------
static const wchar_t* kApi = L"https://api.github.com/repos/VuloB1/glide/releases/latest";
static const wchar_t* kRelPage = L"https://github.com/VuloB1/glide/releases/latest";

static bool Fetch(const std::wstring& url, std::string* mem, HANDLE file) {
    wchar_t host[256], path[2048];
    URL_COMPONENTSW uc = { sizeof(uc) }; uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    HINTERNET ses = WinHttpOpen(L"Glide/" GLIDE_VER_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = false;
    if (req && WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
        if (code == 200) {
            ok = true; static thread_local char buf[16384]; DWORD n;
            while (WinHttpReadData(req, buf, sizeof(buf), &n) && n) {
                if (mem) { mem->append(buf, n); if (mem->size() > (1u << 20)) { ok = false; break; } }
                DWORD w; if (file && (!WriteFile(file, buf, n, &w, nullptr) || w != n)) { ok = false; break; }
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

// First string value of "key" at or after `from` (flat scan; enough for the few fields we need). "" if absent or not a string.
static std::string JStr(const std::string& j, const char* key, size_t from) {
    size_t p = j.find(key, from); if (p == std::string::npos) return "";
    p = j.find(':', p); if (p == std::string::npos) return "";
    for (p++; p < j.size() && j[p] == ' '; p++) {}
    if (p >= j.size() || j[p] != '"') return "";
    size_t e = j.find('"', p + 1); return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
}
static std::wstring Widen(const std::string& a) { return std::wstring(a.begin(), a.end()); } // ASCII only (validated by callers)

static std::string Sha256Hex(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    HCRYPTPROV pv = 0; HCRYPTHASH h = 0; std::string out;
    if (CryptAcquireContextW(&pv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(pv, CALG_SHA_256, 0, 0, &h)) {
        static BYTE buf[65536]; DWORD n;
        while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) CryptHashData(h, buf, n, 0);
        BYTE d[32]; DWORD dl = 32; char x[3];
        if (CryptGetHashParam(h, HP_HASHVAL, d, &dl, 0)) for (int i = 0; i < 32; i++) { sprintf_s(x, "%02x", d[i]); out += x; }
    }
    if (h) CryptDestroyHash(h);
    if (pv) CryptReleaseContext(pv, 0);
    CloseHandle(f);
    return out;
}

static bool IsHex64(const std::string& s) { if (s.size() != 64) return false; for (char c : s) if (!isxdigit((unsigned char)c)) return false; return true; }

static DWORD WINAPI CheckThread(LPVOID) {
    std::string j; LONG st = U_ERR;
    if (Fetch(kApi, &j, nullptr)) {
        std::string tag = JStr(j, "\"tag_name\"", 0);
        bool ok = !tag.empty() && tag.size() < 24;
        for (char c : tag) if (!isdigit((unsigned char)c) && c != '.' && c != 'v') ok = false;
        if (ok) {
            int a = 0, b = 0, c = 0; std::wstring wt = Widen(tag);
            swscanf_s(wt.c_str() + (wt[0] == L'v'), L"%d.%d.%d", &a, &b, &c);
            bool newer = a != GLIDE_MAJ ? a > GLIDE_MAJ : b != GLIDE_MIN ? b > GLIDE_MIN : c > GLIDE_PAT;
            std::string url, sha; size_t np = j.find("\"name\":\"glide-setup.exe\"");
            if (np != std::string::npos) { url = JStr(j, "\"browser_download_url\"", np); sha = JStr(j, "\"digest\"", np); }
            if (sha.compare(0, 7, "sha256:") == 0) sha = sha.substr(7); else sha.clear();
            bool good = IsHex64(sha) && url.compare(0, 50, "https://github.com/VuloB1/glide/releases/download/") == 0 && url.find(' ') == std::string::npos;
            g_upTag = wt.substr(wt[0] == L'v'); g_upUrl = good ? Widen(url) : L""; g_upSha = good ? sha : "";
            st = newer ? U_AVAIL : U_LATEST;
        }
    }
    InterlockedExchange(&g_up, st); PostMessageW(g_wnd, WM_UPDATE, 0, 0);
    return 0;
}
static void StartCheck() {
    InterlockedExchange(&g_up, U_CHECKING);
    if (HANDLE t = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr)) CloseHandle(t); else InterlockedExchange(&g_up, U_ERR);
}

static DWORD WINAPI UpdateThread(LPVOID) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); std::wstring path = std::wstring(tmp) + L"glide-setup-update.exe";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE && Fetch(g_upUrl, nullptr, f);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    ok = ok && _stricmp(Sha256Hex(path.c_str()).c_str(), g_upSha.c_str()) == 0;
    if (ok) {
        InterlockedExchange(&g_up, U_INST); PostMessageW(g_wnd, WM_UPDATE, 0, 0);
#ifndef GLIDE_NOEXEC
        // The installer closes this process, replaces the files and starts the new Glide (/open).
        ok = (INT_PTR)ShellExecuteW(nullptr, L"open", path.c_str(), L"/S /open", nullptr, SW_HIDE) > 32;
#endif
    }
    if (!ok) { DeleteFileW(path.c_str()); InterlockedExchange(&g_up, U_ERR2); PostMessageW(g_wnd, WM_UPDATE, 0, 0); }
    return 0;
}

static void UpdateClick() {
    if (g_up == U_AVAIL) {
        if (g_upSha.empty()) { ShellExecuteW(nullptr, L"open", kRelPage, nullptr, nullptr, SW_SHOWNORMAL); return; } // no verifiable asset: let the user download it
        InterlockedExchange(&g_up, U_DL);
        if (HANDLE t = CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr)) CloseHandle(t); else InterlockedExchange(&g_up, U_ERR2);
    } else if (g_up == U_IDLE || g_up == U_LATEST || g_up == U_ERR || g_up == U_ERR2) StartCheck();
}

static void Command(int id) {
    if (id == ID_PICK) PickProc();
    else if (id == ID_GEAR) { g_page ^= 1; g_hot = 0; ShowWindow(hFps, g_page ? SW_HIDE : SW_SHOW); InvalidateRect(g_wnd, nullptr, FALSE); }
    else if (id == ID_UPCHK) { UpdateClick(); InvalidateRect(g_wnd, nullptr, FALSE); }
    else if (id == ID_AUTOUP) { g_autoUp = !g_autoUp; WritePrivateProfileStringW(L"glide", L"autoupdate", g_autoUp ? L"1" : L"0", g_ini); InvBtn(id); }
    else if (id == ID_GITHUB) ShellExecuteW(nullptr, L"open", L"https://github.com/VuloB1/glide", nullptr, nullptr, SW_SHOWNORMAL);
    else if (id == ID_ISSUES) ShellExecuteW(nullptr, L"open", L"https://github.com/VuloB1/glide/issues", nullptr, nullptr, SW_SHOWNORMAL);
    else if (id == ID_LANG) {
        g_pk.it.clear();
        for (int i = 0; i < NLANG; i++) g_pk.it.push_back({ kLangName[i], L"", false, i == g_lang });
        int i = ShowPick(ID_LANG, 200, 34, false, false);
        if (i >= 0 && i != g_lang) SetLang(i);
    }
    else if (id == ID_STARTUP) { g_startup = !g_startup; StartupSet(g_startup); InvBtn(id); }
    else if (id == ID_BG) {
        g_bg = !g_bg; WritePrivateProfileStringW(L"glide", L"background", g_bg ? L"1" : L"0", g_ini);
        Tray(g_bg ? NIM_ADD : NIM_DELETE); InvBtn(id);
    }
    else if (id == ID_CALIB && !g_calWait) {
        if (!g_st || GetTickCount() - g_lastChange > 1500) { g_c1 = { S_CAL_NOTHING, 0 }; g_c2 = { S_CAL_INJFIRST, 0 }; }
        else {
            wchar_t n[64]; GetWindowTextW(hProc, n, 64); WritePrivateProfileStringW(n, L"calibrate", L"1", g_ini);
            g_calWait = true; g_calSeen = false; g_calT0 = GetTickCount(); g_sugg = 0; g_c1 = { S_CAL_STARTING, 0 }; g_c2 = { S_CAL_REMOVED, 0 };
        }
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
    else if (id == ID_USE && g_sugg) {
        wchar_t t[16]; swprintf_s(t, L"%d", g_sugg); SetWindowTextW(hFps, t); SaveProfile();
        g_c1 = { S_CAL_APPLIED, 0 }; g_c2 = { S_CAL_SAVED, 0 }; g_sugg = 0; InvalidateRect(g_wnd, nullptr, FALSE);
    }
    else if (id == ID_APPLY) SaveProfile();
    else if (id == ID_INJECT) { SaveProfile(); Inject(); }
    else if (id == ID_AUTO) { g_auto = !g_auto; InvBtn(ID_AUTO); SaveProfile(); }
    else if (id >= ID_PRE && id < ID_PRE + 6) { wchar_t t[16]; swprintf_s(t, L"%d", kPre[id - ID_PRE]); SetWindowTextW(hFps, t); SaveProfile(); }
}

static void OpenStats() {
    if (g_st) { UnmapViewOfFile(g_st); g_st = nullptr; }
    if (g_map) { CloseHandle(g_map); g_map = nullptr; }
    g_map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\glide_stats");
    if (g_map) g_st = (Stats*)MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, sizeof(Stats));
}

static void Card(Graphics& g, float x, float y, float w, float h, const wchar_t* title) {
    Round(g, cCard, x, y, w, h, 14, &cBorder);
    Txt(g, title, fLabel, cMuted, x + 16, y + 14);
}

static void Paint(HDC dc, const RECT& clip) {
    RECT rc; GetClientRect(g_wnd, &rc);
    HDC m = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); SelectObject(m, bm);
    {
        Graphics g(m); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(cBg); g.ScaleTransform(g_k, g_k);

        // live data
        bool live = g_st && GetTickCount() - g_lastChange < 1500 && g_st->head > 1;
        float v[240]; int n = 0; double mean = 0, var = 0, mx = 0;
        if (live) {
            LONG head = g_st->head; n = head < 240 ? head : 240;
            for (int i = 0; i < n; i++) { v[i] = g_st->ms[(head - n + i) % 240]; mean += v[i]; if (v[i] > mx) mx = v[i]; }
            mean /= n;
            for (int i = 0; i < n; i++) var += (v[i] - mean) * (v[i] - mean);
        }

        // header
        DrawLogo(g, 24, 14, 48);
        Txt(g, L"Glide", fTitle, cText, 82, 12);
        Txt(g, T(S_SUB), fSub, cMuted, 84, 48);
        Round(g, cCard, (float)kPill.left, (float)kPill.top, 176, 32, 16, &cBorder);
        { SolidBrush dot(live ? cGood : cMuted); g.FillEllipse(&dot, kPill.left + 16.f, kPill.top + 12.f, 8.f, 8.f); }
        Txt(g, live ? T(S_LIMITING) : T(S_WAITING), fSub, live ? cGood : cMuted, kPill.left + 32.f, (float)kPill.top, 140, 32);

        if (g_page == 0) {
        // cards
        Card(g, 24, 84, 512, 128, T(S_GAME));
        Card(g, 24, 228, 512, 140, T(S_FPSLIMIT));
        wchar_t c1[96], c2[96]; swprintf_s(c1, T(g_c1.id), g_c1.arg); swprintf_s(c2, T(g_c2.id), g_c2.arg);
        Txt(g, c1, fBtn, cText, 204, 318, 216, 16);
        Txt(g, c2, fSmall, cMuted, 204, 336, 216, 16);
        Round(g, cInput, 40, 268, 70, 38, 8, &cBorder);
        Round(g, cInput, 40, 122, 330, 32, 8, &cBorder);
        { wchar_t pn[64]; GetWindowTextW(hProc, pn, 64); Txt(g, *pn ? pn : T(S_CHOOSE), fSub, *pn ? cText : cMuted, 52, 122, 270, 32); }
        Card(g, 24, (float)kStatsCard.top, 512, 188, T(S_PERF));

        for (auto& b : g_btns) if (Vis(b) && (b.id != ID_USE || g_sugg)) DrawBtn(g, b);

        // metrics
        wchar_t s[32]; const wchar_t* lab[4] = { L"FPS", T(S_MAVG), T(S_MDEV), T(S_MMAX) };
        double val[4] = { live ? 1000 / mean : 0, mean, sqrt(n ? var / n : 0), mx };
        for (int i = 0; i < 4; i++) {
            float x = 40.f + i * 126;
            if (live) swprintf_s(s, i ? L"%.2f" : L"%.0f", val[i]); else wcscpy_s(s, L"--");
            Txt(g, s, fBig, i == 0 && live ? cAccent : cText, x, 412);
            Txt(g, lab[i], fSmall, cMuted, x, 444);
        }

        // frametime graph
        float gx = 40, gy = 466, gw = 480, gh = 92;
        Round(g, Color(20, 22, 27), gx, gy, gw, gh, 10);
        if (live) {
            double top = mean * 2;
            PointF pts[240];
            for (int i = 0; i < n; i++) {
                float y = (float)(v[i] / top); if (y > 1) y = 1;
                pts[i] = PointF(gx + 6 + (240 - n + i) * (gw - 12) / 239, gy + gh - 6 - y * (gh - 12));
            }
            GraphicsPath area; area.AddLines(pts, n);
            area.AddLine(pts[n - 1].X, gy + gh - 6, pts[0].X, gy + gh - 6); area.CloseFigure();
            LinearGradientBrush grad(PointF(0, gy), PointF(0, gy + gh), Color(80, 91, 140, 255), Color(0, 91, 140, 255));
            g.FillPath(&grad, &area);
            Pen mid(Color(70, 255, 255, 255), 1); mid.SetDashStyle(DashStyleDash);
            g.DrawLine(&mid, gx + 6, gy + gh / 2, gx + gw - 6, gy + gh / 2);
            Pen line(cAccent, 1.8f); line.SetLineJoin(LineJoinRound); g.DrawLines(&line, pts, n);
            SolidBrush bad(cBad);
            for (int i = 0; i < n; i++) if (fabs(v[i] - mean) > mean * 0.15) g.FillEllipse(&bad, pts[i].X - 2.5f, pts[i].Y - 2.5f, 5.f, 5.f);
        } else {
            Txt(g, T(S_GRAPH), fSub, cMuted, gx, gy, gw, gh, StringAlignmentCenter);
        }

        // status
        if (g_status.empty() && g_up == U_AVAIL) { wchar_t ub[128]; swprintf_s(ub, T(S_UP_BANNER), g_upTag.c_str()); Txt(g, ub, fSmall, cAccent, 26, 582, 508); }
        else Txt(g, g_status.empty() ? T(S_FOOT) : g_status.c_str(), fSmall, cMuted, 26, 582, 508);
        } else { // settings page
            Card(g, 24, 84, 512, 160, T(S_SETTINGS));
            Txt(g, T(S_LANGUAGE), fSub, cMuted, 40, 122, 120, 32);
            Round(g, cInput, 170, 122, 200, 32, 8, &cBorder);
            Txt(g, kLangName[g_lang], fSub, cText, 182, 122, 150, 32);
            Card(g, 24, 260, 512, 140, T(S_UPDATES));
            wchar_t b1[128]; swprintf_s(b1, T(S_VERSION), GLIDE_VER_W);
            Txt(g, b1, fBtn, cText, 40, 298, 320, 18);
            const wchar_t* us = L""; Color uc = cMuted;
            switch (g_up) {
            case U_CHECKING: us = T(S_UP_CHECKING); break;
            case U_LATEST: us = T(S_UP_LATEST); uc = cGood; break;
            case U_AVAIL: swprintf_s(b1, T(S_UP_AVAIL), g_upTag.c_str()); us = b1; uc = cAccent; break;
            case U_DL: us = T(S_UP_DL); break;
            case U_INST: us = T(S_UP_INST); break;
            case U_ERR: us = T(S_UP_ERR); uc = cBad; break;
            case U_ERR2: us = T(S_UP_ERR2); uc = cBad; break;
            }
            Txt(g, us, fSmall, uc, 40, 320, 320, 18);
            Card(g, 24, 416, 512, 156, T(S_ABOUT));
            Txt(g, L"Glide " GLIDE_VER_W, fBtn, cText, 40, 450, 470, 18);
            Txt(g, T(S_ABOUT_DESC), fSmall, cMuted, 40, 470, 470, 18);
            swprintf_s(b1, T(S_CREATED), L"VuloB1"); Txt(g, b1, fSmall, cText, 40, 490, 470, 18);
            Txt(g, T(S_LICENSE), fSmall, cMuted, 40, 508, 470, 18);
            for (auto& b : g_btns) if (Vis(b)) DrawBtn(g, b);
        }
    }
    BitBlt(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, m, clip.left, clip.top, SRCCOPY);
    DeleteObject(bm); DeleteDC(m);
}

static LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), ev = HIWORD(wp);
        if (id == ID_PROC && ev == EN_CHANGE) LoadProfile();
        else if (id == ID_FPS && ev == EN_CHANGE) InvPills();
        return 0;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, RGB(230, 232, 238)); SetBkColor((HDC)wp, RGB(38, 42, 51)); return (LRESULT)g_brIn;
    case WM_MOUSEMOVE: {
        int h = HitBtn(lp);
        if (h != g_hot) { int o = g_hot; g_hot = h; InvBtn(o); InvBtn(h); TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, w, 0 }; TrackMouseEvent(&t); }
        return 0;
    }
    case WM_MOUSELEAVE: { int o = g_hot; g_hot = 0; InvBtn(o); return 0; }
    case WM_SETCURSOR:
        if (g_hot && LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: g_down = HitBtn(lp); SetCapture(w); InvBtn(g_down); return 0;
    case WM_LBUTTONUP: { int d = g_down; g_down = 0; ReleaseCapture(); InvBtn(d); if (d && d == HitBtn(lp)) Command(d); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        static int tick;
        if (++tick % 40 == 0) AutoScan();
        if (g_autoUp && GetTickCount64() >= g_nextCheck && (g_up == U_IDLE || g_up == U_LATEST || g_up == U_ERR)) { g_nextCheck = GetTickCount64() + 86400000ULL; StartCheck(); }
        CalibTick();
        if (!g_st) OpenStats();
        if (g_st && g_st->head != g_lastHead) { g_lastHead = g_st->head; g_lastChange = GetTickCount(); }
        else if (g_st && GetTickCount() - g_lastChange > 3000) OpenStats(); // game restarted: re-attach
        RECT a = { S(kStatsCard.left), S(kStatsCard.top), S(kStatsCard.right), S(kStatsCard.bottom) }, b = { S(kPill.left), S(kPill.top), S(kPill.right), S(kPill.bottom) };
        RECT c = { S(24), S(316), S(536), S(372) };
        InvalidateRect(w, &a, FALSE); InvalidateRect(w, &b, FALSE); InvalidateRect(w, &c, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps); Paint(dc, ps.rcPaint); EndPaint(w, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (g_bg && !g_quit) { ShowWindow(w, SW_HIDE); return 0; }
        break;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) ShowMain();
        else if (lp == WM_RBUTTONUP) {
            HMENU mn = CreatePopupMenu(); POINT pt; GetCursorPos(&pt);
            AppendMenuW(mn, MF_STRING, 1, T(S_TRAY_OPEN)); AppendMenuW(mn, MF_STRING, 2, T(S_TRAY_EXIT));
            SetForegroundWindow(w);
            int c = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, w, nullptr);
            DestroyMenu(mn);
            if (c == 1) ShowMain(); else if (c == 2) { g_quit = true; DestroyWindow(w); }
        }
        return 0;
    case WM_SHOWME: ShowMain(); return 0;
    case WM_UPDATE: InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_DESTROY: Tray(NIM_DELETE); PostQuitMessage(0); return 0;
    default:
        if (msg == g_tbCreated && g_bg) Tray(NIM_ADD); // explorer restarted
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    HANDLE once = CreateMutexW(nullptr, FALSE, L"Local\\GlideUI");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { // already running (maybe hidden in the tray): bring it up
        if (HWND o = FindWindowW(L"GlideUI", nullptr)) PostMessageW(o, WM_SHOWME, 0, 0);
        return 0;
    }
    SetProcessDPIAware();
    HDC sdc = GetDC(nullptr); g_k = GetDeviceCaps(sdc, LOGPIXELSX) / 96.f; ReleaseDC(nullptr, sdc);
    ULONG_PTR gdip; GdiplusStartupInput gin; GdiplusStartup(&gdip, &gin, nullptr);

    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    *wcsrchr(g_dir, L'\\') = 0;
    swprintf_s(g_ini, L"%s\\glide.ini", g_dir);

    WORD plang = PRIMARYLANGID(GetUserDefaultUILanguage()); // first run: follow Windows' language, English if unsupported
    int def = plang == LANG_SPANISH ? 1 : plang == LANG_PORTUGUESE ? 2 : plang == LANG_JAPANESE ? 3 : plang == LANG_CHINESE ? 4 : plang == LANG_KOREAN ? 5 : 0;
    g_lang = GetPrivateProfileIntW(L"glide", L"lang", def, g_ini);
    if (g_lang < 0 || g_lang >= NLANG) g_lang = def;
    g_bg = GetPrivateProfileIntW(L"glide", L"background", 0, g_ini) != 0;
    g_startup = StartupGet();
    g_autoUp = GetPrivateProfileIntW(L"glide", L"autoupdate", 1, g_ini) != 0;
    g_nextCheck = GetTickCount64() + 4000;
    fTitle = new Font(L"Segoe UI", 26, FontStyleBold, UnitPixel); fBig = new Font(L"Segoe UI", 24, FontStyleBold, UnitPixel);
    MakeFonts();
    g_brIn = CreateSolidBrush(RGB(38, 42, 51));
    g_brCard = CreateSolidBrush(RGB(27, 30, 37));
    g_icon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_tbCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = { 0, Proc, 0, 0, hi, LoadIconW(hi, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, 0, L"GlideUI" };
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(560), S(608) }; AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(L"GlideUI", L"Glide", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, hi, 0);
    HWND w = g_wnd;
    BOOL dark = TRUE; DwmSetWindowAttribute(w, 20, &dark, sizeof(dark));          // dark title bar
    COLORREF cap = RGB(18, 20, 24); DwmSetWindowAttribute(w, 35, &cap, sizeof(cap)); // Win11 caption color (ignored before)

    HFONT f = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int x, int y, int ww, int hh, int id) {
        HWND c = CreateWindowW(cls, t, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | st, S(x), S(y), S(ww), S(hh), w, (HMENU)(INT_PTR)id, hi, 0);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return c; };

    wchar_t t[64];
    GetPrivateProfileStringW(L"glide", L"process", L"Lethal Company.exe", t, 64, g_ini);
    hProc = mk(L"EDIT", t, ES_AUTOHSCROLL, 52, 130, 276, 18, ID_PROC); // hidden: only stores the chosen process name
    ShowWindow(hProc, SW_HIDE);
    SendMessageW(hProc, EM_LIMITTEXT, 63, 0);
    g_btns = { { ID_PICK, 40, 122, 330, 32, 0, L"", 0 }, { ID_INJECT, 384, 122, 136, 32, S_INJECT, nullptr, 0 }, { ID_AUTO, 40, 170, 330, 26, S_AUTOINJ, nullptr, 0 },
               { ID_APPLY, 120, 268, 80, 38, S_APPLY, nullptr, 0 }, { ID_CALIB, 40, 320, 150, 36, S_CALIB, nullptr, 0 }, { ID_USE, 428, 320, 92, 36, S_USE, nullptr, 0 },
               { ID_GEAR, 496, 20, 40, 36, 0, L"", -1 },
               { ID_LANG, 170, 122, 200, 32, 0, L"", 1 }, { ID_STARTUP, 40, 170, 330, 26, S_STARTUP, nullptr, 1 }, { ID_BG, 40, 204, 330, 26, S_BG, nullptr, 1 },
               { ID_UPCHK, 372, 288, 148, 36, S_UP_CHECK, nullptr, 1 }, { ID_AUTOUP, 40, 350, 330, 26, S_AUTOUP, nullptr, 1 },
               { ID_GITHUB, 40, 532, 120, 32, 0, L"GitHub", 1 }, { ID_ISSUES, 168, 532, 190, 32, S_ISSUES, nullptr, 1 } };

    hFps = mk(L"EDIT", L"", ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL, 50, 277, 50, 20, ID_FPS);
    const wchar_t* pl[6] = { L"60", L"90", L"120", L"144", L"180", L"∞" };
    for (int i = 0; i < 6; i++) g_btns.push_back({ ID_PRE + i, 212.f + i * 50, 268, 46, 38, 0, pl[i], 0 });
    LoadProfile();
    SetTimer(w, ID_TIMER, 50, nullptr);
    if (g_bg) Tray(NIM_ADD);
    if (!(g_bg && wcsstr(cmd, L"/tray"))) ShowWindow(w, SW_SHOW); // autostart with the background option: stay in the tray

    MSG m;
    while (GetMessageW(&m, 0, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    GdiplusShutdown(gdip); CloseHandle(once);
    return 0;
}
// shortcut: 64-bit games only; anti-cheat detection is a name list + module check done once at inject time (an anti-cheat loaded later is missed), upgrade if needed.
