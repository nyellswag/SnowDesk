// SnowDesk — rain mode. See rain.h.
#include "rain.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
const float PI = 3.14159265f;

inline void blend(uint32_t* px, int r, int g, int b, float a) {
    if (a <= 0.003f) return;
    if (a > 1) a = 1;
    uint32_t d = *px;
    float ia = 1.0f - a;
    int da = (d >> 24) & 255, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int oa = (int)(255 * a + da * ia + 0.5f);
    int orr = (int)(r * a + dr * ia + 0.5f);
    int og = (int)(g * a + dg * ia + 0.5f);
    int ob = (int)(b * a + db * ia + 0.5f);
    *px = ((uint32_t)std::min(oa, 255) << 24) | ((uint32_t)std::min(orr, 255) << 16) |
          ((uint32_t)std::min(og, 255) << 8) | (uint32_t)std::min(ob, 255);
}

struct Canvas {
    uint32_t* px; int W, H;
    void put(int x, int y, int r, int g, int b, float a) const {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        blend(&px[y * W + x], r, g, b, a);
    }
    void dot(float cx, float cy, float rad, int r, int g, int b, float a) const {
        int x0 = (int)std::floor(cx - rad - 1), x1 = (int)std::ceil(cx + rad + 1);
        int y0 = (int)std::floor(cy - rad - 1), y1 = (int)std::ceil(cy + rad + 1);
        if (x1 < 0 || y1 < 0 || x0 >= W || y0 >= H) return;
        x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, W - 1); y1 = std::min(y1, H - 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float cov = clampf(rad + 0.5f - std::sqrt(dx * dx + dy * dy), 0, 1);
                if (cov > 0) blend(&px[y * W + x], r, g, b, cov * a);
            }
    }
    // a line whose opacity goes from a0 at the start to a1 at the end
    void line(float x0, float y0, float x1, float y1, int r, int g, int b, float a0, float a1) const {
        float dx = x1 - x0, dy = y1 - y0;
        int n = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)));
        if (n < 1) n = 1;
        for (int i = 0; i <= n; i++) {
            float t = (float)i / n;
            float x = x0 + dx * t, y = y0 + dy * t;
            int ix = (int)std::floor(x), iy = (int)std::floor(y);
            float fx = x - ix;
            float a = a0 + (a1 - a0) * t;
            put(ix, iy, r, g, b, a * (1 - fx));       // spread across two pixels: smooth, thin streaks
            put(ix + 1, iy, r, g, b, a * fx);
        }
    }
};

}  // namespace

// ---------------------------------------------------------------- setup

float Rain::rnd() {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (rng >> 8) * (1.0f / 16777216.0f);
}

void Rain::init(int w, int h, int gy, float scale) {
    S = scale > 0.5f ? scale : 1.0f;
    W = w; H = h; groundY = gy;
    cw = std::max(2.0f, std::round(3.0f * S));
    NC = (int)std::ceil(W / cw) + 1;
    lvl.assign(NC, 0.0f);
    vel.assign(NC, 0.0f);
    drops.clear(); balls.clear(); puddles.clear(); held = -1;
    windTarget = wind = rndr(-60, 60) * S;
}

void Rain::resize(int w, int h, int gy) {
    float m = meanLevel();
    W = w; H = h; groundY = gy;
    NC = (int)std::ceil(W / cw) + 1;
    lvl.assign(NC, std::min(m, maxLevel()));
    vel.assign(NC, 0.0f);
    drops.clear();
    balls.clear(); held = -1;
}

float Rain::meanLevel() const {
    if (NC == 0) return 0;
    double s = 0;
    for (float v : lvl) s += v;
    return (float)(s / NC);
}

float Rain::poolVolume() const { return meanLevel() * NC * cw; }

float Rain::puddleVolume() const {
    float s = 0;
    for (const auto& kv : puddles) for (float v : kv.second.h) s += v * 4 * S;
    return s;
}

int Rain::colOf(float x) const {
    int i = (int)(x / cw);
    return i < 0 ? 0 : (i >= NC ? NC - 1 : i);
}

float Rain::surfaceY(float x) const {
    if (NC == 0) return (float)H;
    float f = x / cw - 0.5f;
    int i0 = (int)std::floor(f);
    float t = f - i0;
    int a = std::max(0, std::min(NC - 1, i0)), b = std::max(0, std::min(NC - 1, i0 + 1));
    return H - (lvl[a] * (1 - t) + lvl[b] * t);
}

float Rain::poolBelow(float x, float y) const {
    float s = surfaceY(x);
    return y > s ? y - s : 0.0f;
}

void Rain::addWater(float x, float vol, float push) {
    int i = colOf(x);
    float d = vol / cw;
    lvl[i] += d * 0.5f;
    if (i > 0) lvl[i - 1] += d * 0.25f; else lvl[i] += d * 0.25f;
    if (i < NC - 1) lvl[i + 1] += d * 0.25f; else lvl[i] += d * 0.25f;
    kick(i, push);
}

// push the surface down at column i and up around it: a wave, without making water appear or vanish
void Rain::kick(int i, float v) {
    if (i < 0 || i >= NC || v == 0) return;
    int a = std::max(0, i - 3), b = std::min(NC - 1, i + 3);
    vel[i] -= v;
    vel[a] += v * 0.5f;
    vel[b] += v * 0.5f;
}

float Rain::takeWater(float x, float radius, float vol) {
    int c0 = colOf(x - radius), c1 = colOf(x + radius);
    float avail = 0;
    for (int i = c0; i <= c1; i++) avail += lvl[i] * cw;
    if (avail <= 0.01f) return 0;
    float take = std::min(vol, avail * 0.5f);
    float k = take / avail;
    for (int i = c0; i <= c1; i++) lvl[i] -= lvl[i] * k;
    return take;
}

