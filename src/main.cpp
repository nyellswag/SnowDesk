// SnowDesk — snow on your Windows desktop.
// Transparent click-through overlay on the primary monitor + tray icon. No installer, no system changes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include "sim.h"

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

static const wchar_t* APP_NAME = L"SnowDesk";
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum {
    IDM_PLAY = 100, IDM_PAUSE, IDM_INT0, IDM_INT1, IDM_INT2, IDM_INT3,
    IDM_DRIFT, IDM_CLEAR, IDM_AUTOSTART, IDM_FOLDER, IDM_UNINSTALL, IDM_EXIT
};
enum { HK_PLAY = 1, HK_ESC = 2 };
#define WM_TRAY (WM_APP + 1)
static const float INTENSITY[4] = {0.4f, 1.0f, 2.0f, 4.0f};

struct App {
    HINSTANCE inst = nullptr;
    HWND overlay = nullptr, ctl = nullptr;
    HANDLE mutex = nullptr;
    std::wstring exe, dir, ini, dat;
    RECT mon{}, work{};
    int W = 0, H = 0, groundY = 0;
    float scale = 1;
    HDC memDC = nullptr;
    HBITMAP dib = nullptr, oldBmp = nullptr;
    uint32_t* bits = nullptr;
    World world;
    bool paused = false, play = false, hiddenFS = false, visible = false, hotkeyOk = false;
    int intensityIdx = 1;
    double lastEnum = 0, lastTop = 0, lastSave = 0, lastInput = 0, lastFrame = 0;
    LARGE_INTEGER freq{};
    NOTIFYICONDATAW nid{};
    UINT wmTaskbarCreated = 0;
    HICON icon = nullptr;
    std::vector<uint32_t> hint;
    int hintW = 0, hintH = 0;
} g;

static double now() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g.freq.QuadPart;
}

// ---------------------------------------------------------------- files

