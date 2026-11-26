// SnowDesk for Linux (X11) — snow on your desktop.
// A transparent, click-through overlay on the primary monitor; the snow physics is shared with Windows (sim.cpp).
// Needs an X11 session (or XWayland) with a compositor for transparency.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XTest.h>
#include <X11/Xft/Xft.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "../sim.h"

static const float INTENSITY[4] = {0.4f, 1.0f, 2.0f, 4.0f};

// ---------------------------------------------------------------- languages

enum Str {
    S_PLAY, S_PAUSE, S_SNOWFALL, S_INT0, S_INT1, S_INT2, S_INT3, S_DRIFT, S_CLEAR, S_INTERACTIVE, S_CURSOR,
    S_AUTOSTART, S_FOLDER, S_LANGUAGE, S_UNINSTALL, S_UNINSTALL_SURE, S_EXIT, S_HINT, S_PLAY_LABEL,
    S_NO_COMPOSITOR, S_COUNT
};
static const char* const STR_EN[S_COUNT] = {
    "Play Mode", "Pause (hide the snow)", "Snowfall", "Light", "Normal", "Heavy", "Blizzard",
    "Add a snowdrift", "Clear all snow", "Make snowballs without Play Mode", "Cursor pushes snow",
    "Start at login", "Open data folder", "Language", "Uninstall SnowDesk completely\xe2\x80\xa6",
    "Click again to uninstall", "Exit", "Ctrl+Alt+M opens this menu", "Play Mode  \xc2\xb7  Esc",
    "No compositor: the snow can't be transparent. Turn on desktop effects / a compositor.",
};
static const char* const STR_RU[S_COUNT] = {
    "Режим игры", "Пауза (снег спрятан)", "Сила снегопада", "Лёгкий", "Обычный", "Сильный", "Метель",
    "Насыпать сугроб", "Убрать весь снег", "Лепить снежки без режима игры", "Курсор толкает снег",
    "Запускать при входе в систему", "Открыть папку с данными", "Язык", "Удалить SnowDesk полностью\xe2\x80\xa6",
    "Нажмите ещё раз, чтобы удалить", "Выход", "Ctrl+Alt+M открывает это меню", "Режим игры  \xc2\xb7  Esc",
    "Нет композитора: снег не может быть прозрачным. Включите эффекты рабочего стола.",
};
static bool g_ru = false;
static const char* T(Str s) { return (g_ru ? STR_RU : STR_EN)[s]; }

// ---------------------------------------------------------------- state

struct Atoms {
    Atom clientList, wmState, hidden, maxV, maxH, fullscreen, wmType, typeNormal, typeDialog, typeDock, typeDesktop,
        frameExtents, gtkExtents, activeWin, wmDesktop, curDesktop, workarea, cmSel, trayOpcode, xembedInfo, manager,
        utf8, wmName;
};

struct App {
    Display* dpy = nullptr;
    int scr = 0;
    Window root = 0, ov = 0, menuWin = 0, trayWin = 0;
    Visual* vis = nullptr;
    Colormap cmap = 0;
    GC gc = 0;
    XImage* img = nullptr;
    XShmSegmentInfo shm{};
    bool useShm = false;
    uint32_t* bits = nullptr;
    int monX = 0, monY = 0, W = 0, H = 0, groundY = 0;
    float scale = 1;
    Atoms a{};
    World world;
    // settings
    int intensityIdx = 1;
    bool interactive = true, cursorPhys = true, welcomed = false;
    // paths
    std::string bin, cfgDir, dataDir, ini, dat, autostartFile, sockPath;
    // runtime
    bool paused = false, play = false, hiddenFS = false, mapped = false, quit = false, compositor = true, uninstalled = false;
    double lastEnum = 0, lastTop = 0, lastSave = 0, lastInput = 0, lastFrame = 0, lastShape = 0, lastMon = 0;
    bool grab = false, grabMoved = false, grabBall = false;
    double grabT = 0; float grabX = 0, grabY = 0;
    std::vector<World::IRect> shapeRects;
    bool shapeFull = false;
    int sockFd = -1;
    // hotkeys
    KeyCode kcS = 0, kcM = 0, kcEsc = 0;
    bool escGrabbed = false;
    // play-mode label
    std::vector<uint32_t> hint;
    int hintW = 0, hintH = 0;
    // menu
    XftFont* font = nullptr;
    bool menuOpen = false;
    int menuHover = -1, menuW = 0, menuRowH = 0;
    bool confirmUninstall = false;
    // tray
    int trayW = 22, trayH = 22;
} g;

static volatile sig_atomic_t g_signal = 0;
static bool g_xerror = false;

static double now() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int xErrorHandler(Display*, XErrorEvent*) { g_xerror = true; return 0; }

// ---------------------------------------------------------------- files & settings

static bool readFile(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 64 * 1024 * 1024) { fclose(f); return false; }
    out.resize((size_t)n);
    bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static bool writeFileAtomic(const std::string& p, const void* data, size_t n) {
    std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), p.c_str()) == 0;
}

static void mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0700);
    }
    mkdir(p.c_str(), 0700);
}

static void saveSnow() {
    auto d = g.world.save();
    mkdirs(g.dataDir);
    writeFileAtomic(g.dat, d.data(), d.size());
    g.lastSave = now();
}

static void saveSettings() {
    mkdirs(g.cfgDir);
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "intensity=%d\ninteractive=%d\ncursor=%d\nlanguage=%s\nwelcomed=%d\n",
                     g.intensityIdx, g.interactive ? 1 : 0, g.cursorPhys ? 1 : 0, g_ru ? "ru" : "en", g.welcomed ? 1 : 0);
    writeFileAtomic(g.ini, buf, (size_t)n);
}

static void loadSettings() {
    std::vector<uint8_t> d;
    bool haveFile = readFile(g.ini, d);
    if (!haveFile) return;   // defaults: English, normal snowfall, everything on
    std::string s(d.begin(), d.end());
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "intensity") g.intensityIdx = std::max(0, std::min(3, atoi(v.c_str())));
        else if (k == "interactive") g.interactive = atoi(v.c_str()) != 0;
        else if (k == "cursor") g.cursorPhys = atoi(v.c_str()) != 0;
        else if (k == "language") g_ru = v == "ru";
        else if (k == "welcomed") g.welcomed = atoi(v.c_str()) != 0;
    }
}

static bool autostartOn() { return access(g.autostartFile.c_str(), F_OK) == 0; }