int Rain::topWindowAt(float x, float y) const {
    for (size_t i = 0; i < wins.size(); i++) {
        const WinRect& w = wins[i];
        if (x >= w.l && x < w.r && y >= w.t && y < w.b) return (int)i;
    }
    return -1;
}

bool Rain::visibleOn(uint64_t id, float x, float y) const {
    int i = topWindowAt(x, y);
    return i >= 0 && wins[i].id == id;
}

// ---------------------------------------------------------------- windows

void Rain::setWindows(const std::vector<WinRect>& ws) {
    static double lastSet = -1;
    float dtW = lastSet < 0 ? 0.1f : (float)std::max(0.016, time - lastSet);
    lastSet = time;
    wins = ws;
    const float colW = 4 * S;
    for (auto& kv : puddles) kv.second.seen = false;
    for (size_t i = 0; i < wins.size(); i++) {
        const WinRect& w = wins[i];
        if (!w.pileable || w.t < 2 || w.t >= H - 20 * S) continue;
        Puddle& p = puddles[w.id];
        int n = std::max(1, (int)((w.r - w.l) / colW));
        bool fresh = p.h.empty();
        if (!fresh) {
            float k = std::min(1.0f, dtW / 0.08f);
            p.vx += ((w.l - p.l) / dtW - p.vx) * k;
            p.vy += ((w.t - p.t) / dtW - p.vy) * k;
        }
        if ((int)p.h.size() != n) {        // resized: keep the water, spread it over the new width
            float tot = 0;
            for (float v : p.h) tot += v;
            p.h.assign(n, fresh ? 0.0f : tot / n);
        }
        p.l = w.l; p.t = w.t; p.r = w.r; p.b = w.b;
        p.vis.assign(n, 0);
        for (int c = 0; c < n; c++) {
            float x = w.l + (c + 0.5f) * colW;
            bool vis = x >= 0 && x < W;
            for (size_t j = 0; vis && j < i; j++) {
                const WinRect& o = wins[j];
                if (x >= o.l && x < o.r && w.t - 1 >= o.t && w.t - 1 < o.b) vis = false;
            }
            p.vis[c] = vis;
        }
        p.seen = true;
    }
    // windows that closed, got minimized or maximized: their water pours down
    for (auto it = puddles.begin(); it != puddles.end();) {
        if (!it->second.seen) { dumpPuddle(it->second, it->first); it = puddles.erase(it); }
        else ++it;
    }
}

void Rain::dumpPuddle(Puddle& p, uint64_t id) {
    const float colW = 4 * S;
    for (size_t c = 0; c < p.h.size(); c++) {
        float v = p.h[c] * colW;
        if (v < 0.3f * S) continue;
        int n = 1 + (int)(v / (6 * S * S));
        for (int k = 0; k < n && k < 4; k++) {
            Drop d{};
            d.kind = D_SPRAY;
            d.x = p.l + (c + rnd()) * colW; d.y = p.t - rnd() * p.h[c];
            d.vx = rndr(-30, 30) * S + p.vx * 0.3f; d.vy = rndr(-40, 60) * S;
            d.vol = v / n; d.life = 4; d.a = 0.8f;
            drops.push_back(d);
        }
    }
    for (auto& d : drops)
        if ((d.kind == D_GLASS || d.kind == D_RUNNER) && d.win == id) {
            d.kind = D_SPRAY; d.vx = rndr(-20, 20) * S; d.vy = rndr(0, 60) * S; d.life = 4;
        }
}

Rain::Puddle* Rain::puddleAt(float x, float yPrev, float yNow, float& topY) {
    // the first visible window top edge crossed while moving down from yPrev to yNow at x
    for (size_t i = 0; i < wins.size(); i++) {
        const WinRect& w = wins[i];
        if (x < w.l || x >= w.r) continue;
        auto it = puddles.find(w.id);
        if (it == puddles.end()) {
            if (x >= w.l && x < w.r && yNow >= w.t && yNow < w.b) return nullptr;   // hidden behind an opaque window
            continue;
        }
        Puddle& p = it->second;
        int c = (int)((x - p.l) / (4 * S));
        if (c < 0 || c >= (int)p.h.size()) continue;
        float top = p.t - p.h[c];
        if (yPrev <= top && yNow >= top) {
            if (!p.vis[c]) return nullptr;
            topY = top;
            return &p;
        }
        if (yNow >= w.t && yNow < w.b) return nullptr;   // inside this window already (came from the side)
    }
    return nullptr;
}

// ---------------------------------------------------------------- effects

void Rain::splash(float x, float y, float vx, float vy, int n, float vol, float speed) {
    for (int i = 0; i < n; i++) {
        Drop d{};
        d.kind = D_SPRAY;
        float ang = rndr(-PI * 0.95f, -PI * 0.05f);       // upwards, fanning out
        float sp = speed * rndr(0.4f, 1.0f);
        d.x = x; d.y = y - 1;
        d.vx = std::cos(ang) * sp + vx; d.vy = std::sin(ang) * sp + vy;
        d.vol = vol / std::max(1, n);
        d.life = rndr(0.6f, 1.6f);
        d.a = rndr(0.55f, 0.85f);
        d.len = rndr(0.7f, 1.4f) * S;
        drops.push_back(d);
    }
}

