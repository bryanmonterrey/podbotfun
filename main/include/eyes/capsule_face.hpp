#pragma once

// Grok Bot capsule face (second personality): the 25-pose stadium-eye
// vocabulary from docs/GROKBOT_FACE_RE.md driven by damped springs. Pure
// host-compilable math; the renderer rasterizes the two capsules over a
// palette-colored disc (two-color model: disc = fg, eyes = bg/black).

#include <array>
#include <cstdint>

#include "eyes/eye_engine.hpp"
#include "eyes/types.hpp"

namespace eyes {

// {cx, cy, angleDeg, halfLen, halfW} per eye, design-square units centered on
// He = 114.2705 (verbatim from the RE doc's "Eyes: 25 pose variants" table).
struct CapsuleEye {
    float cx;
    float cy;
    float angle_deg;
    float half_len;
    float half_w;
};

struct CapsulePose {
    CapsuleEye left;
    CapsuleEye right;
};

inline constexpr std::size_t kCapsulePoseCount = 25;
extern const std::array<CapsulePose, kCapsulePoseCount> kCapsulePoses;

// Design square 228.541 mapped onto the 466 px panel: disc radius ~220 px.
inline constexpr float kCapsuleDiscRadius = 220.0F;
inline constexpr float kCapsuleScale = 440.0F / 228.541F;  // ~1.925 px per design unit

// Grok spinWild celebrate (grok_3d_moves.json flourishes.spinWild): 0.24 s
// backswing (-0.5 rad), 0.3 s quadratic accel, 2.0 s cruise at ~23.167 rad/s,
// 1.25 s quartic decel landing the belt on exactly 9 turns, then a 1.7 s
// wobble tail. The visual face rotation is belt/3 (3 full turns), so the
// transient offset ends at exactly 3*2pi (== 0 mod 2pi) and is cleared to a
// literal 0 when the tail finishes.
inline constexpr std::uint32_t kSpinWildDurationMs = 5490U;

// Closed 12-point cubic path (4 cubics: cap, edge, cap, edge) outlining a
// rotated capsule, ready for Raster::fill_cubic_path. Single-cubic semicircle
// caps use control distance (4/3)*0.9875*w: equioscillating ~1.3% radial
// ripple, so the path bbox stays within 1 px of the analytic capsule bbox at
// on-screen sizes.
std::array<Vec2, 12> capsule_path(float cx, float cy, float angle_rad, float half_len,
                                  float half_w);

// Mood blink-interval table (Grok D1n mapped onto our expressions). Shared by
// the creature engine scheduler and the capsule face.
struct BlinkRange {
    std::uint16_t min_ms;
    std::uint16_t max_ms;
};

inline BlinkRange blink_range_for(Expression expression)
{
    // ~1.35x the Grok table: on a physical desk object the raw web rates read
    // as fidgety (user feedback), and state-entry blinks add to these anyway.
    switch (expression) {
        case Expression::curious:
        case Expression::listening:
            return {5500U, 12000U};
        case Expression::playful:
            return {3000U, 6500U};
        case Expression::happy:
            return {3500U, 7000U};
        case Expression::surprised:
            return {3500U, 8000U};
        case Expression::sleepy:
        case Expression::shy:
            return {4000U, 8000U};
        case Expression::annoyed:
        case Expression::thinking:
            return {5000U, 9500U};
        case Expression::content:
        default:
            return {8000U, 18000U};
    }
}

class CapsuleFace {
  public:
    struct EyeOut {
        float cx;         // screen px
        float cy;         // screen px
        float angle_rad;  // includes face orientation
        float half_len;   // screen px
        float half_w;     // screen px
        // Blink closes the eye along SCREEN-vertical regardless of capsule
        // rotation (a lid comes down): the renderer scales the outline's y
        // about cy by this factor. >1 = the reopen overshoot stretch.
        float squash_y;
    };

    explicit CapsuleFace(std::uint32_t seed = 0x43415053U);
    void reset(std::uint32_t seed);