static void setAutostart(bool on) {
    if (!on) { unlink(g.autostartFile.c_str()); return; }
    std::string dir = g.autostartFile.substr(0, g.autostartFile.find_last_of('/'));
    mkdirs(dir);
    std::string s = "[Desktop Entry]\nType=Application\nName=SnowDesk\nComment=Snow on your desktop\nExec=\"" + g.bin +
                    "\"\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
    writeFileAtomic(g.autostartFile, s.data(), s.size());
}

static void spawn(const char* prog, const char* arg) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        execlp(prog, prog, arg, (char*)nullptr);
        _exit(127);
    }
}

// ---------------------------------------------------------------- X helpers

static bool getCardinals(Window w, Atom prop, std::vector<long>& out, Atom type = XA_CARDINAL) {
    out.clear();
    Atom at; int af; unsigned long n, left; unsigned char* data = nullptr;
    g_xerror = false;
    if (XGetWindowProperty(g.dpy, w, prop, 0, 1024, False, type, &at, &af, &n, &left, &data) != Success || g_xerror) return false;
    if (!data) return false;
    if (af == 32) { long* l = (long*)data; out.assign(l, l + n); }
    XFree(data);
    return !out.empty();
}

static bool getAtoms(Window w, Atom prop, std::vector<long>& out) { return getCardinals(w, prop, out, XA_ATOM); }

static void queryMonitor() {
    int n = 0;
    XRRMonitorInfo* m = XRRGetMonitors(g.dpy, g.root, True, &n);
    int x = 0, y = 0, w = DisplayWidth(g.dpy, g.scr), h = DisplayHeight(g.dpy, g.scr);
    if (m && n > 0) {
        int pick = 0;
        for (int i = 0; i < n; i++) if (m[i].primary) { pick = i; break; }
        x = m[pick].x; y = m[pick].y; w = m[pick].width; h = m[pick].height;
    }
    if (m) XRRFreeMonitors(m);
    g.monX = x; g.monY = y; g.W = w; g.H = h;
    g.groundY = h;
    std::vector<long> desk, wa;
    long cur = 0;
    if (getCardinals(g.root, g.a.curDesktop, desk)) cur = desk[0];
    if (getCardinals(g.root, g.a.workarea, wa) && (long)wa.size() >= (cur + 1) * 4) {
        long wy = wa[cur * 4 + 1], wh = wa[cur * 4 + 3];
        int bottom = (int)std::min<long>(y + h, wy + wh) - y;
        if (bottom > h / 2 && bottom <= h) g.groundY = bottom;
    }
    // DPI from the Xft.dpi resource
    g.scale = 1;
    if (const char* rm = XResourceManagerString(g.dpy)) {
        const char* p = strstr(rm, "Xft.dpi:");
        if (p) { float dpi = (float)atof(p + 8); if (dpi > 0) g.scale = std::max(1.0f, dpi / 96.0f); }
    }
}

// ---------------------------------------------------------------- overlay image

static void freeImage() {
    if (!g.img) return;
    if (g.useShm) { XShmDetach(g.dpy, &g.shm); XDestroyImage(g.img); shmdt(g.shm.shmaddr); }
    else XDestroyImage(g.img);   // frees bits too
    g.img = nullptr; g.bits = nullptr;
}

static bool makeImage() {
    freeImage();
    g.useShm = false;
    if (XShmQueryExtension(g.dpy)) {
        g.img = XShmCreateImage(g.dpy, g.vis, 32, ZPixmap, nullptr, &g.shm, g.W, g.H);
        if (g.img) {
            g.shm.shmid = shmget(IPC_PRIVATE, (size_t)g.img->bytes_per_line * g.H, IPC_CREAT | 0600);
            if (g.shm.shmid >= 0) {
                g.shm.shmaddr = g.img->data = (char*)shmat(g.shm.shmid, nullptr, 0);
                g.shm.readOnly = False;
                g_xerror = false;
                XShmAttach(g.dpy, &g.shm);
                XSync(g.dpy, False);
                shmctl(g.shm.shmid, IPC_RMID, nullptr);
                if (!g_xerror && g.shm.shmaddr != (char*)-1) { g.useShm = true; }
                else { shmdt(g.shm.shmaddr); g.img->data = nullptr; XDestroyImage(g.img); g.img = nullptr; }
            } else { XDestroyImage(g.img); g.img = nullptr; }
        }
    }
    if (!g.img) {
        char* mem = (char*)calloc((size_t)g.W * g.H, 4);
        if (!mem) return false;
        g.img = XCreateImage(g.dpy, g.vis, 32, ZPixmap, 0, mem, g.W, g.H, 32, g.W * 4);
        if (!g.img) { free(mem); return false; }
    }
    g.bits = (uint32_t*)g.img->data;
    memset(g.bits, 0, (size_t)g.W * g.H * 4);
    return true;
}

static void present() {
    if (g.useShm) XShmPutImage(g.dpy, g.ov, g.gc, g.img, 0, 0, 0, 0, g.W, g.H, False);
    else XPutImage(g.dpy, g.ov, g.gc, g.img, 0, 0, 0, 0, g.W, g.H);
}

static void showOverlay(bool show) {
    if (show == g.mapped) return;
    if (show) { XMapRaised(g.dpy, g.ov); }
    else XUnmapWindow(g.dpy, g.ov);
    g.mapped = show;
}

// ---------------------------------------------------------------- input shape: clicks go through, except on snow

static void setShapeRects(const std::vector<World::IRect>& rs, bool full) {
    std::vector<XRectangle> xr;
    if (full) xr.push_back({0, 0, (unsigned short)std::min(g.W, 65535), (unsigned short)std::min(g.H, 65535)});
    else
        for (const auto& r : rs) {
            int x = std::max(0, r.x), y = std::max(0, r.y), x1 = std::min(g.W, r.x + r.w), y1 = std::min(g.H, r.y + r.h);
            if (x1 > x && y1 > y) xr.push_back({(short)x, (short)y, (unsigned short)(x1 - x), (unsigned short)(y1 - y)});
        }
    XShapeCombineRectangles(g.dpy, g.ov, ShapeInput, 0, 0, xr.empty() ? nullptr : xr.data(), (int)xr.size(), ShapeSet, Unsorted);
}

static void updateShape(bool force) {
    if (g.play) {
        if (!g.shapeFull || force) { setShapeRects({}, true); g.shapeFull = true; }
        return;
    }
    std::vector<World::IRect> rs;
    if (g.interactive) g.world.inputRects(rs);
    bool same = !g.shapeFull && rs.size() == g.shapeRects.size() &&
                std::equal(rs.begin(), rs.end(), g.shapeRects.begin(), [](const World::IRect& a, const World::IRect& b) {
                    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; });
    if (same && !force) return;
    g.shapeRects = rs; g.shapeFull = false;
    setShapeRects(rs, false);
}

