// SnowDesk — platform-independent snow simulation.
#include "sim.h"
#include <cmath>
#include <cstring>
#include <algorithm>

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

static const float FLAKE_MASS = 3.0f;
static const float PI = 3.14159265f;

float World::rnd() {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (rng >> 8) * (1.0f / 16777216.0f);
}

float World::groundCap() const {
    return std::min(110.0f * S, groundY * 0.25f);
}

void World::init(int w, int h, int gy, float scale) {
    W = w; H = h; groundY = gy; S = scale < 0.5f ? 0.5f : scale;
    ground.assign(W, 0.0f);
    piles.clear(); wins.clear(); pileOrder.clear(); pileWinIdx.clear();
    parts.clear(); balls.clear(); held = -1;
    int target = (int)(intensity * 260.0f * (float)W * H / (1920.0f * 1080.0f));
    for (int i = 0; i < target; i++) spawnFlake(true);
}

void World::resize(int w, int h, int gy) {
    std::vector<float> old = ground;
    int oldW = W;
    W = w; H = h; groundY = gy;
    ground.assign(W, 0.0f);
    if (oldW > 0)
        for (int x = 0; x < W; x++) ground[x] = old[std::min(oldW - 1, x * oldW / W)];
    piles.clear(); wins.clear(); pileOrder.clear(); pileWinIdx.clear();
    for (auto& b : balls) { b.x = clampf(b.x, b.r, W - b.r); if (b.state == BALL_REST && b.support < 0) restPosition(b); }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& p) { return p.kind == 1; }), parts.end());
}

void World::spawnFlake(bool anywhere) {
    Particle p;
    p.kind = rnd() < 0.62f ? 0 : 2;   // 0 = front flake (lands on windows), 2 = far flake (falls behind windows)
    p.size = (p.kind == 0 ? (1.4f + rnd() * 1.4f) : (0.9f + rnd() * 0.9f)) * S;
    p.x = rndr(-60.0f, (float)W + 60.0f);
    p.y = anywhere ? rndr(-20.0f, (float)groundY) : rndr(-30.0f, -4.0f);
    p.vy = (30.0f + p.size / S * 22.0f + rndr(0, 12)) * S;
    p.vx = 0;
    p.phase = rndr(0, 2 * PI);
    p.amp = (8.0f + rnd() * 16.0f) * S;
    p.freq = 0.6f + rnd() * 1.3f;
    p.mass = FLAKE_MASS * S;
    parts.push_back(p);
}

void World::spawnChunk(float x, float y, float vx, float vy, float mass) {
    Particle p;
    p.kind = 1; p.x = x; p.y = y; p.vx = vx; p.vy = vy; p.mass = mass;
    p.size = clampf(0.8f * std::sqrt(mass), 1.0f * S, 3.2f * S);
    p.phase = p.amp = p.freq = 0;
    parts.push_back(p);
}

// ---------------------------------------------------------------- windows

void World::dropPile(Pile& p) {
    for (int c = 0; c < p.w; c += 2) {
        float m = p.h[c] + (c + 1 < p.w ? p.h[c + 1] : 0.0f);
        if (m < 0.2f) continue;
        spawnChunk((float)(p.l + c + 1), (float)p.t - m * 0.3f, rndr(-45, 45) * S, rndr(-40, 5) * S, m);
    }
    std::fill(p.h.begin(), p.h.end(), 0.0f);
    p.spillL = p.spillR = 0;
}

