// SnowDesk — platform-independent snow simulation.
#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>

// A top-level window as seen by the simulation (overlay pixel coords), in z-order (topmost first).
struct WinRect {
    uint64_t id;
    int l, t, r, b;
    bool pileable;   // normal, non-maximized app window: snow can lie on it
};

struct Pile {
    int l = 0, t = 0, w = 0;
    std::vector<float> h;      // snow height per column (px)
    std::vector<uint8_t> vis;  // column top edge visible (not covered by a higher window)
    float spillL = 0, spillR = 0;
    bool seen = false;
};

struct Particle {
    float x, y, vx, vy, mass, size, phase, amp, freq;
    uint8_t kind; // 0 = front flake, 1 = falling chunk of snow, 2 = far flake
};

enum { BALL_FLY = 0, BALL_REST = 1, BALL_HELD = 2, BALL_DEAD = 3 };

struct Ball {
    float x, y, vx, vy, vol, r;
    int state, support;
    uint64_t lastWin;
    bool thrown;     // released with a swing (can splat); gently released balls are placed
};

class World {
public:
    void init(int w, int h, int groundY, float scale);
    void resize(int w, int h, int groundY);
    void setWindows(const std::vector<WinRect>& ws);
    void step(float dt);
    void render(uint32_t* px) const;  // W*H premultiplied 0xAARRGGBB, blends over existing content

    void mouseDown(float x, float y, double t);
    void mouseMove(float x, float y, double t);
    void mouseUp(float x, float y, double t);
    void cancelHold();

    void clearSnow();
    void addDrift();
    void setIntensity(float k) { intensity = k; }

    std::vector<uint8_t> save() const;
    bool load(const std::vector<uint8_t>& d);

    int width() const { return W; }
    int height() const { return H; }
    float groundSnowTotal() const;
    size_t particleCount() const { return parts.size(); }
    size_t ballCount() const { return balls.size(); }

private:
    int W = 0, H = 0, groundY = 0;
    float S = 1.0f;              // DPI scale
    float intensity = 1.0f;
    double time = 0;
    float wind = 0;
    uint32_t rng = 0x9E3779B9u;

    std::vector<float> ground;
    std::unordered_map<uint64_t, Pile> piles;
    std::vector<WinRect> wins;
    std::vector<Pile*> pileOrder;      // parallel to pileable entries in z-order
    std::vector<int> pileWinIdx;
    std::vector<Particle> parts;
    std::vector<Ball> balls;
    int held = -1;
    bool mouseIsDown = false;
    float mx = 0, my = 0;
    struct Sample { float x, y; double t; };
    std::vector<Sample> hist;
    int relaxDir = 0;

    float rnd();                       // [0,1)
    float rndr(float a, float b) { return a + (b - a) * rnd(); }
    void spawnFlake(bool anywhere);
    void spawnChunk(float x, float y, float vx, float vy, float mass);
    bool tryLand(Particle& p, float y0);
    void deposit(std::vector<float>& h, int c, float m, float cap);
    void relax(std::vector<float>& h, float slope, Pile* p);
    void dropPile(Pile& p);
    float pileCap() const { return 34.0f * S; }
    float groundCap() const;
    uint64_t windowAt(float x, float y) const;
    void collect(Ball& b, float dt);
    void stepBalls(float dt);
    void splat(int i);
    void compactBalls();
    void restPosition(Ball& b);
    bool isTopOfStack(int i) const;
    bool isSupporting(int i) const;
};