// ---------------------------------------------------------------- windows

static std::vector<WinRect> enumWindows(bool& fullscreenActive) {
    std::vector<WinRect> out;
    fullscreenActive = false;
    std::vector<long> list, curD;
    long curDesk = 0;
    if (getCardinals(g.root, g.a.curDesktop, curD)) curDesk = curD[0];
    std::vector<long> act;
    Window active = getCardinals(g.root, g.a.activeWin, act, XA_WINDOW) ? (Window)act[0] : 0;
    if (!getCardinals(g.root, g.a.clientList, list, XA_WINDOW)) return out;
    for (int i = (int)list.size() - 1; i >= 0; i--) {             // topmost first
        Window w = (Window)list[i];
        if (w == g.ov || w == g.menuWin || w == g.trayWin) continue;
        XWindowAttributes wa;
        g_xerror = false;
        if (!XGetWindowAttributes(g.dpy, w, &wa) || g_xerror) continue;
        if (wa.map_state != IsViewable) continue;
        std::vector<long> v;
        if (getCardinals(w, g.a.wmDesktop, v) && v[0] != curDesk && (unsigned long)v[0] != 0xFFFFFFFFul) continue;
        bool hidden = false, maxV = false, maxH = false, full = false;
        if (getAtoms(w, g.a.wmState, v))
            for (long s : v) {
                if ((Atom)s == g.a.hidden) hidden = true;
                if ((Atom)s == g.a.maxV) maxV = true;
                if ((Atom)s == g.a.maxH) maxH = true;
                if ((Atom)s == g.a.fullscreen) full = true;
            }
        if (hidden) continue;
        if (w == active && full) fullscreenActive = true;
        bool normal = true;
        if (getAtoms(w, g.a.wmType, v)) {
            Atom t = (Atom)v[0];
            if (t == g.a.typeDock || t == g.a.typeDesktop) continue;
            normal = t == g.a.typeNormal || t == g.a.typeDialog;
        }
        int x, y; Window child;
        XTranslateCoordinates(g.dpy, w, g.root, 0, 0, &x, &y, &child);
        int l = x, t = y, r = x + wa.width, b = y + wa.height;
        if (getCardinals(w, g.a.frameExtents, v) && v.size() >= 4) { l -= (int)v[0]; r += (int)v[1]; t -= (int)v[2]; b += (int)v[3]; }
        if (getCardinals(w, g.a.gtkExtents, v) && v.size() >= 4) { l += (int)v[0]; r -= (int)v[1]; t += (int)v[2]; b -= (int)v[3]; }
        if (r <= g.monX || l >= g.monX + g.W || b <= g.monY || t >= g.monY + g.H || r <= l || b <= t) continue;
        bool covers = l <= g.monX && t <= g.monY && r >= g.monX + g.W && b >= g.monY + g.H;
        bool solid = normal && !(maxV && maxH) && !full && !covers &&
                     (r - l) >= (int)(150 * g.scale) && (b - t) >= (int)(80 * g.scale);
        out.push_back(WinRect{(uint64_t)w, l - g.monX, t - g.monY, r - g.monX, b - g.monY, solid});
    }
    return out;
}

// ---------------------------------------------------------------- play-mode label (Xft text into a bitmap)

static void buildHint() {
    const char* txt = T(S_PLAY_LABEL);
    XGlyphInfo ext;
    XftTextExtentsUtf8(g.dpy, g.font, (const FcChar8*)txt, (int)strlen(txt), &ext);
    int w = ext.xOff + 2, h = g.font->ascent + g.font->descent + 2;
    Visual* dv = DefaultVisual(g.dpy, g.scr);
    int depth = DefaultDepth(g.dpy, g.scr);
    Pixmap pm = XCreatePixmap(g.dpy, g.root, w, h, depth);
    GC pgc = XCreateGC(g.dpy, pm, 0, nullptr);
    XSetForeground(g.dpy, pgc, BlackPixel(g.dpy, g.scr));
    XFillRectangle(g.dpy, pm, pgc, 0, 0, w, h);
    XftDraw* d = XftDrawCreate(g.dpy, pm, dv, DefaultColormap(g.dpy, g.scr));
    XftColor white;
    XRenderColor rc{0xffff, 0xffff, 0xffff, 0xffff};
    XftColorAllocValue(g.dpy, dv, DefaultColormap(g.dpy, g.scr), &rc, &white);
    XftDrawStringUtf8(d, &white, g.font, 1, g.font->ascent + 1, (const FcChar8*)txt, (int)strlen(txt));
    XImage* im = XGetImage(g.dpy, pm, 0, 0, w, h, AllPlanes, ZPixmap);
    g.hint.assign((size_t)w * h, 0);
    if (im) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned long p = XGetPixel(im, x, y);
                uint32_t a = std::max({(uint32_t)((p >> 16) & 255), (uint32_t)((p >> 8) & 255), (uint32_t)(p & 255)});
                g.hint[(size_t)y * w + x] = (a << 24) | (a << 16) | (a << 8) | a;
            }
        XDestroyImage(im);
    }
    g.hintW = w; g.hintH = h;
    XftColorFree(g.dpy, dv, DefaultColormap(g.dpy, g.scr), &white);
    XftDrawDestroy(d);
    XFreeGC(g.dpy, pgc);
    XFreePixmap(g.dpy, pm);
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
    int padX = (int)(10 * g.scale), padY = (int)(4 * g.scale), rad = (int)(8 * g.scale);
    int bw = g.hintW + padX * 2, bh = g.hintH + padY * 2;
    int x0 = (g.W - bw) / 2, y0 = (int)(10 * g.scale) + (g.H - g.groundY > 0 ? 0 : 0);
    const uint32_t a = 150;
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
            over(g.bits[yy * g.W + xx], g.hint[(size_t)y * g.hintW + x]);
        }
    }
}

// ---------------------------------------------------------------- modes

