// Linux test harness: runs the simulation headless and dumps frames as PPM (composited on a fake desktop).
#include "../src/sim.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>
#include <ctime>
#include <algorithm>

static const int W = 1280, H = 720, GY = 672;
static std::vector<WinRect> wins;

static void dump(const World& w, const char* name) {
    std::vector<uint32_t> buf(W * H, 0);
    w.render(buf.data());
    FILE* f = fopen(name, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            // background: desktop blue, windows grey (draw bottom to top), taskbar dark
            int r = 30, g = 60, b = 110;
            if (y >= GY) { r = g = b = 25; }
            for (int i = (int)wins.size() - 1; i >= 0; i--) {
                const WinRect& wr = wins[i];
                if (x >= wr.l && x < wr.r && y >= wr.t && y < wr.b) {
                    bool title = y < wr.t + 30;
                    r = g = b = title ? 200 - i * 30 : 235 - i * 20;
                }
            }
            uint32_t p = buf[y * W + x];
            float a = ((p >> 24) & 255) / 255.0f;
            int pr = (p >> 16) & 255, pg = (p >> 8) & 255, pb = p & 255;
            unsigned char o[3] = {(unsigned char)std::min(255.f, pr + r * (1 - a)), (unsigned char)std::min(255.f, pg + g * (1 - a)),
                                  (unsigned char)std::min(255.f, pb + b * (1 - a))};
            fwrite(o, 1, 3, f);
        }
    fclose(f);
}

static void run(World& w, float secs) {
    for (int i = 0; i < (int)(secs * 30); i++) w.step(1.0f / 30);
}

int main() {
    World w;
    w.setIntensity(4.0f);
    w.init(W, H, GY, 1.0f);
    wins = {{1, 150, 200, 650, 500, true}, {2, 500, 120, 1000, 420, true}};
    w.setWindows(wins);
    run(w, 600);
    printf("after 10 min: parts=%zu groundTotal=%.0f\n", w.particleCount(), w.groundSnowTotal());
    dump(w, "f1_accum.ppm");

    // close window 2 -> snow falls
    wins = {{1, 150, 200, 650, 500, true}};
    w.setWindows(wins);
    run(w, 0.4f);
    dump(w, "f2_falling.ppm");
    run(w, 3);
    dump(w, "f3_fallen.ppm");
    printf("after drop: groundTotal=%.0f\n", w.groundSnowTotal());

    // play: collect snow from ground, place balls to make a snowman
    auto makeBall = [&](float x, int frames) {
        double t = 100;
        w.mouseDown(x, GY - 10, t);
        for (int i = 0; i < frames; i++) { t += 1.0 / 30; w.mouseMove(x + (i % 20) * 3 - 30, GY - 12, t); w.step(1.0f / 30); }
        return t;
    };
    double t = makeBall(1100, 60);
    // carry slowly to x=1150 high above and release
    for (int i = 0; i < 20; i++) { t += 1.0 / 30; w.mouseMove(1150, 400, t); w.step(1.0f / 30); }
    w.mouseUp(1150, 400, t + 0.2);
    run(w, 2);
    t = makeBall(900, 35);
    for (int i = 0; i < 20; i++) { t += 1.0 / 30; w.mouseMove(1150, 300, t); w.step(1.0f / 30); }
    w.mouseUp(1150, 300, t + 0.2);
    run(w, 2);
    t = makeBall(800, 20);
    for (int i = 0; i < 20; i++) { t += 1.0 / 30; w.mouseMove(1150, 200, t); w.step(1.0f / 30); }
    w.mouseUp(1150, 200, t + 0.2);
    run(w, 2);
    printf("balls=%zu\n", w.ballCount());
    dump(w, "f4_snowman.ppm");

    // throw a ball hard at window 1
    t = makeBall(300, 30);
    w.mouseMove(300, 650, t);
    t += 0.05; w.mouseMove(380, 560, t);
    w.mouseUp(420, 520, t + 0.03);
    run(w, 0.25f);
    dump(w, "f5_throw.ppm");
    run(w, 3);
    printf("balls=%zu\n", w.ballCount());

    // save / load roundtrip
    auto data = w.save();
    World w2; w2.init(W, H, GY, 1.0f);
    int ok = w2.load(data);
    printf("load=%d bytes=%zu balls=%zu ground=%.0f vs %.0f\n", ok, data.size(), w2.ballCount(), w2.groundSnowTotal(), w.groundSnowTotal());

    // perf: time 1000 steps + renders at 1080p
    World w3; w3.setIntensity(1); w3.init(1920, 1080, 1032, 1.0f);
    std::vector<WinRect> ws = {{1, 100, 200, 900, 800, true}, {2, 700, 150, 1600, 900, true}};
    w3.setWindows(ws);
    std::vector<uint32_t> buf(1920 * 1080);
    w3.addDrift();
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 600; i++) { w3.step(1.0f / 30); memset(buf.data(), 0, buf.size() * 4); w3.render(buf.data()); }
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("1080p step+render: %.2f ms/frame\n", ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 600);
    return 0;
}