void World::setWindows(const std::vector<WinRect>& ws) {
    wins = ws;
    for (auto& kv : piles) kv.second.seen = false;

    for (size_t i = 0; i < wins.size(); i++) {
        const WinRect& w = wins[i];
        if (!w.pileable) continue;
        int width = w.r - w.l;
        if (width <= 0) continue;
        Pile& P = piles[w.id];
        if (P.w != width || P.l != w.l || P.t != w.t) {
            if (P.w > 0) dropPile(P);           // moved / resized: snow falls off
            P.l = w.l; P.t = w.t; P.w = width;
            P.h.assign(width, 0.0f);
            P.vis.assign(width, 0);
        }
        P.seen = true;
        float sy = (float)w.t - 1.5f;
        for (int c = 0; c < width; c++) {
            int x = w.l + c;
            bool v = x >= 0 && x < W && w.t > 2 && w.t < groundY - 2;
            if (v) {
                float sx = x + 0.5f;
                for (size_t j = 0; j < i; j++) {
                    const WinRect& o = wins[j];
                    if (sx >= o.l && sx < o.r && sy >= o.t && sy < o.b) { v = false; break; }
                }
            }
            P.vis[c] = v ? 1 : 0;
        }
    }
    // windows that disappeared (closed, minimized, maximized, hidden): snow falls down
    for (auto it = piles.begin(); it != piles.end();) {
        if (!it->second.seen) { dropPile(it->second); it = piles.erase(it); }
        else ++it;
    }
    pileOrder.clear(); pileWinIdx.clear();
    for (size_t i = 0; i < wins.size(); i++) {
        if (!wins[i].pileable) continue;
        auto it = piles.find(wins[i].id);
        if (it != piles.end()) { pileOrder.push_back(&it->second); pileWinIdx.push_back((int)i); }
    }
}

uint64_t World::windowAt(float x, float y) const {
    for (const auto& w : wins)
        if (x >= w.l && x < w.r && y >= w.t && y < w.b) return w.id;
    return 0;
}

// ---------------------------------------------------------------- piles

void World::deposit(std::vector<float>& h, int c, float m, float cap) {
    int n = (int)h.size();
    int rad = m <= 4.0f * S ? 1 : std::min(6, (int)std::sqrt(m));
    float wsum = 0;
    for (int k = -rad; k <= rad; k++) wsum += (float)(rad + 1 - std::abs(k));
    for (int k = -rad; k <= rad; k++) {
        int i = c + k;
        if (i < 0 || i >= n) continue;
        float add = m * (rad + 1 - std::abs(k)) / wsum;
        if (h[i] < cap) h[i] = std::min(cap, h[i] + add);
    }
}

void World::relax(std::vector<float>& h, float slope, Pile* p) {
    int n = (int)h.size();
    if (n < 2) return;
    if (relaxDir & 1) {
        for (int i = 0; i < n - 1; i++) {
            float d = h[i] - h[i + 1];
            if (d > slope) { float m = (d - slope) * 0.3f; h[i] -= m; h[i + 1] += m; }
            else if (d < -slope) { float m = (-d - slope) * 0.3f; h[i] += m; h[i + 1] -= m; }
        }
    } else {
        for (int i = n - 2; i >= 0; i--) {
            float d = h[i] - h[i + 1];
            if (d > slope) { float m = (d - slope) * 0.3f; h[i] -= m; h[i + 1] += m; }
            else if (d < -slope) { float m = (-d - slope) * 0.3f; h[i] += m; h[i + 1] -= m; }
        }
    }
    // gentle diffusion smooths out saw-tooth columns (mass-conserving)
    for (int i = 0; i < n - 1; i++) {
        float f = 0.08f * (h[i] - h[i + 1]);
        h[i] -= f; h[i + 1] += f;
    }
    if (p) { // snow slides off the window edges
        float edge = 1.5f * S;
        if (h[0] > edge) { float e = (h[0] - edge) * 0.35f; h[0] -= e; p->spillL += e; }
        if (h[n - 1] > edge) { float e = (h[n - 1] - edge) * 0.35f; h[n - 1] -= e; p->spillR += e; }
        if (p->spillL >= 2.0f) { spawnChunk(p->l - 1.5f, p->t - 1.0f, -20.0f * S, 0, p->spillL); p->spillL = 0; }
        if (p->spillR >= 2.0f) { spawnChunk(p->l + p->w + 0.5f, p->t - 1.0f, 20.0f * S, 0, p->spillR); p->spillR = 0; }
    }
}

bool World::tryLand(Particle& p, float y0) {
    int cx = (int)std::floor(p.x);
    if (cx < 0 || cx >= W) return false;
    for (size_t k = 0; k < pileOrder.size() && p.kind != 2; k++) {
        Pile& P = *pileOrder[k];
        if (cx < P.l || cx >= P.l + P.w) continue;
        int c = cx - P.l;
        if (!P.vis[c]) continue;
        float surf = P.t - P.h[c];
        if (y0 <= surf && p.y >= surf) {
            deposit(P.h, c, p.mass, pileCap());
            return true;
        }
    }
    float gs = groundY - ground[cx];
    if (p.y >= gs) { deposit(ground, cx, p.mass, groundCap()); return true; }
    return false;
}