static bool readFile(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    if (sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(f); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(f, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(f);
    return ok && rd == out.size();
}

static bool writeFileAtomic(const std::wstring& path, const std::vector<uint8_t>& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(f);
    if (!ok || wr != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

static void saveSnow() {
    writeFileAtomic(g.dat, g.world.save());
    g.lastSave = now();
}

static int iniInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"Settings", key, def, g.ini.c_str()); }
static void iniSet(const wchar_t* key, int v) {
    wchar_t b[32]; swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(L"Settings", key, b, g.ini.c_str());
}

// ---------------------------------------------------------------- autostart

static bool autostartOn() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(k, APP_NAME, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void setAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + g.exe + L"\"";
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- display

static void getMonitor() {
    HMONITOR m = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    g.mon = mi.rcMonitor; g.work = mi.rcWork;
    g.W = g.mon.right - g.mon.left;
    g.H = g.mon.bottom - g.mon.top;
    g.groundY = std::min(g.H, (int)(g.work.bottom - g.mon.top));
    if (g.groundY < g.H / 2) g.groundY = g.H;
    HDC sdc = GetDC(nullptr);
    g.scale = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, sdc);
}

static void freeDib() {
    if (g.memDC) { SelectObject(g.memDC, g.oldBmp); DeleteDC(g.memDC); g.memDC = nullptr; }
    if (g.dib) { DeleteObject(g.dib); g.dib = nullptr; }
    g.bits = nullptr;
}

static bool makeDib() {
    freeDib();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g.W;
    bi.bmiHeader.biHeight = -g.H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    g.dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&g.bits, nullptr, 0);
    g.memDC = CreateCompatibleDC(sdc);
    ReleaseDC(nullptr, sdc);
    if (!g.dib || !g.memDC) return false;
    g.oldBmp = (HBITMAP)SelectObject(g.memDC, g.dib);
    memset(g.bits, 0, (size_t)g.W * g.H * 4);
    return true;
}

static void present() {
    POINT pos{g.mon.left, g.mon.top}, src{0, 0};
    SIZE sz{g.W, g.H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g.overlay, nullptr, &pos, &sz, g.memDC, &src, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------- play-mode hint

static void buildHint() {
    const wchar_t* txt =
        L"Режим игры:  зажмите левую кнопку на снегу — лепите снежок   •   отпустите с размахом — бросок, "
        L"плавно — положить (3 шара друг на друга = снеговик)   •   правая кнопка или Esc — выход";
    HDC sdc = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(sdc);
    int px = (int)(15 * g.scale);
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT of = (HFONT)SelectObject(dc, f);
    RECT rc{0, 0, g.W - (int)(80 * g.scale), 0};
    UINT fmt = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
    DrawTextW(dc, txt, -1, &rc, fmt | DT_CALCRECT);
    int w = std::max(1, (int)rc.right), h = std::max(1, (int)rc.bottom);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint32_t* p = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&p, nullptr, 0);
    if (bm && p) {
        HBITMAP ob = (HBITMAP)SelectObject(dc, bm);
        memset(p, 0, (size_t)w * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r2{0, 0, w, h};
        DrawTextW(dc, txt, -1, &r2, fmt);
        GdiFlush();
        g.hint.resize((size_t)w * h);
        for (int i = 0; i < w * h; i++) {
            uint32_t c = p[i];
            uint32_t a = std::max({(c >> 16) & 255u, (c >> 8) & 255u, c & 255u});
            g.hint[i] = (a << 24) | (a << 16) | (a << 8) | a;
        }
        g.hintW = w; g.hintH = h;
        SelectObject(dc, ob);
        DeleteObject(bm);
    }
    SelectObject(dc, of);
    DeleteObject(f);
    DeleteDC(dc);
    ReleaseDC(nullptr, sdc);
}

static inline void over(uint32_t& d, uint32_t s) {
    uint32_t sa = s >> 24;
    if (sa == 0) return;
    uint32_t ia = 255 - sa;
    uint32_t a = sa + (((d >> 24) & 255) * ia + 127) / 255;
    uint32_t r = ((s >> 16) & 255) + (((d >> 16) & 255) * ia + 127) / 255;
    uint32_t gg = ((s >> 8) & 255) + (((d >> 8) & 255) * ia + 127) / 255;
    uint32_t b = (s & 255) + ((d & 255) * ia + 127) / 255;
    d = (std::min(a, 255u) << 24) | (std::min(r, 255u) << 16) | (std::min(gg, 255u) << 8) | std::min(b, 255u);
}

static void drawHint() {
    if (g.hint.empty()) return;
    int padX = (int)(16 * g.scale), padY = (int)(9 * g.scale), rad = (int)(10 * g.scale);
    int bw = g.hintW + padX * 2, bh = g.hintH + padY * 2;
    int x0 = (g.W - bw) / 2, y0 = (int)(24 * g.scale);
    const uint32_t a = 200;
    uint32_t box = (a << 24) | ((20 * a / 255) << 16) | ((28 * a / 255) << 8) | (44 * a / 255);
    for (int y = 0; y < bh; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= g.H) continue;
        for (int x = 0; x < bw; x++) {
            int xx = x0 + x;
            if (xx < 0 || xx >= g.W) continue;
            int cx = x < rad ? rad - x : (x >= bw - rad ? x - (bw - rad - 1) : 0);
            int cy = y < rad ? rad - y : (y >= bh - rad ? y - (bh - rad - 1) : 0);
            if (cx * cx + cy * cy > rad * rad) continue;
            over(g.bits[yy * g.W + xx], box);
        }
    }
    for (int y = 0; y < g.hintH; y++) {
        int yy = y0 + padY + y;
        if (yy < 0 || yy >= g.H) continue;
        for (int x = 0; x < g.hintW; x++) {
            int xx = x0 + padX + x;
            if (xx < 0 || xx >= g.W) continue;
            over(g.bits[yy * g.W + xx], g.hint[y * g.hintW + x]);
        }
    }
}

// ---------------------------------------------------------------- windows

static bool isShellClass(const wchar_t* cls) {
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
           !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd");
}

static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    auto* out = (std::vector<WinRect>*)lp;
    if (h == g.overlay || h == g.ctl) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    wchar_t cls[96] = {0};
    GetClassNameW(h, cls, 96);
    if (isShellClass(cls)) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) return TRUE;  // other click-through overlays
    RECT wr, r;
    GetWindowRect(h, &wr);
    // visible frame (without invisible resize borders); fall back to the window rect if DWM gives nonsense
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
        r.left < wr.left || r.top < wr.top || r.right > wr.right || r.bottom > wr.bottom || r.right <= r.left)
        r = wr;
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return TRUE;
    if (r.right <= g.mon.left || r.left >= g.mon.right || r.bottom <= g.mon.top || r.top >= g.mon.bottom) return TRUE;
    bool full = r.left <= g.mon.left && r.top <= g.mon.top && r.right >= g.mon.right && r.bottom >= g.mon.bottom;
    bool pile = !IsZoomed(h) && !full && !(ex & WS_EX_TOOLWINDOW) &&
                (GetWindow(h, GW_OWNER) == nullptr || (ex & WS_EX_APPWINDOW)) &&
                GetWindowTextLengthW(h) > 0 &&
                (r.right - r.left) >= (int)(150 * g.scale) && (r.bottom - r.top) >= (int)(80 * g.scale) &&
                wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0;
    out->push_back(WinRect{(uint64_t)(uintptr_t)h, (int)(r.left - g.mon.left), (int)(r.top - g.mon.top),
                           (int)(r.right - g.mon.left), (int)(r.bottom - g.mon.top), pile});
    return TRUE;
}

