// Dynamic-island status glyphs: the eyes give way to a listening pulse, a
// success check, or an error cross — the same "the creature IS the UI" doctrine
// as the audio-bar visualizer. Platform-free and allocation-free (RGB565,
// straight into the framebuffer); deterministic given (phase/progress) so they
// golden-hash in tests. Device and host share these.

#ifndef EYES_STATUS_GLYPHS_HPP
#define EYES_STATUS_GLYPHS_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace eyes {

// A filled disc, clipped to the framebuffer. The thickness primitive the line
// and dot glyphs are built from.
inline void fill_disc(std::uint16_t *fb, int w, int h, int cx, int cy, int r,
                      std::uint16_t color)
{
    if (r < 1) r = 1;
    const int y0 = cy - r < 0 ? 0 : cy - r;
    const int y1 = cy + r >= h ? h - 1 : cy + r;
    for (int y = y0; y <= y1; ++y) {
        const int dy = y - cy;
        const int span = static_cast<int>(std::sqrt(static_cast<float>(r * r - dy * dy)));
        int xa = cx - span;
        int xb = cx + span;
        if (xa < 0) xa = 0;
        if (xb >= w) xb = w - 1;
        std::uint16_t *row = fb + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = xa; x <= xb; ++x) row[x] = color;
    }
}

// A thick round-capped line from (x0,y0) to (x1,y1), drawn as overlapping discs.
// `reveal` in [0,1] draws only that leading fraction (for animated strokes).
inline void thick_line(std::uint16_t *fb, int w, int h, int x0, int y0, int x1, int y1,
                       int radius, float reveal, std::uint16_t color)
{
    if (reveal <= 0.0F) return;
    if (reveal > 1.0F) reveal = 1.0F;
    const float dx = static_cast<float>(x1 - x0);
    const float dy = static_cast<float>(y1 - y0);
    const float len = std::sqrt(dx * dx + dy * dy);
    const int steps = static_cast<int>(len) + 1;
    const int drawn = static_cast<int>(static_cast<float>(steps) * reveal);
    for (int i = 0; i <= drawn; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        fill_disc(fb, w, h, x0 + static_cast<int>(dx * t), y0 + static_cast<int>(dy * t), radius, color);
    }
}

// Success check: the ✓ draws itself in — short down-stroke then the long
// up-stroke — as `progress` goes 0→1. Sized off the panel.
inline void draw_check(std::uint16_t *fb, int w, int h, int cx, int cy, float progress,
                       std::uint16_t color)
{
    const int s = w / 6;              // arm scale (~77px on 466)
    const int r = w / 40;             // stroke half-thickness
    const int ax = cx - s, ay = cy;                 // left tip
    const int bx = cx - s / 3, by = cy + s / 2;     // bottom vertex
    const int ex = cx + s, ey = cy - s / 2;         // upper-right tip
    // First 40% draws the short arm, the rest the long arm.
    const float p = progress < 0.0F ? 0.0F : (progress > 1.0F ? 1.0F : progress);
    thick_line(fb, w, h, ax, ay, bx, by, r, p / 0.4F, color);
    if (p > 0.4F) thick_line(fb, w, h, bx, by, ex, ey, r, (p - 0.4F) / 0.6F, color);
}

// Error cross: an ✗ draws in (first stroke, then the second).
inline void draw_cross(std::uint16_t *fb, int w, int h, int cx, int cy, float progress,
                       std::uint16_t color)
{
    const int s = w / 8;
    const int r = w / 40;
    const float p = progress < 0.0F ? 0.0F : (progress > 1.0F ? 1.0F : progress);
    thick_line(fb, w, h, cx - s, cy - s, cx + s, cy + s, r, p / 0.5F, color);
    if (p > 0.5F) thick_line(fb, w, h, cx + s, cy - s, cx - s, cy + s, r, (p - 0.5F) / 0.5F, color);
}

// Listening: three dots that swell in a travelling wave (a Siri-style "I'm
// hearing you"). `phase` in radians animates the wave.
inline void draw_listening(std::uint16_t *fb, int w, int h, int cx, int cy, float phase,
                           std::uint16_t color)
{
    const int base = w / 34;          // resting dot radius (~13px)
    const int amp = w / 60;           // swell
    const int gap = w / 7;
    for (int i = 0; i < 3; ++i) {
        const float s = 0.5F + 0.5F * std::sin(phase - static_cast<float>(i) * 1.1F);
        const int r = base + static_cast<int>(static_cast<float>(amp) * s);
        fill_disc(fb, w, h, cx + (i - 1) * gap, cy, r, color);
    }
}

}  // namespace eyes

#endif  // EYES_STATUS_GLYPHS_HPP