// ---------------------------------------------------------------- step

void World::step(float dt) {
    if (W <= 0) return;
    time += dt;
    wind = (float)(14.0 * std::sin(time * 0.05) + 7.0 * std::sin(time * 0.13 + 1.3)) * S;

    int flakes = 0;
    for (const auto& p : parts) if (p.kind != 1) flakes++;
    int target = (int)(intensity * 260.0f * (float)W * H / (1920.0f * 1080.0f));
    target = std::min(target, 4000);
    int deficit = target - flakes;
    int maxSpawn = std::max(1, (int)(target * dt * 0.25f) + 1);
    for (int i = 0; i < deficit && i < maxSpawn; i++) spawnFlake(false);

    float g = 1000.0f * S;
    for (size_t i = 0; i < parts.size();) {
        Particle& p = parts[i];
        float y0 = p.y;
        if (p.kind != 1) {
            p.x += (wind + std::sin((float)time * p.freq + p.phase) * p.amp) * dt;
            p.y += p.vy * dt;
            if (p.x < -60) p.x += W + 120;
            if (p.x > W + 60) p.x -= W + 120;
        } else {
            p.vy = std::min(p.vy + g * dt, 900.0f * S);
            p.vx *= 0.995f;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            if (p.x < 0) { p.x = 0; p.vx = -p.vx * 0.3f; }
            if (p.x > W - 1) { p.x = (float)W - 1; p.vx = -p.vx * 0.3f; }
        }
        bool gone = false;
        if (p.y > -2 && tryLand(p, y0)) gone = true;
        else if (p.y > H + 20) gone = true;
        if (gone) { parts[i] = parts.back(); parts.pop_back(); }
        else i++;
    }

    relaxDir++;
    relax(ground, 1.3f, nullptr);
    for (Pile* P : pileOrder) relax(P->h, 1.1f, P);

    stepBalls(dt);
}

// ---------------------------------------------------------------- snowballs

void World::restPosition(Ball& b) {
    int gx = clampi((int)b.x, 0, W - 1);
    b.y = groundY - ground[gx] - b.r * 0.75f;
}

bool World::isSupporting(int i) const {
    for (size_t j = 0; j < balls.size(); j++)
        if ((int)j != i && balls[j].state == BALL_REST && balls[j].support == i) return true;
    return false;
}

bool World::isTopOfStack(int i) const {
    return balls[i].state == BALL_REST && balls[i].support >= 0 && !isSupporting(i);
}

void World::collect(Ball& b, float dt) {
    const float maxVol = PI * (70.0f * S) * (70.0f * S);
    if (b.vol >= maxVol) return;
    float R = std::max(16.0f * S, b.r + 8.0f * S);
    float rate = 55.0f * S * dt;
    int x0 = clampi((int)(b.x - R), 0, W - 1), x1 = clampi((int)(b.x + R), 0, W - 1);
    for (int cx = x0; cx <= x1; cx++) {
        float dx = cx + 0.5f - b.x;
        if (std::fabs(dx) > R) continue;
        float dy = std::sqrt(R * R - dx * dx);
        // ground
        {
            float surf = groundY - ground[cx];
            float lo = std::max(surf, b.y - dy), hi = std::min((float)groundY, b.y + dy);
            float ov = hi - lo;
            if (ov > 0) { float take = std::min(std::min(ov, ground[cx]), rate); ground[cx] -= take; b.vol += take; }
        }
        // window piles
        for (Pile* P : pileOrder) {
            if (cx < P->l || cx >= P->l + P->w) continue;
            int c = cx - P->l;
            if (!P->vis[c] || P->h[c] <= 0) continue;
            float surf = P->t - P->h[c];
            float lo = std::max(surf, b.y - dy), hi = std::min((float)P->t, b.y + dy);
            float ov = hi - lo;
            if (ov > 0) { float take = std::min(std::min(ov, P->h[c]), rate); P->h[c] -= take; b.vol += take; }
        }
    }
}