static bool foregroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g.overlay || fg == g.ctl || IsZoomed(fg)) return false;
    wchar_t cls[96] = {0};
    GetClassNameW(fg, cls, 96);
    if (isShellClass(cls)) return false;
    RECT r;
    if (!GetWindowRect(fg, &r)) return false;
    return r.left <= g.mon.left && r.top <= g.mon.top && r.right >= g.mon.right && r.bottom >= g.mon.bottom;
}

// ---------------------------------------------------------------- state

static void showOverlay(bool show) {
    if (show == g.visible) return;
    ShowWindow(g.overlay, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    g.visible = show;
}

static void setPlay(bool on) {
    if (on == g.play) return;
    if (on && g.paused) return;
    g.play = on;
    LONG ex = GetWindowLongW(g.overlay, GWL_EXSTYLE);
    if (on) ex &= ~WS_EX_TRANSPARENT; else ex |= WS_EX_TRANSPARENT;
    SetWindowLongW(g.overlay, GWL_EXSTYLE, ex);
    if (on) {
        RegisterHotKey(g.ctl, HK_ESC, MOD_NOREPEAT, VK_ESCAPE);
        g.lastInput = now();
        buildHint();
        SetTimer(g.ctl, 1, 15, nullptr);
    } else {
        UnregisterHotKey(g.ctl, HK_ESC);
        g.world.cancelHold();
        if (GetCapture() == g.overlay) ReleaseCapture();
        SetTimer(g.ctl, 1, 30, nullptr);
    }
}

static void setPaused(bool p) {
    if (p == g.paused) return;
    if (p) {
        setPlay(false);
        g.paused = true;
        KillTimer(g.ctl, 1);
        showOverlay(false);
        saveSnow();
    } else {
        g.paused = false;
        g.lastFrame = now();
        SetTimer(g.ctl, 1, 30, nullptr);
    }
}

static void reconfigure() {
    RECT oldMon = g.mon; int oldGround = g.groundY;
    getMonitor();
    if (EqualRect(&oldMon, &g.mon) && oldGround == g.groundY) return;
    makeDib();
    g.world.resize(g.W, g.H, g.groundY);
    SetWindowPos(g.overlay, HWND_TOPMOST, g.mon.left, g.mon.top, g.W, g.H, SWP_NOACTIVATE);
    if (g.play) buildHint();
}

static void frame() {
    double t = now();
    float dt = (float)std::min(0.05, std::max(0.0, t - g.lastFrame));
    g.lastFrame = t;

    if (t - g.lastEnum > 0.1) {
        g.lastEnum = t;
        g.hiddenFS = foregroundIsFullscreen();
        if (g.hiddenFS) setPlay(false);
        if (!g.hiddenFS) {
            std::vector<WinRect> ws;
            EnumWindows(enumProc, (LPARAM)&ws);
            g.world.setWindows(ws);
#ifdef SNOWDESK_DEBUG
            static double lastLog = 0;
            if (t - lastLog > 5) {
                lastLog = t;
                FILE* f = _wfopen((g.dir + L"\\debug.log").c_str(), L"a");
                if (f) {
                    fprintf(f, "--- W=%d H=%d ground=%d\n", g.W, g.H, g.groundY);
                    for (auto& w : ws) {
                        wchar_t cls[96] = {0}; GetClassNameW((HWND)(uintptr_t)w.id, cls, 96);
                        fprintf(f, "%ls  %d,%d,%d,%d pile=%d\n", cls, w.l, w.t, w.r, w.b, (int)w.pileable);
                    }
                    fclose(f);
                }
            }
#endif
        }
    }
    if (g.hiddenFS) { showOverlay(false); return; }

    g.world.step(dt);
    if (g.play) {
        const uint32_t a = 26;
        uint32_t tint = (a << 24) | ((150 * a / 255) << 16) | ((175 * a / 255) << 8) | (215 * a / 255);
        std::fill(g.bits, g.bits + (size_t)g.W * g.H, tint);
    } else {
        memset(g.bits, 0, (size_t)g.W * g.H * 4);
    }
    g.world.render(g.bits);
    if (g.play) drawHint();
    present();
    showOverlay(true);

    if (t - g.lastTop > 3) {
        g.lastTop = t;
        SetWindowPos(g.overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (t - g.lastSave > 60) saveSnow();
    if (g.play && t - g.lastInput > 60) setPlay(false);  // safety: never leave the mouse captured
}

// ---------------------------------------------------------------- tray

static void addTray() {
    g.nid = NOTIFYICONDATAW{};
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.ctl;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = WM_TRAY;
    g.nid.hIcon = g.icon;
    wcsncpy(g.nid.szTip, L"SnowDesk — снег на рабочем столе\nЛевый клик: играть  •  правый: меню", 127);
    Shell_NotifyIconW(NIM_ADD, &g.nid);
}

static void balloon(const wchar_t* title, const wchar_t* text) {
    g.nid.uFlags = NIF_INFO;
    wcsncpy(g.nid.szInfoTitle, title, 63);
    wcsncpy(g.nid.szInfo, text, 255);
    g.nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void showMenu() {
    HMENU m = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g.play ? MF_CHECKED : 0) | (g.paused ? MF_GRAYED : 0), IDM_PLAY,
                g.hotkeyOk ? L"Играть со снегом\tCtrl+Alt+S" : L"Играть со снегом");
    AppendMenuW(m, MF_STRING | (g.paused ? MF_CHECKED : 0), IDM_PAUSE, L"Пауза (снег спрятан)");
    AppendMenuW(sub, MF_STRING, IDM_INT0, L"Лёгкий");
    AppendMenuW(sub, MF_STRING, IDM_INT1, L"Обычный");
    AppendMenuW(sub, MF_STRING, IDM_INT2, L"Сильный");
    AppendMenuW(sub, MF_STRING, IDM_INT3, L"Метель");
    CheckMenuRadioItem(sub, IDM_INT0, IDM_INT3, IDM_INT0 + g.intensityIdx, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Сила снегопада");
    AppendMenuW(m, MF_STRING, IDM_DRIFT, L"Насыпать сугроб");
    AppendMenuW(m, MF_STRING, IDM_CLEAR, L"Убрать весь снег");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (autostartOn() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Запускать вместе с Windows");
    AppendMenuW(m, MF_STRING, IDM_FOLDER, L"Открыть папку программы");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_UNINSTALL, L"Удалить SnowDesk полностью…");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Выход");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g.ctl);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.ctl, nullptr);
    PostMessageW(g.ctl, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void shutdown(bool save) {
    KillTimer(g.ctl, 1);
    if (save) saveSnow();
    Shell_NotifyIconW(NIM_DELETE, &g.nid);
    UnregisterHotKey(g.ctl, HK_PLAY);
    UnregisterHotKey(g.ctl, HK_ESC);
    DestroyWindow(g.overlay);
    DestroyWindow(g.ctl);
}

static std::wstring dirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}

static bool endsWithSnowDesk(const std::wstring& d) {
    size_t p = d.find_last_of(L"\\/");
    std::wstring name = p == std::wstring::npos ? d : d.substr(p + 1);
    return _wcsicmp(name.c_str(), L"SnowDesk") == 0;
}

static void uninstall() {
    bool wasPaused = g.paused;
    setPaused(true);
    int r = MessageBoxW(nullptr,
        L"Удалить SnowDesk полностью?\n\n"
        L"Будут удалены: сама программа, настройки, сохранённый снег и автозапуск.\n"
        L"Другие файлы на компьютере не затрагиваются.",
        L"SnowDesk", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST);
    if (r != IDYES) { if (!wasPaused) setPaused(false); return; }

    setAutostart(false);
    DeleteFileW(g.dat.c_str());
    DeleteFileW((g.dat + L".tmp").c_str());
    DeleteFileW(g.ini.c_str());
    // A running .exe cannot delete itself, so a hidden cmd waits ~2 s for us to exit, then removes the file
    // and the folder (only if it is our own, now-empty "SnowDesk" folder).
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + g.exe + L"\"";
    if (endsWithSnowDesk(g.dir)) cmd += L" & rmdir \"" + g.dir + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    std::wstring parent = dirOf(g.dir);              // cmd must not sit inside the folder it removes
    if (parent.size() == 2 && parent[1] == L':') parent += L"\\";
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, parent.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    shutdown(false);
}

// ---------------------------------------------------------------- window procs

static LRESULT CALLBACK overlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (g.play) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN:
        if (g.play) { SetCapture(h); g.world.mouseDown((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), now()); g.lastInput = now(); }
        return 0;
    case WM_MOUSEMOVE:
        if (g.play) { g.world.mouseMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), now()); g.lastInput = now(); }
        return 0;
    case WM_LBUTTONUP:
        if (g.play) { if (GetCapture() == h) ReleaseCapture(); g.world.mouseUp((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), now()); }
        return 0;
    case WM_RBUTTONUP:
        setPlay(false);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK ctlProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.wmTaskbarCreated && g.wmTaskbarCreated) { addTray(); return 0; }
    switch (msg) {
    case WM_TIMER:
        if (wp == 1) frame();
        return 0;
    case WM_HOTKEY:
        if (wp == HK_PLAY) { if (g.paused) setPaused(false); setPlay(!g.play); }
        else if (wp == HK_ESC) setPlay(false);
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) { if (g.paused) setPaused(false); setPlay(!g.play); }
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_PLAY: setPlay(!g.play); break;
        case IDM_PAUSE: setPaused(!g.paused); break;
        case IDM_INT0: case IDM_INT1: case IDM_INT2: case IDM_INT3:
            g.intensityIdx = LOWORD(wp) - IDM_INT0;
            g.world.setIntensity(INTENSITY[g.intensityIdx]);
            iniSet(L"Intensity", g.intensityIdx);
            break;
        case IDM_DRIFT: g.world.addDrift(); break;
        case IDM_CLEAR: g.world.clearSnow(); saveSnow(); break;
        case IDM_AUTOSTART: setAutostart(!autostartOn()); break;
        case IDM_FOLDER: ShellExecuteW(nullptr, L"open", g.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_UNINSTALL: uninstall(); break;
        case IDM_EXIT: shutdown(true); break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        reconfigure();
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) reconfigure();
        break;
    case WM_QUERYENDSESSION:
        saveSnow();
        return TRUE;
    case WM_ENDSESSION:
        if (wp) saveSnow();
        return 0;
    case WM_DESTROY:
        if (h == g.ctl) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- startup helpers

static void setDpiAware() {
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    auto fn = (SetCtxFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!fn || !fn((HANDLE)(intptr_t)-4)) SetProcessDPIAware();
}


// The program lives wherever its .exe is. If that's drive C: and a D: drive exists, offer to move there once.
static bool offerMoveToD() {
    if (g.exe.size() < 3 || towupper(g.exe[0]) != L'C' || g.exe[1] != L':') return false;
    if (GetDriveTypeW(L"D:\\") != DRIVE_FIXED) return false;
    if (iniInt(L"AskedMove", 0)) return false;
    int r = MessageBoxW(nullptr,
        L"SnowDesk сейчас лежит на диске C:.\n\n"
        L"Перенести программу и все её данные в папку D:\\SnowDesk?\n"
        L"(рекомендуется — на диске C: ничего не останется)",
        L"SnowDesk", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    if (r != IDYES) { iniSet(L"AskedMove", 1); return false; }
    const std::wstring nd = L"D:\\SnowDesk";
    CreateDirectoryW(nd.c_str(), nullptr);
    std::wstring ne = nd + L"\\SnowDesk.exe";
    if (!CopyFileW(g.exe.c_str(), ne.c_str(), FALSE)) {
        MessageBoxW(nullptr, L"Не получилось скопировать программу на диск D:. SnowDesk запустится с текущего места.",
                    L"SnowDesk", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }
    CopyFileW(g.ini.c_str(), (nd + L"\\SnowDesk.ini").c_str(), FALSE);
    CopyFileW(g.dat.c_str(), (nd + L"\\snow.dat").c_str(), FALSE);
    std::wstring cmd = L"\"" + ne + L"\" /movedfrom \"" + g.exe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(ne.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nd.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

static void cleanupOldCopy(const std::wstring& oldExe) {
    if (_wcsicmp(oldExe.c_str(), g.exe.c_str()) == 0) return;
    std::wstring od = dirOf(oldExe);
    for (int i = 0; i < 20; i++) {
        if (DeleteFileW(oldExe.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
        Sleep(250);
    }
    DeleteFileW((od + L"\\SnowDesk.ini").c_str());
    DeleteFileW((od + L"\\snow.dat").c_str());
    if (endsWithSnowDesk(od)) RemoveDirectoryW(od.c_str());
}

// ---------------------------------------------------------------- main

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    setDpiAware();
    g.inst = inst;
    QueryPerformanceFrequency(&g.freq);

    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    g.exe = path;
    g.dir = dirOf(g.exe);
    g.ini = g.dir + L"\\SnowDesk.ini";
    g.dat = g.dir + L"\\snow.dat";

    std::wstring movedFrom;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++)
        if (_wcsicmp(argv[i], L"/movedfrom") == 0) movedFrom = argv[i + 1];
    if (argv) LocalFree(argv);

    // single instance (after a move, wait for the old copy to exit)
    for (int tries = 0;; tries++) {
        g.mutex = CreateMutexW(nullptr, TRUE, L"Local\\SnowDesk.SingleInstance");
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        CloseHandle(g.mutex); g.mutex = nullptr;
        if (movedFrom.empty() || tries > 40) {
            MessageBoxW(nullptr,
                L"SnowDesk уже запущен.\n\nЕго значок-снежинка — справа внизу, у часов (может быть спрятан под стрелкой ^).",
                L"SnowDesk", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
            return 0;
        }
        Sleep(250);
    }

    if (movedFrom.empty() && offerMoveToD()) {
        ReleaseMutex(g.mutex); CloseHandle(g.mutex);
        return 0;
    }
    if (!movedFrom.empty()) {
        cleanupOldCopy(movedFrom);
        if (autostartOn()) setAutostart(true);   // keep autostart pointing at the new location
    }
    if (autostartOn()) setAutostart(true);       // refresh path in case the folder was moved by hand

    getMonitor();
    if (!makeDib()) {
        MessageBoxW(nullptr, L"Не хватило памяти для запуска SnowDesk.", L"SnowDesk", MB_OK | MB_ICONERROR);
        return 1;
    }

    g.icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g.icon) g.icon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlayProc; wc.hInstance = inst; wc.lpszClassName = L"SnowDeskOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    WNDCLASSEXW wc2{}; wc2.cbSize = sizeof(wc2);
    wc2.lpfnWndProc = ctlProc; wc2.hInstance = inst; wc2.lpszClassName = L"SnowDeskCtl";
    wc2.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    RegisterClassExW(&wc2);

    g.ctl = CreateWindowExW(WS_EX_TOOLWINDOW, L"SnowDeskCtl", L"SnowDesk", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, inst, nullptr);
    g.overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                L"SnowDeskOverlay", L"SnowDesk", WS_POPUP,
                                g.mon.left, g.mon.top, g.W, g.H, nullptr, nullptr, inst, nullptr);
    if (!g.ctl || !g.overlay) {
        MessageBoxW(nullptr, L"Не удалось создать окно SnowDesk.", L"SnowDesk", MB_OK | MB_ICONERROR);
        return 1;
    }

    g.intensityIdx = std::max(0, std::min(3, iniInt(L"Intensity", 1)));
    g.world.setIntensity(INTENSITY[g.intensityIdx]);
    g.world.init(g.W, g.H, g.groundY, g.scale);
    std::vector<uint8_t> data;
    if (readFile(g.dat, data)) g.world.load(data);

    g.wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();
    g.hotkeyOk = RegisterHotKey(g.ctl, HK_PLAY, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S') != 0;

    if (!iniInt(L"Welcomed", 0)) {
        balloon(L"SnowDesk запущен ❄",
                g.hotkeyOk ? L"Ctrl+Alt+S или левый клик по снежинке у часов — играть со снегом. Правый клик — меню."
                           : L"Левый клик по снежинке у часов — играть со снегом. Правый клик — меню.");
        iniSet(L"Welcomed", 1);
    }

    g.lastFrame = g.lastSave = now();
    memset(g.bits, 0, (size_t)g.W * g.H * 4);
    present();
    SetTimer(g.ctl, 1, 30, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    freeDib();
    if (g.mutex) { ReleaseMutex(g.mutex); CloseHandle(g.mutex); }
    return 0;
}