void Rain::spawnRain(float dt) {
    float k = intensity * (1.0f + (burst > 0 ? 3.0f : 0.0f));
    float rate = k * (W / (1920.0f * S)) * 230.0f;
    spawnAcc += rate * dt;
    farAcc += rate * 0.9f * dt;
    float drift = std::fabs(wind) * (H / (900.0f * S)) + 20 * S;
    while (spawnAcc >= 1) {
        spawnAcc -= 1;
        Drop d{};
        d.kind = D_RAIN;
        d.vy = rndr(820, 1080) * S;
        d.vx = wind * rndr(0.9f, 1.1f);
        d.x = rndr(-drift, W + drift) - (wind > 0 ? drift * 0.5f : -drift * 0.5f);
        d.y = rndr(-60, -4) * S;
        d.len = d.vy * rndr(0.018f, 0.026f);
        d.a = rndr(0.32f, 0.55f);
        d.vol = 9 * S * S;
        d.life = 10;
        drops.push_back(d);
    }
    while (farAcc >= 1) {
        farAcc -= 1;
        Drop d{};
        d.kind = D_FAR;
        d.vy = rndr(480, 640) * S;
        d.vx = wind * 0.6f;
        d.x = rndr(-drift, W + drift);
        d.y = rndr(-40, -4) * S;
        d.len = d.vy * 0.02f;
        d.a = rndr(0.10f, 0.20f);
        d.life = rndr(0.45f, 1.0f) * (H + 40 * S) / d.vy;
        drops.push_back(d);
    }
}

// ---------------------------------------------------------------- simulation

void Rain::step(float dt) {
    if (dt > 0.05f) dt = 0.05f;
    if (dt <= 0) return;
    time += dt;
    windTimer -= dt;
    if (windTimer <= 0) { windTarget = rndr(-150, 150) * S; windTimer = rndr(4, 10); }
    wind += (windTarget - wind) * std::min(1.0f, dt * 0.5f);
    if (burst > 0) burst -= dt;

    spawnRain(dt);
    stepPuddles(dt);
    stepDrops(dt);
    stepBalls(dt);
    stepSwirl(dt);
    stepPool(dt);
}

void Rain::stepPool(float dt) {
    // the cursor moving through the water pushes a bow wave and leaves a wake
    if (cursorPhysics && cursorValid && held < 0 && poolBelow(curX, curY) > 2 * S) {
        float sp = clampf(curVx, -2500 * S, 2500 * S);
        float imp = sp * 0.04f * dt * 60;
        int ahead = colOf(curX + (sp > 0 ? 10 : -10) * S), behind = colOf(curX - (sp > 0 ? 10 : -10) * S);
        vel[ahead] += std::fabs(imp) * 0.5f;
        vel[behind] -= std::fabs(imp) * 0.5f;
    }

    // whirlpool: a funnel around swirlX with a raised rim; forces sum to zero so no water appears or vanishes
    float D = 0, wdt = 1, base = 0;
    if (swirl > 0.01f) {
        int c0 = colOf(swirlX - 140 * S), c1 = colOf(swirlX + 140 * S);
        double sum = 0;
        for (int i = c0; i <= c1; i++) sum += lvl[i];
        base = (float)(sum / std::max(1, c1 - c0 + 1));
        D = swirl * std::min((float)base * 0.85f, 90 * S);
        wdt = 34 * S + D * 0.5f;
    }

    const int SUB = std::max(4, (int)std::ceil(dt * 400.0f));   // small steps keep the waves stable at any frame rate
    float h = dt / SUB;
    float c = 230 * S / cw;
    float k = c * c, damp = 1.6f;
    for (int s = 0; s < SUB; s++) {
        for (int i = 0; i < NC; i++) {
            float l = lvl[i > 0 ? i - 1 : i], r = lvl[i < NC - 1 ? i + 1 : i];
            float vl = vel[i > 0 ? i - 1 : i], vr = vel[i < NC - 1 ? i + 1 : i];
            vel[i] += (k * (l + r - 2 * lvl[i]) - damp * vel[i] + 40.0f * (vl + vr - 2 * vel[i])) * h;
        }
        if (D > 0.5f) {
            int c0 = colOf(swirlX - 3 * wdt), c1 = colOf(swirlX + 3 * wdt);
            float sum = 0;
            int n = 0;
            for (int i = c0; i <= c1; i++) {
                float x = (i + 0.5f) * cw - swirlX;
                float u = x / wdt;
                float shape = -D * std::exp(-u * u) + D * 0.45f * std::exp(-(std::fabs(u) - 1.7f) * (std::fabs(u) - 1.7f) * 2.5f);
                float push = (base + shape - lvl[i]) * 90.0f * swirl * h;   // pull the surface towards the funnel shape
                vel[i] += push;
                sum += push; n++;
            }
            if (n) { float corr = sum / n; for (int i = c0; i <= c1; i++) vel[i] -= corr; }
        }
        for (int i = 0; i < NC; i++) {
            lvl[i] += vel[i] * h;
            if (lvl[i] < 0) { lvl[i] = 0; if (vel[i] < 0) vel[i] = 0; }
            if (!(lvl[i] < 4.0f * H)) { lvl[i] = 0; vel[i] = 0; }    // never let a glitch spread (also catches NaN)
        }
    }

    // slowly soaks away; much faster when it's above the maximum
    float m = meanLevel();
    float drain = m / 90.0f;
    if (m > maxLevel()) drain += (m - maxLevel()) * 1.5f;
    if (drain > 0) {
        float d = drain * dt;
        for (int i = 0; i < NC; i++) lvl[i] = std::max(0.0f, lvl[i] - d);
    }
}