void World::splat(int i) {
    Ball& b = balls[i];
    if (b.state == BALL_DEAD) return;
    int n = clampi((int)(b.vol / 5.0f), 8, 300);
    float m = b.vol / n;
    for (int k = 0; k < n; k++) {
        float a = rndr(0, 2 * PI), rr = b.r * std::sqrt(rnd());
        float sp = rndr(40, 260) * S;
        spawnChunk(b.x + std::cos(a) * rr, b.y + std::sin(a) * rr,
                   std::cos(a) * sp + b.vx * 0.15f, std::sin(a) * sp + b.vy * 0.1f - 120.0f * S, m);
    }
    b.state = BALL_DEAD;
    if (held == i) held = -1;
    for (auto& o : balls) if (o.support == i) { o.support = -1; if (o.state == BALL_REST) { o.state = BALL_FLY; o.thrown = false; } }
}

void World::compactBalls() {
    std::vector<int> remap(balls.size(), -1);
    int n = 0;
    for (size_t i = 0; i < balls.size(); i++) if (balls[i].state != BALL_DEAD) remap[i] = n++;
    std::vector<Ball> out; out.reserve(n);
    for (size_t i = 0; i < balls.size(); i++) {
        if (balls[i].state == BALL_DEAD) continue;
        Ball b = balls[i];
        if (b.support >= 0) {
            b.support = remap[b.support];
            if (b.support < 0 && b.state == BALL_REST) b.state = BALL_FLY;
        }
        out.push_back(b);
    }
    if (held >= 0) held = remap[held];
    balls.swap(out);
}

void World::stepBalls(float dt) {
    const float G = 1500.0f * S;
    const float splatSpeed = 700.0f * S;
    bool anyDead = false;

    for (size_t i = 0; i < balls.size(); i++) {
        Ball& b = balls[i];
        if (b.state == BALL_DEAD) continue;
        b.r = std::sqrt(b.vol / PI);

        if (b.state == BALL_HELD) {
            b.x = mx; b.y = my;
            if (mouseIsDown) collect(b, dt);
            b.r = std::sqrt(b.vol / PI);
            continue;
        }
        if (b.state == BALL_REST) {
            if (b.support >= 0) {
                const Ball& s = balls[b.support];
                if (s.state != BALL_REST) { b.state = BALL_FLY; b.support = -1; b.vx = b.vy = 0; b.thrown = false; }
                else { b.x = s.x; b.y = s.y - (s.r + b.r) * 0.82f; }
            } else {
                int gx = clampi((int)b.x, 0, W - 1);
                float target = groundY - ground[gx] - b.r * 0.75f;
                if (target > b.y + 2.0f * S) { b.state = BALL_FLY; b.vx = b.vy = 0; b.thrown = false; }
            }
            continue;
        }
        // flying
        b.vy += G * dt;
        b.vx *= 0.999f;
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        if (b.x - b.r < 0) { b.x = b.r; b.vx = -b.vx * 0.35f; }
        if (b.x + b.r > W) { b.x = W - b.r; b.vx = -b.vx * 0.35f; }
        float speed = std::sqrt(b.vx * b.vx + b.vy * b.vy);

        uint64_t wid = windowAt(b.x, b.y);
        if (b.thrown && wid && wid != b.lastWin && speed > 450.0f * S) { splat((int)i); anyDead = true; continue; }
        b.lastWin = wid;

        bool done = false;
        for (size_t j = 0; j < balls.size() && !done; j++) {
            if (j == i) continue;
            Ball& o = balls[j];
            if (o.state != BALL_REST) continue;
            float dx = b.x - o.x, dy = b.y - o.y;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d >= b.r + o.r * 0.95f || d < 1e-3f) continue;
            if (b.thrown && speed > splatSpeed) {
                if (speed > 1200.0f * S) splat((int)j);
                splat((int)i);
                anyDead = true; done = true;
            } else if (dy < 0 && std::fabs(dx) < o.r * 0.8f) {
                b.state = BALL_REST; b.support = (int)j; b.vx = b.vy = 0;
                b.x = o.x; b.y = o.y - (o.r + b.r) * 0.82f;
                done = true;
            } else {
                float nx = dx / d, ny = dy / d;
                b.x = o.x + nx * (b.r + o.r);
                b.y = o.y + ny * (b.r + o.r);
                float vn = b.vx * nx + b.vy * ny;
                if (vn < 0) { b.vx -= 1.4f * vn * nx; b.vy -= 1.4f * vn * ny; }
            }
        }
        if (done) continue;

        int gx = clampi((int)b.x, 0, W - 1);
        float gs = groundY - ground[gx];
        if (b.y + b.r * 0.75f >= gs) {
            if (b.thrown && speed > splatSpeed) { splat((int)i); anyDead = true; }
            else { b.state = BALL_REST; b.support = -1; b.vx = b.vy = 0; b.y = gs - b.r * 0.75f; }
            continue;
        }
        if (b.y > H + b.r) { b.state = BALL_DEAD; anyDead = true; }
    }
    if (anyDead) compactBalls();
}