static void grabEsc(bool on) {
    if (on == g.escGrabbed || !g.kcEsc) return;
    unsigned int mods[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (unsigned m : mods) {
        if (on) XGrabKey(g.dpy, g.kcEsc, m, g.root, True, GrabModeAsync, GrabModeAsync);
        else XUngrabKey(g.dpy, g.kcEsc, m, g.root);
    }
    g.escGrabbed = on;
}

static void setPlay(bool on) {
    if (on == g.play) return;
    if (on && g.paused) return;
    if (g.grab) { g.grab = false; g.world.cancelHold(); }
    g.play = on;
    if (on) { g.lastInput = now(); buildHint(); }
    else { g.world.cancelHold(); XUngrabPointer(g.dpy, CurrentTime); }
    grabEsc(on);
    updateShape(true);
}

static void setPaused(bool p) {
    if (p == g.paused) return;
    if (p) { setPlay(false); if (g.grab) { g.grab = false; g.world.cancelHold(); } saveSnow(); showOverlay(false); }
    g.paused = p;
    g.lastFrame = now();
}

// ---------------------------------------------------------------- menu (drawn with Xft, no toolkit needed)

enum Cmd { C_NONE, C_PLAY, C_PAUSE, C_INT0, C_INT1, C_INT2, C_INT3, C_DRIFT, C_CLEAR, C_INTERACTIVE, C_CURSOR,
           C_AUTOSTART, C_FOLDER, C_EN, C_RU, C_UNINSTALL, C_EXIT };
struct Item { std::string text, right; Cmd cmd; int mark; bool sep, header; };   // mark: 0 none, 1 check, 2 radio off, 3 radio on, 4 check off

static std::vector<Item> g_items;

static void buildItems() {
    g_items.clear();
    auto add = [](std::string t, Cmd c, int mark = 0, std::string right = "") { g_items.push_back({t, right, c, mark, false, false}); };
    auto header = [](std::string t) { g_items.push_back({t, "", C_NONE, 0, false, true}); };
    auto sep = []() { g_items.push_back({"", "", C_NONE, 0, true, false}); };
    header(std::string("SnowDesk  \xc2\xb7  ") + T(S_HINT));
    if (!g.compositor) header(T(S_NO_COMPOSITOR));
    sep();
    add(T(S_PLAY), C_PLAY, g.play ? 1 : 4, "Ctrl+Alt+S");
    add(T(S_PAUSE), C_PAUSE, g.paused ? 1 : 4);
    header(T(S_SNOWFALL));
    for (int i = 0; i < 4; i++) add(T((Str)(S_INT0 + i)), (Cmd)(C_INT0 + i), g.intensityIdx == i ? 3 : 2);
    add(T(S_DRIFT), C_DRIFT);
    add(T(S_CLEAR), C_CLEAR);
    sep();
    add(T(S_INTERACTIVE), C_INTERACTIVE, g.interactive ? 1 : 4);
    add(T(S_CURSOR), C_CURSOR, g.cursorPhys ? 1 : 4);
    sep();
    add(T(S_AUTOSTART), C_AUTOSTART, autostartOn() ? 1 : 4);
    add(T(S_FOLDER), C_FOLDER);
    header(g_ru ? "Язык / Language" : "Language / Язык");
    add("English", C_EN, g_ru ? 2 : 3);
    add("Русский", C_RU, g_ru ? 3 : 2);
    sep();
    add(g.confirmUninstall ? T(S_UNINSTALL_SURE) : T(S_UNINSTALL), C_UNINSTALL);
    add(T(S_EXIT), C_EXIT);
}

static int textW(const std::string& s) {
    XGlyphInfo e;
    XftTextExtentsUtf8(g.dpy, g.font, (const FcChar8*)s.data(), (int)s.size(), &e);
    return e.xOff;
}

static int itemH(const Item& it) { return it.sep ? (int)(9 * g.scale) : g.menuRowH; }

static void drawMenu() {
    if (!g.menuOpen) return;
    XWindowAttributes wa;
    XGetWindowAttributes(g.dpy, g.menuWin, &wa);
    Visual* dv = DefaultVisual(g.dpy, g.scr);
    Colormap cm = DefaultColormap(g.dpy, g.scr);
    Pixmap pm = XCreatePixmap(g.dpy, g.menuWin, wa.width, wa.height, DefaultDepth(g.dpy, g.scr));
    GC pgc = XCreateGC(g.dpy, pm, 0, nullptr);
    auto px = [&](unsigned r, unsigned gg, unsigned b) {
        XColor c; c.red = r * 257; c.green = gg * 257; c.blue = b * 257; c.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(g.dpy, cm, &c); return c.pixel;
    };
    XSetForeground(g.dpy, pgc, px(30, 34, 44));
    XFillRectangle(g.dpy, pm, pgc, 0, 0, wa.width, wa.height);
    XftDraw* d = XftDrawCreate(g.dpy, pm, dv, cm);
    XftColor fg, dim, acc;
    XRenderColor c1{0xEEEE, 0xF0F0, 0xF4F4, 0xFFFF}, c2{0x9A9A, 0xA4A4, 0xB4B4, 0xFFFF}, c3{0x8C8C, 0xC8C8, 0xFFFF, 0xFFFF};
    XftColorAllocValue(g.dpy, dv, cm, &c1, &fg);
    XftColorAllocValue(g.dpy, dv, cm, &c2, &dim);
    XftColorAllocValue(g.dpy, dv, cm, &c3, &acc);
    int y = (int)(6 * g.scale), pad = (int)(12 * g.scale), markW = (int)(22 * g.scale);
    for (size_t i = 0; i < g_items.size(); i++) {
        const Item& it = g_items[i];
        int h = itemH(it);
        if (it.sep) {
            XSetForeground(g.dpy, pgc, px(60, 66, 80));
            XFillRectangle(g.dpy, pm, pgc, pad, y + h / 2, wa.width - 2 * pad, 1);
        } else {
            if ((int)i == g.menuHover && !it.header) {
                XSetForeground(g.dpy, pgc, px(52, 86, 140));
                XFillRectangle(g.dpy, pm, pgc, (int)(4 * g.scale), y, wa.width - (int)(8 * g.scale), h);
            }
            int base = y + (h + g.font->ascent - g.font->descent) / 2;
            const char* mark = it.mark == 1 ? "\xe2\x9c\x93" : (it.mark == 3 ? "\xe2\x97\x8f" : (it.mark == 2 ? "\xe2\x97\x8b" : ""));
            if (*mark) XftDrawStringUtf8(d, it.mark == 2 ? &dim : &acc, g.font, pad, base, (const FcChar8*)mark, (int)strlen(mark));
            XftDrawStringUtf8(d, it.header ? &dim : &fg, g.font, it.header ? pad : pad + markW, base,
                              (const FcChar8*)it.text.data(), (int)it.text.size());
            if (!it.right.empty())
                XftDrawStringUtf8(d, &dim, g.font, wa.width - pad - textW(it.right), base, (const FcChar8*)it.right.data(), (int)it.right.size());
        }
        y += h;
    }
    XCopyArea(g.dpy, pm, g.menuWin, pgc, 0, 0, wa.width, wa.height, 0, 0);
    XftColorFree(g.dpy, dv, cm, &fg); XftColorFree(g.dpy, dv, cm, &dim); XftColorFree(g.dpy, dv, cm, &acc);
    XftDrawDestroy(d);
    XFreeGC(g.dpy, pgc);
    XFreePixmap(g.dpy, pm);
}

static void closeMenu() {
    if (!g.menuOpen) return;
    XUngrabPointer(g.dpy, CurrentTime);
    XUngrabKeyboard(g.dpy, CurrentTime);
    XUnmapWindow(g.dpy, g.menuWin);
    g.menuOpen = false;
    g.confirmUninstall = false;
}

static void openMenu(int atX, int atY) {
    buildItems();
    g.menuRowH = g.font->ascent + g.font->descent + (int)(10 * g.scale);
    int w = 0, h = (int)(12 * g.scale);
    int pad = (int)(12 * g.scale), markW = (int)(22 * g.scale);
    for (const auto& it : g_items) {
        int tw = textW(it.text) + (it.header ? 0 : markW) + (it.right.empty() ? 0 : textW(it.right) + (int)(30 * g.scale));
        w = std::max(w, tw + 2 * pad);
        h += itemH(it);
    }
    g.menuW = w;
    int x = std::max(g.monX, std::min(atX, g.monX + g.W - w));
    int y = atY + h > g.monY + g.H ? atY - h : atY;
    y = std::max(g.monY, std::min(y, g.monY + g.H - h));
    XMoveResizeWindow(g.dpy, g.menuWin, x, y, w, h);
    XMapRaised(g.dpy, g.menuWin);
    g.menuOpen = true;
    g.menuHover = -1;
    XSync(g.dpy, False);
    for (int i = 0; i < 20; i++) {   // the pointer may still be grabbed by a click for a moment
        if (XGrabPointer(g.dpy, g.menuWin, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess) break;
        usleep(20000);
    }
    XGrabKeyboard(g.dpy, g.menuWin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    drawMenu();
}

static int menuItemAt(int x, int y) {
    XWindowAttributes wa;
    XGetWindowAttributes(g.dpy, g.menuWin, &wa);
    if (x < 0 || y < 0 || x >= wa.width || y >= wa.height) return -1;
    int yy = (int)(6 * g.scale);
    for (size_t i = 0; i < g_items.size(); i++) {
        int h = itemH(g_items[i]);
        if (y >= yy && y < yy + h) return (g_items[i].sep || g_items[i].header) ? -1 : (int)i;
        yy += h;
    }
    return -1;
}

static void uninstallNow() {
    setAutostart(false);
    unlink(g.dat.c_str()); unlink((g.dat + ".tmp").c_str());
    unlink(g.ini.c_str()); unlink((g.ini + ".tmp").c_str());
    rmdir(g.dataDir.c_str()); rmdir(g.cfgDir.c_str());
    // Linux lets a running program delete its own file; only touch it if it's really ours
    std::string name = g.bin.substr(g.bin.find_last_of('/') + 1);
    std::string lower = name;
    for (auto& ch : lower) ch = (char)tolower(ch);
    if (lower.rfind("snowdesk", 0) == 0) unlink(g.bin.c_str());
    g.uninstalled = true;
    g.quit = true;
}

static void runCmd(Cmd c) {
    switch (c) {
    case C_PLAY: if (g.paused) setPaused(false); setPlay(!g.play); break;
    case C_PAUSE: setPaused(!g.paused); break;
    case C_INT0: case C_INT1: case C_INT2: case C_INT3:
        g.intensityIdx = c - C_INT0; g.world.setIntensity(INTENSITY[g.intensityIdx]); saveSettings(); break;
    case C_DRIFT: g.world.addDrift(); break;
    case C_CLEAR: g.world.clearSnow(); saveSnow(); break;
    case C_INTERACTIVE: g.interactive = !g.interactive; saveSettings(); updateShape(true); break;
    case C_CURSOR: g.cursorPhys = !g.cursorPhys; g.world.setCursorPhysics(g.cursorPhys); saveSettings(); break;
    case C_AUTOSTART: setAutostart(!autostartOn()); break;
    case C_FOLDER: mkdirs(g.dataDir); spawn("xdg-open", g.dataDir.c_str()); break;
    case C_EN: case C_RU: g_ru = c == C_RU; saveSettings(); if (g.play) buildHint(); break;
    case C_UNINSTALL:
        if (!g.confirmUninstall) { g.confirmUninstall = true; buildItems(); drawMenu(); return; }
        closeMenu(); uninstallNow(); return;
    case C_EXIT: g.quit = true; break;
    default: return;
    }
    closeMenu();
}

// ---------------------------------------------------------------- tray icon (XEmbed system tray, where one exists)

static void drawTrayIcon() {
    if (!g.trayWin) return;
    int w = g.trayW, h = g.trayH;
    Visual* dv = DefaultVisual(g.dpy, g.scr);
    XImage* im = XCreateImage(g.dpy, dv, DefaultDepth(g.dpy, g.scr), ZPixmap, 0, (char*)calloc((size_t)w * h, 4), w, h, 32, 0);
    if (!im) return;
    float cx = (w - 1) / 2.0f, cy = (h - 1) / 2.0f, R = std::min(w, h) * 0.5f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy);
            unsigned long c = 0x265CAA;                                   // blue background
            float a = std::atan2(dy, dx);
            float arm = std::fabs(std::sin(a * 3.0f));                    // six arms
            if (r < R * 0.78f && (arm < 0.16f + 0.25f / (r + 1) || r < R * 0.14f)) c = 0xFFFFFF;
            if (r > R * 0.95f) c = 0x1A3F75;
            XPutPixel(im, x, y, c);
        }
    XPutImage(g.dpy, g.trayWin, DefaultGC(g.dpy, g.scr), im, 0, 0, 0, 0, w, h);
    XDestroyImage(im);
}

static void dockTray() {
    char name[64];
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", g.scr);
    Atom sel = XInternAtom(g.dpy, name, False);
    Window owner = XGetSelectionOwner(g.dpy, sel);
    if (!owner) return;
    if (!g.trayWin) {
        XSetWindowAttributes at{};
        at.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
        at.background_pixel = 0x265CAA;
        g.trayWin = XCreateWindow(g.dpy, g.root, 0, 0, g.trayW, g.trayH, 0, CopyFromParent, InputOutput, CopyFromParent,
                                  CWEventMask | CWBackPixel, &at);
        long info[2] = {0, 1};
        XChangeProperty(g.dpy, g.trayWin, g.a.xembedInfo, g.a.xembedInfo, 32, PropModeReplace, (unsigned char*)info, 2);
        XStoreName(g.dpy, g.trayWin, "SnowDesk");
    }
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = owner;
    ev.xclient.message_type = g.a.trayOpcode;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = 0;                 // SYSTEM_TRAY_REQUEST_DOCK
    ev.xclient.data.l[2] = (long)g.trayWin;
    XSendEvent(g.dpy, owner, False, NoEventMask, &ev);
    XFlush(g.dpy);
}

// ---------------------------------------------------------------- single instance & commands

static void handleCommand(const std::string& cmd) {
    if (cmd == "menu") {
        Window r, c; int rx, ry, wx, wy; unsigned m;
        XQueryPointer(g.dpy, g.root, &r, &c, &rx, &ry, &wx, &wy, &m);
        openMenu(rx, ry);
    } else if (cmd == "play") runCmd(C_PLAY);
    else if (cmd == "quit") g.quit = true;
}

static bool sendToRunning(const char* cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, g.sockPath.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { close(fd); return false; }
    std::string s = std::string(cmd) + "\n";
    ssize_t wr = write(fd, s.data(), s.size());
    (void)wr;
    close(fd);
    return true;
}

static bool listenSocket() {
    unlink(g.sockPath.c_str());
    g.sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g.sockFd < 0) return false;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, g.sockPath.c_str(), sizeof(a.sun_path) - 1);
    if (bind(g.sockFd, (sockaddr*)&a, sizeof(a)) != 0 || listen(g.sockFd, 4) != 0) { close(g.sockFd); g.sockFd = -1; return false; }
    fcntl(g.sockFd, F_SETFL, O_NONBLOCK);
    return true;
}