void Rain::stepPuddles(float dt) {
    const float colW = 4 * S;
    const float maxH = 3.2f * S;
    int glassCount = 0;
    for (const auto& d : drops) if (d.kind == D_GLASS) glassCount++;
    int glassCap = (int)(30 + 40 * std::sqrt(intensity));

    for (auto& kv : puddles) {
        Puddle& p = kv.second;
        int n = (int)p.h.size();
        if (n == 0) continue;
        // water spreads along the top and runs off the ends
        for (int i = 0; i + 1 < n; i++) {
            float f = (p.h[i] - p.h[i + 1]) * std::min(1.0f, dt * 12);
            p.h[i] -= f * 0.5f; p.h[i + 1] += f * 0.5f;
        }
        float spillVol[2] = {0, 0};
        for (int e = 0; e < 2; e++) {
            int i = e == 0 ? 0 : n - 1;
            if (p.h[i] > 0.15f * S) {
                float out = p.h[i] * std::min(1.0f, dt * 4);
                p.h[i] -= out;
                spillVol[e] += out * colW;
            }
        }
        for (int i = 0; i < n; i++)
            if (p.h[i] > maxH) {
                float ex = p.h[i] - maxH;
                p.h[i] = maxH;
                spillVol[i < n / 2 ? 0 : 1] += ex * colW;
            }
        for (int e = 0; e < 2; e++) {
            if (spillVol[e] <= 0) continue;
            Drop d{};
            d.kind = D_RUNNER;
            d.win = kv.first;
            d.ox = e == 0 ? -1.0f : (float)(p.r - p.l);
            d.oy = 0;
            d.vy = rndr(140, 240) * S;
            d.vol = spillVol[e];
            d.life = 30;
            d.a = 0.7f;
            // merge into a runner that just started on this side, so it's a trickle, not a swarm
            bool merged = false;
            for (auto& o : drops)
                if (o.kind == D_RUNNER && o.win == kv.first && std::fabs(o.ox - d.ox) < 1 && o.oy < 12 * S) { o.vol += d.vol; merged = true; break; }
            if (!merged) drops.push_back(d);
        }
        // shaken or yanked: the water sloshes off
        float shake = std::sqrt(p.vx * p.vx + p.vy * p.vy);
        if (shake > 650 * S) {
            for (int i = 0; i < n; i++) {
                if (p.h[i] < 0.2f * S || !p.vis[i]) continue;
                float v = p.h[i] * colW * 0.6f;
                p.h[i] *= 0.4f;
                Drop d{};
                d.kind = D_SPRAY;
                d.x = p.l + (i + rnd()) * colW; d.y = p.t - 1;
                d.vx = -p.vx * rndr(0.2f, 0.5f) + rndr(-60, 60) * S;
                d.vy = -std::fabs(p.vy) * 0.2f - rndr(60, 220) * S;
                d.vol = v; d.life = 3; d.a = 0.8f; d.len = rndr(0.8f, 1.5f) * S;
                drops.push_back(d);
            }
        }
        // new drops landing on the glass
        float area = (float)(p.r - p.l) * (p.b - p.t) / (S * S);
        p.glassAcc += dt * intensity * area / 160000.0f * (burst > 0 ? 3.0f : 1.0f);
        while (p.glassAcc >= 1) {
            p.glassAcc -= 1;
            if (glassCount >= glassCap) continue;
            float x = rndr((float)p.l + 8 * S, (float)p.r - 8 * S), y = rndr((float)p.t + 14 * S, (float)p.b - 20 * S);
            if (y >= p.b - 20 * S || !visibleOn(kv.first, x, y)) continue;
            Drop d{};
            d.kind = D_GLASS;
            d.win = kv.first;
            d.ox = x - p.l; d.oy = y - p.t;
            d.vol = rndr(4, 14) * S * S;
            d.hold = rndr(0.5f, 4.0f);
            d.life = rndr(10, 28);
            d.a = 1;
            drops.push_back(d);
            glassCount++;
        }
    }
}

