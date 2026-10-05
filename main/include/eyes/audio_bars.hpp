// The eyes AS the dynamic island: when the device is speaking or playing audio,
// the two pill eyes give way to four thick pill bars that bounce like an EQ —
// the same "the creature IS the UI" doctrine as the notification glance. Voice
// out, no chrome.
//
// Platform-free and allocation-free (RGB565, straight into the framebuffer), so
// the host simulator and the device draw the identical visualizer. Deterministic
// given (phase, levels) — no wall clock, no rand — so it golden-hashes in tests.

#ifndef EYES_AUDIO_BARS_HPP
#define EYES_AUDIO_BARS_HPP

#include <cmath>
#include <cstdint>

namespace eyes {

// Four EQ levels in [0,1] for a given animation phase (radians). Each bar runs
// at its own rate + offset so the set never marches in lockstep; `energy`
// (0..1) scales the swing so a quiet passage barely moves and a loud one snaps
// to full height. Pure function of its inputs.
inline void audio_bar_levels(float phase, float energy, float out[4])
{
    static const float rate[4] = {1.7F, 2.6F, 2.1F, 3.1F};
    static const float off[4] = {0.0F, 1.9F, 3.4F, 5.2F};
    const float e = energy < 0.0F ? 0.0F : (energy > 1.0F ? 1.0F : energy);
    for (int i = 0; i < 4; ++i) {
        const float s = 0.5F + 0.5F * std::sin(phase * rate[i] + off[i]);
        out[i] = 0.16F + (0.14F + 0.70F * e) * s; // floor so a bar is never a sliver
    }
}

// Fill one vertical stadium (pill): width 2r, rounded caps of radius r, spanning
// rows [top, bottom]. Clipped to the framebuffer. Allocation-free scanline fill.
inline void fill_pill(std::uint16_t *fb, int w, int h, int cx, int top, int bottom, int r,
                      std::uint16_t color)
{
    if (r < 1) r = 1;
    const int y0 = top < 0 ? 0 : top;
    const int y1 = bottom >= h ? h - 1 : bottom;
    for (int y = y0; y <= y1; ++y) {
        int half = r; // straight middle: half-width == cap radius
        if (y < top + r) {
            const float dy = static_cast<float>(top + r - y);
            const float inside = static_cast<float>(r) * static_cast<float>(r) - dy * dy;
            half = inside <= 0.0F ? 0 : static_cast<int>(std::sqrt(inside));
        } else if (y > bottom - r) {
            const float dy = static_cast<float>(y - (bottom - r));
            const float inside = static_cast<float>(r) * static_cast<float>(r) - dy * dy;
            half = inside <= 0.0F ? 0 : static_cast<int>(std::sqrt(inside));
        }
        int xa = cx - half;
        int xb = cx + half;
        if (xa < 0) xa = 0;
        if (xb >= w) xb = w - 1;
        std::uint16_t *row = fb + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = xa; x <= xb; ++x) row[x] = color;
    }
}

// Draw the four-bar visualizer centered at (cx, cy). `levels[4]` are 0..1 (use
// audio_bar_levels for a synthesized bounce, or feed real amplitude). The caller
// owns the background (bars are opaque pills over it), matching how the face is
// composited. Sizes scale off the panel so it reads on the 466px round display.
inline void draw_audio_bars(std::uint16_t *fb, int w, int h, int cx, int cy,
                            const float levels[4], std::uint16_t color)
{
    const int r = w / 22;              // pill half-width (~21px on 466)
    const int gap = r;                 // even spacing
    const int max_half = (h * 22) / 100; // tallest bar's half-height
    const int min_half = r;            // shortest bar is a dot
    const int stride = 2 * r + gap;
    const int total = 4 * (2 * r) + 3 * gap;
    const int left = cx - total / 2 + r; // center of bar 0
    for (int i = 0; i < 4; ++i) {
        float lv = levels[i];
        lv = lv < 0.0F ? 0.0F : (lv > 1.0F ? 1.0F : lv);
        const int half = min_half + static_cast<int>(static_cast<float>(max_half - min_half) * lv);
        const int bx = left + i * stride;
        fill_pill(fb, w, h, bx, cy - half, cy + half, r, color);
    }
}

}  // namespace eyes

#endif  // EYES_AUDIO_BARS_HPP
