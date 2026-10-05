#include "eyes/eye_engine.hpp"

#include <algorithm>
#include <cmath>

#include "eyes/capsule_face.hpp"  // shared mood blink-interval table
#include "eyes/creature_animation_data.hpp"

namespace eyes {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kGravity = 9.80665F;
#if CONFIG_LILGUY_WORLD_VIEW
constexpr float kDragThreshold = 18.0F;
constexpr float kProjectionSeconds = 0.14F;
constexpr std::uint32_t kBrowseHoldMs = 300U;
#endif
// Wide enough to swallow uncalibrated accelerometer bias (~0.1-0.2 observed on
// device), so resting boards don't jerk into phantom tilt episodes; deliberate
// tilts easily exceed it. BOOT-hold calibration tightens effective response.
constexpr float kImuDeadZone = 0.24F;
constexpr float kLookStep = 0.25F;
constexpr std::uint32_t kTouchHoldMs = 250U;
// 320 (was 200) so a tilt reaction lingers visibly after the board levels out
// instead of snapping back the moment the sample re-enters the dead zone.
constexpr std::uint32_t kImuHoldMs = 320U;
constexpr std::uint32_t kImuEpisodeMs = 1000U;
constexpr std::uint32_t kImuRecoilMs = 400U;
constexpr std::uint32_t kShakeMs = 400U;
constexpr std::uint32_t kShakeCooldownMs = 2500U;
constexpr std::uint32_t kEligibleStableMs = 250U;
constexpr std::uint32_t kSoundHoldMs = 400U;
// Sound-direction gaze: how far the eyes glance along the mic axis, and the
// board-dependent sign (+1: positive cross-correlation lag = look up).
constexpr float kSoundGazeGain = 0.45F;
constexpr float kSoundAxisSign = 1.0F;
constexpr std::uint32_t kSoundCooldownMs = 1600U;
constexpr std::uint32_t kSoundStaleMs = 80U;
// Reference client fires one random rotation clip every 5000-10000 ms.
constexpr std::uint32_t kRotIntervalMinMs = 5000U;
constexpr std::uint32_t kRotIntervalMaxMs = 10000U;
// Winks: content moods only, rare (Grok winks every 4.5-10 s; ours are shyer).
constexpr std::uint32_t kWinkIntervalMinMs = 25000U;
constexpr std::uint32_t kWinkIntervalMaxMs = 60000U;

bool wink_mood(Expression expression)
{
    return expression == Expression::content || expression == Expression::playful ||
           expression == Expression::happy;
}

// Mood-cadenced saccades: amplitude scale on the idle look step plus retarget
// cadence (Grok gaze-wander table: curious darts larger/faster, sleepy drifts
// smaller/slower).
struct SaccadeTune {
    float amp;
    std::uint16_t min_ms;
    std::uint16_t max_ms;
};

SaccadeTune saccade_for(Expression expression)
{
    switch (expression) {
        case Expression::curious:
            return {1.4F, 700U, 1800U};
        case Expression::playful:
            return {1.2F, 900U, 2200U};
        case Expression::happy:
            return {1.1F, 1000U, 2400U};
        case Expression::surprised:
            return {1.3F, 600U, 1500U};
        case Expression::sleepy:
            return {0.6F, 2000U, 4500U};
        case Expression::annoyed:
            return {0.9F, 1200U, 2800U};
        case Expression::shy:
            return {0.8F, 1500U, 3200U};
        case Expression::listening:
            return {0.7F, 1500U, 3500U};
        case Expression::thinking:
            return {1.2F, 900U, 2200U};
        case Expression::content:
        default:
            return {1.0F, 1200U, 3000U};
    }
}

ExpressionPose pose_for(Expression expression)
{
    switch (expression) {
        case Expression::curious:
            return {1.04F, -0.035F, 0.92F};
        case Expression::playful:
            return {0.82F, -0.065F, 1.05F};
        case Expression::happy:
            return {0.62F, -0.055F, 1.08F};
        case Expression::surprised:
            return {1.18F, 0.0F, 0.72F};
        case Expression::sleepy:
            return {0.48F, 0.018F, 0.92F};
        case Expression::annoyed:
            return {0.64F, 0.085F, 0.88F};
        case Expression::shy:
            return {0.72F, -0.025F, 0.84F};
        case Expression::listening:
            return {0.98F, -0.018F, 0.90F};
        case Expression::thinking:
            return {0.82F, 0.055F, 0.86F};
        case Expression::content:
        default:
            return {};
    }
}

float approach(float current, float target, float amount)
{
    if (current < target) {
        return std::min(current + amount, target);
    }
    return std::max(current - amount, target);
}

bool time_reached(std::uint32_t now, std::uint32_t deadline)
{
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

bool blink_blocked(AttentionSource source)
{
    return source == AttentionSource::sleep || source == AttentionSource::browse ||
           source == AttentionSource::shake;
}

}  // namespace

EyeEngine::EyeEngine(std::uint32_t seed) { reset(seed); }

void EyeEngine::reset(std::uint32_t seed)
{
    frame_ = {};
    // One circle/OG identity. Expressions change its pose, not its profile.
    frame_.selection = {1, 35};
    base_selection_ = frame_.selection;
    frame_.expression = Expression::content;
    frame_.expression_pose = pose_for(frame_.expression);
    expression_target_ = frame_.expression_pose;
    idle_expression_ = frame_.expression;
    frame_.blink_open = 1.0F;
    frame_.blink_active = false;
    rng_state_ = seed == 0U ? 0x4C494C47U : seed;
    pointer_is_down_ = false;
    dragging_ = false;
    settling_ = false;
    sleeping_ = false;
    selection_changed_ = false;
    selection_locked_ = true;
    browse_active_ = false;
    blink_active_ = false;
    blink_pending_ = false;
    dizzy_active_ = false;
    imu_input_active_ = false;
    imu_episode_active_ = false;
    imu_armed_ = true;
    micro_saccade_active_ = false;
    poke_active_ = false;
    sound_gate_open_ = false;
    sound_active_ = false;
    pointer_start_ = {};
    pointer_last_ = {};
    drag_base_ = {};
    pointer_velocity_ = {};
    grid_velocity_ = {};
    settle_target_ = {};
    imu_target_ = {};
    idle_target_ = {};
    idle_return_target_ = {};
    shake_target_ = {};
    imu_sensitivity_ = 1.3F;  // MED (menu LOW/MED/HIGH = 0.8/1.3/1.9)
    orientation_ = 0.0F;
    pointer_down_ms_ = 0;
    pointer_last_ms_ = 0;
    next_blink_ms_ = 0U;
    blink_start_ms_ = 0;
    dizzy_until_ms_ = 0;
    shake_cooldown_until_ms_ = 0;
    browse_until_ms_ = 0;
    touch_until_ms_ = 0;
    imu_until_ms_ = 0;
    imu_episode_until_ms_ = 0;
    imu_recoil_until_ms_ = 0;
    next_idle_ms_ = 1000U;
    micro_saccade_until_ms_ = 0;
    next_emotion_ms_ = 0U;
    eligible_since_ms_ = 0;
    poke_until_ms_ = 0;
    sound_until_ms_ = 0;
    sound_cooldown_until_ms_ = 0;
    sound_floor_ = 0;
    sound_calibration_sum_ = 0;
    sound_calibration_windows_ = 0;
    sound_entry_streak_ = 0;
    sound_exit_streak_ = 0;
    shake_streak_ = 0;
    pending_row_step_ = 0;
    pending_col_step_ = 0;
    rot_start_ms_ = 0;
    schedule_blink();
    next_emotion_ms_ = random_range(5200U, 7800U);
    next_wink_ms_ = random_range(kWinkIntervalMinMs, kWinkIntervalMaxMs);
    next_rot_ms_ = random_range(kRotIntervalMinMs, kRotIntervalMaxMs);
}

void EyeEngine::set_selection(Selection selection)
{
    selection = wrap_selection(selection);
    if (base_selection_ != selection) {
        const int previous_shape = frame_.selection.shape;
        base_selection_ = selection;
        frame_.selection = selection;
        selection_changed_ = true;
        if (selection.shape != previous_shape) {
            begin_shape_morph(previous_shape);
        }
    }
}

// Start (or retarget) the 800 ms shape morph toward frame_.selection.shape.
// The new "from" is whatever is on screen right now: one-hot(previous shape)
// at rest, or the current lerped blend mid-morph, so double presses stay
// continuous. Palette changes never morph (reference behavior: instant).
void EyeEngine::begin_shape_morph(int previous_shape)
{
    std::array<float, kShapeCount> weights{};
    if (frame_.morph_active) {
        const float eased = ease_in_out_cubic(static_cast<float>(frame_.morph_elapsed_ms) /
                                              static_cast<float>(kShapeMorphMs));
        for (std::size_t shape = 0; shape < kShapeCount; ++shape) {
            weights[shape] = frame_.morph_from_weights[shape] * (1.0F - eased);
        }
        weights[static_cast<std::size_t>(previous_shape)] += eased;
    } else {
        weights[static_cast<std::size_t>(previous_shape)] = 1.0F;
    }
    frame_.morph_from_weights = weights;
    frame_.morph_elapsed_ms = 0U;
    frame_.morph_active = true;
}

void EyeEngine::nudge_selection(int delta_shape, int delta_palette)
{
    set_selection(offset_selection(base_selection_, delta_shape, delta_palette));
    poke_until_ms_ = frame_.time_ms + 340U;
    poke_active_ = true;
}

void EyeEngine::randomize_selection()
{
    const std::uint32_t roll = random_u32();
    // Shape excludes the current one so the change is visible and the morph
    // always plays; palette is uniform over the whole catalog.
    const int shape = (base_selection_.shape + 1 +
                       static_cast<int>(roll % (kShapeCount - 1U))) %
                      static_cast<int>(kShapeCount);
    const int palette = static_cast<int>((roll / kShapeCount) % kPaletteCount);
    set_selection({shape, palette});
    poke_until_ms_ = frame_.time_ms + 340U;
    poke_active_ = true;
}

#if CONFIG_LILGUY_WORLD_VIEW
bool EyeEngine::toggle_selection_lock()
{
    selection_locked_ = !selection_locked_;
    return selection_locked_;
}
#endif

void EyeEngine::pointer_down(float x, float y, std::uint32_t timestamp_ms)
{
    suppress_sound();
    const bool waking = sleeping_;
    pointer_is_down_ = true;
    dragging_ = false;
    settling_ = false;
    sleeping_ = false;
    if (waking) {
        blink_active_ = false;
        frame_.blink_active = false;
        frame_.blink_elapsed_ms = 0;
        // blink_open eases back to 1.0 in update_blink() instead of snapping.
    }
    pointer_start_ = {x, y};
    pointer_last_ = {x, y};
    pointer_velocity_ = {};
    grid_velocity_ = {};
    // grid_offset eases home in update() instead of snapping.
    pointer_down_ms_ = timestamp_ms;
    pointer_last_ms_ = timestamp_ms;
    touch_until_ms_ = frame_.time_ms + kTouchHoldMs;
    frame_.mode = InteractionMode::touching;
    set_attention(AttentionSource::touch);
}

void EyeEngine::pointer_move(float x, float y, std::uint32_t timestamp_ms)
{
    if (!pointer_is_down_) {
        return;
    }

#if CONFIG_LILGUY_WORLD_VIEW
    const std::uint32_t elapsed_ms = std::max<std::uint32_t>(1U, timestamp_ms - pointer_last_ms_);
    const float sample_vx = (x - pointer_last_.x) * 1000.0F / static_cast<float>(elapsed_ms);
    const float sample_vy = (y - pointer_last_.y) * 1000.0F / static_cast<float>(elapsed_ms);
    pointer_velocity_.x = pointer_velocity_.x * 0.58F + sample_vx * 0.42F;
    pointer_velocity_.y = pointer_velocity_.y * 0.58F + sample_vy * 0.42F;
#endif
    pointer_last_ = {x, y};
    pointer_last_ms_ = timestamp_ms;
    touch_until_ms_ = frame_.time_ms + kTouchHoldMs;

#if CONFIG_LILGUY_WORLD_VIEW
    const Vec2 displacement{x - pointer_start_.x, y - pointer_start_.y};
    if (!selection_locked_ && !dragging_ && length(displacement) >= kDragThreshold) {
        dragging_ = true;
        // Continue from wherever a prior drag/settle left the field (no snap).
        drag_base_ = frame_.grid_offset;
        begin_browse();
        frame_.mode = InteractionMode::dragging;
        // grid_visibility fades in via approach() in update() instead of snapping.
    }

    if (dragging_) {
        frame_.grid_offset = wrap_grid_offset(
            {drag_base_.x + displacement.x, drag_base_.y + displacement.y});
        grid_velocity_ = pointer_velocity_;
    }
#endif
}

void EyeEngine::pointer_up(float x, float y, std::uint32_t timestamp_ms)
{
    if (!pointer_is_down_) {
        return;
    }

    pointer_move(x, y, timestamp_ms);
    pointer_is_down_ = false;

#if CONFIG_LILGUY_WORLD_VIEW
    if (dragging_) {
        const Vec2 projected = wrap_grid_offset({
            frame_.grid_offset.x + pointer_velocity_.x * kProjectionSeconds,
            frame_.grid_offset.y + pointer_velocity_.y * kProjectionSeconds,
        });
        // The hex cell nearest the screen center after the fling becomes the
        // selection: find integer (row, col) relative to the anchor minimizing
        // the distance between its lattice position and -projected.
        const int anchor_row = base_selection_.palette / kHexTorusSize;
        const float target_x = -projected.x;
        const float target_y = -projected.y;
        const int base_row = static_cast<int>(std::lround(target_y / kHexRowPitch));
        float best_distance = 0.0F;
        for (int row = base_row - 1; row <= base_row + 1; ++row) {
            const float shift = hex_row_shift(anchor_row + row) - hex_row_shift(anchor_row);
            const int col = static_cast<int>(std::lround((target_x - shift) / kHexCellPitch));
            const float dx = static_cast<float>(col) * kHexCellPitch + shift - target_x;
            const float dy = static_cast<float>(row) * kHexRowPitch - target_y;
            const float distance = dx * dx + dy * dy;
            if (row == base_row - 1 || distance < best_distance) {
                best_distance = distance;
                pending_row_step_ = row;
                pending_col_step_ = col;
            }
        }
        const float settle_shift = hex_row_shift(anchor_row + pending_row_step_) -
                                   hex_row_shift(anchor_row);
        settle_target_ = {
            -(static_cast<float>(pending_col_step_) * kHexCellPitch + settle_shift),
            -static_cast<float>(pending_row_step_) * kHexRowPitch,
        };
        settling_ = true;
        dragging_ = false;
        frame_.mode = InteractionMode::settling;
        return;
    }
#endif

    poke_until_ms_ = frame_.time_ms + 340U;
    poke_active_ = true;
    ++frame_.poke_count;  // screen tap: double-tap detection for the capsule face
    touch_until_ms_ = frame_.time_ms + kTouchHoldMs;
    request_blink();
    frame_.mode = InteractionMode::idle;
}

void EyeEngine::sleep()
{
    suppress_sound();
    pointer_is_down_ = false;
    dragging_ = false;
    settling_ = false;
    browse_active_ = false;
    sleeping_ = true;
    blink_pending_ = blink_pending_ || blink_active_;
    blink_active_ = false;
    dizzy_active_ = false;
    imu_input_active_ = false;
    imu_episode_active_ = false;
    imu_armed_ = true;
    poke_active_ = false;
    pointer_velocity_ = {};
    grid_velocity_ = {};
    settle_target_ = {};
    imu_target_ = {};
    // grid_offset/grid_visibility/blink_open/poke all ease toward their sleep
    // values in update() instead of snapping shut.
    frame_.blink_active = false;
    frame_.blink_elapsed_ms = 0;
    frame_.face_rotation = orientation_;
    frame_.mode = InteractionMode::sleeping;
    frame_.attention = AttentionSource::sleep;
    pending_row_step_ = 0;
    pending_col_step_ = 0;
    shake_streak_ = 0U;
    touch_until_ms_ = 0U;
    imu_until_ms_ = 0U;
    imu_episode_until_ms_ = 0U;
    imu_recoil_until_ms_ = 0U;
}

void EyeEngine::wake()
{
    if (!sleeping_) {
        return;
    }
    sleeping_ = false;
    blink_active_ = false;
    frame_.blink_active = false;
    frame_.blink_elapsed_ms = 0;
    // blink_open eases back to 1.0 in update_blink(); attention re-arbitrates
    // in update(). A poke acknowledges the wake visibly.
    frame_.mode = InteractionMode::idle;
    poke_until_ms_ = frame_.time_ms + 340U;
    poke_active_ = true;
}

void EyeEngine::pointer_cancel()
{
    pointer_is_down_ = false;
    dragging_ = false;
    settling_ = false;
    pointer_velocity_ = {};
    grid_velocity_ = {};
    settle_target_ = {};
    // grid_offset eases home in update() instead of snapping.
    pending_row_step_ = 0;
    pending_col_step_ = 0;
    touch_until_ms_ = frame_.time_ms;
    frame_.mode = sleeping_ ? InteractionMode::sleeping : InteractionMode::idle;
}

void EyeEngine::motion_sample(const MotionSample &sample)
{
    if (browse_active_ || sleeping_) {
        return;
    }
    const bool touch_claimed = pointer_is_down_ || !time_reached(frame_.time_ms, touch_until_ms_);
    if (touch_claimed) {
        shake_streak_ = 0U;
        return;
    }

    Vec2 raw_target{sample.accel_y / kGravity * imu_sensitivity_,
                    sample.accel_x / kGravity * imu_sensitivity_};
    const float magnitude = length(raw_target);
    if (magnitude <= kImuDeadZone) {
        raw_target = {};
        imu_armed_ = true;
        if (imu_episode_active_) {
            imu_until_ms_ = frame_.time_ms + kImuHoldMs;
            imu_input_active_ = false;
            imu_episode_active_ = false;
        }
    } else {
        if (imu_armed_ && time_reached(frame_.time_ms, imu_recoil_until_ms_)) {
            imu_armed_ = false;
            imu_episode_active_ = true;
            imu_episode_until_ms_ = frame_.time_ms + kImuEpisodeMs;
        }
        if (imu_episode_active_ && !time_reached(frame_.time_ms, imu_episode_until_ms_)) {
            imu_input_active_ = true;
            const float linear = std::min(1.0F, (magnitude - kImuDeadZone) /
                                                     (1.0F - kImuDeadZone));
            // Ease-out response: a moderate deliberate tilt (~20 deg, linear
            // t ~ 0.27 at MED sensitivity) already reaches ~0.5 gaze instead
            // of crawling; still monotonic and C1-smooth into the clamp at 1.
            const float strength = 1.0F - (1.0F - linear) * (1.0F - linear);
            raw_target.x *= strength / magnitude;
            raw_target.y *= strength / magnitude;
        } else {
            raw_target = {};
        }
    }
    if (imu_input_active_) {
        // Board-frame tilt rotated into the anchored face frame.
        const Vec2 anchored = rotate_to_face(raw_target);
        imu_target_ = {
            clamp_unit(anchored.x),
            clamp_unit(anchored.y),
        };
    }

    const float rotation = std::sqrt(sample.gyro_x * sample.gyro_x +
                                     sample.gyro_y * sample.gyro_y +
                                     sample.gyro_z * sample.gyro_z);
    const bool shake = std::fabs(sample.accel_magnitude - kGravity) > 7.0F || rotation > 8.0F;
    if (shake) {
        shake_streak_ = std::min<std::uint8_t>(3U, static_cast<std::uint8_t>(shake_streak_ + 1U));
    } else {
        shake_streak_ = 0U;
    }
    if (shake_streak_ == 3U && time_reached(frame_.time_ms, shake_cooldown_until_ms_)) {
        suppress_sound();
        const std::uint32_t move = random_u32();
        int dx = static_cast<int>(move % 5U) - 2;
        int dy = static_cast<int>((move / 5U) % 5U) - 2;
        if (dx == 0 && dy == 0) {
            dx = 1;
        }
        shake_target_ = {
            clamp_unit(std::round(frame_.gaze.x / kLookStep) * kLookStep +
                       static_cast<float>(dx) * kLookStep),
            clamp_unit(std::round(frame_.gaze.y / kLookStep) * kLookStep +
                       static_cast<float>(dy) * kLookStep),
        };
        dizzy_until_ms_ = frame_.time_ms + kShakeMs;
        shake_cooldown_until_ms_ = frame_.time_ms + kShakeCooldownMs;
        dizzy_active_ = true;
        shake_streak_ = 0U;
    }
}

void EyeEngine::sound_sample(const SoundPacket &sample, std::uint32_t now_ms)
{
    if (now_ms - sample.timestamp_ms > kSoundStaleMs) {
        sound_entry_streak_ = 0U;
        return;
    }

    // Track the loud source's direction while listening: mic axis is vertical
    // (top-left vs bottom-left mics) in BOARD frame, so the glance axis is
    // rotated into the anchored face frame. Gentle LPF; the axis sign is
    // board-dependent (kSoundAxisSign flips it).
    if (sample.direction_valid) {
        const float toward = std::clamp(sample.direction, -1.0F, 1.0F) * kSoundAxisSign;
        const Vec2 axis = rotate_to_face({0.0F, toward * kSoundGazeGain});
        sound_target_.x = sound_target_.x + (axis.x - sound_target_.x) * 0.3F;
        sound_target_.y = sound_target_.y + (axis.y - sound_target_.y) * 0.3F;
    }

    const std::uint64_t level = sample.energy;
    if (sound_calibration_windows_ < 50U) {
        sound_calibration_sum_ += level;
        ++sound_calibration_windows_;
        if (sound_calibration_windows_ == 50U) {
            sound_floor_ = std::max<std::uint64_t>(1U, sound_calibration_sum_ / 50U);
        }
        return;
    }

    const bool higher_priority = sleeping_ || browse_active_ || pointer_is_down_ ||
                                 !time_reached(frame_.time_ms, touch_until_ms_) || dizzy_active_;
    if (higher_priority) {
        suppress_sound();
    }

    if (sound_gate_open_) {
        if (level * 4U < sound_floor_ * 9U) {
            sound_exit_streak_ = std::min<std::uint8_t>(
                3U, static_cast<std::uint8_t>(sound_exit_streak_ + 1U));
            if (sound_exit_streak_ == 3U) {
                sound_gate_open_ = false;
                sound_exit_streak_ = 0U;
            }
        } else {
            sound_exit_streak_ = 0U;
        }
        return;
    }

    const std::uint64_t entry_threshold = sound_floor_ * 4U;
    if (level < entry_threshold) {
        if (level > sound_floor_) {
            sound_floor_ += (level - sound_floor_) / 64U;
        } else {
            sound_floor_ -= (sound_floor_ - level) / 64U;
        }
    }
    if (higher_priority || !time_reached(frame_.time_ms, sound_cooldown_until_ms_)) {
        sound_entry_streak_ = 0U;
        return;
    }
    if (level >= entry_threshold) {
        sound_entry_streak_ = std::min<std::uint8_t>(
            3U, static_cast<std::uint8_t>(sound_entry_streak_ + 1U));
    } else {
        sound_entry_streak_ = 0U;
    }
    if (sound_entry_streak_ != 3U) {
        return;
    }

    sound_entry_streak_ = 0U;
    sound_gate_open_ = true;
    sound_active_ = true;
    sound_until_ms_ = frame_.time_ms + kSoundHoldMs;
    sound_cooldown_until_ms_ = frame_.time_ms + kSoundCooldownMs;
}

void EyeEngine::set_imu_sensitivity(float sensitivity)
{
    imu_sensitivity_ = std::clamp(sensitivity, 0.5F, 2.0F);
}

void EyeEngine::set_orientation(float radians)
{
    // Wrap into [-pi, pi); applied immediately so a live two-finger rotate
    // moves the face on the very next render.
    radians -= 2.0F * kPi * std::floor((radians + kPi) / (2.0F * kPi));
    orientation_ = radians;
    frame_.face_rotation = orientation_;
}

// Board frame -> anchored face frame: the render rotates by +orientation_, so
// sensor vectors rotate by -orientation_ to keep reactions world-true.
Vec2 EyeEngine::rotate_to_face(Vec2 value) const
{
    const float cosine = std::cos(orientation_);
    const float sine = std::sin(orientation_);
    return {value.x * cosine + value.y * sine, -value.x * sine + value.y * cosine};
}

void EyeEngine::update(std::uint32_t delta_ms)
{
    std::uint32_t remaining = std::min<std::uint32_t>(delta_ms, 100U);
    while (remaining > 0U) {
        const std::uint32_t step_ms = std::min<std::uint32_t>(remaining, 8U);
        remaining -= step_ms;
        frame_.time_ms += step_ms;
        const float dt = static_cast<float>(step_ms) / 1000.0F;

        // Shape morph clock; runs through browse/sleep so an in-flight morph
        // always completes (at t >= 800 ms the blend equals the pure target).
        if (frame_.morph_active) {
            const std::uint32_t morph_elapsed = frame_.morph_elapsed_ms + step_ms;
            if (morph_elapsed >= kShapeMorphMs) {
                frame_.morph_active = false;
                frame_.morph_elapsed_ms = 0U;
            } else {
                frame_.morph_elapsed_ms = static_cast<std::uint16_t>(morph_elapsed);
            }
        }

        if (imu_episode_active_ && time_reached(frame_.time_ms, imu_episode_until_ms_)) {
            imu_episode_active_ = false;
            imu_input_active_ = false;
            imu_until_ms_ = 0U;
            imu_target_ = {};
            imu_recoil_until_ms_ = imu_episode_until_ms_ + kImuRecoilMs;
        }

#if CONFIG_LILGUY_WORLD_VIEW
        if (settling_) {
            constexpr float spring = 96.0F;
            constexpr float damping = 19.6F;
            const float ax = (settle_target_.x - frame_.grid_offset.x) * spring -
                             grid_velocity_.x * damping;
            const float ay = (settle_target_.y - frame_.grid_offset.y) * spring -
                             grid_velocity_.y * damping;
            grid_velocity_.x += ax * dt;
            grid_velocity_.y += ay * dt;
            frame_.grid_offset.x += grid_velocity_.x * dt;
            frame_.grid_offset.y += grid_velocity_.y * dt;

            const Vec2 error{settle_target_.x - frame_.grid_offset.x,
                             settle_target_.y - frame_.grid_offset.y};
            if (length(error) < 0.8F && length(grid_velocity_) < 5.0F) {
                finish_settle();
            }
        }

        const float grid_target = (dragging_ || settling_) ? 1.0F : 0.0F;
        frame_.grid_visibility = approach(frame_.grid_visibility, grid_target, dt * 5.5F);
        if (!dragging_ && !settling_) {
            // Ease an abandoned drag offset home (pointer_down/cancel/sleep used
            // to snap it); ~1 grid pitch settles in roughly 200 ms.
            frame_.grid_offset.x = approach(frame_.grid_offset.x, 0.0F, dt * 1400.0F);
            frame_.grid_offset.y = approach(frame_.grid_offset.y, 0.0F, dt * 1400.0F);
        }
#endif

        // Rotation flourish. Authored curves start and end at zero delta, so an
        // in-flight clip always plays to completion (no snap); new clips are not
        // started while asleep or browsing. Runs before the browse early-out so
        // elapsed time keeps tracking for the live grid cell.
        if (frame_.rot_active) {
            const std::uint32_t rot_elapsed = frame_.time_ms - rot_start_ms_;
            if (rot_elapsed >= creature::kRotClips[frame_.rot_clip].duration_ms) {
                frame_.rot_active = false;
                frame_.rot_elapsed_ms = 0U;
                next_rot_ms_ = frame_.time_ms + random_range(kRotIntervalMinMs, kRotIntervalMaxMs);
            } else {
                frame_.rot_elapsed_ms = static_cast<std::uint16_t>(rot_elapsed);
            }
        } else if (time_reached(frame_.time_ms, next_rot_ms_)) {
            if (sleeping_ || browse_active_ || dragging_ || settling_ ||
                frame_.grid_visibility > 0.015F) {
                next_rot_ms_ = frame_.time_ms + random_range(kRotIntervalMinMs, kRotIntervalMaxMs);
            } else {
                frame_.rot_active = true;
                frame_.rot_clip = static_cast<std::uint8_t>(
                    random_range(0U, creature::kRotClipCount - 1U));
                frame_.rot_elapsed_ms = 0U;
                rot_start_ms_ = frame_.time_ms;
            }
        }

#if CONFIG_LILGUY_WORLD_VIEW
        if (browse_active_ && !dragging_ && !settling_ &&
            time_reached(frame_.time_ms, browse_until_ms_)) {
            browse_active_ = false;
        }
        if (browse_active_) {
            suppress_sound();
            set_attention(AttentionSource::browse);
            // Keep the eased quantities settling while browse freezes the gaze,
            // so an aborted blink/poke doesn't snap.
            frame_.blink_open = approach(frame_.blink_open, 1.0F, dt * 6.0F);
            frame_.poke = approach(frame_.poke, 0.0F, dt * 12.0F);
            continue;
        }
#endif

        if (dizzy_active_ && time_reached(frame_.time_ms, dizzy_until_ms_)) {
            dizzy_active_ = false;
        }
        if (sound_active_ && time_reached(frame_.time_ms, sound_until_ms_)) {
            sound_active_ = false;
        }
        const bool touch_claimed = pointer_is_down_ ||
                                   !time_reached(frame_.time_ms, touch_until_ms_);
        Vec2 target{};
        if (sleeping_) {
            suppress_sound();
            set_attention(AttentionSource::sleep);
            frame_.mode = InteractionMode::sleeping;
        } else if (touch_claimed) {
            suppress_sound();
            set_attention(AttentionSource::touch);
            // Screen-space finger position -> anchored face frame, so "look
            // at my finger" stays true under a two-finger orientation offset.
            target = rotate_to_face({
                clamp_unit((pointer_last_.x - static_cast<float>(kScreenWidth) * 0.5F) /
                           (static_cast<float>(kScreenWidth) * 0.42F)) * 0.75F,
                clamp_unit((pointer_last_.y - static_cast<float>(kScreenHeight) * 0.5F) /
                           (static_cast<float>(kScreenHeight) * 0.42F)) * 0.75F,
            });
        } else if (dizzy_active_) {
            suppress_sound();
            set_attention(AttentionSource::shake);
            target = shake_target_;
            frame_.mode = InteractionMode::dizzy;
        } else if (sound_active_ && !time_reached(frame_.time_ms, sound_until_ms_)) {
            set_attention(AttentionSource::sound);
            target = sound_target_;
        } else if (!time_reached(frame_.time_ms, imu_recoil_until_ms_)) {
            set_attention(AttentionSource::imu);
            target = {};
        } else if ((imu_input_active_ || !time_reached(frame_.time_ms, imu_until_ms_)) &&
                   length(imu_target_) > 0.0F) {
            set_attention(AttentionSource::imu);
            target = imu_target_;
        } else {
            set_attention(AttentionSource::idle);
            update_idle_gaze();
            target = idle_target_;
            frame_.mode = InteractionMode::idle;
            if (!expression_held_ && time_reached(frame_.time_ms, next_emotion_ms_)) {
                choose_emotion();
            }
            // Rare one-eye wink in content moods (timestamp-scheduled).
            if (time_reached(frame_.time_ms, next_wink_ms_)) {
                if (wink_mood(frame_.expression) && !blink_active_ && !blink_pending_) {
                    start_wink();
                }
                next_wink_ms_ = frame_.time_ms + random_range(kWinkIntervalMinMs,
                                                              kWinkIntervalMaxMs);
            }
        }
        frame_.face_rotation = orientation_;
        update_blink(dt);

        frame_.expression_pose.openness =
            approach(frame_.expression_pose.openness, expression_target_.openness, dt * 1.5F);
        frame_.expression_pose.tilt =
            approach(frame_.expression_pose.tilt, expression_target_.tilt, dt * 0.32F);
        frame_.expression_pose.pupil_scale = approach(
            frame_.expression_pose.pupil_scale, expression_target_.pupil_scale, dt * 0.8F);

        // Reference gaze cascade: client exponential LPF on the cursor (tau
        // U(100,250) ms) plus in-wasm per-node smoothing k=(0.8,0.9) on the eye
        // outline while pupil nodes snap. Model both stages as exponential
        // low-passes: eyes tau 150 ms, pupils tau 40 ms (light filtering hides
        // 60 Hz touch-input steps instead of tracking raw) -> pupils lead the
        // sockets on fast gaze changes, eyes follow.
        constexpr float kGazeEyesTau = 0.150F;
        constexpr float kGazePupilsTau = 0.040F;
        const float eyes_amount = 1.0F - std::exp(-dt / kGazeEyesTau);
        const float pupils_amount = 1.0F - std::exp(-dt / kGazePupilsTau);
        frame_.gaze.x = clamp_unit(frame_.gaze.x + (target.x - frame_.gaze.x) * eyes_amount);
        frame_.gaze.y = clamp_unit(frame_.gaze.y + (target.y - frame_.gaze.y) * eyes_amount);
        frame_.gaze_pupils.x =
            clamp_unit(frame_.gaze_pupils.x + (target.x - frame_.gaze_pupils.x) * pupils_amount);
        frame_.gaze_pupils.y =
            clamp_unit(frame_.gaze_pupils.y + (target.y - frame_.gaze_pupils.y) * pupils_amount);

        if (poke_active_ && time_reached(frame_.time_ms, poke_until_ms_)) {
            poke_active_ = false;
        }
        float poke_target = 0.0F;
        if (poke_active_) {
            const float remaining_ratio = static_cast<float>(poke_until_ms_ - frame_.time_ms) / 340.0F;
            poke_target = std::sin((1.0F - remaining_ratio) * kPi) * remaining_ratio;
        }
        // Ease toward the poke curve so restarts (double tap, nudge) and cancels
        // (sleep, browse) don't snap; the rate is fast enough to track the curve.
        frame_.poke = approach(frame_.poke, poke_target, dt * 12.0F);
    }
}

bool EyeEngine::consume_selection_changed()
{
    const bool changed = selection_changed_;
    selection_changed_ = false;
    return changed;
}

float EyeEngine::clamp_unit(float value) { return std::clamp(value, -1.0F, 1.0F); }

float EyeEngine::smoothstep(float value)
{
    value = std::clamp(value, 0.0F, 1.0F);
    return value * value * (3.0F - 2.0F * value);
}

float EyeEngine::length(Vec2 value) { return std::sqrt(value.x * value.x + value.y * value.y); }

#if CONFIG_LILGUY_WORLD_VIEW
// The hex field repeats every 10 rows/columns (palette torus, even row count
// preserves the odd-row shift), so offsets can wrap into one period and never
// grow unbounded. Wrapping is pixel-identical, hence invisible.
Vec2 EyeEngine::wrap_grid_offset(Vec2 offset)
{
    constexpr float period_x = kHexCellPitch * static_cast<float>(kHexTorusSize);
    constexpr float period_y = kHexRowPitch * static_cast<float>(kHexTorusSize);
    offset.x -= period_x * std::round(offset.x / period_x);
    offset.y -= period_y * std::round(offset.y / period_y);
    return offset;
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

std::uint32_t EyeEngine::random_u32()
{
    std::uint32_t value = rng_state_;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    rng_state_ = value;
    return value;
}

std::uint32_t EyeEngine::random_range(std::uint32_t minimum, std::uint32_t maximum)
{
    return minimum + random_u32() % (maximum - minimum + 1U);
}

#if CONFIG_LILGUY_WORLD_VIEW
void EyeEngine::begin_browse()
{
    suppress_sound();
    browse_active_ = true;
    frame_.selection = base_selection_;
    blink_pending_ = blink_pending_ || blink_active_;
    blink_active_ = false;
    frame_.blink_active = false;
    frame_.blink_elapsed_ms = 0;
    // blink_open and poke ease toward rest in update() instead of snapping.
    dizzy_active_ = false;
    poke_active_ = false;
    frame_.face_rotation = orientation_;
    imu_target_ = {};
    shake_streak_ = 0U;
}
#endif

void EyeEngine::suppress_sound()
{
    sound_entry_streak_ = 0U;
    sound_active_ = false;
}

void EyeEngine::set_attention(AttentionSource source)
{
    if (frame_.attention == source) {
        return;
    }
    const bool became_blink_eligible = blink_blocked(frame_.attention) && !blink_blocked(source);
    frame_.attention = source;
    if (became_blink_eligible) {
        eligible_since_ms_ = frame_.time_ms;
    }
    if (source == AttentionSource::idle) {
        next_idle_ms_ = frame_.time_ms + 1000U;
    }
    switch (source) {
        case AttentionSource::touch:
            set_expression(Expression::playful);
            break;
        case AttentionSource::shake:
            set_expression(Expression::surprised);
            break;
        case AttentionSource::sound:
            set_expression(Expression::listening);
            break;
        case AttentionSource::imu:
            set_expression(Expression::curious);
            break;
        case AttentionSource::sleep:
            set_expression(Expression::sleepy);
            break;
        case AttentionSource::idle:
            set_expression(expression_held_ ? held_expression_ : idle_expression_);
            break;
        case AttentionSource::browse:
            break;
    }
    if ((source == AttentionSource::sleep || source == AttentionSource::browse ||
         source == AttentionSource::shake) && blink_active_) {
        blink_pending_ = true;
        blink_active_ = false;
        frame_.blink_active = false;
        frame_.blink_elapsed_ms = 0;
        // blink_open eases to its resting value in update_blink() instead of snapping.
    }
}

void EyeEngine::choose_idle_gaze()
{
    // Mood scales both saccade amplitude and retarget cadence.
    const SaccadeTune tune = saccade_for(frame_.expression);
    const float step = kLookStep * tune.amp;
    const std::uint32_t choice = random_range(0U, 99U);
    if (choice < 50U) {
        frame_.idle_gaze_kind = IdleGazeKind::hold_or_center;
        if (choice >= 25U) {
            idle_target_ = {};
        }
    } else if (choice < 85U) {
        frame_.idle_gaze_kind = IdleGazeKind::slow_drift;
        std::uint32_t direction = random_range(0U, 7U);
        direction += direction >= 4U ? 1U : 0U;
        const int dx = static_cast<int>(direction % 3U) - 1;
        const int dy = static_cast<int>(direction / 3U) - 1;
        idle_target_.x = clamp_unit(idle_target_.x + static_cast<float>(dx) * step);
        idle_target_.y = clamp_unit(idle_target_.y + static_cast<float>(dy) * step);
    } else if (choice >= 85U) {
        frame_.idle_gaze_kind = IdleGazeKind::micro_saccade;
        idle_return_target_ = idle_target_;
        std::uint32_t direction = random_range(0U, 7U);
        direction += direction >= 4U ? 1U : 0U;
        const int dx = static_cast<int>(direction % 3U) - 1;
        const int dy = static_cast<int>(direction / 3U) - 1;
        idle_target_.x = clamp_unit(idle_target_.x + static_cast<float>(dx) * step);
        idle_target_.y = clamp_unit(idle_target_.y + static_cast<float>(dy) * step);
        micro_saccade_active_ = true;
        micro_saccade_until_ms_ = frame_.time_ms + 100U;
    }
    next_idle_ms_ = frame_.time_ms + random_range(tune.min_ms, tune.max_ms);
}

void EyeEngine::choose_emotion()
{
    const std::uint32_t choice = random_range(0U, 99U);
    idle_expression_ = choice < 32U   ? Expression::content
                       : choice < 47U ? Expression::curious
                       : choice < 59U ? Expression::playful
                       : choice < 71U ? Expression::happy
                       : choice < 76U ? Expression::surprised
                       : choice < 83U ? Expression::sleepy
                       : choice < 87U ? Expression::annoyed
                       : choice < 92U ? Expression::shy
                       : choice < 96U ? Expression::listening
                                      : Expression::thinking;
    set_expression(idle_expression_);
    next_emotion_ms_ = frame_.time_ms + random_range(5200U, 7800U);
    eligible_since_ms_ = frame_.time_ms;
    if (blink_active_) {
        blink_pending_ = true;
        blink_active_ = false;
        frame_.blink_active = false;
        frame_.blink_elapsed_ms = 0;
        // blink_open eases back to 1.0 in update_blink() instead of snapping.
    }
}

void EyeEngine::hold_expression(Expression expression)
{
    expression_held_ = true;
    held_expression_ = expression;
    set_expression(expression);
}

void EyeEngine::release_expression()
{
    if (!expression_held_) {
        return;
    }
    expression_held_ = false;
    set_expression(idle_expression_);
    // Fresh drift schedule so the face doesn't sit frozen after a long turn.
    next_emotion_ms_ = frame_.time_ms + random_range(2000U, 4000U);
}

void EyeEngine::set_expression(Expression expression)
{
    const bool changed = frame_.expression != expression;
    frame_.expression = expression;
    expression_target_ = pose_for(expression);
    if (changed) {
        // Grok blinks on state entry; a 2.5s refractory guard keeps rapid
        // mood churn (touch bursts, attention flips) from reading as a tic.
        if (frame_.time_ms - last_entry_blink_ms_ > 2500U) {
            last_entry_blink_ms_ = frame_.time_ms;
            request_blink();
        }
        schedule_blink();
    }
}

void EyeEngine::schedule_blink()
{
    // Mood-driven cadence (Grok per-state intervals D1n): content/idle 6-14 s,
    // curious/listening 4-9 s, surprised 2.5-6 s, sleepy 3-6 s, ...
    const BlinkRange range = blink_range_for(frame_.expression);
    next_blink_ms_ = frame_.time_ms + random_range(range.min_ms, range.max_ms);
}

void EyeEngine::request_blink()
{
    if (!blink_active_) {
        blink_pending_ = true;
    }
}

void EyeEngine::start_blink()
{
    if (blink_active_ || sleeping_ || browse_active_ || dragging_ || settling_ ||
        frame_.grid_visibility > 0.015F || dizzy_active_) {
        request_blink();
        return;
    }
    blink_start_ms_ = frame_.time_ms;
    blink_active_ = true;
    blink_pending_ = false;
    frame_.blink_active = true;
    frame_.blink_elapsed_ms = 0;
    frame_.wink = 0U;
    // The site pool is blink/blink2/blink3 uniform; the authored-but-unused
    // drowsy blink4/blink5 join the hero pool at low weight (30/30/30/5/5).
    const std::uint32_t roll = random_range(0U, 99U);
    frame_.blink_clip = static_cast<std::uint8_t>(
        roll < 90U ? roll / 30U : 3U + (roll - 90U) / 5U);
    static_assert(creature::kBlinkClipCount == 5, "blink pool weights assume 5 clips");
}

// Wink = the shortest authored blink clip on one random eye; the renderer
// suppresses the other eye's blink deltas.
void EyeEngine::start_wink()
{
    blink_start_ms_ = frame_.time_ms;
    blink_active_ = true;
    blink_pending_ = false;
    frame_.blink_active = true;
    frame_.blink_elapsed_ms = 0;
    frame_.blink_clip = 0U;  // "blink", 783 ms, the shortest clip
    frame_.wink = static_cast<std::uint8_t>(1U + (random_u32() & 1U));
}

void EyeEngine::update_blink(float dt)
{
    // Outside an authored blink clip the lids ease toward their resting value
    // (~160 ms full sweep) so sleep/wake/aborted blinks never snap.
    if (sleeping_) {
        frame_.blink_open = approach(frame_.blink_open, 0.035F, dt * 6.0F);
        frame_.blink_active = false;
        frame_.wink = 0U;  // a sleep entered mid-wink must not pin one eye open
        return;
    }

    if (!blink_active_) {
        frame_.blink_open = approach(frame_.blink_open, 1.0F, dt * 6.0F);
        frame_.blink_active = false;
        frame_.wink = 0U;  // aborted/finished winks lose the one-eye flag
        if (time_reached(frame_.time_ms, next_blink_ms_)) {
            request_blink();
        }
        const bool eligible = frame_.attention != AttentionSource::sleep &&
                              frame_.attention != AttentionSource::browse &&
                              frame_.attention != AttentionSource::shake &&
                              frame_.grid_visibility <= 0.015F;
        if (blink_pending_ && eligible &&
            frame_.time_ms - eligible_since_ms_ >= kEligibleStableMs) {
            start_blink();
        }
        return;
    }

    const std::uint32_t elapsed = frame_.time_ms - blink_start_ms_;
    const std::uint32_t total_ms = creature::kBlinkClips[frame_.blink_clip].duration_ms;
    const std::uint32_t close_ms = total_ms * 250U / 783U;
    const std::uint32_t hold_ms = total_ms * 84U / 783U;
    const std::uint32_t open_ms = total_ms - close_ms - hold_ms;
    frame_.blink_elapsed_ms = static_cast<std::uint16_t>(std::min(elapsed, total_ms));
    frame_.blink_active = true;

    if (elapsed < close_ms) {
        frame_.blink_open =
            1.0F - smoothstep(static_cast<float>(elapsed) / static_cast<float>(close_ms));
    } else if (elapsed < close_ms + hold_ms) {
        frame_.blink_open = 0.025F;
    } else if (elapsed < total_ms) {
        frame_.blink_open = smoothstep(static_cast<float>(elapsed - close_ms - hold_ms) /
                                       static_cast<float>(open_ms));
    } else {
        frame_.blink_open = 1.0F;
        blink_active_ = false;
        frame_.blink_active = false;
        schedule_blink();
    }
}

void EyeEngine::update_idle_gaze()
{
    if (micro_saccade_active_ && time_reached(frame_.time_ms, micro_saccade_until_ms_)) {
        idle_target_ = idle_return_target_;
        micro_saccade_active_ = false;
    }
    if (!micro_saccade_active_ && time_reached(frame_.time_ms, next_idle_ms_)) {
        choose_idle_gaze();
    }
}

#if CONFIG_LILGUY_WORLD_VIEW
void EyeEngine::finish_settle()
{
    settling_ = false;
    // Re-anchor on the settled hex cell: the selection adopts that cell's
    // torus palette AND its hash-assigned shape (exactly what the cell was
    // already rendering as a neighbor, so nothing pops). Settling back onto
    // the same cell keeps the current selection untouched. Zeroing the offset
    // at the same time is a pure coordinate rebase, so nothing moves on screen.
    const int anchor_row = base_selection_.palette / kHexTorusSize;
    const int anchor_col = base_selection_.palette % kHexTorusSize;
    const int new_palette =
        hex_torus_palette(anchor_row + pending_row_step_, anchor_col + pending_col_step_);
    const bool changed = new_palette != base_selection_.palette;
    if (changed) {
        base_selection_ = {hex_cell_shape(new_palette), new_palette};
        // No morph on settle adoption: the settled cell already rendered the
        // new shape as a grid neighbor, so the on-screen anchor IS the new
        // shape — morphing from the old shape here would snap backwards. Any
        // in-flight morph belonged to the old anchor, which just slid away.
        frame_.morph_active = false;
        frame_.morph_elapsed_ms = 0U;
    }
    frame_.selection = base_selection_;
    frame_.grid_offset = {};
    grid_velocity_ = {};
    settle_target_ = {};
    if (changed) {
        selection_changed_ = true;
        poke_until_ms_ = frame_.time_ms + 340U;
        poke_active_ = true;
    }
    pending_row_step_ = 0;
    pending_col_step_ = 0;
    browse_until_ms_ = frame_.time_ms + kBrowseHoldMs;
    frame_.mode = InteractionMode::idle;
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

}  // namespace eyes