void Rain::stepDrops(float dt) {
    const float G = 1500 * S;
    const float cr = 10 * S;   // cursor radius for splashing
    for (size_t i = 0; i < drops.size();) {
        Drop& d = drops[i];
        bool kill = false;
        switch (d.kind) {
        case D_RAIN: {
            float y0 = d.y;
            d.vy += 200 * S * dt;
            d.x += d.vx * dt; d.y += d.vy * dt;
            if (d.x < -200 * S || d.x > W + 200 * S) { kill = true; break; }
            // the cursor is an umbrella
            if (cursorPhysics && cursorValid && std::fabs(d.x - curX) < cr && y0 < curY + cr && d.y > curY - cr) {
                splash(curX + (d.x - curX), curY - cr * 0.5f, curVx * 0.2f, 0, 2 + (int)(rnd() * 2), 0, 160 * S);
                kill = true; break;
            }
            for (auto& b : balls)
                if ((d.x - b.x) * (d.x - b.x) + (d.y - b.y) * (d.y - b.y) < b.r * b.r) { b.vol += d.vol; b.wobV += 0.4f; kill = true; break; }
            if (kill) break;
            float ys = surfaceY(d.x);
            float top;
            Puddle* p = (y0 < ys) ? puddleAt(d.x, y0, std::min(d.y, ys), top) : nullptr;
            if (p) {
                int c = (int)((d.x - p->l) / (4 * S));
                p->h[c] += d.vol * 0.7f / (4 * S);
                if (rnd() < 0.6f) splash(d.x, top, d.vx * 0.1f, 0, 1 + (int)(rnd() * 2), 0, 110 * S);
                kill = true; break;
            }
            if (d.y >= ys) {
                addWater(d.x, d.vol, 6 * S);
                if (rnd() < 0.45f) splash(d.x, ys, d.vx * 0.1f, 0, 1 + (int)(rnd() * 2), 0, 130 * S);
                kill = true; break;
            }
            break;
        }
        case D_FAR:
            d.x += d.vx * dt; d.y += d.vy * dt; d.life -= dt;
            if (d.life <= 0 || d.y >= surfaceY(d.x)) kill = true;
            break;
        case D_SPRAY: {
            float y0 = d.y;
            d.vy += G * dt;
            d.vx *= (1 - 0.6f * dt);
            d.x += d.vx * dt; d.y += d.vy * dt;
            d.life -= dt;
            if (d.life <= 0 || d.x < -20 * S || d.x > W + 20 * S || d.y > H + 20 * S) { kill = true; break; }
            if (d.vy > 0) {
                float ys = surfaceY(d.x), top;
                Puddle* p = (y0 < ys) ? puddleAt(d.x, y0, std::min(d.y, ys), top) : nullptr;
                if (p) {
                    int c = (int)((d.x - p->l) / (4 * S));
                    p->h[c] += d.vol / (4 * S);
                    kill = true; break;
                }
                if (d.y >= ys) {
                    addWater(d.x, d.vol, std::min(d.vy, 900 * S) * 0.01f + (d.vol > 20 * S * S ? 20 * S : 0));
                    if (d.vol > 30 * S * S && rnd() < 0.5f) splash(d.x, ys, 0, 0, 2, 0, 120 * S);
                    kill = true; break;
                }
            }
            break;
        }
        case D_GLASS: case D_RUNNER: {
            auto it = puddles.find(d.win);
            if (it == puddles.end()) { d.kind = D_SPRAY; d.life = 4; d.vy = 30 * S; break; }
            Puddle& p = it->second;
            float shake = std::sqrt(p.vx * p.vx + p.vy * p.vy);
            if (shake > 650 * S) {                 // flung off the glass
                d.x = p.l + d.ox; d.y = p.t + d.oy;
                d.kind = D_SPRAY; d.vx = -p.vx * rndr(0.3f, 0.6f); d.vy = -p.vy * 0.3f - rndr(20, 120) * S; d.life = 3; d.len = 1.2f * S;
                break;
            }
            if (d.kind == D_GLASS) {
                d.life -= dt;
                if (d.hold > 0) {
                    d.hold -= dt;
                    d.trail = std::max(0.0f, d.trail - 6 * S * dt);
                } else {
                    float sp = (50 + d.vol / (S * S) * 6) * S;   // heavier drops run faster
                    d.oy += sp * dt;
                    d.trail = std::min(d.trail + sp * dt, 70 * S);
                    d.ox += rndr(-1, 1) * 8 * S * dt;
                    if (rnd() < dt * 0.9f) d.hold = rndr(0.2f, 1.8f);
                    // picks up smaller drops on its way
                    for (auto& o : drops)
                        if (&o != &d && o.kind == D_GLASS && o.win == d.win && o.life > 0 &&
                            std::fabs(o.ox - d.ox) < 3 * S && o.oy > d.oy - 2 * S && o.oy < d.oy + 3 * S) { d.vol += o.vol; o.life = -1; o.vol = 0; }
                }
                if (d.life <= 0) { kill = true; break; }       // dried up (or merged into another drop)
            } else {
                d.oy += d.vy * dt;
                d.ox += 0;
            }
            d.x = p.l + d.ox; d.y = p.t + d.oy;
            if (d.y >= p.b - 2 * S) {                // reached the bottom edge: falls
                d.vy = (d.kind == D_RUNNER ? 120 : 40) * S;
                d.kind = D_SPRAY; d.y = (float)p.b; d.vx = p.vx * 0.2f; d.life = 4; d.len = 1.3f * S;
            }
            break;
        }
        case D_FOAM:
            d.life -= dt;
            if (d.life <= 0 || swirl < 0.03f) { kill = true; break; }
            d.ox += swirlDir * (2.5f + 3.0f * 40 * S / std::max(d.oy, 4 * S)) * dt;   // angle
            d.oy -= d.oy * 0.25f * dt + 4 * S * dt;                                     // spirals inwards
            if (d.oy < 3 * S) { kill = true; break; }
            break;
        }
        if (kill) { drops[i] = drops.back(); drops.pop_back(); }
        else i++;
    }
}

// ---------------------------------------------------------------- water balls

void Rain::stepBalls(float dt) {
    const float G = 1400 * S;
    const float maxVol = PI * (46 * S) * (46 * S);
    for (size_t i = 0; i < balls.size();) {
        Ball& b = balls[i];
        // jiggle
        b.wobV += (-120.0f * b.wob - 6.0f * b.wobV) * dt;
        b.wob += b.wobV * dt;
        b.wob = clampf(b.wob, -0.25f, 0.25f);
        if (b.held) {
            float tx = hist.empty() ? curX : hist.back().x, ty = hist.empty() ? curY : hist.back().y;
            float k = std::min(1.0f, dt * 22);
            float ox = b.x, oy = b.y;
            b.x += (tx - b.x) * k; b.y += (ty - b.y) * k;
            b.vx = (b.x - ox) / dt; b.vy = (b.y - oy) / dt;
            // scooping: water flows into the ball while it's in the pool
            if (poolBelow(b.x, b.y) > 0 && b.vol < maxVol) {
                float rate = (2200 + b.vol * 1.5f / (S * S)) * S * S;
                float got = takeWater(b.x, 36 * S + b.r, std::min(rate * dt, maxVol - b.vol));
                if (got > 0) { b.vol += got; b.wobV += got / (S * S) * 0.002f; }
            }
            b.r = std::sqrt(b.vol / PI);
            i++;
            continue;
        }
        b.vy += G * dt;
        b.x += b.vx * dt; b.y += b.vy * dt;
        b.r = std::sqrt(b.vol / PI);
        bool burstIt = false, intoPool = false;
        if (b.x - b.r < 0 || b.x + b.r > W || b.y + b.r < -200 * S) burstIt = true;
        if (b.x - b.r < 0) b.x = b.r;
        if (b.x + b.r > W) b.x = W - b.r;
        if (!burstIt && b.vy > 0 && b.y + b.r * 0.4f >= surfaceY(b.x)) { burstIt = true; intoPool = true; }
        if (!burstIt && time - b.born > 0.05) {
            // hitting a window
            for (int k = 0; k < 5 && !burstIt; k++) {
                float a = k * PI * 0.5f;
                float px = b.x + (k < 4 ? std::cos(a) * b.r * 0.8f : 0), py = b.y + (k < 4 ? std::sin(a) * b.r * 0.8f : 0);
                int wi = topWindowAt(px, py);
                if (wi >= 0 && wins[wi].pileable && wins[wi].id != b.ignoreWin) burstIt = true;
            }
            if (b.ignoreWin) {
                int wi = topWindowAt(b.x, b.y);
                if (wi < 0 || wins[wi].id != b.ignoreWin) b.ignoreWin = 0;    // left that window: it's solid again
            }
        }
        if (burstIt) { burstBall((int)i, intoPool); continue; }
        i++;
    }
}