// ---------------------------------------------------------------- input

void World::mouseDown(float x, float y, double t) {
    mx = x; my = y; mouseIsDown = true;
    hist.clear(); hist.push_back({x, y, t});
    if (held >= 0) return;
    for (int i = (int)balls.size() - 1; i >= 0; i--) {
        Ball& b = balls[i];
        if (b.state == BALL_DEAD || b.state == BALL_HELD) continue;
        float dx = x - b.x, dy = y - b.y;
        if (dx * dx + dy * dy < (b.r + 6 * S) * (b.r + 6 * S)) {
            for (auto& o : balls) if (o.support == i) { o.support = -1; o.state = BALL_FLY; o.vx = o.vy = 0; o.thrown = false; }
            b.state = BALL_HELD; b.support = -1; held = i;
            return;
        }
    }
    if (balls.size() >= 40) return;
    Ball b{x, y, 0, 0, 0, 0, BALL_HELD, -1, 0, false};
    balls.push_back(b);
    held = (int)balls.size() - 1;
}

void World::mouseMove(float x, float y, double t) {
    mx = x; my = y;
    hist.push_back({x, y, t});
    while (hist.size() > 2 && t - hist.front().t > 0.15) hist.erase(hist.begin());
}

void World::mouseUp(float x, float y, double t) {
    mouseMove(x, y, t);
    mouseIsDown = false;
    if (held < 0) return;
    Ball& b = balls[held];
    if (b.vol < 25.0f * S) {
        if (b.vol > 0) spawnChunk(b.x, b.y, 0, 0, b.vol);
        b.state = BALL_DEAD; held = -1; compactBalls();
        return;
    }
    float vx = 0, vy = 0;
    for (const auto& s : hist) {
        double dt = t - s.t;
        if (dt <= 0.1 && dt > 0.008) { vx = (float)((x - s.x) / dt); vy = (float)((y - s.y) / dt); break; }
    }
    float sp = std::sqrt(vx * vx + vy * vy), maxSp = 3500.0f * S;
    if (sp > maxSp) { vx *= maxSp / sp; vy *= maxSp / sp; }
    b.vx = vx; b.vy = vy; b.state = BALL_FLY; b.support = -1;
    b.thrown = sp > 350.0f * S;
    b.lastWin = windowAt(b.x, b.y);
    held = -1;
}

void World::cancelHold() {
    mouseIsDown = false;
    if (held >= 0) {
        Ball& b = balls[held];
        b.state = BALL_FLY; b.vx = b.vy = 0; b.thrown = false; b.lastWin = windowAt(b.x, b.y);
        held = -1;
    }
}

// ---------------------------------------------------------------- misc

void World::clearSnow() {
    std::fill(ground.begin(), ground.end(), 0.0f);
    for (auto& kv : piles) std::fill(kv.second.h.begin(), kv.second.h.end(), 0.0f);
    balls.clear(); held = -1;
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& p) { return p.kind == 1; }), parts.end());
}

void World::addDrift() {
    float p1 = rndr(0, 6.28f), p2 = rndr(0, 6.28f), cap = groundCap();
    for (int x = 0; x < W; x++) {
        float add = (22.0f + 9.0f * std::sin(x * 0.006f / S + p1) + 5.0f * std::sin(x * 0.023f / S + p2)) * S;
        ground[x] = std::min(cap, ground[x] + add);
    }
}

