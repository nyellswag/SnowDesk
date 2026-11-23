// SnowDesk — rain mode: falling rain, splashes, water on and down the windows, and a pool of water
// at the bottom of the screen with waves. The pool can be scooped into water balls and stirred into a whirlpool.
#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "effect.h"

class Rain : public Effect {
public:
    void init(int w, int h, int groundY, float scale) override;
    void resize(int w, int h, int groundY) override;
    void setWindows(const std::vector<WinRect>& ws) override;
    void step(float dt) override;
    void render(uint32_t* px) const override;

    void mouseDown(float x, float y, double t) override;
    void mouseMove(float x, float y, double t) override;
    void mouseUp(float x, float y, double t) override;
    void cancelHold() override;

    void setCursor(float x, float y, double t, bool valid) override;
    void setCursorPhysics(bool on) override { cursorPhysics = on; }
    int hitTest(float x, float y) const override;
    void inputRects(std::vector<IRect>& out) const override;

    void clearSnow() override;
    void addDrift() override;
    void setIntensity(float k) override { intensity = k; }

    std::vector<uint8_t> save() const override;
    bool load(const std::vector<uint8_t>& d) override;

    // for tests / diagnostics
    float meanLevel() const;
    float poolVolume() const;
    size_t dropCount() const { return drops.size(); }
    size_t ballCount() const { return balls.size(); }
    size_t screenDropCount() const { return sdrops.size(); }
    float swirlStrength() const { return swirl; }
    float puddleVolume() const;

private:
    enum Kind : uint8_t {
        D_RAIN,      // front raindrop: a streak that splashes on what it hits
        D_FAR,       // background drop: thin, no collisions
        D_SPRAY,     // droplet thrown by a splash, a burst ball or a spilling window
        D_GLASS,     // a drop stuck on a window's face, sliding down it
        D_RUNNER,    // water running down a window's side edge
        D_FOAM,      // a fleck of foam circling in a whirlpool
        D_BUBBLE,    // an air bubble rising through the pool
        D_RING,      // a ring spreading on the water where a drop landed
        D_SUCK,      // water being drawn up into the ball you're scooping
    };
    struct SDrop {           // a drop on the screen itself, as if on the glass in front of the desktop
        float x, y, r;       // r: radius (px)
        float vy, trail, hold, life, travelled;
    };
    struct Drop {
        float x, y, vx, vy;
        float vol;           // water carried (px^2); returns to whatever catches it
        float life, len, a;  // seconds left, streak length, opacity
        uint64_t win;        // glass / runner: the window it's on
        float ox, oy;        // glass / runner: position relative to that window's top-left
        float trail;         // glass: length of the wet trail above it
        float hold;          // glass: seconds it stays put before sliding again
        uint8_t kind;
    };
    struct Puddle {          // a thin layer of water on a window's top edge
        int l = 0, t = 0, r = 0, b = 0;
        std::vector<float> h;        // depth per column (px)
        std::vector<uint8_t> vis;    // column's top edge visible
        bool seen = false;
        float vx = 0, vy = 0;        // window velocity (px/s), for sloshing
        float glassAcc = 0;          // accumulator for new drops on the glass
    };
    struct Ball {
        float x, y, vx, vy, vol, r;
        bool held;
        float wob, wobV;     // jiggle
        double born;
        uint64_t ignoreWin;  // the window it was released over: don't burst on that one
    };

    int W = 0, H = 0, groundY = 0;
    float S = 1.0f;
    float intensity = 1.0f;
    double time = 0;
    float wind = 0, windTarget = 0, windTimer = 0;
    uint32_t rng = 0x2545F491u;
    float spawnAcc = 0, farAcc = 0, burst = 0;

    // pool: height of the water above the bottom of the screen, per column
    float cw = 3;                // column width (px)
    int NC = 0;
    std::vector<float> lvl, vel;
    std::vector<float> rip, ripv;    // fine, fast ripples on top of the big waves (carry no water)
    std::vector<float> foam;         // white water at the surface after a disturbance
    std::vector<SDrop> sdrops;
    float screenAcc = 0;

    std::vector<Drop> drops;
    std::unordered_map<uint64_t, Puddle> puddles;
    std::vector<WinRect> wins;
    std::vector<Ball> balls;
    int held = -1;

    bool cursorPhysics = true, cursorValid = false;
    float curX = 0, curY = 0, curVx = 0, curVy = 0, curAng = 0;
    double curT = 0;
    struct Sample { float x, y; double t; };
    std::vector<Sample> hist;

    // whirlpool
    float swirlE = 0, swirl = 0, swirlX = 0, swirlDir = 1, foamAcc = 0;

    float rnd();
    float rndr(float a, float b) { return a + (b - a) * rnd(); }
    float maxLevel() const { return 0.17f * H; }
    float surfaceY(float x) const;                // screen y of the water surface at x
    int colOf(float x) const;
    void addWater(float x, float vol, float push);
    void kick(int col, float v);
    void ripple(int col, float v);
    void splatScreen(float x, float y, int n, float spread, float maxR);
    void stepScreen(float dt);
    float surfaceAt(int col) const { return lvl[col] + rip[col]; } // pour vol into the pool at x, pushing the surface down by push px/s
    float takeWater(float x, float radius, float vol);   // returns what it could take
    float poolBelow(float x, float y) const;      // water depth under (x, y), 0 if above the surface
    int topWindowAt(float x, float y) const;      // index in wins of the topmost window containing the point, -1 none
    bool visibleOn(uint64_t id, float x, float y) const;
    Puddle* puddleAt(float x, float yPrev, float yNow, float& topY);
    void splash(float x, float y, float vx, float vy, int n, float vol, float speed);
    void spawnRain(float dt);
    void stepPool(float dt);
    void stepPuddles(float dt);
    void stepDrops(float dt);
    void stepBalls(float dt);
    void stepSwirl(float dt);
    void burstBall(int i, bool intoPool);
    void dumpPuddle(Puddle& p, uint64_t id);
};
