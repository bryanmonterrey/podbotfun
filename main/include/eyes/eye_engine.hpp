#pragma once

// CONFIG_LILGUY_WORLD_VIEW comes from Kconfig on device; host builds pass it
// via the Makefile (there is no sdkconfig.h off-device).
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include <array>
#include <cstdint>

#include "eyes/catalog.hpp"
#include "eyes/types.hpp"

namespace eyes {

// Shape-switch morph: the reference lerps the raw path coordinates over
// 800 ms with scalar easeInOutCubic on every state change (RE doc section 6.3).
constexpr std::uint16_t kShapeMorphMs = 800;

inline float ease_in_out_cubic(float t)
{
    t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
    const float inverse = 1.0F - t;
    return t < 0.5F ? 4.0F * t * t * t : 1.0F - 4.0F * inverse * inverse * inverse;
}

#if CONFIG_LILGUY_WORLD_VIEW
// Hexagonal browse grid (reference client's infinite canvas): pointy-top hex
// lattice on a 10x10 palette torus. Cell (row, col) sits at
// (col * kHexCellPitch + hex_row_shift(row), row * kHexRowPitch); its palette
// is hex_torus_palette(row, col). The current selection's palette encodes the
// anchor cell: row = palette / 10, col = palette % 10.
constexpr float kHexCellPitch =
    0.165F * static_cast<float>(kScreenWidth < kScreenHeight ? kScreenWidth : kScreenHeight);
constexpr float kHexRowPitch = kHexCellPitch * 0.86602540F;  // sqrt(3)/2
constexpr int kHexTorusSize = 10;
static_assert(kHexTorusSize * kHexTorusSize == static_cast<int>(kPaletteCount),
              "hex torus must cover the palette catalog exactly");

inline float hex_row_shift(int row) { return (row & 1) != 0 ? kHexCellPitch * 0.5F : 0.0F; }

inline int hex_wrap(int value)
{
    return ((value % kHexTorusSize) + kHexTorusSize) % kHexTorusSize;
}

inline int hex_torus_palette(int row, int col)
{
    return hex_wrap(row) * kHexTorusSize + hex_wrap(col);
}

// Each torus cell owns a fixed shape. The row/col ramp (2,3 are coprime-ish
// steps mod 4) guarantees all four shapes appear within any 2x2 neighborhood.
inline int hex_cell_shape(int palette_index)
{
    const int row = palette_index / kHexTorusSize;
    const int col = palette_index % kHexTorusSize;
    return (row * 2 + col * 3) % static_cast<int>(kShapeCount);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

// Deterministic per-cell scrambler, keyed by the cell's torus palette index so
// it is stable across pans, settles, offset wraps, and sessions. Drives the
// per-cell animation phases (hero uses it too) and the shape assignment.
inline std::uint32_t hex_cell_hash(int torus_index)
{
    std::uint32_t hash = static_cast<std::uint32_t>(torus_index + 1) * 2654435761U;
    hash ^= hash >> 15U;
    hash *= 2246822519U;
    hash ^= hash >> 13U;
    return hash;
}

enum class InteractionMode : std::uint8_t {
    idle,
    touching,
    dragging,
    settling,
    dizzy,
    sleeping,
};

enum class AttentionSource : std::uint8_t {
    idle,
    imu,
    sound,
    shake,
    touch,
    browse,
    sleep,
};

enum class IdleGazeKind : std::uint8_t {
    hold_or_center,
    slow_drift,
    micro_saccade,
};

enum class Expression : std::uint8_t {
    content,
    curious,
    playful,
    happy,
    surprised,
    sleepy,
    annoyed,
    shy,
    listening,
    thinking,
};

struct ExpressionPose {
    float openness{1.0F};
    float tilt{0.0F};
    float pupil_scale{1.0F};
};

struct FrameState {
    Selection selection{};
    InteractionMode mode{InteractionMode::idle};
    AttentionSource attention{AttentionSource::idle};
    IdleGazeKind idle_gaze_kind{IdleGazeKind::hold_or_center};
    Expression expression{Expression::content};
    ExpressionPose expression_pose{};
    // Eye-outline gaze (slow low-pass; lids and blink corrections follow it).
    Vec2 gaze{};
    // Pupil gaze (fast low-pass): pupils lead, eyes follow on gaze changes.
    Vec2 gaze_pupils{};
    Vec2 grid_offset{};
    float grid_visibility{0.0F};
    float blink_open{1.0F};
    std::uint16_t blink_elapsed_ms{0};
    std::uint8_t blink_clip{0};
    // 0 = both eyes blink; 1 = left-eye wink; 2 = right-eye wink (the other
    // eye's blink deltas are suppressed in the renderer).
    std::uint8_t wink{0};
    bool blink_active{false};
    float poke{0.0F};
    // Monotone tap counter (wraps): bumped on each screen-tap poke so
    // consumers can detect double-taps even while the eased poke value is
    // still decaying from the first tap.
    std::uint8_t poke_count{0};
    float face_rotation{0.0F};
    std::uint32_t time_ms{0};
    // One-shot rotation flourish (rot_1..rot3d_2), additive on top of gaze/idle.
    std::uint16_t rot_elapsed_ms{0};
    std::uint8_t rot_clip{0};
    bool rot_active{false};
    // Shape-switch morph: while active, rendered points are
    //   lerp(sum_i morph_from_weights[i] * points(shape_i), points(selection.shape),
    //        ease_in_out_cubic(morph_elapsed_ms / kShapeMorphMs)).
    // "From" is a weight vector rather than a single previous shape so a
    // mid-morph retarget continues from the currently displayed blend instead
    // of snapping to the old target (deliberate deviation from the reference,
    // which snaps: project rule is that nothing snaps).
    std::array<float, kShapeCount> morph_from_weights{};
    std::uint16_t morph_elapsed_ms{0};
    bool morph_active{false};
};

class EyeEngine {
  public:
    explicit EyeEngine(std::uint32_t seed = 0x4C494C47U);

    void reset(std::uint32_t seed);
    void set_selection(Selection selection);
    Selection selection() const { return base_selection_; }
    void nudge_selection(int delta_shape, int delta_palette);
    // Random look: always a DIFFERENT shape (so the 800 ms morph plays) plus a
    // uniformly random palette.
    void randomize_selection();
#if CONFIG_LILGUY_WORLD_VIEW
    bool toggle_selection_lock();
    bool selection_locked() const { return selection_locked_; }
#endif

    void pointer_down(float x, float y, std::uint32_t timestamp_ms);
    void pointer_move(float x, float y, std::uint32_t timestamp_ms);
    void pointer_up(float x, float y, std::uint32_t timestamp_ms);
    void pointer_cancel();
    void sleep();
    // Pointer-independent wake (PWR-button toggle); pointer_down also wakes.
    void wake();
    void motion_sample(const MotionSample &sample);
    void sound_sample(const SoundPacket &sample, std::uint32_t now_ms);
    void set_imu_sensitivity(float sensitivity);
    float imu_sensitivity() const { return imu_sensitivity_; }
    // Anchored face orientation (two-finger rotate), radians in [-pi, pi).
    // Flows into FrameState.face_rotation; board-frame sensor vectors (IMU
    // tilt, sound glance axis, touch gaze target) are rotated by -offset so
    // the anchored face keeps reacting world-true.
    void set_orientation(float radians);
    float orientation() const { return orientation_; }
    Vec2 imu_gaze_target() const { return imu_target_; }
    void update(std::uint32_t delta_ms);

    const FrameState &frame() const { return frame_; }
    bool consume_selection_changed();

    // One deliberate blink layered on the scheduler: the transition beat the
    // shell plays around state changes (app switches, viewfinder close). The
    // face stays the engine's; callers only request the beat.
    void request_blink();

    // Conversation mood: while held, the face stays on this expression and
    // the idle emotion drift pauses. Reactions (touch, shake, sound) still
    // play over it and return to the held expression, not the idle one —
    // live events outrank mood, mood outranks idle. The shell holds
    // `listening` while the mic is open, `thinking` while the agent works,
    // and releases when the turn ends.
    void hold_expression(Expression expression);
    void release_expression();
    bool expression_held() const { return expression_held_; }

  private:
    static float clamp_unit(float value);
    static float smoothstep(float value);
    static float length(Vec2 value);
#if CONFIG_LILGUY_WORLD_VIEW
    static Vec2 wrap_grid_offset(Vec2 offset);
#endif
    Vec2 rotate_to_face(Vec2 value) const;
    std::uint32_t random_u32();
    std::uint32_t random_range(std::uint32_t minimum, std::uint32_t maximum);
#if CONFIG_LILGUY_WORLD_VIEW
    void begin_browse();
#endif
    void set_attention(AttentionSource source);
    void choose_idle_gaze();
    void choose_emotion();
    void set_expression(Expression expression);
    void begin_shape_morph(int previous_shape);
    void start_blink();
    void start_wink();
    void schedule_blink();
    void update_blink(float dt);
    void update_idle_gaze();
#if CONFIG_LILGUY_WORLD_VIEW
    void finish_settle();
#endif
    void suppress_sound();

    FrameState frame_{};
    ExpressionPose expression_target_{};
    Expression idle_expression_{Expression::content};
    Expression held_expression_{Expression::content};
    bool expression_held_{false};
    Selection base_selection_{};
    std::uint32_t rng_state_{0};
    bool pointer_is_down_{false};
    bool dragging_{false};
    bool settling_{false};
    bool sleeping_{false};
    bool selection_changed_{false};
    bool selection_locked_{false};
    bool browse_active_{false};
    bool blink_active_{false};
    bool blink_pending_{false};
    bool dizzy_active_{false};
    bool imu_input_active_{false};
    bool imu_episode_active_{false};
    bool imu_armed_{true};
    bool micro_saccade_active_{false};
    bool poke_active_{false};
    bool sound_gate_open_{false};
    bool sound_active_{false};
    Vec2 sound_target_{};
    Vec2 pointer_start_{};
    Vec2 pointer_last_{};
    Vec2 drag_base_{};
    Vec2 pointer_velocity_{};
    Vec2 grid_velocity_{};
    Vec2 settle_target_{};
    Vec2 imu_target_{};
    Vec2 idle_target_{};
    Vec2 idle_return_target_{};
    Vec2 shake_target_{};
    float imu_sensitivity_{1.0F};
    float orientation_{0.0F};
    std::uint32_t pointer_down_ms_{0};
    std::uint32_t pointer_last_ms_{0};
    std::uint32_t last_entry_blink_ms_{0};
    std::uint32_t next_blink_ms_{0};
    std::uint32_t blink_start_ms_{0};
    std::uint32_t dizzy_until_ms_{0};
    std::uint32_t shake_cooldown_until_ms_{0};
    std::uint32_t browse_until_ms_{0};
    std::uint32_t touch_until_ms_{0};
    std::uint32_t imu_until_ms_{0};
    std::uint32_t imu_episode_until_ms_{0};
    std::uint32_t imu_recoil_until_ms_{0};
    std::uint32_t next_idle_ms_{0};
    std::uint32_t micro_saccade_until_ms_{0};
    std::uint32_t next_emotion_ms_{0};
    std::uint32_t next_wink_ms_{0};
    std::uint32_t next_rot_ms_{0};
    std::uint32_t rot_start_ms_{0};
    std::uint32_t eligible_since_ms_{0};
    std::uint32_t poke_until_ms_{0};
    std::uint32_t sound_until_ms_{0};
    std::uint32_t sound_cooldown_until_ms_{0};
    std::uint64_t sound_floor_{0};
    std::uint64_t sound_calibration_sum_{0};
    std::uint8_t sound_calibration_windows_{0};
    std::uint8_t sound_entry_streak_{0};
    std::uint8_t sound_exit_streak_{0};
    std::uint8_t shake_streak_{0};
    // Relative hex-cell steps chosen at release; applied to the anchor on settle.
    int pending_row_step_{0};
    int pending_col_step_{0};
};

}  // namespace eyes