static void pollSocket() {
    if (g.sockFd < 0) return;
    for (;;) {
        int c = accept(g.sockFd, nullptr, nullptr);
        if (c < 0) break;
        char buf[64] = {0};
        pollfd p{c, POLLIN, 0};
        if (poll(&p, 1, 200) > 0) {
            ssize_t n = read(c, buf, sizeof(buf) - 1);
            if (n > 0) {
                std::string s(buf, (size_t)n);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                handleCommand(s);
            }
        }
        close(c);
    }
}

// ---------------------------------------------------------------- events

static void replayClick() {
    // a quick click on snow wasn't meant for the snow: let it through to the window underneath
    setShapeRects({}, false);
    g.shapeRects.clear(); g.shapeFull = false;
    XSync(g.dpy, False);
    XTestFakeButtonEvent(g.dpy, 1, True, CurrentTime);
    XTestFakeButtonEvent(g.dpy, 1, False, CurrentTime);
    XFlush(g.dpy);
    g.lastShape = now() + 0.15;   // restore the snow's input area a moment later
}

static void handleEvent(XEvent& e) {
    if (g.menuOpen && (e.xany.window == g.menuWin)) {
        switch (e.type) {
        case Expose: if (e.xexpose.count == 0) drawMenu(); break;
        case MotionNotify: {
            int h = menuItemAt(e.xmotion.x, e.xmotion.y);
            if (h != g.menuHover) { g.menuHover = h; drawMenu(); }
            break;
        }
        case ButtonPress: if (menuItemAt(e.xbutton.x, e.xbutton.y) < 0) {
            XWindowAttributes wa; XGetWindowAttributes(g.dpy, g.menuWin, &wa);
            if (e.xbutton.x < 0 || e.xbutton.y < 0 || e.xbutton.x >= wa.width || e.xbutton.y >= wa.height) closeMenu();
        } break;
        case ButtonRelease: {
            int i = menuItemAt(e.xbutton.x, e.xbutton.y);
            if (i >= 0) runCmd(g_items[i].cmd);
            break;
        }
        case KeyPress: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape) closeMenu();
            else if (ks == XK_Down || ks == XK_Up) {
                int n = (int)g_items.size(), i = g.menuHover;
                for (int k = 0; k < n; k++) {
                    i = (i + (ks == XK_Down ? 1 : n - 1)) % n;
                    if (!g_items[i].sep && !g_items[i].header) break;
                }
                g.menuHover = i; drawMenu();
            } else if ((ks == XK_Return || ks == XK_KP_Enter) && g.menuHover >= 0) runCmd(g_items[g.menuHover].cmd);
            break;
        }
        }
        return;
    }
    if (e.xany.window == g.trayWin && g.trayWin) {
        if (e.type == Expose) drawTrayIcon();
        else if (e.type == ConfigureNotify) { g.trayW = e.xconfigure.width; g.trayH = e.xconfigure.height; drawTrayIcon(); }
        else if (e.type == ButtonPress) {
            if (e.xbutton.button == 1) runCmd(C_PLAY);
            else openMenu(e.xbutton.x_root, e.xbutton.y_root);
        }
        return;
    }
    if (e.type == ClientMessage && e.xclient.message_type == g.a.manager) { dockTray(); return; }   // a tray appeared
    if (e.type == KeyPress) {
        KeyCode k = (KeyCode)e.xkey.keycode;
        if (k == g.kcS) runCmd(C_PLAY);
        else if (k == g.kcM) handleCommand("menu");
        else if (k == g.kcEsc && g.play) setPlay(false);
        return;
    }
    if (e.xany.window != g.ov) return;
    float x = 0, y = 0;
    double t = now();
    switch (e.type) {
    case ButtonPress:
        x = (float)e.xbutton.x; y = (float)e.xbutton.y;
        if (e.xbutton.button == 3) { if (g.play) setPlay(false); else openMenu(e.xbutton.x_root, e.xbutton.y_root); break; }
        if (e.xbutton.button != 1) break;
        if (g.play) { g.world.mouseDown(x, y, t); g.lastInput = t; break; }
        if (g.interactive) {
            int hit = g.world.hitTest(x, y);
            g.grab = true; g.grabMoved = false; g.grabBall = hit == 1;
            g.grabT = t; g.grabX = x; g.grabY = y;
            g.world.mouseDown(x, y, t);
        }
        break;
    case MotionNotify:
        x = (float)e.xmotion.x; y = (float)e.xmotion.y;
        if (g.play) { g.world.mouseMove(x, y, t); g.lastInput = t; }
        else if (g.grab) {
            g.world.mouseMove(x, y, t);
            float dx = x - g.grabX, dy = y - g.grabY;
            if (dx * dx + dy * dy > 25.0f * g.scale * g.scale) g.grabMoved = true;
        }
        break;
    case ButtonRelease:
        if (e.xbutton.button != 1) break;
        x = (float)e.xbutton.x; y = (float)e.xbutton.y;
        if (g.play) { g.world.mouseUp(x, y, t); break; }
        if (g.grab) {
            g.grab = false;
            bool quick = !g.grabMoved && (t - g.grabT) < 0.25;
            g.world.mouseUp(x, y, t);
            if (quick && !g.grabBall) replayClick();
        }
        break;
    }
}

