// SnowDesk — what every weather mode (snow, rain, ...) offers to the app shell.
#pragma once
#include <cstdint>
#include <vector>

// A top-level window as seen by the simulation (overlay pixel coords), in z-order (topmost first).
struct WinRect {
    uint64_t id;
    int l, t, r, b;
    bool pileable;   // normal, non-maximized app window: snow / water can lie on it
};

class Effect {
public:
    virtual ~Effect() {}
    virtual void init(int w, int h, int groundY, float scale) = 0;
    virtual void resize(int w, int h, int groundY) = 0;
    virtual void setWindows(const std::vector<WinRect>& ws) = 0;
    virtual void step(float dt) = 0;
    virtual void render(uint32_t* px) const = 0;   // W*H premultiplied 0xAARRGGBB, blends over existing content

    virtual void mouseDown(float x, float y, double t) = 0;
    virtual void mouseMove(float x, float y, double t) = 0;
    virtual void mouseUp(float x, float y, double t) = 0;
    virtual void cancelHold() = 0;

    // mouse cursor as a physical obstacle (call every frame); valid=false when the cursor is off this screen
    virtual void setCursor(float x, float y, double t, bool valid) = 0;
    virtual void setCursorPhysics(bool on) = 0;
    // what is under a point: 0 = nothing, 1 = a ball (snowball / water ball), 2 = snow or water
    virtual int hitTest(float x, float y) const = 0;
    // screen areas the mouse can grab; used to shape input on Linux
    struct IRect { int x, y, w, h; };
    virtual void inputRects(std::vector<IRect>& out) const = 0;

    virtual void clearSnow() = 0;     // "clear": remove all snow / water
    virtual void addDrift() = 0;      // "add more": a snowdrift / a downpour
    virtual void setIntensity(float k) = 0;

    virtual std::vector<uint8_t> save() const = 0;
    virtual bool load(const std::vector<uint8_t>& d) = 0;
};
