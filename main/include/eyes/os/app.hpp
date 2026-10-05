#pragma once

// The app framework seed. An "app" in LilGuy OS is a foreground SURFACE the
// shell transitions over the face — the face itself is home, not an app: it
// keeps rendering (and feeling) underneath, and returns with a blink when a
// surface closes. Exactly one surface is up at a time; switching apps closes
// the current surface fully before the next opens (exactly-one-picture, per
// the motion doctrine — never two half-visible surfaces).
//
// Platform-free and host-tested like everything in eyes/os. The simulator's
// camera and now-playing surfaces route through AppSwitcher today; the App
// interface below is the contract they (and firmware apps) migrate onto.

#include <algorithm>
#include <cstdint>

namespace eyes {

// The surface contract. Frames are whole 466x466 panels; the shell owns the
// transition and composites the surface over the face. Input reaches a
// surface only while it is fully open.
class App {
public:
    virtual ~App() = default;
    virtual void tick(std::uint32_t now_ms, std::uint32_t dt_ms) = 0;
    virtual void render(std::uint16_t *frame) = 0;
    // true = consumed; false lets the shell interpret the touch.
    virtual bool touch(float x, float y) = 0;
    virtual void opened() {}
    virtual void closed() {}
};

// Which surface is up, and where its open/close transition stands. App ids
// are small ints owned by the shell; 0 means "face only". Open and close
// durations follow the doctrine's asymmetry (arrive settling, leave fast).
class AppSwitcher {
public:
    AppSwitcher(float open_ms, float close_ms) : open_ms_(open_ms), close_ms_(close_ms) {}

    void toggle(int app) { target_ = target_ == app ? 0 : app; }
    void request(int app) { target_ = app; }
    void close_all() { target_ = 0; }

    // Advance the transition. Emits one-shot opened/closed events for the
    // shell's side effects (start hardware, stop recording, blink).
    void tick(std::uint32_t dt_ms)
    {
        opened_event_ = 0;
        closed_event_ = 0;
        if (current_ == 0) {
            current_ = target_;
            if (current_ == 0) {
                return;
            }
        }
        const bool opening = target_ == current_;
        const float step = static_cast<float>(dt_ms) / (opening ? open_ms_ : close_ms_);
        const float before = blend_;
        blend_ = std::clamp(blend_ + (opening ? step : -step), 0.0F, 1.0F);
        if (opening && before < 1.0F && blend_ >= 1.0F) {
            opened_event_ = current_;
        }
        if (!opening && before > 0.0F && blend_ <= 0.0F) {
            closed_event_ = current_;
            current_ = 0;  // a pending different target begins opening next tick
        }
    }

    int foreground() const { return current_; }
    int target() const { return target_; }
    float blend() const { return blend_; }
    bool opening() const { return current_ != 0 && target_ == current_; }
    bool fully_open(int app) const { return current_ == app && blend_ >= 1.0F; }
    bool active() const { return current_ != 0; }

    // One-shot events, cleared on read.
    int take_opened()
    {
        const int event = opened_event_;
        opened_event_ = 0;
        return event;
    }
    int take_closed()
    {
        const int event = closed_event_;
        closed_event_ = 0;
        return event;
    }

private:
    float open_ms_;
    float close_ms_;
    int current_{0};
    int target_{0};
    float blend_{0.0F};
    int opened_event_{0};
    int closed_event_{0};
};

}  // namespace eyes