    // Advance springs and scheduling to state.time_ms and recompute geometry.
    void update(const FrameState &state);
    EyeOut eye(int index) const { return out_[static_cast<std::size_t>(index & 1)]; }

    // Power gate: false when the rendered pixels would be identical to the
    // last frame marked rendered (outputs quantized to 1/4 px).
    bool needs_frame() const;
    void mark_rendered() { last_rendered_ = quantized_; }

    float blink_openness() const { return blink_.x; }
    int pose_index() const { return pose_index_; }

    // spinWild celebrate: transient rotation offset added on top of
    // state.face_rotation (radians; exactly 0 when inactive — the persisted
    // user orientation is never touched). Triggered by a poke double-tap
    // (two pokes within 600 ms), 10% on entering happy/playful, or manually.
    bool spin_active() const { return spin_active_; }
    float spin_offset_rad() const { return spin_offset_rad_; }
    void celebrate(std::uint32_t now_ms);

  private:
    struct Spring {
        float x{0.0F};
        float v{0.0F};
    };

    static void step_spring(Spring &spring, float target, float omega, float zeta, float h);
    std::uint32_t random_u32();
    std::uint32_t random_range(std::uint32_t minimum, std::uint32_t maximum);
    void retarget_pose(int pose, bool entry);
    void select_pose(const FrameState &state, bool entry);
    void start_blink(std::uint32_t now_ms);
    float blink_target(float time_ms, const FrameState &state) const;
    void refresh_outputs(const FrameState &state);

    // 10 eye params (2 x {cx, cy, angle, halfLen, halfW}) + blink + breath +
    // gaze x/y + Qi head roll/x/y + wave gain = 18 spring channels, all
    // stepped at 1/120 s.
    Spring eyes_[2][5]{};
    Spring blink_{};
    Spring breath_{};
    Spring gaze_[2]{};
    Spring face_roll_{};  // Qi head roll, degrees (omega 5, zeta .9)
    Spring face_x_{};     // Qi head x, design units (omega 3.5)
    Spring face_y_{};     // Qi head y, design units (omega 4)
    Spring wave_{};       // sound-attention wave-bar gain 0..1 (omega 10)
    std::array<EyeOut, 2> out_{};
    std::array<std::int16_t, 12> quantized_{};
    std::array<std::int16_t, 12> last_rendered_{};
    std::uint32_t rng_state_{1U};
    std::uint32_t last_ms_{0U};
    float substep_carry_ms_{0.0F};
    float last_poke_{0.0F};
    Expression last_expression_{Expression::content};
    InteractionMode last_mode_{InteractionMode::idle};
    int pose_index_{0};
    float pose_omega_{6.0F};
    bool initialized_{false};
    // Timestamp scheduling (nextAt = now + rand(min, max); no timers).
    std::uint32_t next_pose_ms_{0U};
    std::uint32_t last_blink_start_ms_{0};
    std::uint32_t next_blink_ms_{0U};
    std::uint32_t next_wink_ms_{0U};
    std::uint32_t wink_start_ms_{0U};
    int wink_eye_{0};
    bool wink_active_{false};
    // Blink envelope keyframe queue (absolute ms, Grok pr(t) values).
    std::array<float, 6> blink_key_ms_{};
    std::array<float, 6> blink_key_value_{};
    std::size_t blink_key_count_{0U};
    // spinWild celebrate flourish.
    bool spin_active_{false};
    float spin_dir_{1.0F};
    float spin_offset_rad_{0.0F};
    std::uint32_t spin_start_ms_{0U};
    std::uint8_t last_poke_count_{0U};
    std::uint32_t last_poke_event_ms_{0U};
    // Qi timed burst (listening nod / curious tilt-bounce / angry tremor).
    bool burst_active_{false};
    std::uint32_t burst_start_ms_{0U};
    std::uint32_t next_burst_ms_{0U};
    std::uint32_t state_entry_ms_{0U};  // sleeping Qi u-ramp reference
    // Drowsy nod-off scheduling (sleepy expression only).
    bool nod_active_{false};
    std::uint32_t nod_start_ms_{0U};
    std::uint32_t next_nod_ms_{0U};
};

}  // namespace eyes