float World::groundSnowTotal() const {
    float s = 0;
    for (float v : ground) s += v;
    return s;
}

// ---------------------------------------------------------------- save / load

namespace {
struct Writer {
    std::vector<uint8_t> d;
    void raw(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; d.insert(d.end(), b, b + n); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
};
struct Reader {
    const std::vector<uint8_t>& d; size_t pos = 0; bool ok = true;
    explicit Reader(const std::vector<uint8_t>& v) : d(v) {}
    void raw(void* p, size_t n) { if (pos + n > d.size()) { ok = false; memset(p, 0, n); return; } memcpy(p, &d[pos], n); pos += n; }
    int32_t i32() { int32_t v; raw(&v, 4); return v; }
    float f32() { float v; raw(&v, 4); return v; }
};
}

std::vector<uint8_t> World::save() const {
    Writer w;
    w.raw("SNOWDSK1", 8);
    w.i32(W);
    for (float v : ground) w.f32(v / S);
    std::vector<int> idx(balls.size(), -1);
    int n = 0;
    for (size_t i = 0; i < balls.size(); i++) if (balls[i].state == BALL_REST) idx[i] = n++;
    w.i32(n);
    for (size_t i = 0; i < balls.size(); i++) {
        const Ball& b = balls[i];
        if (b.state != BALL_REST) continue;
        int sup = b.support >= 0 ? idx[b.support] : -1;
        w.f32(b.x / W); w.f32((b.y - groundY) / S); w.f32(b.vol / (S * S)); w.i32(sup);
    }
    return w.d;
}

bool World::load(const std::vector<uint8_t>& d) {
    Reader r(d);
    char magic[8];
    r.raw(magic, 8);
    if (!r.ok || memcmp(magic, "SNOWDSK1", 8) != 0) return false;
    int w = r.i32();
    if (!r.ok || w <= 0 || w > 100000) return false;
    std::vector<float> g(w);
    for (int i = 0; i < w; i++) g[i] = r.f32();
    int n = r.i32();
    if (!r.ok || n < 0 || n > 1000) return false;
    std::vector<Ball> bs;
    for (int i = 0; i < n; i++) {
        Ball b{};
        b.x = r.f32() * W; b.y = groundY + r.f32() * S; b.vol = r.f32() * S * S; b.support = r.i32();
        b.state = BALL_REST; b.r = std::sqrt(std::max(0.0f, b.vol) / PI);
        if (b.support >= i) b.support = -1;
        bs.push_back(b);
    }
    if (!r.ok) return false;
    float cap = groundCap();
    for (int x = 0; x < W; x++) ground[x] = std::min(cap, std::max(0.0f, g[std::min(w - 1, x * w / W)] * S));
    balls = bs; held = -1;
    return true;
}

// ---------------------------------------------------------------- rendering

namespace {
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
    void column(int x, int base, float hh) const {
        if (x < 0 || x >= W || hh <= 0.05f) return;
        float top = base - hh;
        int iy0 = (int)std::floor(top);
        if (iy0 - 1 >= 0 && iy0 - 1 < H) blend(&px[(iy0 - 1) * W + x], 90, 110, 140, 0.18f);
        for (int y = std::max(0, iy0); y < std::min(H, base); y++) {
            float cov = (y == iy0) ? (iy0 + 1 - top) : 1.0f;
            float d = (y + 1) - top;
            float k = clampf((d - 2.0f) / 38.0f, 0, 1);
            int r = (int)(255 - 50 * k), g = (int)(255 - 33 * k), b = (int)(255 - 13 * k);
            blend(&px[y * W + x], r, g, b, cov * 0.97f);
        }
    }
    void tri(float ax, float ay, float bx, float by, float cx, float cy, int r, int g, int b) const {
        int x0 = std::max(0, (int)std::floor(std::min({ax, bx, cx}))), x1 = std::min(W - 1, (int)std::ceil(std::max({ax, bx, cx})));
        int y0 = std::max(0, (int)std::floor(std::min({ay, by, cy}))), y1 = std::min(H - 1, (int)std::ceil(std::max({ay, by, cy})));
        auto e = [](float x0, float y0, float x1, float y1, float x, float y) { return (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0); };
        float area = e(ax, ay, bx, by, cx, cy);
        if (std::fabs(area) < 1e-3f) return;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                int hits = 0;
                for (int s = 0; s < 4; s++) {
                    float px_ = x + 0.25f + 0.5f * (s & 1), py_ = y + 0.25f + 0.5f * (s >> 1);
                    float w0 = e(bx, by, cx, cy, px_, py_) / area, w1 = e(cx, cy, ax, ay, px_, py_) / area, w2 = e(ax, ay, bx, by, px_, py_) / area;
                    if (w0 >= 0 && w1 >= 0 && w2 >= 0) hits++;
                }
                if (hits) blend(&px[y * W + x], r, g, b, hits / 4.0f);
            }
    }
    void ball(float cx, float cy, float rad) const {
        int x0 = std::max(0, (int)std::floor(cx - rad - 1)), x1 = std::min(W - 1, (int)std::ceil(cx + rad + 1));
        int y0 = std::max(0, (int)std::floor(cy - rad - 1)), y1 = std::min(H - 1, (int)std::ceil(cy + rad + 1));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float d = std::sqrt(dx * dx + dy * dy);
                float cov = clampf(rad + 0.5f - d, 0, 1);
                if (cov <= 0) continue;
                float lam = clampf(0.55f - 0.5f * (dx * 0.5f + dy * 0.75f) / rad, 0, 1);
                lam = std::pow(lam, 0.7f);
                if (d > rad - 1.5f) lam *= 0.85f;
                int r = (int)(190 + 65 * lam), g = (int)(206 + 49 * lam), b = (int)(228 + 27 * lam);
                blend(&px[y * W + x], r, g, b, cov);
            }
    }
};
}