void Rain::burstBall(int i, bool intoPool) {
    Ball b = balls[i];
    balls.erase(balls.begin() + i);
    if (held == i) held = -1;
    else if (held > i) held--;
    // most of the water turns to spray and mist and is gone; about a quarter comes back down
    float back = b.vol * 0.25f;
    int n = (int)clampf(10 + b.vol / (90 * S * S), 10, 70);
    for (int k = 0; k < n; k++) {
        Drop d{};
        d.kind = D_SPRAY;
        float ang = rndr(0, 2 * PI);
        float sp = rndr(120, 480) * S * (0.7f + std::min(b.r / (40 * S), 1.0f) * 0.5f);
        d.x = b.x + std::cos(ang) * b.r * 0.7f; d.y = b.y + std::sin(ang) * b.r * 0.7f;
        d.vx = std::cos(ang) * sp + b.vx * 0.25f;
        d.vy = std::sin(ang) * sp + b.vy * 0.15f;
        if (intoPool) d.vy = -std::fabs(d.vy) * rndr(0.8f, 1.5f) - 120 * S;     // a fountain up out of the water
        d.vol = back / n;
        d.life = rndr(0.8f, 2.2f);
        d.a = rndr(0.6f, 0.9f);
        d.len = rndr(1.0f, 2.0f) * S;
        drops.push_back(d);
    }
    if (intoPool) {   // the impact throws a big wave (no water added: the spray brings back what returns)
        int c = colOf(b.x);
        int rad = std::max(2, (int)(b.r / cw));
        for (int k = -rad; k <= rad; k++) {
            int j = c + k;
            if (j < 0 || j >= NC) continue;
            kick(j, std::min(b.vy, 1500 * S) * 0.35f * (1 - std::fabs((float)k) / (rad + 1)));
        }
    }
}

void Rain::mouseDown(float x, float y, double t) {
    hist.clear();
    hist.push_back({x, y, t});
    curX = x; curY = y;
    for (int i = (int)balls.size() - 1; i >= 0; i--) {    // catch a flying ball
        Ball& b = balls[i];
        if ((x - b.x) * (x - b.x) + (y - b.y) * (y - b.y) < (b.r + 6 * S) * (b.r + 6 * S)) {
            b.held = true; b.ignoreWin = 0; held = i; b.wobV += 1.0f;
            return;
        }
    }
    if (poolBelow(x, y) > 0) {
        Ball b{};
        b.x = x; b.y = y; b.vol = 0; b.r = 0; b.held = true; b.born = time;
        balls.push_back(b);
        held = (int)balls.size() - 1;
    }
}

void Rain::mouseMove(float x, float y, double t) {
    hist.push_back({x, y, t});
    while (hist.size() > 2 && t - hist.front().t > 0.15) hist.erase(hist.begin());
}

void Rain::mouseUp(float x, float y, double t) {
    mouseMove(x, y, t);
    if (held < 0 || held >= (int)balls.size()) { held = -1; return; }
    Ball& b = balls[held];
    b.held = false;
    float vx = 0, vy = 0;
    for (const auto& s : hist) {
        double d = t - s.t;
        if (d > 0.012 && d <= 0.12) { vx = (float)((x - s.x) / d); vy = (float)((y - s.y) / d); break; }
    }
    float sp = std::sqrt(vx * vx + vy * vy), mx = 2600 * S;
    if (sp > mx) { vx *= mx / sp; vy *= mx / sp; }
    if (b.vol < 60 * S * S) {                    // just a click: nothing scooped
        addWater(b.x, b.vol, 0);
        balls.erase(balls.begin() + held);
        held = -1;
        return;
    }
    b.vx = vx; b.vy = vy; b.born = time;
    int wi = topWindowAt(b.x, b.y);
    b.ignoreWin = wi >= 0 ? wins[wi].id : 0;
    held = -1;
}

void Rain::cancelHold() {
    if (held >= 0 && held < (int)balls.size()) { balls[held].held = false; balls[held].vx = balls[held].vy = 0; balls[held].born = time; }
    held = -1;
}

// ---------------------------------------------------------------- cursor & whirlpool

void Rain::setCursor(float x, float y, double t, bool valid) {
    if (valid && cursorValid && t > curT) {
        float dt = (float)(t - curT);
        if (dt > 0.001f) {
            float k = std::min(1.0f, dt / 0.04f);
            curVx += ((x - curX) / dt - curVx) * k;
            curVy += ((y - curY) / dt - curVy) * k;
        }
    } else {
        curVx = curVy = 0;
    }
    // crossing the water surface fast makes a splash
    if (valid && cursorValid && cursorPhysics && held < 0) {
        bool was = poolBelow(curX, curY) > 0, now = poolBelow(x, y) > 0;
        float sp = std::sqrt(curVx * curVx + curVy * curVy);
        if (was != now && sp > 500 * S && meanLevel() > 4 * S)
            splash(x, surfaceY(x), curVx * 0.2f, 0, 4, 0, std::min(sp * 0.3f, 400 * S));
    }
    curX = x; curY = y; curT = t; cursorValid = valid;
}