// ---------------------------------------------------------------- frame

static void frame() {
    double t = now();
    float dt = (float)std::min(0.05, std::max(0.0, t - g.lastFrame));
    g.lastFrame = t;
    if (g.paused) return;

    if (t - g.lastEnum > 0.1) {
        g.lastEnum = t;
        bool fs = false;
        std::vector<WinRect> ws = enumWindows(fs);
        g.hiddenFS = fs;
        if (fs) setPlay(false);
        else g.world.setWindows(ws);
    }
    if (g.hiddenFS) { showOverlay(false); return; }

    Window r, c; int rx, ry, wx, wy; unsigned m;
    bool ok = XQueryPointer(g.dpy, g.root, &r, &c, &rx, &ry, &wx, &wy, &m) &&
              rx >= g.monX && rx < g.monX + g.W && ry >= g.monY && ry < g.monY + g.H;
    g.world.setCursor((float)(rx - g.monX), (float)(ry - g.monY), t, ok);
    g.world.step(dt);
    if (g.play) std::fill(g.bits, g.bits + (size_t)g.W * g.H, 0x01000000u);   // invisible, but catches the mouse
    else memset(g.bits, 0, (size_t)g.W * g.H * 4);
    g.world.render(g.bits);
    if (g.play) drawHint();
    present();
    showOverlay(true);

    if (t - g.lastShape > 0.1) { g.lastShape = t; updateShape(false); }
    if (t - g.lastTop > 2) { g.lastTop = t; XRaiseWindow(g.dpy, g.ov); if (g.menuOpen) XRaiseWindow(g.dpy, g.menuWin); }
    if (t - g.lastSave > 60) saveSnow();
    if (g.play && t - g.lastInput > 60) setPlay(false);
    XFlush(g.dpy);
}