void World::render(uint32_t* px) const {
    Canvas cv{px, W, H};
    for (int x = 0; x < W; x++) cv.column(x, groundY, ground[x]);
    for (int k = (int)pileOrder.size() - 1; k >= 0; k--) {
        const Pile& P = *pileOrder[k];
        for (int c = 0; c < P.w; c++) if (P.vis[c]) cv.column(P.l + c, P.t, P.h[c]);
    }
    for (const auto& p : parts) {
        // faint blue-grey halo keeps snow visible over white windows
        float a = p.kind == 2 ? 0.7f : 0.92f;
        cv.dot(p.x, p.y + 0.3f, p.size * 0.6f + 0.9f, 90, 110, 140, 0.22f * a);
        cv.dot(p.x, p.y, p.size * 0.6f, 255, 255, 255, a);
    }
    for (size_t i = 0; i < balls.size(); i++) {
        const Ball& b = balls[i];
        if (b.state == BALL_DEAD || b.r < 1.5f) continue;
        cv.ball(b.x, b.y, b.r);
        if (b.r < 8 * S) continue;
        if (isTopOfStack((int)i)) {   // snowman head
            float er = std::max(1.3f * S, b.r * 0.09f);
            cv.dot(b.x - 0.32f * b.r, b.y - 0.2f * b.r, er, 28, 28, 34, 1);
            cv.dot(b.x + 0.12f * b.r, b.y - 0.2f * b.r, er, 28, 28, 34, 1);
            cv.tri(b.x - 0.08f * b.r, b.y - 0.02f * b.r, b.x - 0.08f * b.r, b.y + 0.16f * b.r,
                   b.x + 0.55f * b.r, b.y + 0.1f * b.r, 240, 122, 32);
            for (int k = 0; k < 5; k++) {
                float a = 0.35f + k * 0.6f;
                cv.dot(b.x - 0.1f * b.r + std::cos(a) * 0.38f * b.r, b.y + 0.08f * b.r + std::sin(a) * 0.32f * b.r,
                       std::max(0.9f * S, b.r * 0.045f), 40, 40, 46, 1);
            }
        } else if (b.state == BALL_REST && b.support >= 0) {  // middle ball: buttons
            for (int k = -1; k <= 1; k++)
                cv.dot(b.x, b.y + k * 0.32f * b.r, std::max(1.2f * S, b.r * 0.07f), 35, 35, 40, 1);
        }
    }
}
