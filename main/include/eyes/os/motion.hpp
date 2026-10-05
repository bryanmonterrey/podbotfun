#pragma once

// The motion doctrine's curves (.claude/skills/lilguy-motion): one liquid
// language for everything that moves in LilGuy OS. Normalized time in [0, 1]
// in, position out. Header-only and platform-free so the device and the
// simulator share the exact constants; the simulator's `house_spring` is this
// one.

#include <cmath>

namespace eyes {
namespace motion {

// HOUSE spring: zeta 0.434, omega 22.46 rad/s over ~0.30 s. Overshoots ~22 %
// at t ~0.39, rings once, settles. Everything that appears or springs back.
inline float house_spring(float t)
{
    constexpr float kZeta = 0.434F;
    constexpr float kOmega = 6.74F;  // 22.46 rad/s * 0.30 s
    const float root = std::sqrt(1.0F - kZeta * kZeta);
    const float damped = kOmega * root;
    return 1.0F - std::exp(-kZeta * kOmega * t) *
                      (std::cos(damped * t) + (kZeta / root) * std::sin(damped * t));
}

// POP spring: zeta 0.479, omega 18.09 rad/s over ~0.30 s (~18 % overshoot).
// Anything launched.
inline float pop_spring(float t)
{
    constexpr float kZeta = 0.479F;
    constexpr float kOmega = 5.43F;  // 18.09 rad/s * 0.30 s
    const float root = std::sqrt(1.0F - kZeta * kZeta);
    const float damped = kOmega * root;
    return 1.0F - std::exp(-kZeta * kOmega * t) *
                      (std::cos(damped * t) + (kZeta / root) * std::sin(damped * t));
}

inline float clamp01(float t) { return t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t); }
inline float ease_in(float t) { t = clamp01(t); return t * t; }
inline float ease_out(float t) { t = clamp01(t); const float u = 1.0F - t; return 1.0F - u * u * u; }
inline float smooth_step(float t) { t = clamp01(t); return t * t * (3.0F - 2.0F * t); }

// Two-beat launch (ooze -> pop). Bulge to 38 % over the first 140 ms with a
// gentle in-out (surface tension), then the POP spring fires the rest. The
// spring's overshoot is confined to the last 15 % of the travel, so the
// visible bounce lands ~3 % of the element: character without wobble.
constexpr float kOozeMs = 140.0F;
inline float two_beat_launch(float elapsed_ms, float total_ms)
{
    if (elapsed_ms <= 0.0F) {
        return 0.0F;
    }
    if (elapsed_ms < kOozeMs) {
        return 0.38F * smooth_step(elapsed_ms / kOozeMs);
    }
    const float u = clamp01((elapsed_ms - kOozeMs) / (total_ms - kOozeMs));
    return 0.38F + 0.47F * ease_out(u) + 0.15F * pop_spring(u);
}

// Dip-to-black, the AMOLED mode swap: out 220 ms ease-in, in 340 ms ease-out.
// The asymmetry is the tell. Both return brightness in [0, 1].
constexpr float kDipOutMs = 220.0F;
constexpr float kDipInMs = 340.0F;
inline float dip_out(float elapsed_ms) { return 1.0F - ease_in(elapsed_ms / kDipOutMs); }
inline float dip_in(float elapsed_ms) { return ease_out(elapsed_ms / kDipInMs); }

}  // namespace motion
}  // namespace eyes