static void reconfigure() {
    int oX = g.monX, oY = g.monY, oW = g.W, oH = g.H, oG = g.groundY;
    queryMonitor();
    if (oX == g.monX && oY == g.monY && oW == g.W && oH == g.H && oG == g.groundY) return;
    makeImage();
    g.world.resize(g.W, g.H, g.groundY);
    XMoveResizeWindow(g.dpy, g.ov, g.monX, g.monY, g.W, g.H);
    updateShape(true);
}

static int ioErrorHandler(Display*) {   // the X session went away (logout): keep the snow and leave
    if (g.bits) saveSnow();
    if (!g.sockPath.empty()) unlink(g.sockPath.c_str());
    _exit(0);
}

static void onSignal(int s) { g_signal = s; }

// ---------------------------------------------------------------- main

static void usage() {
    printf("SnowDesk - snow on your desktop\n\n"
           "  snowdesk            start (or open the menu of the running SnowDesk)\n"
           "  snowdesk --menu     open the menu\n"
           "  snowdesk --play     toggle Play Mode\n"
           "  snowdesk --quit     quit the running SnowDesk\n\n"
           "Shortcuts: Ctrl+Alt+M menu, Ctrl+Alt+S Play Mode, Esc leaves Play Mode.\n");
}

int main(int argc, char** argv) {
    std::string cmd;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(); return 0; }
        if (a == "--menu") cmd = "menu";
        else if (a == "--play") cmd = "play";
        else if (a == "--quit") cmd = "quit";
    }

    // paths
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    g.bin = n > 0 ? std::string(buf, (size_t)n) : std::string(argv[0]);
    const char* home = getenv("HOME");
    std::string h = home ? home : "/tmp";
    const char* xc = getenv("XDG_CONFIG_HOME");
    const char* xd = getenv("XDG_DATA_HOME");
    std::string cfgBase = xc && *xc ? xc : h + "/.config";
    g.cfgDir = cfgBase + "/snowdesk";
    g.dataDir = (xd && *xd ? std::string(xd) : h + "/.local/share") + "/snowdesk";
    g.ini = g.cfgDir + "/settings.ini";
    g.dat = g.dataDir + "/snow.dat";
    g.autostartFile = cfgBase + "/autostart/snowdesk.desktop";
    const char* rt = getenv("XDG_RUNTIME_DIR");
    g.sockPath = rt && *rt ? std::string(rt) + "/snowdesk.sock" : "/tmp/snowdesk-" + std::to_string(getuid()) + ".sock";

    // already running? hand the request over to it
    if (sendToRunning(cmd.empty() ? "menu" : cmd.c_str())) {
        if (cmd == "quit") printf("SnowDesk is closing.\n");
        else if (cmd == "play") printf("Play Mode toggled.\n");
        else printf("SnowDesk is already running - its menu is open now.\n");
        return 0;
    }
    if (cmd == "quit") { printf("SnowDesk isn't running.\n"); return 0; }

    loadSettings();

    g.dpy = XOpenDisplay(nullptr);
    if (!g.dpy) {
        fprintf(stderr, "SnowDesk needs a graphical X11 session (DISPLAY is not set or not reachable).\n");
        return 1;
    }
    XSetErrorHandler(xErrorHandler);
    XSetIOErrorHandler(ioErrorHandler);
    g.scr = DefaultScreen(g.dpy);
    g.root = RootWindow(g.dpy, g.scr);
    Atoms& A = g.a;
    A.clientList = XInternAtom(g.dpy, "_NET_CLIENT_LIST_STACKING", False);
    A.wmState = XInternAtom(g.dpy, "_NET_WM_STATE", False);
    A.hidden = XInternAtom(g.dpy, "_NET_WM_STATE_HIDDEN", False);
    A.maxV = XInternAtom(g.dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    A.maxH = XInternAtom(g.dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    A.fullscreen = XInternAtom(g.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A.wmType = XInternAtom(g.dpy, "_NET_WM_WINDOW_TYPE", False);
    A.typeNormal = XInternAtom(g.dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    A.typeDialog = XInternAtom(g.dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    A.typeDock = XInternAtom(g.dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    A.typeDesktop = XInternAtom(g.dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    A.frameExtents = XInternAtom(g.dpy, "_NET_FRAME_EXTENTS", False);
    A.gtkExtents = XInternAtom(g.dpy, "_GTK_FRAME_EXTENTS", False);
    A.activeWin = XInternAtom(g.dpy, "_NET_ACTIVE_WINDOW", False);
    A.wmDesktop = XInternAtom(g.dpy, "_NET_WM_DESKTOP", False);
    A.curDesktop = XInternAtom(g.dpy, "_NET_CURRENT_DESKTOP", False);
    A.workarea = XInternAtom(g.dpy, "_NET_WORKAREA", False);
    char cmName[32]; snprintf(cmName, sizeof(cmName), "_NET_WM_CM_S%d", g.scr);
    A.cmSel = XInternAtom(g.dpy, cmName, False);
    A.trayOpcode = XInternAtom(g.dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    A.xembedInfo = XInternAtom(g.dpy, "_XEMBED_INFO", False);
    A.manager = XInternAtom(g.dpy, "MANAGER", False);
    A.utf8 = XInternAtom(g.dpy, "UTF8_STRING", False);
    A.wmName = XInternAtom(g.dpy, "_NET_WM_NAME", False);
    g.compositor = XGetSelectionOwner(g.dpy, A.cmSel) != None;

    int shapeEv, shapeErr;
    if (!XShapeQueryExtension(g.dpy, &shapeEv, &shapeErr)) { fprintf(stderr, "SnowDesk needs the X Shape extension.\n"); return 1; }
    XVisualInfo vi;
    if (!XMatchVisualInfo(g.dpy, g.scr, 32, TrueColor, &vi)) { fprintf(stderr, "SnowDesk needs a 32-bit (transparent) visual.\n"); return 1; }
    g.vis = vi.visual;
    g.cmap = XCreateColormap(g.dpy, g.root, g.vis, AllocNone);

    queryMonitor();
    if (!makeImage()) { fprintf(stderr, "Not enough memory to start SnowDesk.\n"); return 1; }

    XSetWindowAttributes at{};
    at.override_redirect = True;
    at.colormap = g.cmap;
    at.border_pixel = 0;
    at.background_pixel = 0;
    at.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    g.ov = XCreateWindow(g.dpy, g.root, g.monX, g.monY, g.W, g.H, 0, 32, InputOutput, g.vis,
                         CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &at);
    XClassHint ch{(char*)"snowdesk", (char*)"SnowDesk"};
    XSetClassHint(g.dpy, g.ov, &ch);
    XStoreName(g.dpy, g.ov, "SnowDesk");
    g.gc = XCreateGC(g.dpy, g.ov, 0, nullptr);
    setShapeRects({}, false);

    XSetWindowAttributes ma{};
    ma.override_redirect = True;
    ma.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
    ma.background_pixel = 0x1E222C;
    g.menuWin = XCreateWindow(g.dpy, g.root, 0, 0, 10, 10, 1, CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWEventMask | CWBackPixel, &ma);
    XSetClassHint(g.dpy, g.menuWin, &ch);

    char fname[64];
    snprintf(fname, sizeof(fname), "Sans:pixelsize=%d", (int)std::lround(13 * g.scale));
    g.font = XftFontOpenName(g.dpy, g.scr, fname);
    if (!g.font) g.font = XftFontOpenName(g.dpy, g.scr, "fixed");

    // global shortcuts
    XSelectInput(g.dpy, g.root, StructureNotifyMask | PropertyChangeMask);
    g.kcS = XKeysymToKeycode(g.dpy, XK_s);
    g.kcM = XKeysymToKeycode(g.dpy, XK_m);
    g.kcEsc = XKeysymToKeycode(g.dpy, XK_Escape);
    unsigned int mods[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (unsigned mm : mods) {
        XGrabKey(g.dpy, g.kcS, ControlMask | Mod1Mask | mm, g.root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(g.dpy, g.kcM, ControlMask | Mod1Mask | mm, g.root, True, GrabModeAsync, GrabModeAsync);
    }

    g.world.setIntensity(INTENSITY[g.intensityIdx]);
    g.world.setCursorPhysics(g.cursorPhys);
    g.world.init(g.W, g.H, g.groundY, g.scale);
    std::vector<uint8_t> data;
    if (readFile(g.dat, data)) g.world.load(data);

    if (autostartOn()) setAutostart(true);       // keep the autostart entry pointing at this file
    listenSocket();
    dockTray();
    signal(SIGINT, onSignal); signal(SIGTERM, onSignal); signal(SIGHUP, onSignal);
    signal(SIGCHLD, SIG_IGN);

    printf("SnowDesk is running. Ctrl+Alt+M opens the menu, Ctrl+Alt+S starts Play Mode.\n");
    if (!g.compositor) fprintf(stderr, "Warning: no compositor is running, so the snow can't be transparent.\n");
    if (getenv("WAYLAND_DISPLAY"))
        fprintf(stderr, "Note: on Wayland SnowDesk only sees X11 (XWayland) windows; snow still falls and piles up at the bottom.\n");
    fflush(stdout);

    g.lastFrame = g.lastSave = now();
    if (!g.welcomed) {
        g.welcomed = true; saveSettings();
        openMenu(g.monX + g.W / 2 - 150, g.monY + g.H / 3);
    }

    int xfd = ConnectionNumber(g.dpy);
    double nextFrame = now();
    while (!g.quit && !g_signal) {
        double interval = g.paused ? 0.25 : (g.play ? 1.0 / 60 : 1.0 / 30);
        int timeout = (int)std::max(0.0, (nextFrame - now()) * 1000);
        pollfd fds[2] = {{xfd, POLLIN, 0}, {g.sockFd, POLLIN, 0}};
        if (!XPending(g.dpy)) poll(fds, g.sockFd >= 0 ? 2 : 1, timeout);
        while (XPending(g.dpy)) { XEvent e; XNextEvent(g.dpy, &e); handleEvent(e); }
        pollSocket();
        double t = now();
        if (t >= nextFrame) {
            frame();
            nextFrame = t + interval;
            if (t - g.lastMon > 2) { g.lastMon = t; reconfigure(); }
        }
    }

    if (g.bits && !g.uninstalled) saveSnow();
    if (g.sockFd >= 0) { close(g.sockFd); unlink(g.sockPath.c_str()); }
    closeMenu();
    freeImage();
    XCloseDisplay(g.dpy);
    return 0;
}