void Rain::stepSwirl(float dt) {
    // stirring: fast, curving cursor movement under the water builds the whirlpool up; it fades by itself
    float sp = std::sqrt(curVx * curVx + curVy * curVy);
    bool inWater = cursorPhysics && cursorValid && held < 0 && poolBelow(curX, curY) > 6 * S && meanLevel() > 12 * S;
    if (inWater && sp > 220 * S) {
        float ang = std::atan2(curVy, curVx);
        float d = ang - curAng;
        while (d > PI) d -= 2 * PI;
        while (d < -PI) d += 2 * PI;
        if (std::fabs(d) < 1.2f) {
            float gain = std::fabs(d) * std::min(sp / (600 * S), 2.0f);
            swirlE += gain;
            if (gain > 0.01f) swirlDir += ((d > 0 ? 1.0f : -1.0f) - swirlDir) * 0.1f;
        }
        swirlX += (curX - swirlX) * std::min(1.0f, dt * (swirl < 0.05f ? 30.0f : 2.0f));
        curAng = ang;
    }
    swirlE -= dt * (0.6f + swirlE * 0.12f);
    swirlE = clampf(swirlE, 0, 9);
    float target = clampf((swirlE - 1.0f) / 5.0f, 0, 1);
    swirl += (target - swirl) * std::min(1.0f, dt * 2.0f);
    if (swirl > 0.08f) {
        foamAcc += dt * swirl * 50;
        while (foamAcc >= 1) {
            foamAcc -= 1;
            Drop d{};
            d.kind = D_FOAM;
            d.ox = rndr(0, 2 * PI);
            d.oy = rndr(20, 75) * S;
            d.life = rndr(1.5f, 3.5f);
            d.a = rndr(0.4f, 0.75f);
            drops.push_back(d);
        }
    }
}

// ---------------------------------------------------------------- input areas

int Rain::hitTest(float x, float y) const {
    for (int i = (int)balls.size() - 1; i >= 0; i--) {
        const Ball& b = balls[i];
        if ((x - b.x) * (x - b.x) + (y - b.y) * (y - b.y) < (b.r + 6 * S) * (b.r + 6 * S)) return 1;
    }
    int c = colOf(x);
    if (lvl[c] > 8 * S && poolBelow(x, y) > 2 * S) return 2;
    return 0;
}

void Rain::inputRects(std::vector<IRect>& out) const {
    out.clear();
    float top = (float)H;
    bool any = false;
    for (int i = 0; i < NC; i++) if (lvl[i] > 8 * S) { any = true; top = std::min(top, H - lvl[i]); }
    if (any) {
        int y = (int)(top + 2 * S);
        if (y < H) out.push_back({0, y, W, H - y});
    }
    for (const auto& b : balls) {
        int r = (int)(b.r + 6 * S);
        out.push_back({(int)b.x - r, (int)b.y - r, 2 * r, 2 * r});
    }
}

// ---------------------------------------------------------------- menu actions & saving

void Rain::clearSnow() {
    std::fill(lvl.begin(), lvl.end(), 0.0f);
    std::fill(vel.begin(), vel.end(), 0.0f);
    drops.clear(); balls.clear(); held = -1;
    for (auto& kv : puddles) std::fill(kv.second.h.begin(), kv.second.h.end(), 0.0f);
    swirlE = swirl = 0;
}

void Rain::addDrift() {
    burst = 4.0f;    // a short downpour, and the pool rises
    float add = std::min(0.04f * H, std::max(0.0f, maxLevel() - meanLevel()));
    for (int i = 0; i < NC; i++) lvl[i] += add;
    for (int k = 0; k < 6; k++) kick((int)(rnd() * NC), rndr(200, 500) * S);
}

std::vector<uint8_t> Rain::save() const {
    std::vector<uint8_t> d(12);
    memcpy(d.data(), "RAINDSK1", 8);
    float m = meanLevel() / S;
    memcpy(d.data() + 8, &m, 4);
    return d;
}

bool Rain::load(const std::vector<uint8_t>& d) {
    if (d.size() < 12 || memcmp(d.data(), "RAINDSK1", 8) != 0) return false;
    float m;
    memcpy(&m, d.data() + 8, 4);
    if (!(m >= 0) || m > 10000) return false;
    m = std::min(m * S, maxLevel());
    std::fill(lvl.begin(), lvl.end(), m);
    std::fill(vel.begin(), vel.end(), 0.0f);
    return true;
}

// ---------------------------------------------------------------- drawing

void Rain::render(uint32_t* px) const {
    Canvas cv{px, W, H};

    // far rain, behind everything
    for (const auto& d : drops)
        if (d.kind == D_FAR) {
            float k = d.len / d.vy;
            cv.line(d.x - d.vx * k, d.y - d.len, d.x, d.y, 175, 195, 220, 0, d.a);
        }

    // water on the windows: the film on the top edge, drops and their trails on the glass, trickles down the sides
    const float colW = 4 * S;
    for (const auto& kv : puddles) {
        const Puddle& p = kv.second;
        for (size_t c = 0; c < p.h.size(); c++) {
            if (!p.vis[c] || p.h[c] < 0.15f) continue;
            int x0 = p.l + (int)(c * colW), x1 = p.l + (int)((c + 1) * colW);
            float top = p.t - p.h[c];
            for (int x = std::max(0, x0); x < std::min(W, x1); x++) {
                int iy = (int)std::floor(top);
                for (int y = iy; y < p.t; y++) {
                    float cov = y == iy ? (iy + 1 - top) : 1.0f;
                    cv.put(x, y, 110, 175, 235, 0.5f * cov);
                }
                cv.put(x, iy, 225, 242, 255, 0.55f * (iy + 1 - top));
            }
        }
    }
    for (const auto& d : drops) {
        if (d.kind != D_GLASS && d.kind != D_RUNNER) continue;
        if (!visibleOn(d.win, d.x, d.y + 1)) continue;
        if (d.kind == D_GLASS) {
            float r = (1.1f + std::sqrt(d.vol) / S * 0.22f) * S;
            if (d.trail > 1) cv.line(d.x, d.y - d.trail, d.x, d.y - r, 120, 165, 215, 0.0f, 0.3f);
            cv.dot(d.x, d.y + 0.5f, r + 0.8f, 40, 70, 110, 0.38f);
            cv.dot(d.x, d.y, r, 165, 205, 245, 0.6f);
            cv.dot(d.x - r * 0.35f, d.y - r * 0.35f, std::max(0.5f, r * 0.35f), 255, 255, 255, 0.85f);
        } else {
            float r = (1.0f + std::min(std::sqrt(d.vol) / S * 0.12f, 1.2f)) * S;
            cv.line(d.x, d.y - 10 * S, d.x, d.y, 150, 200, 245, 0.0f, 0.5f);
            cv.dot(d.x, d.y, r, 185, 220, 252, 0.75f);
        }
    }

    // the pool
    {
        std::vector<float> sx(W), sy(H);
        float t = (float)time;
        for (int x = 0; x < W; x++) sx[x] = std::sin(x * 0.043f / S + t * 1.6f) * std::sin(x * 0.011f / S - t * 0.55f);
        for (int y = 0; y < H; y++) sy[y] = std::sin(y * 0.11f / S - t * 2.1f);
        float deep = std::max(1.0f, maxLevel());
        for (int x = 0; x < W; x++) {
            float top = surfaceY(x + 0.5f);
            if (top >= H - 0.3f) continue;
            int iy = std::max(0, (int)std::floor(top));
            if (iy - 1 >= 0) cv.put(x, iy - 1, 50, 80, 110, 0.12f);        // keeps the line visible over white
            for (int y = iy; y < H; y++) {
                float d = y + 0.5f - top;
                float cov = y == iy ? clampf(iy + 1 - top, 0, 1) : 1.0f;
                float k = clampf(d / deep, 0, 1);
                int r = (int)(78 - 55 * k), g = (int)(158 - 72 * k), b = (int)(222 - 62 * k);
                float a = 0.26f + 0.22f * k + 0.05f * sx[x] * sy[y];
                if (d < 1.8f * S) { r = 215; g = 238; b = 255; a = 0.75f; }   // the shiny surface line
                else if (d < 5 * S) { r += 40; g += 30; b += 15; a += 0.04f; }
                cv.put(x, y, std::min(r, 255), std::min(g, 255), std::min(b, 255), a * cov);
            }
        }
    }

    // whirlpool foam
    if (swirl > 0.02f)
        for (const auto& d : drops) {
            if (d.kind != D_FOAM) continue;
            float x = swirlX + std::cos(d.ox) * d.oy;
            float y = surfaceY(x) + 3 * S + (std::sin(d.ox) * 0.18f + 0.15f) * d.oy;
            cv.dot(x, y, 1.1f * S, 240, 250, 255, d.a * std::min(1.0f, swirl * 2) * std::min(1.0f, d.life));
        }

    // splashes and spray
    for (const auto& d : drops) {
        if (d.kind != D_SPRAY) continue;
        float r = std::max(0.7f * S, d.len);
        cv.dot(d.x, d.y + 0.4f, r + 0.6f, 50, 80, 115, 0.18f * d.a);
        cv.dot(d.x, d.y, r, 205, 228, 252, d.a * std::min(1.0f, d.life * 3));
    }

    // rain
    for (const auto& d : drops) {
        if (d.kind != D_RAIN) continue;
        float k = d.len / d.vy;
        float x0 = d.x - d.vx * k, y0 = d.y - d.len;
        cv.line(x0 + 1, y0, d.x + 1, d.y, 60, 85, 120, 0, 0.28f * d.a);   // faint shadow edge: visible over white
        cv.line(x0, y0, d.x, d.y, 210, 228, 250, 0, d.a);
    }

    // water balls
    for (const auto& b : balls) {
        if (b.r < 2) continue;
        float sxk = 1 + b.wob, syk = 1 - b.wob;
        float rx = b.r * sxk, ry = b.r * syk;
        int x0 = std::max(0, (int)(b.x - rx - 2)), x1 = std::min(W - 1, (int)(b.x + rx + 2));
        int y0 = std::max(0, (int)(b.y - ry - 2)), y1 = std::min(H - 1, (int)(b.y + ry + 2));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float nx = (x + 0.5f - b.x) / rx, ny = (y + 0.5f - b.y) / ry;
                float dd = nx * nx + ny * ny;
                if (dd > 1.0f + 2.0f / b.r) continue;
                float cov = clampf((1.0f - std::sqrt(dd)) * b.r + 0.5f, 0, 1);
                float nz = std::sqrt(std::max(0.0f, 1 - dd));
                float edge = 1 - nz;                                       // more opaque towards the rim
                float a = 0.30f + 0.45f * edge * edge;
                int r = (int)(70 + 40 * nz), g = (int)(150 + 40 * nz), bl = (int)(220 + 25 * nz);
                float spec = std::max(0.0f, -0.45f * nx - 0.55f * ny + 0.7f * nz);
                spec = std::pow(spec, 18.0f);
                cv.put(x, y, r, g, bl, a * cov);
                if (spec > 0.02f) cv.put(x, y, 255, 255, 255, spec * 0.95f * cov);
            }
    }
}
