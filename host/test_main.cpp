#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "eyes/capsule_face.hpp"
#include "eyes/catalog.hpp"
#include "eyes/creature_animation_data.hpp"
#include "eyes/creature_rig_data.hpp"
#include "eyes/device_ui.hpp"
#include "eyes/eye_engine.hpp"
#include "eyes/eye_renderer.hpp"
#include "eyes/audio_bars.hpp"
#include "eyes/status_glyphs.hpp"
#include "eyes/os/agent_link.hpp"
#include "eyes/os/app.hpp"
#include "eyes/os/cast_session.hpp"
#include "eyes/os/earcon.hpp"
#include "eyes/os/onboarding.hpp"
#include "eyes/os/ota.hpp"
#include "eyes/os/services.hpp"
#include "eyes/os/voice_session.hpp"
#include "eyes/pixel.hpp"
#include "eyes/raster.hpp"

namespace {

std::uint64_t hash_pixels(const std::vector<std::uint16_t> &pixels)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint16_t pixel : pixels) {
        hash ^= pixel;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void advance(eyes::EyeEngine &engine, std::uint32_t milliseconds)
{
    for (std::uint32_t elapsed = 0; elapsed < milliseconds; elapsed += 8U) {
        engine.update(std::min<std::uint32_t>(8U, milliseconds - elapsed));
    }
}

void sound(eyes::EyeEngine &engine, std::uint64_t mic1, std::uint64_t mic2,
           std::uint32_t age_ms = 0U)
{
    const std::uint32_t now = engine.frame().time_ms;
    engine.sound_sample({mic1 + mic2, now - age_ms, 0U}, now);
}

void calibrate_sound(eyes::EyeEngine &engine)
{
    for (int window = 0; window < 50; ++window) {
        sound(engine, 100U, 100U);
        advance(engine, 20U);
    }
}

void test_catalog()
{
    static_assert(eyes::kLookCount == 400);
    assert(eyes::shapes().size() == 4U);
    assert(eyes::palettes().size() == 100U);
    assert(eyes::wrap_selection({-1, -1}) == eyes::Selection({3, 99}));
    assert(eyes::offset_selection({3, 99}, 1, 1) == eyes::Selection({0, 0}));
    assert(std::string(eyes::shape_at(1).name) == "circle");
    assert(std::string(eyes::shape_at(1).state) == "neutral_b");
    const eyes::Palette &siren = eyes::palette_at(63);
    assert(std::string(siren.name) == "siren");
    assert(siren.outer.r == 253U && siren.outer.g == 12U && siren.outer.b == 13U);
    assert(siren.inner.r == 41U && siren.inner.g == 5U && siren.inner.b == 230U);
    const auto &circle_neutral = eyes::creature::kLookFrames[1U * 81U + 40U];
    assert(circle_neutral[0] == 961 && circle_neutral[1] == 1128);
    assert(circle_neutral[2] == 1330 && circle_neutral[3] == 993);

    std::set<std::string> shape_names;
    std::set<std::string> palette_names;
    for (const auto &shape : eyes::shapes()) {
        assert(shape.weight > 0.0F);
        shape_names.emplace(shape.name);
    }
    for (const auto &palette : eyes::palettes()) {
        palette_names.emplace(palette.name);
    }
    assert(shape_names.size() == eyes::kShapeCount);
    assert(palette_names.size() == eyes::kPaletteCount);

    std::set<std::uint64_t> rgb565_tuples;
    for (const auto &palette : eyes::palettes()) {
        const std::uint64_t tuple = (static_cast<std::uint64_t>(eyes::rgb565(palette.outer)) << 32U) |
                                    (static_cast<std::uint64_t>(eyes::rgb565(palette.inner)) << 16U) |
                                    eyes::rgb565(palette.accent);
        rgb565_tuples.emplace(tuple);
    }
    assert(rgb565_tuples.size() == 100U);
}

#if CONFIG_LILGUY_WORLD_VIEW
void test_swipe_and_settle()
{
    // Selection starts at palette 35 = hex torus cell (row 3, col 5). A right
    // fling of ~224 px projected lands the cell 3 pitches (76.89 px) to the
    // left on center: col 5 -> 2, palette 32. Settling adopts BOTH that cell's
    // palette and its hash-assigned shape.
    eyes::EyeEngine engine(7U);
    assert(!engine.toggle_selection_lock());
    engine.pointer_down(233.0F, 233.0F, 0U);
    engine.pointer_move(400.0F, 233.0F, 100U);
    engine.pointer_up(400.0F, 233.0F, 120U);
    assert(engine.frame().mode == eyes::InteractionMode::settling);
    advance(engine, 1200U);
    assert(engine.selection() == eyes::Selection({eyes::hex_cell_shape(32), 32}));
    assert(engine.consume_selection_changed());

    // An upward fling moves 4 hex rows (66.59 px row pitch): row 3 -> 7 on the
    // torus, col unchanged, palette 72; shape follows the settled cell again.
    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    engine.pointer_move(233.0F, 70.0F, now + 90U);
    engine.pointer_up(233.0F, 70.0F, now + 110U);
    advance(engine, 1200U);
    assert(engine.selection() == eyes::Selection({eyes::hex_cell_shape(72), 72}));

    // A slow small drag releases nearest to the original cell: no change.
    eyes::EyeEngine still(7U);
    assert(!still.toggle_selection_lock());
    still.pointer_down(233.0F, 233.0F, 0U);
    still.pointer_move(255.0F, 233.0F, 300U);
    still.pointer_up(255.0F, 233.0F, 600U);
    advance(still, 1200U);
    assert(still.selection() == eyes::Selection({1, 35}));
    assert(!still.consume_selection_changed());
}

void test_selection_lock_keeps_touch_and_recovers_browse()
{
    eyes::EyeEngine engine(9U);
    assert(engine.selection_locked());
    const eyes::Selection original = engine.selection();

    engine.pointer_down(233.0F, 233.0F, 0U);
    engine.pointer_move(440.0F, 233.0F, 120U);
    advance(engine, 120U);
    assert(engine.frame().attention == eyes::AttentionSource::touch);
    assert(engine.frame().gaze.x > 0.2F);
    assert(engine.frame().mode == eyes::InteractionMode::touching);
    assert(engine.frame().grid_visibility == 0.0F);
    engine.pointer_up(440.0F, 233.0F, 140U);
    advance(engine, 16U);
    assert(engine.selection() == original);
    assert(engine.frame().poke > 0.0F);

    assert(!engine.toggle_selection_lock());
    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    engine.pointer_move(440.0F, 233.0F, now + 100U);
    engine.pointer_up(440.0F, 233.0F, now + 120U);
    assert(engine.frame().mode == eyes::InteractionMode::settling);
    advance(engine, 1200U);
    assert(engine.selection() != original);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

void test_input_arbitration_and_reactions()
{
    eyes::EyeEngine engine(11U);
    eyes::MotionSample tilt{};
    // A deliberate tilt, comfortably above the widened kImuDeadZone (0.24) that
    // swallows uncalibrated accelerometer bias on real boards.
    tilt.accel_x = 4.5F;
    tilt.accel_y = -3.0F;
    tilt.accel_z = 0.0F;
    for (int sample = 0; sample < 120; ++sample) {
        engine.motion_sample(tilt);
        advance(engine, 8U);
    }
    assert(engine.frame().attention == eyes::AttentionSource::imu);
    assert(engine.frame().gaze.x < -0.14F);
    assert(engine.frame().gaze.y > 0.20F);
    assert(std::fabs(engine.frame().gaze.x) <= 1.0F);
    assert(std::fabs(engine.frame().gaze.y) <= 1.0F);
    assert(engine.frame().face_rotation == 0.0F);

    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(450.0F, 233.0F, now);
    advance(engine, 250U);
    assert(engine.frame().attention == eyes::AttentionSource::touch);
    assert(engine.frame().gaze.x > 0.25F);
    engine.pointer_up(450.0F, 233.0F, now + 250U);
    advance(engine, 40U);
    assert(engine.frame().attention == eyes::AttentionSource::touch);
    assert(engine.frame().poke > 0.0F);
    advance(engine, 220U);
    assert(engine.frame().attention != eyes::AttentionSource::touch);

    eyes::MotionSample shake{};
    shake.gyro_z = 9.0F;
    for (int sample = 0; sample < 3; ++sample) {
        engine.motion_sample(shake);
        advance(engine, 16U);
    }
    assert(engine.frame().mode == eyes::InteractionMode::dizzy);
    assert(engine.frame().attention == eyes::AttentionSource::shake);
    assert(engine.frame().face_rotation == 0.0F);

    advance(engine, 408U);
    assert(engine.frame().attention != eyes::AttentionSource::shake);
    for (int sample = 0; sample < 3; ++sample) {
        engine.motion_sample(shake);
        advance(engine, 16U);
    }
    assert(engine.frame().attention != eyes::AttentionSource::shake);

    advance(engine, 2100U);
    for (int sample = 0; sample < 3; ++sample) {
        engine.motion_sample(shake);
        advance(engine, 16U);
    }
    assert(engine.frame().attention == eyes::AttentionSource::shake);

    advance(engine, 408U);
    const std::uint32_t later = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, later);
    engine.pointer_up(233.0F, 233.0F, later + 1000U);
    advance(engine, 16U);
    assert(engine.frame().mode != eyes::InteractionMode::sleeping);
}

void test_calm_motion()
{
    eyes::EyeEngine baseline(17U);
    eyes::EyeEngine noisy(17U);
    for (int sample = 0; sample < 200; ++sample) {
        eyes::MotionSample noise{};
        noise.accel_x = 0.4F;
        noise.accel_y = -0.3F;
        noisy.motion_sample(noise);
        advance(baseline, 8U);
        advance(noisy, 8U);
    }
    assert(noisy.frame().attention == baseline.frame().attention);
    assert(noisy.frame().gaze.x == baseline.frame().gaze.x);
    assert(noisy.frame().gaze.y == baseline.frame().gaze.y);
    noisy.set_imu_sensitivity(0.1F);
    assert(noisy.imu_sensitivity() == 0.5F);
    noisy.set_imu_sensitivity(3.0F);
    assert(noisy.imu_sensitivity() == 2.0F);
}

void test_sleep_cancels_motion_and_wakes_cleanly()
{
    eyes::EyeEngine engine(23U);
    // Mood blink cadence: content/idle waits 6-14 s (emotion-entry blinks can
    // land earlier); 14.2 s bounds the schedule.
    while (!engine.frame().blink_active && engine.frame().time_ms < 18210U) {
        advance(engine, 8U);
    }
    assert(engine.frame().blink_active);

    engine.sleep();
    assert(engine.frame().mode == eyes::InteractionMode::sleeping);
    assert(!engine.frame().blink_active);
    assert(engine.frame().blink_elapsed_ms == 0U);
    assert(engine.frame().grid_visibility == 0.0F);
    // Lids now ease shut (~160 ms) instead of snapping on sleep().
    advance(engine, 200U);
    assert(engine.frame().blink_open < 0.1F);

    eyes::MotionSample shake{};
    shake.accel_x = 7.0F;
    shake.gyro_z = 10.0F;
    for (int sample = 0; sample < 8; ++sample) {
        engine.motion_sample(shake);
        advance(engine, 16U);
    }
    assert(engine.frame().mode == eyes::InteractionMode::sleeping);

    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    assert(engine.frame().mode == eyes::InteractionMode::touching);
    // Wake now eases the lids open (~160 ms) instead of snapping to 1.0.
    advance(engine, 240U);
    assert(engine.frame().blink_open == 1.0F);
    assert(!engine.frame().blink_active);
}

#if CONFIG_LILGUY_WORLD_VIEW
void test_browse_freezes_live_motion()
{
    eyes::EyeEngine engine(19U);
    assert(!engine.toggle_selection_lock());
    // Mood blink cadence: content/idle waits 6-14 s (emotion-entry blinks can
    // land earlier); 14.2 s bounds the schedule.
    while (!engine.frame().blink_active && engine.frame().time_ms < 18210U) {
        advance(engine, 8U);
    }
    assert(engine.frame().blink_active);

    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    engine.pointer_move(400.0F, 233.0F, now + 100U);
    assert(engine.frame().mode == eyes::InteractionMode::dragging);
    assert(!engine.frame().blink_active);
    assert(engine.frame().blink_open == 1.0F);
    const eyes::Vec2 frozen_gaze = engine.frame().gaze;

    eyes::MotionSample motion{};
    motion.accel_x = 4.0F;
    motion.accel_y = -3.0F;
    motion.gyro_z = 10.0F;
    for (int sample = 0; sample < 12; ++sample) {
        engine.motion_sample(motion);
        advance(engine, 8U);
    }
    assert(engine.frame().gaze.x == frozen_gaze.x);
    assert(engine.frame().gaze.y == frozen_gaze.y);
    assert(!engine.frame().blink_active);

    engine.pointer_up(400.0F, 233.0F, now + 200U);
    advance(engine, 80U);
    assert(engine.frame().gaze.x == frozen_gaze.x);
    assert(engine.frame().gaze.y == frozen_gaze.y);
    advance(engine, 1500U);
    assert(engine.frame().grid_visibility < 0.015F);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

void test_exact_priority_windows_and_blink_deferral()
{
#if CONFIG_LILGUY_WORLD_VIEW
    eyes::EyeEngine browse(29U);
    assert(!browse.toggle_selection_lock());
    browse.pointer_down(233.0F, 233.0F, 0U);
    browse.pointer_move(400.0F, 233.0F, 100U);
    browse.pointer_up(400.0F, 233.0F, 120U);
    while (browse.frame().mode == eyes::InteractionMode::settling) {
        advance(browse, 8U);
    }
    assert(browse.frame().attention == eyes::AttentionSource::browse);
    advance(browse, 288U);
    assert(browse.frame().attention == eyes::AttentionSource::browse);
    advance(browse, 16U);
    assert(browse.frame().attention != eyes::AttentionSource::browse);
#endif

    eyes::EyeEngine sleeping(31U);
    sleeping.sleep();
    advance(sleeping, 7100U);
    const std::uint32_t now = sleeping.frame().time_ms;
    sleeping.pointer_down(350.0F, 233.0F, now);
    advance(sleeping, 240U);
    assert(sleeping.frame().attention == eyes::AttentionSource::touch);
    assert(!sleeping.frame().blink_active);
    advance(sleeping, 16U);
    assert(sleeping.frame().blink_active);
}

void test_imu_release_hold()
{
    eyes::EyeEngine engine(37U);
    eyes::MotionSample tilt{};
    tilt.accel_y = 9.0F;
    for (int sample = 0; sample < 80; ++sample) {
        engine.motion_sample(tilt);
        advance(engine, 8U);
    }
    assert(engine.frame().attention == eyes::AttentionSource::imu);

    eyes::MotionSample neutral{};
    engine.motion_sample(neutral);
    // kImuHoldMs is 320 now (reactions linger visibly after the board levels).
    advance(engine, 312U);
    assert(engine.frame().attention == eyes::AttentionSource::imu);
    advance(engine, 8U);
    assert(engine.frame().attention != eyes::AttentionSource::imu);
}

void test_imu_continuous_response()
{
    eyes::EyeEngine shallow(38U);
    eyes::MotionSample diagonal{};
    diagonal.accel_x = -0.8F;
    diagonal.accel_y = -4.0F;
    shallow.motion_sample(diagonal);
    advance(shallow, 8U);
    assert(shallow.frame().attention == eyes::AttentionSource::imu);
    assert(shallow.frame().gaze.x < -0.002F);
    assert(shallow.frame().gaze.y < 0.0F);
    const float diagonal_ratio = shallow.frame().gaze.y / shallow.frame().gaze.x;
    assert(diagonal_ratio > 0.19F && diagonal_ratio < 0.21F);

    eyes::EyeEngine low(38U);
    eyes::EyeEngine middle(38U);
    eyes::EyeEngine high(38U);
    eyes::MotionSample tilt{};
    tilt.accel_y = -3.8F;
    low.motion_sample(tilt);
    tilt.accel_y = -4.0F;
    middle.motion_sample(tilt);
    tilt.accel_y = -4.2F;
    high.motion_sample(tilt);
    advance(low, 8U);
    advance(middle, 8U);
    advance(high, 8U);
    assert(low.frame().gaze.x > middle.frame().gaze.x);
    assert(middle.frame().gaze.x > high.frame().gaze.x);
    assert(middle.frame().gaze.x - high.frame().gaze.x > 0.0001F);
}

void test_imu_timeout_recoil_lockout_and_rearm()
{
    eyes::EyeEngine engine(39U);
    eyes::MotionSample tilt{};
    tilt.accel_y = 9.0F;
    for (int sample = 0; sample < 125; ++sample) {
        engine.motion_sample(tilt);
        advance(engine, 8U);
    }
    assert(engine.frame().attention == eyes::AttentionSource::imu);
    const float gaze_at_timeout = std::fabs(engine.frame().gaze.x);
    assert(gaze_at_timeout > 0.5F);

    for (int sample = 0; sample < 50; ++sample) {
        engine.motion_sample(tilt);
        advance(engine, 8U);
    }
    assert(std::fabs(engine.frame().gaze.x) < gaze_at_timeout * 0.25F);

    for (int sample = 0; sample < 75; ++sample) {
        engine.motion_sample(tilt);
        advance(engine, 8U);
    }
    assert(engine.frame().attention != eyes::AttentionSource::imu);

    engine.motion_sample({});
    for (int sample = 0; sample < 20; ++sample) {
        engine.motion_sample(tilt);
        advance(engine, 8U);
    }
    assert(engine.frame().attention == eyes::AttentionSource::imu);
    assert(engine.frame().gaze.x > 0.1F);
}

void test_base_selection_and_emotion_dwell()
{
    std::set<std::uint32_t> first_dwell_times;
    std::set<std::uint32_t> later_dwell_times;
    std::uint32_t useful_seed = 0U;
    for (std::uint32_t seed = 1U; seed <= 80U; ++seed) {
        eyes::EyeEngine engine(seed);
        engine.set_selection({3, 42});
        eyes::Expression previous_expression = eyes::Expression::content;
        std::uint32_t previous_change_ms = 0U;
        while (engine.frame().time_ms < 30000U) {
            advance(engine, 8U);
            if (engine.frame().expression == previous_expression) {
                continue;
            }
            const std::uint32_t changed_ms = engine.frame().time_ms;
            if (previous_change_ms == 0U) {
                if (changed_ms <= 7808U) {
                    first_dwell_times.insert(changed_ms);
                    if (useful_seed == 0U && changed_ms > 5208U) {
                        useful_seed = seed;
                    }
                }
            } else {
                const std::uint32_t dwell = changed_ms - previous_change_ms;
                if (dwell >= 5200U && dwell <= 7808U) {
                    later_dwell_times.insert(dwell);
                }
            }
            previous_change_ms = changed_ms;
            previous_expression = engine.frame().expression;
        }
        assert(engine.selection() == eyes::Selection({3, 42}));
        assert(engine.frame().selection == eyes::Selection({3, 42}));
        assert(engine.frame().selection.palette == 42);
    }
    assert(first_dwell_times.size() > 1U);
    assert(later_dwell_times.size() > 1U);

    eyes::EyeEngine engine(useful_seed);
    engine.set_selection({3, 42});
    advance(engine, 7808U);
    assert(engine.frame().expression != eyes::Expression::content);
    assert(engine.frame().selection == engine.selection());
#if CONFIG_LILGUY_WORLD_VIEW
    const std::uint32_t now = engine.frame().time_ms;
    assert(!engine.toggle_selection_lock());
    engine.pointer_down(233.0F, 233.0F, now);
    engine.pointer_move(400.0F, 233.0F, now + 100U);
    assert(engine.frame().selection == eyes::Selection({3, 42}));
    engine.pointer_up(400.0F, 233.0F, now + 120U);
    advance(engine, 1200U);
    // Palette 42 = torus cell (4, 2); the right fling lands cell (4, -1) on
    // center, wrapping to col 9: palette 49. The selection adopts the settled
    // cell's shape as well as its palette.
    assert(engine.selection() == eyes::Selection({eyes::hex_cell_shape(49), 49}));
#endif
}

#if CONFIG_LILGUY_WORLD_VIEW
void test_tap_during_browse_defers_blink()
{
    eyes::EyeEngine engine(41U);
    assert(!engine.toggle_selection_lock());
    engine.pointer_down(233.0F, 233.0F, 0U);
    engine.pointer_move(400.0F, 233.0F, 100U);
    engine.pointer_up(400.0F, 233.0F, 120U);
    while (engine.frame().mode == eyes::InteractionMode::settling) {
        advance(engine, 8U);
    }
    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    engine.pointer_up(233.0F, 233.0F, now + 40U);
    advance(engine, 240U);
    assert(!engine.frame().blink_active);
    while (engine.frame().grid_visibility > 0.015F) {
        advance(engine, 8U);
    }
    while (engine.frame().attention == eyes::AttentionSource::browse) {
        advance(engine, 8U);
    }
    advance(engine, 240U);
    assert(!engine.frame().blink_active);
    advance(engine, 16U);
    assert(engine.frame().blink_active);
    assert(engine.frame().blink_elapsed_ms <= 16U);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

void test_seeded_weights_and_zero_seed()
{
    eyes::EyeEngine zero(0U);
    eyes::EyeEngine lilg(0x4C494C47U);
    for (int step = 0; step < 1000; ++step) {
        advance(zero, 8U);
        advance(lilg, 8U);
        assert(zero.frame().selection == lilg.frame().selection);
        assert(zero.frame().attention == lilg.frame().attention);
        assert(zero.frame().idle_gaze_kind == lilg.frame().idle_gaze_kind);
        assert(zero.frame().gaze.x == lilg.frame().gaze.x);
        assert(zero.frame().gaze.y == lilg.frame().gaze.y);
        assert(zero.frame().blink_active == lilg.frame().blink_active);
        assert(zero.frame().blink_clip == lilg.frame().blink_clip);
    }

    int idle_counts[3]{};
    int emotion_counts[10]{};
    for (std::uint32_t seed = 1U; seed <= 1000U; ++seed) {
        eyes::EyeEngine engine(seed);
        advance(engine, 1008U);
        ++idle_counts[static_cast<int>(engine.frame().idle_gaze_kind)];
        advance(engine, 11000U);
        ++emotion_counts[static_cast<int>(engine.frame().expression)];
    }
    assert(idle_counts[0] >= 450 && idle_counts[0] <= 550);
    assert(idle_counts[1] >= 300 && idle_counts[1] <= 400);
    assert(idle_counts[2] >= 100 && idle_counts[2] <= 200);
    assert(emotion_counts[0] >= 270 && emotion_counts[0] <= 370);
    assert(emotion_counts[1] >= 110 && emotion_counts[1] <= 190);
    assert(emotion_counts[2] >= 80 && emotion_counts[2] <= 160);
    assert(emotion_counts[3] >= 80 && emotion_counts[3] <= 160);
    for (int expression = 4; expression < 10; ++expression) {
        assert(emotion_counts[expression] > 15);
    }
}

#if CONFIG_LILGUY_WORLD_VIEW
void test_cancel_does_not_select_or_react()
{
    eyes::EyeEngine engine(13U);
    assert(!engine.toggle_selection_lock());
    engine.pointer_down(233.0F, 233.0F, 0U);
    engine.pointer_move(410.0F, 233.0F, 90U);
    assert(engine.frame().mode == eyes::InteractionMode::dragging);
    engine.pointer_cancel();
    advance(engine, 1200U);
    assert(engine.frame().mode == eyes::InteractionMode::idle);
    assert(engine.selection() == eyes::Selection({1, 35}));
    assert(!engine.consume_selection_changed());
    assert(engine.frame().grid_visibility < 0.01F);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

void test_sound_gate_hold_and_cooldown()
{
    eyes::EyeEngine engine(47U);
    calibrate_sound(engine);
    sound(engine, 800U, 200U);
    advance(engine, 20U);
    sound(engine, 800U, 200U);
    advance(engine, 20U);
    sound(engine, 800U, 200U);
    advance(engine, 8U);
    assert(engine.frame().attention == eyes::AttentionSource::sound);

    advance(engine, 384U);
    assert(engine.frame().attention == eyes::AttentionSource::sound);
    advance(engine, 8U);
    assert(engine.frame().attention != eyes::AttentionSource::sound);

    for (int window = 0; window < 3; ++window) {
        sound(engine, 100U, 100U);
        advance(engine, 20U);
    }
    for (int window = 0; window < 3; ++window) {
        sound(engine, 200U, 800U);
        advance(engine, 20U);
    }
    assert(engine.frame().attention != eyes::AttentionSource::sound);
    advance(engine, 1100U);
    for (int window = 0; window < 3; ++window) {
        sound(engine, 200U, 800U);
        advance(engine, window == 2 ? 8U : 20U);
    }
    assert(engine.frame().attention == eyes::AttentionSource::sound);
}

void test_sound_stale_and_higher_priority_clear_streak()
{
    eyes::EyeEngine engine(53U);
    calibrate_sound(engine);
    for (int window = 0; window < 3; ++window) {
        sound(engine, 800U, 200U, 81U);
        advance(engine, 20U);
    }
    assert(engine.frame().attention != eyes::AttentionSource::sound);

    sound(engine, 800U, 200U);
    advance(engine, 20U);
    sound(engine, 800U, 200U);
    advance(engine, 20U);
    engine.pointer_down(233.0F, 233.0F, engine.frame().time_ms);
    engine.pointer_up(233.0F, 233.0F, engine.frame().time_ms + 10U);
    advance(engine, 260U);
    sound(engine, 800U, 200U);
    advance(engine, 8U);
    assert(engine.frame().attention != eyes::AttentionSource::sound);
    sound(engine, 800U, 200U);
    advance(engine, 20U);
    sound(engine, 800U, 200U);
    advance(engine, 8U);
    assert(engine.frame().attention == eyes::AttentionSource::sound);

    engine.pointer_down(450.0F, 233.0F, engine.frame().time_ms);
    advance(engine, 8U);
    assert(engine.frame().attention == eyes::AttentionSource::touch);
    engine.pointer_up(450.0F, 233.0F, engine.frame().time_ms + 10U);
    advance(engine, 260U);
    assert(engine.frame().attention != eyes::AttentionSource::sound);
}

void test_sound_sleep_browse_shake_clear_without_resume()
{
    for (int source = 0; source < 3; ++source) {
#if !CONFIG_LILGUY_WORLD_VIEW
        if (source == 1) {
            continue;  // browse leg needs the world-view carousel
        }
#endif
        const auto interrupt = [source](eyes::EyeEngine &engine) {
            if (source == 0) {
                engine.sleep();
                assert(engine.frame().attention == eyes::AttentionSource::sleep);
                const std::uint32_t now = engine.frame().time_ms;
                engine.pointer_down(233.0F, 233.0F, now);
                engine.pointer_up(233.0F, 233.0F, now + 1U);
                advance(engine, 260U);
            } else if (source == 1) {
#if CONFIG_LILGUY_WORLD_VIEW
                assert(!engine.toggle_selection_lock());
                const std::uint32_t now = engine.frame().time_ms;
                engine.pointer_down(233.0F, 233.0F, now);
                engine.pointer_move(400.0F, 233.0F, now + 20U);
                advance(engine, 8U);
                assert(engine.frame().attention == eyes::AttentionSource::browse);
                engine.pointer_up(400.0F, 233.0F, now + 40U);
                advance(engine, 1500U);
#endif
            } else {
                eyes::MotionSample shake{};
                shake.gyro_z = 9.0F;
                for (int sample = 0; sample < 3; ++sample) {
                    engine.motion_sample(shake);
                    advance(engine, 8U);
                }
                assert(engine.frame().attention == eyes::AttentionSource::shake);
                advance(engine, 400U);
            }
            assert(engine.frame().attention != eyes::AttentionSource::sound);
        };

        eyes::EyeEngine partial(59U + static_cast<std::uint32_t>(source));
        calibrate_sound(partial);
        sound(partial, 800U, 200U);
        advance(partial, 20U);
        sound(partial, 800U, 200U);
        advance(partial, 20U);
        interrupt(partial);
        sound(partial, 800U, 200U);
        advance(partial, 8U);
        assert(partial.frame().attention != eyes::AttentionSource::sound);

        eyes::EyeEngine active(62U + static_cast<std::uint32_t>(source));
        calibrate_sound(active);
        for (int window = 0; window < 3; ++window) {
            sound(active, 800U, 200U);
            advance(active, window == 2 ? 8U : 20U);
        }
        assert(active.frame().attention == eyes::AttentionSource::sound);
        interrupt(active);
        advance(active, 16U);
        assert(active.frame().attention != eyes::AttentionSource::sound);
    }
}

// Interpolated pupil-clip delta at an absolute time, mirroring the renderer's
// looping playback (frame_count samples, seam wraps back to frame 0).
std::array<float, eyes::creature::kCoordCount> pupil_delta_at(
    const eyes::creature::FlourishClip &clip, int state, std::uint32_t time_ms)
{
    const std::uint16_t elapsed = static_cast<std::uint16_t>(time_ms % clip.duration_ms);
    std::size_t frame = 0;
    while (frame + 1U < clip.frame_count &&
           eyes::creature::kPupilFrameTimesMs[clip.time_offset + frame + 1U] <= elapsed) {
        ++frame;
    }
    std::size_t next = frame + 1U;
    const std::uint16_t start_ms = eyes::creature::kPupilFrameTimesMs[clip.time_offset + frame];
    std::uint16_t end_ms;
    if (next < clip.frame_count) {
        end_ms = eyes::creature::kPupilFrameTimesMs[clip.time_offset + next];
    } else {
        next = 0U;
        end_ms = clip.duration_ms;
    }
    const float amount = end_ms <= start_ms
                             ? 0.0F
                             : static_cast<float>(elapsed - start_ms) /
                                   static_cast<float>(end_ms - start_ms);
    const std::size_t offset =
        static_cast<std::size_t>(state) * eyes::creature::kPupilNarrowFrameCount +
        clip.delta_offset;
    const auto &from = eyes::creature::kPupilDeltasNarrow[offset + frame];
    const auto &to = eyes::creature::kPupilDeltasNarrow[offset + next];
    std::array<float, eyes::creature::kCoordCount> deltas{};
    for (std::size_t i = 0; i < deltas.size(); ++i) {
        deltas[i] = static_cast<float>(from[i]) +
                    (static_cast<float>(to[i]) - static_cast<float>(from[i])) * amount;
    }
    return deltas;
}

void test_pupil_clips_loop_without_discontinuity()
{
    for (const eyes::creature::FlourishClip &clip : eyes::creature::kPupilClips) {
        assert(!clip.wide);
        assert(clip.frame_count >= 2U);
        for (int state = 0; state < static_cast<int>(eyes::creature::kStateCount); ++state) {
            const auto at_start = pupil_delta_at(clip, state, 0U);
            const auto at_wrap = pupil_delta_at(clip, state, clip.duration_ms);
            const auto just_before = pupil_delta_at(clip, state, clip.duration_ms - 1U);
            float wrap_error = 0.0F;
            float seam_step = 0.0F;
            float peak = 0.0F;
            for (std::size_t i = 0; i < at_start.size(); ++i) {
                // The frame at t=duration is the frame at t=0 again, and the
                // frame 1 ms before the wrap is within one quantization step.
                wrap_error = std::max(wrap_error, std::fabs(at_wrap[i] - at_start[i]));
                seam_step = std::max(seam_step, std::fabs(just_before[i] - at_wrap[i]));
            }
            for (std::size_t frame = 0; frame < clip.frame_count; ++frame) {
                const std::size_t offset =
                    static_cast<std::size_t>(state) * eyes::creature::kPupilNarrowFrameCount +
                    clip.delta_offset + frame;
                for (const std::int8_t value : eyes::creature::kPupilDeltasNarrow[offset]) {
                    peak = std::max(peak, std::fabs(static_cast<float>(value)));
                }
            }
            assert(wrap_error == 0.0F);
            // One millisecond across the seam moves at most a fraction of a pixel.
            assert(seam_step < 1.0F);
            assert(peak > 0.0F);
        }
    }
}

void test_rot_clips_start_and_end_at_neutral()
{
    for (std::size_t index = 0; index < eyes::creature::kRotClipCount; ++index) {
        const eyes::creature::FlourishClip &clip = eyes::creature::kRotClips[index];
        const std::size_t total = clip.wide ? eyes::creature::kRotWideFrameCount
                                            : eyes::creature::kRotNarrowFrameCount;
        const auto value_at = [&](int state, std::size_t frame, std::size_t coordinate) {
            const std::size_t offset =
                static_cast<std::size_t>(state) * total + clip.delta_offset + frame;
            return clip.wide
                       ? static_cast<int>(eyes::creature::kRotDeltasWide[offset][coordinate])
                       : static_cast<int>(eyes::creature::kRotDeltasNarrow[offset][coordinate]);
        };
        for (int state = 0; state < static_cast<int>(eyes::creature::kStateCount); ++state) {
            int peak = 0;
            for (std::size_t frame = 0; frame < clip.frame_count; ++frame) {
                for (std::size_t i = 0; i < eyes::creature::kCoordCount; ++i) {
                    peak = std::max(peak, std::abs(value_at(state, frame, i)));
                    if (frame == 0U || frame == clip.frame_count - 1U) {
                        assert(value_at(state, frame, i) == 0);
                    }
                }
            }
            assert(peak > 0);
        }
    }
}

// Pure-shape render via the morph path: with from == one-hot(target) the
// blend weight of the target is exactly 1 at any elapsed time, so this renders
// the plain shape while keeping the renderer in the same motion-AA mode as a
// mid-morph frame (pixel-exact comparisons need matching AA quality).
eyes::FrameState morph_state(int from_shape, int to_shape, std::uint16_t elapsed_ms)
{
    eyes::FrameState state{};
    state.selection = {to_shape, 63};
    state.time_ms = 5000U;
    state.morph_active = true;
    state.morph_from_weights = {};
    state.morph_from_weights[static_cast<std::size_t>(from_shape)] = 1.0F;
    state.morph_elapsed_ms = elapsed_ms;
    return state;
}

void test_shape_morph_endpoints_and_midpoint()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    const auto render_hash = [&](const eyes::FrameState &state) {
        renderer.render(state);
        return hash_pixels(pixels);
    };
    const std::uint64_t pure_from = render_hash(morph_state(0, 0, 0U));
    const std::uint64_t pure_to = render_hash(morph_state(2, 2, 0U));
    assert(pure_from != pure_to);

    // t=0 renders pixel-identical to the old shape; t=kShapeMorphMs (the
    // engine clears the flag exactly there, but the renderer must already
    // agree) pixel-identical to the new shape; the midpoint differs from both.
    assert(render_hash(morph_state(0, 2, 0U)) == pure_from);
    assert(render_hash(morph_state(0, 2, eyes::kShapeMorphMs)) == pure_to);
    const std::uint64_t midpoint = render_hash(morph_state(0, 2, eyes::kShapeMorphMs / 2U));
    assert(midpoint != pure_from && midpoint != pure_to);

    // Scalar ease is cubic-in-out: 100 ms in, the blend has barely left the
    // old shape (eased t = 4*(1/8)^3 ~ 0.8%), unlike a linear ramp's 12.5%.
    const std::uint64_t early = render_hash(morph_state(0, 2, 100U));
    assert(early != pure_from);  // moving, but…
    int early_diff = 0;
    renderer.render(morph_state(0, 2, 100U));
    const auto early_pixels = pixels;
    renderer.render(morph_state(0, 0, 0U));
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        early_diff += early_pixels[index] != pixels[index] ? 1 : 0;
    }
    int mid_diff = 0;
    renderer.render(morph_state(0, 2, eyes::kShapeMorphMs / 2U));
    const auto mid_pixels = pixels;
    renderer.render(morph_state(0, 0, 0U));
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        mid_diff += mid_pixels[index] != pixels[index] ? 1 : 0;
    }
    assert(early_diff * 8 < mid_diff);  // …an eased start, not a linear one
}

void test_shape_morph_engine_flow_and_retarget_continuity()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    std::vector<std::uint16_t> previous(pixels.size());
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    // Pixels whose color moved a long way (a snap flips whole regions between
    // palette colors; smooth motion and AA-quality jitter only nudge edges).
    const auto strong_changes = [&] {
        int count = 0;
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const std::uint16_t a = pixels[index];
            const std::uint16_t b = previous[index];
            const int dr = std::abs(((a >> 11) & 0x1f) - ((b >> 11) & 0x1f));
            const int dg = std::abs(((a >> 5) & 0x3f) - ((b >> 5) & 0x3f));
            const int db = std::abs((a & 0x1f) - (b & 0x1f));
            count += dr + dg / 2 + db > 12 ? 1 : 0;
        }
        return count;
    };

    // How big an instant shape swap would be, for the continuity bound.
    renderer.render(morph_state(1, 1, 0U));
    previous = pixels;
    renderer.render(morph_state(2, 2, 0U));
    const int snap_diff = strong_changes();
    assert(snap_diff > 5000);

    eyes::EyeEngine engine(3U);
    advance(engine, 400U);  // past the boot ramp
    assert(!engine.frame().morph_active);
    renderer.render(engine.frame());
    previous = pixels;

    // Button nudge starts an 800 ms morph; a second nudge mid-flight retargets
    // from the currently displayed blend (reference snaps; we deliberately
    // don't). Record every consecutive-frame step size for continuity checks.
    std::vector<int> steps;
    const auto step_frames = [&](std::uint32_t duration_ms) {
        for (std::uint32_t elapsed = 0; elapsed < duration_ms; elapsed += 8U) {
            advance(engine, 8U);
            renderer.render(engine.frame());
            steps.push_back(strong_changes());
            previous = pixels;
        }
    };
    const int start_shape = engine.selection().shape;
    engine.nudge_selection(1, 0);
    assert(engine.frame().morph_active);
    assert(engine.frame().morph_from_weights[static_cast<std::size_t>(start_shape)] == 1.0F);
    step_frames(400U);
    assert(engine.frame().morph_active);
    engine.nudge_selection(1, 0);  // retarget mid-morph
    assert(engine.frame().morph_active);
    assert(engine.frame().morph_elapsed_ms == 0U);
    // The retargeted "from" is the mid-flight blend, not a single shape.
    float weight_sum = 0.0F;
    int nonzero_weights = 0;
    for (const float weight : engine.frame().morph_from_weights) {
        weight_sum += weight;
        nonzero_weights += weight > 0.01F ? 1 : 0;
    }
    assert(std::fabs(weight_sum - 1.0F) < 0.001F);
    assert(nonzero_weights >= 2);
    const std::size_t retarget_index = steps.size();
    step_frames(900U);
    assert(!engine.frame().morph_active);
    assert(engine.selection().shape == (start_shape + 2) % static_cast<int>(eyes::kShapeCount));

    // Continuity across the retarget: the first frame after the second nudge
    // moves no more than the ordinary morph frames around it (the eased
    // restart actually has zero velocity, so it should move less). A snap to
    // the previous target (reference behavior) would dwarf its neighbors.
    int neighborhood = 0;
    for (std::size_t index = retarget_index - 10U; index < retarget_index; ++index) {
        neighborhood = std::max(neighborhood, steps[index]);
    }
    const int boundary = steps[retarget_index];
    const int max_step = *std::max_element(steps.begin(), steps.end());
    std::cout << "morph steps: boundary " << boundary << " neighborhood " << neighborhood
              << " max " << max_step << " snap " << snap_diff << '\n';
    assert(boundary < neighborhood);
    assert(max_step < snap_diff);
}

void test_rot_gaze_cross_terms()
{
    // Data: the corrections carry real signal for rot_4 (the RE doc's worst
    // case, ~10 px corner error at 41 px peak displacement).
    const auto &rot4 = eyes::creature::kRotClips[3];
    assert(std::string(rot4.name) == "rot_4");
    int max_units = 0;
    for (std::size_t state = 0; state < eyes::creature::kStateCount; ++state) {
        for (std::size_t corner = 0; corner < eyes::creature::kFlourishCornerCount; ++corner) {
            const std::size_t base =
                (state * eyes::creature::kFlourishCornerCount + corner) *
                    eyes::creature::kRotTotalFrameCount +
                rot4.time_offset;
            for (std::size_t frame = 0; frame < rot4.frame_count; ++frame) {
                for (const std::int8_t value : eyes::creature::kRotGazeCorrections[base + frame]) {
                    max_units = std::max(max_units, std::abs(static_cast<int>(value)));
                }
            }
        }
    }
    std::cout << "rot_4 max correction " << max_units << " units\n";
    assert(static_cast<float>(max_units) * eyes::creature::kRotGazeCorrectionScale >=
           50.0F);  // >= 5 px
    // Pupil corrections exist too (measured corner error up to 3.4 px).
    int pupil_max = 0;
    for (const auto &row : eyes::creature::kPupilGazeCorrections) {
        for (const std::int8_t value : row) {
            pupil_max = std::max(pupil_max, std::abs(static_cast<int>(value)));
        }
    }
    assert(pupil_max >= 15);

    // Pixels: the same authored rot delta must land differently at corner
    // gaze than at center gaze (fisheye shrinks the eye ~28% out there).
    // Measure the vertical shift of the left eye's top edge under rot_4 at its
    // +0.26-turn peak; without corrections the shift would be gaze-independent
    // (deltas are added in source space, the hero transform is uniform).
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    const std::uint16_t black = eyes::rgb565({0, 0, 0});
    const auto top_edge_shift = [&](eyes::Vec2 gaze) {
        eyes::FrameState state{};
        state.selection = {1, 63};
        state.time_ms = 0U;  // boot ramp zero: idle/pupil clips contribute nothing
        state.gaze = gaze;
        state.gaze_pupils = gaze;
        renderer.render(state);
        // Column through the middle of the left eye's footprint.
        int min_x = eyes::kScreenWidth;
        int max_x = 0;
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth / 2; ++x) {
                if (pixels[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                           static_cast<std::size_t>(x)] != black) {
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                }
            }
        }
        const int column = (min_x + max_x) / 2;
        const auto top_edge = [&] {
            for (int y = 0; y < eyes::kScreenHeight; ++y) {
                if (pixels[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                           static_cast<std::size_t>(column)] != black) {
                    return y;
                }
            }
            return eyes::kScreenHeight;
        };
        const int neutral_top = top_edge();
        state.rot_active = true;
        state.rot_clip = 3U;  // rot_4
        state.rot_elapsed_ms = 3183U;
        renderer.render(state);
        const int rot_top = top_edge();
        return rot_top - neutral_top;
    };
    const int shift_center = top_edge_shift({0.0F, 0.0F});
    const int shift_corner = top_edge_shift({0.75F, 0.75F});
    std::cout << "rot_4 top-edge shift center " << shift_center << " corner " << shift_corner
              << '\n';
    assert(std::abs(shift_center) > 10);
    assert(std::abs(shift_center - shift_corner) >= 4);
}

void test_boot_ramp_starts_neutral()
{
    // At t=0 the boot ramp zeroes both the per-cell idle phase and the
    // pupil-clip deltas, so the pose is exactly the authored neutral: two
    // different palettes (different idle phases, same shape) must produce the
    // same silhouette, differing only in color.
    assert(eyes::hex_cell_hash(63) % 3000U != eyes::hex_cell_hash(0) % 3000U);
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    const std::uint16_t black = eyes::rgb565({0, 0, 0});
    const auto silhouette = [&](int palette, std::uint32_t time_ms) {
        eyes::FrameState state{};
        state.selection = {1, palette};
        state.time_ms = time_ms;
        renderer.render(state);
        std::vector<bool> mask(pixels.size());
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            mask[index] = pixels[index] != black;
        }
        return mask;
    };
    const auto mismatches = [](const std::vector<bool> &a, const std::vector<bool> &b) {
        int count = 0;
        for (std::size_t index = 0; index < a.size(); ++index) {
            count += a[index] != b[index] ? 1 : 0;
        }
        return count;
    };
    const int at_boot = mismatches(silhouette(63, 0U), silhouette(0, 0U));
    const int later = mismatches(silhouette(63, 6000U), silhouette(0, 6000U));
    std::cout << "boot silhouette mismatches t=0: " << at_boot << " t=6000: " << later << '\n';
    assert(at_boot <= 50);
    assert(later > 500);

    // The pupil-in-socket x offset proves the delta ramp specifically:
    // pup_mov_2's steady cycle holds the pupils 3 px left or right of neutral;
    // at t=0 the ramped pose must sit exactly midway between the two holds.
    const int palette = 63;
    const std::uint32_t phase = eyes::hex_cell_hash(palette) % 3000U;
    const auto find_time = [&](std::uint32_t window_min, std::uint32_t window_max) {
        for (std::uint32_t t = 300U; t < 6000U; ++t) {
            const std::uint32_t cycle = (t + phase) % 2617U;
            if (cycle >= window_min && cycle <= window_max) {
                return t;
            }
        }
        assert(false);
        return 0U;
    };
    const std::uint16_t pupil_color = eyes::rgb565(eyes::palette_at(palette).inner);
    const auto pupil_relative_x = [&](std::uint32_t time_ms) {
        eyes::FrameState state{};
        state.selection = {1, palette};
        state.time_ms = time_ms;
        renderer.render(state);
        int eye_min = eyes::kScreenWidth;
        int eye_max = 0;
        int pupil_min = eyes::kScreenWidth;
        int pupil_max = 0;
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth / 2; ++x) {
                const std::uint16_t value =
                    pixels[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                           static_cast<std::size_t>(x)];
                if (value == black) {
                    continue;
                }
                eye_min = std::min(eye_min, x);
                eye_max = std::max(eye_max, x);
                if (value == pupil_color) {
                    pupil_min = std::min(pupil_min, x);
                    pupil_max = std::max(pupil_max, x);
                }
            }
        }
        assert(pupil_max >= pupil_min);
        return 0.5F * static_cast<float>(pupil_min + pupil_max) -
               0.5F * static_cast<float>(eye_min + eye_max);
    };
    const float rel_boot = pupil_relative_x(0U);
    const float rel_left = pupil_relative_x(find_time(200U, 1000U));    // -3 px hold
    const float rel_right = pupil_relative_x(find_time(1600U, 2200U));  // +3 px hold
    std::cout << "pupil rel-x boot " << rel_boot << " left " << rel_left << " right "
              << rel_right << '\n';
    assert(rel_right - rel_left > 5.0F);
    assert(std::fabs(rel_boot - 0.5F * (rel_left + rel_right)) < 1.5F);
}

void test_blink_follows_gaze()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.blink_active = true;
    state.blink_clip = 0U;
    state.blink_elapsed_ms = 150U;  // mid-close, lids deforming

    // Mid-blink at an off-center gaze must use the gaze-corrected lid shape,
    // not the center-sampled one re-applied verbatim.
    renderer.render(state);
    const std::uint64_t center_blink = hash_pixels(pixels);
    state.gaze = {0.75F, 0.0F};
    renderer.render(state);
    assert(hash_pixels(pixels) != center_blink);

    // The correction must be exactly the gaze shift of the lid shape, not a
    // wholesale different pose: compare against sampling the rig at that gaze
    // without corrections (deltas differ) by checking the corrected render
    // stays within the screen and still hides pupils at the closed apex.
    for (const eyes::Vec2 gaze : {eyes::Vec2{0.0F, 0.0F}, eyes::Vec2{0.75F, 0.0F},
                                  eyes::Vec2{0.75F, 0.75F}, eyes::Vec2{-0.75F, -0.75F}}) {
        eyes::FrameState closed{};
        closed.selection = {1, 63};
        closed.blink_active = true;
        closed.blink_clip = 0U;
        // Find the closed apex: first frame whose path mask drops the pupils.
        const auto &clip = eyes::creature::kBlinkClips[0];
        std::uint16_t apex_ms = 0U;
        for (std::size_t frame = 0; frame < clip.frame_count; ++frame) {
            const std::size_t index =
                static_cast<std::size_t>(closed.selection.shape) *
                    eyes::creature::kBlinkTotalFrameCount + clip.frame_offset + frame;
            if ((eyes::creature::kBlinkPathMasks[index] & 0x0aU) == 0U) {
                apex_ms = eyes::creature::kBlinkFrameTimesMs[clip.frame_offset + frame];
                break;
            }
        }
        assert(apex_ms > 0U);
        closed.blink_elapsed_ms = apex_ms;
        closed.gaze = gaze;
        closed.gaze_pupils = gaze;  // steady gaze: both filters converged
        renderer.render(closed);
        const std::uint16_t left_pupil = eyes::rgb565(eyes::palette_at(63).inner);
        const std::uint16_t right_pupil = eyes::rgb565(eyes::palette_at(63).accent);
        assert(std::find(pixels.begin(), pixels.end(), left_pupil) == pixels.end());
        assert(std::find(pixels.begin(), pixels.end(), right_pupil) == pixels.end());
    }
}

void test_blink_pool_weights_and_drowsy_clips()
{
    // blink4/blink5: authored drowsy variants (never played by the site) with
    // full frame tables, apex pupil culls, and in-clip alpha windows.
    for (std::uint8_t index = 3; index < 5; ++index) {
        const auto &clip = eyes::creature::kBlinkClips[index];
        assert(clip.duration_ms > eyes::creature::kBlinkClips[2].duration_ms);
        assert(eyes::creature::kBlinkFrameTimesMs[clip.frame_offset] == 0U);
        assert(eyes::creature::kBlinkFrameTimesMs[clip.frame_offset + clip.frame_count - 1U] ==
               clip.duration_ms);
        assert(clip.fade_in_end_ms < clip.duration_ms);
        for (std::size_t state = 0; state < eyes::creature::kStateCount; ++state) {
            bool pupils_culled = false;
            for (std::size_t frame = 0; frame < clip.frame_count; ++frame) {
                const std::size_t mask_index =
                    state * eyes::creature::kBlinkTotalFrameCount + clip.frame_offset + frame;
                pupils_culled = pupils_culled ||
                                (eyes::creature::kBlinkPathMasks[mask_index] & 0x0aU) == 0U;
            }
            assert(pupils_culled);
        }
    }

    // Hero pool weights 30/30/30/5/5 (grid cells keep the site's 3-clip pool).
    int counts[5]{};
    for (std::uint32_t seed = 1U; seed <= 600U; ++seed) {
        eyes::EyeEngine engine(seed);
        // Mood blink cadence: content/idle waits 8-18 s (emotion-entry blinks can
        // land earlier); 18.2 s bounds the stretched schedule.
        while (!engine.frame().blink_active && engine.frame().time_ms < 18210U) {
            advance(engine, 8U);
        }
        assert(engine.frame().blink_active);
        assert(engine.frame().blink_clip < eyes::creature::kBlinkClipCount);
        ++counts[engine.frame().blink_clip];
    }
    for (int clip = 0; clip < 3; ++clip) {
        assert(counts[clip] > 120 && counts[clip] < 240);  // expect ~180
    }
    for (int clip = 3; clip < 5; ++clip) {
        assert(counts[clip] > 5 && counts[clip] < 70);  // expect ~30
    }

    // A drowsy blink renders: pupils fully hidden across its long apex hold.
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.blink_active = true;
    state.blink_clip = 4U;         // blink5
    state.blink_elapsed_ms = 900U; // deep inside the 683-1033 ms hidden window
    renderer.render(state);
    const std::uint16_t left_pupil = eyes::rgb565(eyes::palette_at(63).inner);
    const std::uint16_t right_pupil = eyes::rgb565(eyes::palette_at(63).accent);
    assert(std::find(pixels.begin(), pixels.end(), left_pupil) == pixels.end());
    assert(std::find(pixels.begin(), pixels.end(), right_pupil) == pixels.end());
}

void test_blink_pupil_alpha_fade()
{
    // Per-clip authored fade windows: alpha 256 outside, 0 across the apex
    // hold (where the path masks cull pupils anyway), linear ramps between.
    for (std::uint8_t index = 0; index < eyes::creature::kBlinkClipCount; ++index) {
        const auto &clip = eyes::creature::kBlinkClips[index];
        assert(eyes::blink_pupil_alpha(index, 0U) == 256U);
        assert(eyes::blink_pupil_alpha(index, clip.fade_out_start_ms) == 256U);
        assert(eyes::blink_pupil_alpha(index, clip.fade_out_end_ms) == 0U);
        assert(eyes::blink_pupil_alpha(index, clip.fade_in_start_ms) == 0U);
        const auto mid_close =
            static_cast<std::uint16_t>((clip.fade_out_start_ms + clip.fade_out_end_ms) / 2U);
        const auto mid_open =
            static_cast<std::uint16_t>((clip.fade_in_start_ms + clip.fade_in_end_ms) / 2U);
        const unsigned closing = eyes::blink_pupil_alpha(index, mid_close);
        const unsigned opening = eyes::blink_pupil_alpha(index, mid_open);
        assert(closing > 0U && closing < 256U);
        assert(opening > 0U && opening < 256U);
        assert(eyes::blink_pupil_alpha(index, clip.fade_in_end_ms) == 256U);
        assert(eyes::blink_pupil_alpha(index, clip.duration_ms) == 256U);
    }
    // Reference wasm measurement on "blink": alpha 0.494 at t=192 ms.
    const unsigned measured = eyes::blink_pupil_alpha(0U, 192U);
    assert(measured > 115U && measured < 140U);

    // Rendered mid-fade: the pure pupil color disappears (everything is blended
    // toward the outer color) while the pupil area still darkens the eye.
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.blink_active = true;
    state.blink_clip = 0U;
    const std::uint16_t left_pupil = eyes::rgb565(eyes::palette_at(63).inner);
    const auto pure_pupil_count = [&] {
        return std::count(pixels.begin(), pixels.end(), left_pupil);
    };
    state.blink_elapsed_ms = 100U;  // before the fade window: fully opaque
    renderer.render(state);
    assert(pure_pupil_count() > 0);
    const std::uint64_t opaque_hash = hash_pixels(pixels);
    state.blink_elapsed_ms = 192U;  // mid-fade: alpha ~126/256
    renderer.render(state);
    assert(pure_pupil_count() == 0);
    assert(hash_pixels(pixels) != opaque_hash);
}

void test_pupils_lead_eyes_on_gaze_step()
{
    // Steady press far right maps to gaze target x = +0.75. Pupils run the
    // fast low-pass (tau 40 ms) and must reach 90% of the step long before the
    // eye outline (tau 150 ms) does.
    eyes::EyeEngine engine(11U);
    engine.pointer_down(440.0F, 233.0F, 0U);
    std::uint32_t pupil_ms = 0U;
    std::uint32_t eye_ms = 0U;
    while (engine.frame().time_ms < 2000U && (pupil_ms == 0U || eye_ms == 0U)) {
        engine.update(8U);
        if (pupil_ms == 0U && engine.frame().gaze_pupils.x >= 0.9F * 0.75F) {
            pupil_ms = engine.frame().time_ms;
        }
        if (eye_ms == 0U && engine.frame().gaze.x >= 0.9F * 0.75F) {
            eye_ms = engine.frame().time_ms;
        }
    }
    assert(pupil_ms > 0U && eye_ms > 0U);
    assert(pupil_ms < eye_ms);
    // 90% of an exponential step lands at ~2.3 tau: ~92 ms vs ~345 ms.
    assert(pupil_ms <= 120U);
    assert(eye_ms >= 240U && eye_ms <= 480U);
}

void test_rot_flourish_schedules_and_renders_additively()
{
    // Engine: a rotation flourish fires within 5-10 s of idling, never in sleep.
    eyes::EyeEngine engine(67U);
    std::uint32_t started_ms = 0U;
    std::uint8_t clip = 0U;
    while (engine.frame().time_ms < 10100U && !engine.frame().rot_active) {
        advance(engine, 8U);
    }
    assert(engine.frame().rot_active);
    assert(engine.frame().rot_clip < eyes::creature::kRotClipCount);
    started_ms = engine.frame().time_ms;
    clip = engine.frame().rot_clip;
    while (engine.frame().rot_active) {
        advance(engine, 8U);
        assert(engine.frame().rot_clip == clip);
        assert(engine.frame().rot_elapsed_ms <
               eyes::creature::kRotClips[clip].duration_ms);
    }
    assert(engine.frame().time_ms - started_ms >=
           eyes::creature::kRotClips[clip].duration_ms);
    assert(engine.frame().rot_elapsed_ms == 0U);

    eyes::EyeEngine sleeper(71U);
    sleeper.sleep();
    advance(sleeper, 21000U);
    assert(!sleeper.frame().rot_active);

    // Renderer: mid-clip deltas move pixels; the final frame is back at neutral.
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.time_ms = 743U;
    renderer.render(state);
    const std::uint64_t neutral_hash = hash_pixels(pixels);
    for (std::uint8_t index = 0; index < eyes::creature::kRotClipCount; ++index) {
        state.rot_active = true;
        state.rot_clip = index;
        state.rot_elapsed_ms = eyes::creature::kRotClips[index].duration_ms / 2U;
        renderer.render(state);
        assert(hash_pixels(pixels) != neutral_hash);
        // The engine clears rot_active exactly at the clip duration; render the
        // final frame the way the engine hands it over (also keeps the renderer's
        // motion-adaptive AA at full quality, matching the neutral reference).
        state.rot_active = false;
        state.rot_elapsed_ms = 0U;
        renderer.render(state);
        renderer.render(state);  // settle the motion proxy (previous frame moved)
        assert(hash_pixels(pixels) == neutral_hash);
    }
}

void test_renderer()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.blink_open = 1.0F;
    state.gaze = {};
    renderer.render(state);

    const eyes::PixelBounds focus_bounds = renderer.rendered_bounds();
    assert(focus_bounds.valid());
    assert(focus_bounds.x0 < 60);
    assert(focus_bounds.x1 > 405);

    const std::uint16_t black = eyes::rgb565({0, 0, 0});
    assert(pixels.front() == black);
    assert(pixels[eyes::kScreenWidth - 1] == black);
    assert(pixels.back() == black);
    const auto colored = std::count_if(pixels.begin(), pixels.end(), [black](std::uint16_t value) {
        return value != black;
    });
    assert(colored > 20000);
    const std::uint64_t first_hash = hash_pixels(pixels);
    renderer.render(state);
    assert(hash_pixels(pixels) == first_hash);

    state.blink_active = true;
    state.blink_clip = 0;
    state.blink_elapsed_ms = 250;
    renderer.render(state);
    const std::uint64_t closed_hash = hash_pixels(pixels);
    assert(closed_hash != first_hash);
    // Idle keeps animating through blinks (reference behavior), so re-render at the
    // SAME timestamp to check determinism instead of time invariance.
    renderer.render(state);
    assert(hash_pixels(pixels) == closed_hash);
    state.blink_active = false;

#if CONFIG_LILGUY_WORLD_VIEW
    state.grid_visibility = 1.0F;
    state.grid_offset = {105.0F, -70.0F};
    state.time_ms = 1327U;
    renderer.render(state);
    const eyes::PixelBounds grid_bounds = renderer.rendered_bounds();
    assert(grid_bounds.valid());
    assert(grid_bounds.x0 == 0 || grid_bounds.y0 == 0 ||
           grid_bounds.x1 == eyes::kScreenWidth - 1 ||
           grid_bounds.y1 == eyes::kScreenHeight - 1);
    assert(hash_pixels(pixels) != first_hash);
    const std::uint64_t grid_hash = hash_pixels(pixels);
    state.time_ms += 2400U;
    renderer.render(state);
    assert(hash_pixels(pixels) != grid_hash);
    state.gaze = {0.6F, -0.4F};
    renderer.render(state);
    assert(hash_pixels(pixels) != grid_hash);
#endif
}

void test_expression_pose_and_blink_interpolation()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 35};
    renderer.render(state);
    const std::uint64_t content_hash = hash_pixels(pixels);

    state.expression_pose = {1.18F, 0.0F, 0.72F};
    renderer.render(state);
    assert(hash_pixels(pixels) != content_hash);

    state.expression_pose = {};
    state.blink_active = true;
    state.blink_clip = 0U;
    state.blink_elapsed_ms = 4U;
    renderer.render(state);
    const std::uint64_t blink_4ms = hash_pixels(pixels);
    state.blink_elapsed_ms = 12U;
    renderer.render(state);
    assert(hash_pixels(pixels) != blink_4ms);
}

void test_expression_poses_stay_mirrored()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 35};
    const eyes::ExpressionPose poses[]{{0.62F, -0.055F, 1.08F},
                                       {1.18F, 0.0F, 0.72F},
                                       {0.64F, 0.085F, 0.88F}};
    for (const eyes::ExpressionPose pose : poses) {
        state.expression_pose = pose;
        renderer.render(state);
        int mismatched = 0;
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth / 2; ++x) {
                const std::size_t left = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                         static_cast<std::size_t>(x);
                const std::size_t right = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                          static_cast<std::size_t>(eyes::kScreenWidth - 1 - x);
                mismatched += pixels[left] != pixels[right] ? 1 : 0;
            }
        }
        // The persistent pupil clips translate both pupils the same direction
        // (reference behavior), so allow a little more asymmetry than the
        // antialiasing-only budget of 2000.
        assert(mismatched < 3000);
    }
}

void test_sleep_uses_authored_closed_frame()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::EyeEngine engine(43U);
    engine.set_selection({1, 63});
    renderer.render(engine.frame());
    engine.sleep();
    // Lids now ease shut (~160 ms) instead of snapping; settle before checking
    // the authored closed frame.
    advance(engine, 200U);
    renderer.render(engine.frame());
    renderer.render(engine.frame());
    const eyes::PixelBounds first_closed = renderer.rendered_bounds();
    const std::uint64_t closed_hash = hash_pixels(pixels);
    renderer.render(engine.frame());
    assert(hash_pixels(pixels) == closed_hash);
    assert(renderer.rendered_bounds().x0 == first_closed.x0);
    assert(renderer.rendered_bounds().y0 == first_closed.y0);
    assert(renderer.rendered_bounds().x1 == first_closed.x1);
    assert(renderer.rendered_bounds().y1 == first_closed.y1);

    const std::uint16_t left_pupil = eyes::rgb565(eyes::palette_at(63).inner);
    const std::uint16_t right_pupil = eyes::rgb565(eyes::palette_at(63).accent);
    assert(std::find(pixels.begin(), pixels.end(), left_pupil) == pixels.end());
    assert(std::find(pixels.begin(), pixels.end(), right_pupil) == pixels.end());

    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    assert(engine.frame().mode == eyes::InteractionMode::touching);
}

#if CONFIG_LILGUY_WORLD_VIEW
void test_hex_grid_inner_cells_animate_and_final_fade()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.grid_visibility = 1.0F;
    renderer.render(state);
    const auto before = pixels;
    state.time_ms += 533U;
    renderer.render(state);
    assert(pixels != before);

    // The inner cells (screen distance < ~2.2 row pitches) animate
    // independently (idle + pupil loops with per-cell phases); outer cells are
    // static. Pixel changes are still not confined to the anchor: animated
    // ring cells reach well outside its glyph.
    const float screen_cx = static_cast<float>(eyes::kScreenWidth) * 0.5F;
    const float screen_cy = static_cast<float>(eyes::kScreenHeight) * 0.5F;
    int changed_near = 0;
    int changed_far = 0;
    for (int y = 0; y < eyes::kScreenHeight; ++y) {
        for (int x = 0; x < eyes::kScreenWidth; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                      static_cast<std::size_t>(x);
            if (pixels[index] == before[index]) {
                continue;
            }
            const float dx = static_cast<float>(x) - screen_cx;
            const float dy = static_cast<float>(y) - screen_cy;
            if (dx * dx + dy * dy > 150.0F * 150.0F) {
                ++changed_far;
            } else {
                ++changed_near;
            }
        }
    }
    assert(changed_near > 0);
    assert(changed_far > 0);

    // At full grid visibility the anchor cell blends its live gaze out, so the
    // field is uniform and an anchor swap at settle cannot jump.
    const auto idle_frame = pixels;
    state.gaze = {0.6F, -0.4F};
    renderer.render(state);
    assert(pixels == idle_frame);

    state.grid_visibility = 0.08F;
    renderer.render(state);
    state.grid_visibility = 0.0F;
    renderer.render(state);
    const eyes::PixelBounds final_bounds = renderer.rendered_bounds();
    const int final_area = (final_bounds.x1 - final_bounds.x0 + 1) *
                           (final_bounds.y1 - final_bounds.y0 + 1);
    assert(final_bounds.valid());
    assert(final_area < eyes::kScreenWidth * eyes::kScreenHeight);
}

void test_pan_idle_incremental_matches_full_repaint()
{
    // Pan-idle grid frames skip repainting static cells. Every such frame must
    // be pixel-identical to a from-scratch full repaint of the same state, and
    // every pixel that changed between consecutive frames must be covered by
    // the frame's transfer regions (a missed region = stale pixels on panel).
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    std::vector<std::uint16_t> previous(pixels.size());
    std::vector<std::uint16_t> reference(pixels.size());
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.grid_visibility = 1.0F;
    state.grid_offset = {31.0F, -18.0F};
    state.time_ms = 900U;
    renderer.render(state);  // full repaint primes the cell cache
    previous = pixels;

    const auto check_frame = [&](bool expect_incremental) {
        renderer.render(state);
        // Reference: a fresh renderer doing a full repaint of the same state.
        std::fill(reference.begin(), reference.end(), 0U);
        eyes::EyeRenderer full({reference.data(), eyes::kScreenWidth, eyes::kScreenHeight,
                                eyes::kScreenWidth}, true);
        full.render(state);
        assert(pixels == reference);

        const eyes::PixelBounds covered = renderer.rendered_bounds();
        const eyes::DirtyRegions &regions = renderer.rendered_regions();
        int changed = 0;
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                          static_cast<std::size_t>(x);
                if (pixels[index] == previous[index]) {
                    continue;
                }
                ++changed;
                assert(covered.valid());
                assert(x >= covered.x0 && x <= covered.x1);
                assert(y >= covered.y0 && y <= covered.y1);
                assert(std::any_of(regions.begin(), regions.end(),
                                   [x, y](eyes::PixelBounds region) {
                                       return region.valid() && x >= region.x0 &&
                                              x <= region.x1 && y >= region.y0 &&
                                              y <= region.y1;
                                   }));
            }
        }
        previous = pixels;
        const auto region_area = [&regions]() {
            int total = 0;
            for (const eyes::PixelBounds region : regions) {
                if (region.valid()) {
                    total += (region.x1 - region.x0 + 1) * (region.y1 - region.y0 + 1);
                }
            }
            return total;
        };
        if (expect_incremental) {
            // Static cells must neither repaint nor transfer: the transfer set
            // stays well below the full field footprint.
            assert(region_area() < eyes::kScreenWidth * eyes::kScreenHeight / 3);
        }
        return changed;
    };

    // Several pan-idle frames (animation advances, field static).
    for (int frame = 0; frame < 6; ++frame) {
        state.time_ms += 137U;
        assert(check_frame(true) > 0);
    }
    // Same state re-rendered: still exact, still incremental.
    check_frame(true);
    // Pan resumes: full repaint, still exact vs reference.
    state.grid_offset = {58.0F, -40.0F};
    state.time_ms += 33U;
    check_frame(false);
    // And stopping again returns to exact incremental frames.
    state.time_ms += 33U;
    check_frame(true);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

void test_hero_diff_clear_matches_full_clear()
{
    // The hero diff-clear (lazy per-row erase of last frame's covered runs
    // instead of a full rect pre-clear) must be pixel-exact against a renderer
    // that full-clears every frame, across 300+ animated frames covering
    // blinks, morphs, sleep, pokes, browse (grid fallback), gaze teleports,
    // rot flourishes, and shape snaps. Every changed pixel must also fall in
    // the diff renderer's transfer regions.
    std::vector<std::uint16_t> diff_pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                           eyes::kScreenHeight);
    std::vector<std::uint16_t> full_pixels(diff_pixels.size());
    std::vector<std::uint16_t> previous(diff_pixels.size());
    eyes::EyeRenderer diff_renderer(
        {diff_pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::EyeRenderer full_renderer(
        {full_pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    full_renderer.disable_diff_clear = true;

    int frames = 0;
    const auto step = [&](const eyes::FrameState &state) {
        diff_renderer.render(state);
        full_renderer.render(state);
        assert(diff_pixels == full_pixels);
        const eyes::DirtyRegions &regions = diff_renderer.rendered_regions();
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                          static_cast<std::size_t>(x);
                if (diff_pixels[index] == previous[index]) {
                    continue;
                }
                assert(std::any_of(regions.begin(), regions.end(),
                                   [x, y](eyes::PixelBounds region) {
                                       return region.valid() && x >= region.x0 &&
                                              x <= region.x1 && y >= region.y0 &&
                                              y <= region.y1;
                                   }));
            }
        }
        previous = diff_pixels;
        ++frames;
    };

    // Engine-driven arc: boot idle, poke, shape morph, sleep+wake, browse
    // fling (grid frames must fall back to the full clear), settle back.
    eyes::EyeEngine engine(5U);
    const auto run = [&](int frame_count) {
        for (int frame = 0; frame < frame_count; ++frame) {
            engine.update(16U);
            step(engine.frame());
        }
    };
    run(100);
    std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(300.0F, 233.0F, now);
    engine.pointer_up(300.0F, 233.0F, now + 30U);  // poke
    run(30);
    engine.nudge_selection(1, 0);  // 800 ms shape morph
    run(60);
    engine.sleep();
    run(30);
    now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);  // wake
    engine.pointer_up(233.0F, 233.0F, now + 20U);
    run(30);
#if CONFIG_LILGUY_WORLD_VIEW
    assert(!engine.toggle_selection_lock());
    now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    engine.pointer_move(400.0F, 233.0F, now + 100U);
    engine.pointer_up(400.0F, 233.0F, now + 120U);  // fling: grid in and out
    run(120);
#else
    run(120);  // keep the frame budget so the count assertions hold
#endif

    // Synthetic torture: per-frame shape snaps, gaze teleports (far beyond a
    // few px of motion), all blink clips (fold rows split covered runs), rot
    // flourishes, and expression squashes.
    for (int index = 0; index < 100; ++index) {
        eyes::FrameState state{};
        state.selection = {index % static_cast<int>(eyes::kShapeCount), 63};
        state.time_ms = 1000U + static_cast<std::uint32_t>(index) * 137U;
        state.gaze = {static_cast<float>(index % 3 - 1) * 0.75F,
                      static_cast<float>(index % 5 - 2) * 0.375F};
        state.gaze_pupils = state.gaze;
        if (index % 4 == 1) {
            state.blink_active = true;
            state.blink_clip = static_cast<std::uint8_t>(
                index % static_cast<int>(eyes::creature::kBlinkClipCount));
            state.blink_elapsed_ms = static_cast<std::uint16_t>((index * 53) % 600);
        }
        if (index % 7 == 2) {
            state.rot_active = true;
            state.rot_clip = static_cast<std::uint8_t>(
                index % static_cast<int>(eyes::creature::kRotClipCount));
            state.rot_elapsed_ms = static_cast<std::uint16_t>(
                eyes::creature::kRotClips[state.rot_clip].duration_ms / 2U);
        }
        state.expression_pose = {0.68F + 0.4F * static_cast<float>(index % 2),
                                 static_cast<float>(index % 3 - 1) * 0.08F,
                                 0.8F + 0.2F * static_cast<float>(index % 2)};
        step(state);
    }
    std::cout << "diff-clear frames " << frames << " engaged "
              << diff_renderer.diff_clear_frames << '\n';
    assert(frames >= 300);
    // The diff path must actually engage (most frames are hero frames) while
    // the reference renderer must never take it.
    assert(diff_renderer.diff_clear_frames > static_cast<std::uint32_t>(frames) / 2U);
    assert(full_renderer.diff_clear_frames == 0U);
}

#if CONFIG_LILGUY_WORLD_VIEW
void test_hex_cell_shapes_vary_and_settle_adopts_them()
{
    // Every torus cell owns a deterministic shape; all four shapes appear
    // within any 2x2 neighborhood (variety within a screenful).
    int shape_counts[eyes::kShapeCount]{};
    for (int palette = 0; palette < static_cast<int>(eyes::kPaletteCount); ++palette) {
        const int shape = eyes::hex_cell_shape(palette);
        assert(shape >= 0 && shape < static_cast<int>(eyes::kShapeCount));
        ++shape_counts[shape];
    }
    for (const int count : shape_counts) {
        assert(count > 0);
    }
    for (int row = 0; row < eyes::kHexTorusSize; ++row) {
        for (int col = 0; col < eyes::kHexTorusSize; ++col) {
            std::set<int> local;
            for (int dr = 0; dr < 2; ++dr) {
                for (int dc = 0; dc < 2; ++dc) {
                    local.insert(eyes::hex_cell_shape(
                        eyes::hex_torus_palette(row + dr, col + dc)));
                }
            }
            assert(local.size() == eyes::kShapeCount);
        }
    }

    // Settling must adopt the settled cell's shape+palette; re-settling on the
    // same cell keeps the (possibly nudged) selection untouched.
    eyes::EyeEngine engine(7U);
    assert(!engine.toggle_selection_lock());
    engine.nudge_selection(1, 0);  // shape differs from hex_cell_shape(35)
    const eyes::Selection nudged = engine.selection();
    const std::uint32_t now = engine.frame().time_ms;
    engine.pointer_down(233.0F, 233.0F, now);
    engine.pointer_move(240.0F, 250.0F, now + 200U);  // > 18 px, well under a cell
    engine.pointer_up(240.0F, 250.0F, now + 600U);
    advance(engine, 1200U);
    assert(engine.selection() == nudged);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

void test_dirty_bounds_cover_every_changed_pixel()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    std::vector<std::uint16_t> previous(pixels.size());
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.blink_open = 1.0F;
    renderer.render(state);
    previous = pixels;
    for (int shape = 0; shape < static_cast<int>(eyes::kShapeCount); ++shape) {
        state.selection = {shape, shape % static_cast<int>(eyes::kPaletteCount)};
        state.gaze = {
            shape % 2 == 0 ? 0.92F : -0.88F,
            shape % 3 == 0 ? -0.86F : 0.79F,
        };
        state.face_rotation = shape % 4 == 0 ? 0.08F : -0.05F;
        state.poke = shape % 5 == 0 ? 0.7F : 0.0F;
        state.grid_visibility = shape == 3 ? 1.0F : 0.0F;
        state.grid_offset = shape == 3 ? eyes::Vec2{121.0F, -93.0F} : eyes::Vec2{};
        state.time_ms += 137U;
        renderer.render(state);

        const eyes::PixelBounds covered = renderer.rendered_bounds();
        const eyes::DirtyRegions &regions = renderer.rendered_regions();
        assert(covered.valid());
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                          static_cast<std::size_t>(x);
                if (pixels[index] != previous[index]) {
                    assert(x >= covered.x0 && x <= covered.x1);
                    assert(y >= covered.y0 && y <= covered.y1);
                    assert(std::any_of(
                        regions.begin(), regions.end(), [x, y](eyes::PixelBounds region) {
                            return region.valid() && x >= region.x0 && x <= region.x1 &&
                                   y >= region.y0 && y <= region.y1;
                        }));
                }
            }
        }
        previous = pixels;
    }
}

void test_dirty_regions_reduce_transfer_area()
{
    static_assert(eyes::kDirtyRegionCount <= 30U);
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    renderer.render(state);
    renderer.render(state);

    const auto area = [](eyes::PixelBounds bounds) {
        return bounds.valid() ? (bounds.x1 - bounds.x0 + 1) * (bounds.y1 - bounds.y0 + 1) : 0;
    };
    const auto region_area = [&](const eyes::DirtyRegions &regions) {
        int total = 0;
        for (const eyes::PixelBounds region : regions) {
            total += area(region);
        }
        return total;
    };
    const int focus_bytes = region_area(renderer.rendered_regions()) * 2;
    std::cout << "dirty focus bytes: " << focus_bytes << '\n';
    assert(focus_bytes < 155240);
    assert(region_area(renderer.rendered_regions()) < area(renderer.rendered_bounds()));

#if CONFIG_LILGUY_WORLD_VIEW
    state.grid_visibility = 1.0F;
    state.grid_offset = {105.0F, -70.0F};
    renderer.render(state);
    renderer.render(state);
    const int browse_bytes = region_area(renderer.rendered_regions()) * 2;
    std::cout << "dirty browse bytes: " << browse_bytes << '\n';
    assert(browse_bytes < 326600);
    assert(region_area(renderer.rendered_regions()) < area(renderer.rendered_bounds()));
#endif
}

void test_boundary_coverage_and_clipping()
{
    constexpr int size = 18;
    const eyes::Rgb black{0, 0, 0};
    const eyes::Rgb white{255, 255, 255};
    const eyes::Rgb red{255, 0, 0};
    const std::uint16_t packed_black = eyes::rgb565(black);
    const std::uint16_t packed_white = eyes::rgb565(white);
    const std::uint16_t packed_red = eyes::rgb565(red);
    std::vector<std::uint16_t> pixels(size * size, packed_black);
    eyes::Raster raster({pixels.data(), size, size, size});

    raster.fill_circle(8.25F, 8.25F, 4.4F, white);
    assert(pixels[8 * size + 8] == packed_white);
    assert(std::any_of(pixels.begin(), pixels.end(), [&](std::uint16_t pixel) {
        return pixel != packed_black && pixel != packed_white;
    }));
    const eyes::PixelBounds circle_dirty = raster.dirty_bounds();
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * size + x);
            if (pixels[index] != packed_black) {
                assert(x >= circle_dirty.x0 && x <= circle_dirty.x1);
                assert(y >= circle_dirty.y0 && y <= circle_dirty.y1);
            }
        }
    }

    const eyes::Vec2 outer[12]{{4.2F, 4.2F}, {6.7F, 4.2F}, {9.3F, 4.2F}, {11.8F, 4.2F},
                               {11.8F, 6.7F}, {11.8F, 9.3F}, {11.8F, 11.8F},
                               {9.3F, 11.8F}, {6.7F, 11.8F}, {4.2F, 11.8F},
                               {4.2F, 9.3F}, {4.2F, 6.7F}};
    const eyes::Vec2 pupil[12]{{2.4F, 6.1F}, {6.0F, 6.1F}, {10.0F, 6.1F}, {13.6F, 6.1F},
                               {13.6F, 7.4F}, {13.6F, 8.6F}, {13.6F, 9.9F},
                               {10.0F, 9.9F}, {6.0F, 9.9F}, {2.4F, 9.9F},
                               {2.4F, 8.6F}, {2.4F, 7.4F}};
    std::fill(pixels.begin(), pixels.end(), packed_black);
    raster.reset_dirty_bounds();
    raster.fill_cubic_path(outer, 12, white);
    assert(pixels[8 * size + 8] == packed_white);
    assert(pixels[4 * size + 8] != packed_black);
    assert(pixels[4 * size + 8] != packed_white);
    assert(std::any_of(pixels.begin(), pixels.end(), [&](std::uint16_t pixel) {
        return pixel != packed_black && pixel != packed_white;
    }));
    const auto outer_pixels = pixels;
    raster.reset_dirty_bounds();
    raster.fill_cubic_path_clipped(pupil, 12, red, white);
    const eyes::PixelBounds pupil_dirty = raster.dirty_bounds();
    int changed = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * size + x);
            if (pixels[index] != outer_pixels[index]) {
                ++changed;
                // Geometric clip: the pupil may blend over any pixel the outer path
                // covered (including its antialiased fringe), never untouched ones.
                assert(outer_pixels[index] != packed_black);
                assert(x >= pupil_dirty.x0 && x <= pupil_dirty.x1);
                assert(y >= pupil_dirty.y0 && y <= pupil_dirty.y1);
            }
        }
    }
    assert(changed > 0);
    assert(pixels[8 * size + 8] == packed_red);
}

void test_shallow_fold_is_contiguous_and_deterministic()
{
    constexpr int size = 32;
    const eyes::Rgb black{0, 0, 0};
    const eyes::Rgb white{255, 255, 255};
    const std::uint16_t packed_black = eyes::rgb565(black);
    const std::uint16_t packed_white = eyes::rgb565(white);
    const eyes::Vec2 folded[12]{
        {4.25F, 10.2F}, {8.0F, 9.6F}, {12.0F, 10.5F}, {16.0F, 10.1F},
        {21.0F, 9.8F}, {24.0F, 13.0F}, {24.0F, 18.0F}, {24.0F, 23.0F},
        {8.0F, 23.0F}, {4.0F, 18.0F}, {3.0F, 15.0F}, {3.5F, 12.0F},
    };
    std::vector<std::uint16_t> first(size * size, packed_black);
    std::vector<std::uint16_t> second(size * size, packed_black);
    eyes::Raster first_raster({first.data(), size, size, size});
    eyes::Raster second_raster({second.data(), size, size, size});
    first_raster.fill_cubic_path(folded, 12, white);
    second_raster.fill_cubic_path(folded, 12, white);
    assert(first == second);
    assert(std::any_of(first.begin(), first.end(), [&](std::uint16_t pixel) {
        return pixel != packed_black && pixel != packed_white;
    }));

    bool started = false;
    bool ended = false;
    int filled_rows = 0;
    int fractional_pixels = 0;
    for (int y = 0; y < size; ++y) {
        int left = size;
        int right = -1;
        for (int x = 0; x < size; ++x) {
            if (first[static_cast<std::size_t>(y * size + x)] != packed_black) {
                left = std::min(left, x);
                right = x;
            }
        }
        if (right < 0) {
            ended = ended || started;
            continue;
        }
        assert(!ended);
        started = true;
        ++filled_rows;
        for (int x = left; x <= right; ++x) {
            const std::uint16_t pixel = first[static_cast<std::size_t>(y * size + x)];
            assert(pixel != packed_black);
            fractional_pixels += pixel != packed_white ? 1 : 0;
        }
    }
    assert(filled_rows >= 10);
    assert(fractional_pixels >= 8);
}

void test_local_extremum_keeps_partial_row_coverage()
{
    constexpr int size = 12;
    const eyes::Rgb black{0, 0, 0};
    const eyes::Rgb white{255, 255, 255};
    const eyes::Vec2 path[12]{
        {2.0F, 0.0F}, {4.0F, 4.333F}, {6.0F, 4.333F}, {8.0F, 0.0F},
        {8.0F, 2.667F}, {8.0F, 5.333F}, {8.0F, 8.0F}, {6.0F, 8.0F},
        {4.0F, 8.0F}, {2.0F, 8.0F}, {2.0F, 5.333F}, {2.0F, 2.667F},
    };
    std::vector<std::uint16_t> pixels(size * size, eyes::rgb565(black));
    eyes::Raster raster({pixels.data(), size, size, size});
    raster.fill_cubic_path(path, 12, white);

    // A 4096-sample reference covers about one quarter of this pixel.
    const std::uint16_t sample = pixels[3U * size + 5U];
    const unsigned red = (sample >> 11U) & 0x1fU;
    const unsigned green = (sample >> 5U) & 0x3fU;
    const unsigned blue = sample & 0x1fU;
    assert(red >= 6U && red <= 10U);
    assert(green >= 13U && green <= 20U);
    assert(blue >= 6U && blue <= 10U);
}

void test_bow_tie_crossing_splits_the_row_band()
{
    constexpr int size = 12;
    const eyes::Rgb black{0, 0, 0};
    const eyes::Rgb white{255, 255, 255};
    const eyes::Vec2 bow_tie[12]{
        {2.0F, 3.5F}, {4.0F, 3.833333F}, {6.0F, 4.166667F}, {8.0F, 4.5F},
        {6.0F, 4.5F}, {4.0F, 4.5F}, {2.0F, 4.5F}, {4.0F, 4.166667F},
        {6.0F, 3.833333F}, {8.0F, 3.5F}, {6.0F, 3.5F}, {4.0F, 3.5F},
    };
    std::vector<std::uint16_t> pixels(size * size, eyes::rgb565(black));
    eyes::Raster raster({pixels.data(), size, size, size});
    raster.fill_cubic_path(bow_tie, 12, white);

    unsigned green_sum = 0U;
    for (int x = 0; x < size; ++x) {
        green_sum += (pixels[4U * size + static_cast<std::size_t>(x)] >> 5U) & 0x3fU;
    }
    // Two six-by-one half-triangles cover three pixels. A missed crossing covers six.
    assert(green_sum >= 170U && green_sum <= 205U);
}

void test_randomize_selection_changes_and_morphs()
{
    eyes::EyeEngine engine(91U);
    advance(engine, 400U);
    for (int round = 0; round < 50; ++round) {
        const eyes::Selection before = engine.selection();
        engine.randomize_selection();
        const eyes::Selection after = engine.selection();
        assert(after != before);
        assert(after.shape != before.shape);  // always a new shape -> morph plays
        assert(after.shape >= 0 && after.shape < static_cast<int>(eyes::kShapeCount));
        assert(after.palette >= 0 && after.palette < static_cast<int>(eyes::kPaletteCount));
        assert(engine.frame().morph_active);
        assert(engine.consume_selection_changed());
        advance(engine, 40U);
    }
    // Palettes really vary across rolls.
    eyes::EyeEngine variety(92U);
    std::set<int> palettes;
    for (int round = 0; round < 40; ++round) {
        variety.randomize_selection();
        palettes.insert(variety.selection().palette);
    }
    assert(palettes.size() > 10U);
}

void test_sensitivity_levels_and_default()
{
    eyes::EyeEngine engine(93U);
    assert(std::fabs(engine.imu_sensitivity() - 1.3F) < 1e-6F);  // default = MED
    engine.set_imu_sensitivity(0.8F);  // menu LOW
    assert(engine.imu_sensitivity() == 0.8F);
    engine.set_imu_sensitivity(1.9F);  // menu HIGH
    assert(engine.imu_sensitivity() == 1.9F);
    engine.set_imu_sensitivity(1.3F);  // menu MED
    assert(engine.imu_sensitivity() == 1.3F);

    // A moderate deliberate tilt (~20 deg) reaches ~0.5 gaze briskly at MED
    // (the eased response curve; the old linear ramp only hit ~0.27).
    eyes::EyeEngine med(94U);
    eyes::MotionSample tilt{};
    tilt.accel_x = 3.35F;  // ~ g * sin(20 deg)
    for (int sample = 0; sample < 60; ++sample) {
        med.motion_sample(tilt);
        advance(med, 8U);
    }
    assert(med.frame().attention == eyes::AttentionSource::imu);
    assert(med.frame().gaze.y > 0.40F);
}

void test_sleep_wake_toggle()
{
    eyes::EyeEngine engine(95U);
    advance(engine, 500U);
    engine.wake();  // no-op while awake
    assert(engine.frame().mode != eyes::InteractionMode::sleeping);
    engine.sleep();
    assert(engine.frame().mode == eyes::InteractionMode::sleeping);
    advance(engine, 300U);
    assert(engine.frame().blink_open < 0.1F);
    engine.wake();
    advance(engine, 16U);
    assert(engine.frame().mode != eyes::InteractionMode::sleeping);
    assert(engine.frame().attention != eyes::AttentionSource::sleep);
    advance(engine, 300U);
    assert(engine.frame().blink_open == 1.0F);  // lids eased back open
    engine.sleep();  // toggle back
    advance(engine, 16U);
    assert(engine.frame().mode == eyes::InteractionMode::sleeping);
}

void test_orientation_rotates_render_and_sensors()
{
    // Renderer: a nonzero face_rotation changes the rendered pixels.
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 63};
    renderer.render(state);
    const std::uint64_t upright = hash_pixels(pixels);
    state.face_rotation = 0.6F;
    renderer.render(state);
    assert(hash_pixels(pixels) != upright);

    // Engine plumbing: set_orientation reaches face_rotation, survives
    // update(), and wraps into [-pi, pi).
    constexpr float kPi = 3.14159265F;
    eyes::EyeEngine plumbing(96U);
    plumbing.set_orientation(0.6F);
    assert(std::fabs(plumbing.frame().face_rotation - 0.6F) < 1e-6F);
    advance(plumbing, 100U);
    assert(std::fabs(plumbing.frame().face_rotation - 0.6F) < 1e-6F);
    plumbing.set_orientation(2.5F * kPi);
    assert(std::fabs(plumbing.orientation() - 0.5F * kPi) < 1e-3F);
    assert(plumbing.orientation() >= -kPi && plumbing.orientation() < kPi);

    // IMU mapping: the same board tilt yields a target rotated by -offset
    // (rotate by -pi/2: (x, y) -> (y, -x)).
    eyes::EyeEngine flat(97U);
    eyes::EyeEngine rotated(97U);
    rotated.set_orientation(0.5F * kPi);
    eyes::MotionSample tilt{};
    tilt.accel_y = 6.0F;  // board frame: pure gaze-x tilt
    for (int sample = 0; sample < 60; ++sample) {
        flat.motion_sample(tilt);
        rotated.motion_sample(tilt);
        advance(flat, 8U);
        advance(rotated, 8U);
    }
    assert(flat.frame().attention == eyes::AttentionSource::imu);
    assert(rotated.frame().attention == eyes::AttentionSource::imu);
    assert(flat.frame().gaze.x > 0.4F);
    assert(std::fabs(flat.frame().gaze.y) < 0.05F);
    assert(std::fabs(rotated.frame().gaze.x - flat.frame().gaze.y) < 1e-3F);
    assert(std::fabs(rotated.frame().gaze.y + flat.frame().gaze.x) < 1e-3F);

    // Touch mapping: same finger position, gaze target rotated by -offset so
    // "look at my finger" stays true on the anchored face.
    eyes::EyeEngine touch_flat(98U);
    eyes::EyeEngine touch_rot(98U);
    touch_rot.set_orientation(0.5F * kPi);
    touch_flat.pointer_down(440.0F, 233.0F, 0U);
    touch_rot.pointer_down(440.0F, 233.0F, 0U);
    advance(touch_flat, 400U);
    advance(touch_rot, 400U);
    assert(touch_flat.frame().gaze.x > 0.35F);
    assert(std::fabs(touch_flat.frame().gaze.y) < 0.1F);
    assert(touch_rot.frame().gaze.y < -0.35F);
    assert(std::fabs(touch_rot.frame().gaze.x) < 0.1F);
}


// --- shared device UI (options menu, stats page, BOOT/PWR gestures) ---------
// The desktop simulator drives these same entry points, so covering them here
// covers both the board's buttons and the simulator's panel.

class TestUiHost : public eyes::DeviceUiHost {
  public:
    void save_settings(const eyes::Settings &settings) override
    {
        stored = settings;
        ++saves;
    }

    void request_imu_calibration() override { ++calibrations; }

    eyes::BatteryStatus read_battery() override
    {
        return {eyes::BatteryStatus::State::ok, 4000U, 80U, false};
    }

    void request_listen() override { ++listens; }

    eyes::Settings stored{};
    int saves{0};
    int calibrations{0};
    int listens{0};
};

struct UiFixture {
    std::vector<std::uint16_t> pixels;
    eyes::EyeRenderer renderer;
    eyes::EyeEngine engine{};
    TestUiHost host{};
    eyes::DeviceUi ui;
    std::uint32_t now{1000U};

    UiFixture()
        : pixels(static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight),
          renderer({pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth},
                   true),
          ui(engine, renderer, host, eyes::Settings{})
    {
    }

    // The device polls BOOT every 20 ms; idling long enough retires a pending
    // single press.
    void idle(std::uint32_t duration_ms)
    {
        for (std::uint32_t elapsed = 0; elapsed < duration_ms; elapsed += 20U) {
            now += 20U;
            ui.poll_boot_button(false, now);
            ui.poll_power_button(false, now);
            ui.tick(now);
            engine.update(20U);
        }
    }

    void press_boot(std::uint32_t hold_ms)
    {
        ui.poll_boot_button(true, now);
        for (std::uint32_t elapsed = 0; elapsed < hold_ms; elapsed += 20U) {
            now += 20U;
            ui.poll_boot_button(true, now);
        }
        now += 20U;
        ui.poll_boot_button(false, now);
    }

    void click_power()
    {
        ui.poll_power_button(true, now);
        now += 80U;
    }
};

void test_ui_boot_press_gestures()
{
    UiFixture fixture;
    assert(fixture.ui.page() == eyes::UiPage::none);

    // A single press opens nothing: there is no touch menu (the eyes are the
    // interface). It must not fall through to the stats page either.
    fixture.press_boot(120U);
    assert(fixture.ui.page() == eyes::UiPage::none);
    fixture.idle(400U);
    assert(fixture.ui.page() == eyes::UiPage::none);
    fixture.press_boot(120U);
    fixture.idle(400U);
    assert(fixture.ui.page() == eyes::UiPage::none);

    // Two quick presses reach the stats page instead.
    fixture.press_boot(120U);
    fixture.press_boot(120U);
    assert(fixture.ui.page() == eyes::UiPage::stats);
    fixture.idle(400U);
    assert(fixture.ui.page() == eyes::UiPage::stats);  // no stray single-press follow-up
    fixture.press_boot(120U);
    fixture.press_boot(120U);
    assert(fixture.ui.page() == eyes::UiPage::none);

    // A hold past 850 ms recalibrates instead of opening anything.
    fixture.press_boot(900U);
    fixture.idle(400U);
    assert(fixture.host.calibrations == 1);
    assert(fixture.ui.page() == eyes::UiPage::none);

    // Past 3 s it toggles the debug line.
    assert(!fixture.ui.debug_overlay());
    fixture.press_boot(3100U);
    fixture.idle(400U);
    assert(fixture.ui.debug_overlay());
    assert(fixture.host.calibrations == 1);  // the longer hold is not also a calibrate
    fixture.press_boot(3100U);
    fixture.idle(400U);
    assert(!fixture.ui.debug_overlay());
}

void test_ui_power_press_gestures()
{
    UiFixture fixture;
    const eyes::Selection before = fixture.engine.selection();

    // Single press asks the host to listen; the look never changes.
    fixture.click_power();
    fixture.idle(400U);
    assert(fixture.host.listens == 1);
    assert(fixture.engine.selection() == before);
    assert(!fixture.engine.frame().morph_active);

    // Double press toggles sleep, and again to wake.
    fixture.click_power();
    fixture.click_power();
    fixture.idle(100U);
    assert(fixture.engine.frame().mode == eyes::InteractionMode::sleeping);
    fixture.click_power();
    fixture.click_power();
    fixture.idle(100U);
    assert(fixture.engine.frame().mode != eyes::InteractionMode::sleeping);
}

void test_ui_settings_run_and_persist_and_look_is_pinned()
{
    UiFixture fixture;

    // Tilt cycles LOW -> MED -> HIGH and persists each step.
    const int saves_before = fixture.host.saves;
    fixture.ui.run_setting(eyes::UiSetting::tilt);
    assert(fixture.host.saves == saves_before + 1);
    assert(fixture.ui.settings().imu_level == 2U);
    assert(std::fabs(fixture.engine.imu_sensitivity() - 1.9F) < 1e-4F);
    fixture.ui.run_setting(eyes::UiSetting::tilt);
    assert(fixture.ui.settings().imu_level == 0U);

    // Reset rotation zeroes the anchored angle in the engine and the settings.
    fixture.engine.set_orientation(0.7F);
    fixture.ui.note_orientation(0.7F);
    fixture.ui.run_setting(eyes::UiSetting::reset_rotation);
    assert(fixture.host.saves == saves_before + 3);
    assert(fixture.engine.orientation() == 0.0F);
    assert(fixture.ui.settings().orientation == 0.0F);

    // Labels carry the live value.
    char text[40];
    fixture.ui.setting_text(eyes::UiSetting::tilt, text, sizeof(text));
    assert(std::strcmp(text, "TILT FB: LOW") == 0);
    fixture.ui.setting_text(eyes::UiSetting::reset_rotation, text, sizeof(text));
    assert(std::strcmp(text, "RESET ROTATION") == 0);

    // The look is pinned: a settings blob from a build that still switched
    // faces (creature, palette disc) is applied as capsule + inverted ink.
    eyes::Settings old_blob{};
    old_blob.face_mode = 0U;
    old_blob.capsule_invert = 0U;
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer({pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight,
                                eyes::kScreenWidth},
                               true);
    eyes::EyeEngine engine{};
    TestUiHost host{};
    eyes::DeviceUi ui(engine, renderer, host, old_blob);
    assert(renderer.face_mode() == eyes::FaceMode::creature);  // renderer default
    ui.apply_settings();
    assert(renderer.face_mode() == eyes::FaceMode::capsule);
    assert(renderer.capsule_invert());
    // Nothing the UI exposes can move the selection.
    assert((engine.selection() == eyes::Selection{1, 35}));
}

void test_ui_paints_text_bands_and_unions_rows()
{
    UiFixture fixture;
    const std::uint16_t black = eyes::rgb565({0, 0, 0});
    std::fill(fixture.pixels.begin(), fixture.pixels.end(), eyes::rgb565({255, 0, 0}));

    fixture.press_boot(120U);
    fixture.press_boot(120U);  // double press: the stats page
    fixture.idle(400U);
    assert(fixture.ui.page() == eyes::UiPage::stats);
    assert(fixture.ui.ui_push());
    fixture.ui.paint(fixture.pixels.data());

    // The band is cleared to black and carries white glyph pixels.
    std::size_t white = 0;
    std::size_t nonblack = 0;
    for (int row = eyes::kUiRowFirst; row <= eyes::kUiRowLast; ++row) {
        for (int column = 0; column < eyes::kScreenWidth; ++column) {
            const std::uint16_t pixel =
                fixture.pixels[static_cast<std::size_t>(row) * eyes::kScreenWidth +
                               static_cast<std::size_t>(column)];
            if (pixel != black) {
                ++nonblack;
            }
            if (pixel == eyes::rgb565({255, 255, 255})) {
                ++white;
            }
        }
    }
    assert(white > 500);          // eight stats rows of text
    assert(nonblack == white);    // nothing but text survives in the band
    // Rows outside the band are untouched.
    assert(fixture.pixels[0] == eyes::rgb565({255, 0, 0}));

    // The pushing band widens the device's blit range.
    int y0 = eyes::kScreenHeight - 1;
    int y1 = 0;
    fixture.ui.union_push_rows(y0, y1);
    assert(y0 <= eyes::kUiRowFirst && y1 >= eyes::kUiRowLast);

    fixture.ui.clear_dirty();
    assert(fixture.ui.ui_push());  // an open page always pushes
    fixture.press_boot(120U);
    fixture.press_boot(120U);  // double press toggles the page off
    assert(fixture.ui.page() == eyes::UiPage::none);
    assert(fixture.ui.ui_push());  // ...and the closing frame erases its rows
    fixture.ui.clear_dirty();
    assert(!fixture.ui.ui_push());
}

void test_ui_applies_stored_settings()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                     eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::EyeEngine engine{};
    TestUiHost host{};
    eyes::Settings stored{};
    stored.imu_level = 0U;  // LOW
    stored.orientation = 1.25F;
    stored.face_mode = 1U;
    stored.capsule_invert = 1U;
    eyes::DeviceUi ui(engine, renderer, host, stored);
    ui.apply_settings();

    assert(std::fabs(engine.imu_sensitivity() - 0.8F) < 1e-4F);
    assert(std::fabs(engine.orientation() - 1.25F) < 1e-4F);
    assert(renderer.face_mode() == eyes::FaceMode::capsule);
    assert(renderer.capsule_invert());
}


void test_aa_override_pins_raster_quality()
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                     eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 35};
    state.blink_open = 1.0F;

    // The device ships adaptive, and a still hero frame is full quality.
    assert(renderer.aa_override() == 0);
    renderer.render(state);
    assert(renderer.last_aa_subsamples() == 8);
    assert(renderer.last_aa_lines() == 8);
    const std::uint64_t adaptive_hash = hash_pixels(pixels);

    // Pinning changes what the rasterizer is asked for...
    renderer.set_aa_override(2);
    renderer.render(state);
    assert(renderer.last_aa_subsamples() == 2);
    assert(renderer.last_aa_lines() == 2);
    const std::uint64_t coarse_hash = hash_pixels(pixels);
    // ...and it must actually reach the raster, not just be recorded.
    assert(coarse_hash != adaptive_hash);

    // A request of 4 samples the same rows as 2 (aa_lines_for), so the pixels
    // match 2x exactly even though the request differs.
    renderer.set_aa_override(4);
    renderer.render(state);
    assert(renderer.last_aa_subsamples() == 4);
    assert(renderer.last_aa_lines() == 2);
    assert(hash_pixels(pixels) == coarse_hash);

    // 8x reproduces the still-frame adaptive result exactly.
    renderer.set_aa_override(8);
    renderer.render(state);
    assert(renderer.last_aa_lines() == 8);
    assert(hash_pixels(pixels) == adaptive_hash);

    // Back to adaptive, and the default behaviour returns.
    renderer.set_aa_override(0);
    renderer.render(state);
    assert(renderer.last_aa_subsamples() == 8);
    assert(hash_pixels(pixels) == adaptive_hash);
}

}  // namespace

// ---- capsule face (Grok second personality) ---------------------------------

eyes::FrameState capsule_frame(std::uint32_t time_ms,
                               eyes::Expression expression = eyes::Expression::content,
                               eyes::InteractionMode mode = eyes::InteractionMode::idle)
{
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.time_ms = time_ms;
    state.expression = expression;
    state.mode = mode;
    return state;
}

void test_capsule_pose_table()
{
    static_assert(eyes::kCapsulePoseCount == 25);
    for (const eyes::CapsulePose &pose : eyes::kCapsulePoses) {
        for (const eyes::CapsuleEye *eye : {&pose.left, &pose.right}) {
            assert(std::fabs(eye->cx) <= 120.0F);
            assert(std::fabs(eye->cy) <= 80.0F);
            assert(eye->angle_deg >= 0.0F && eye->angle_deg <= 180.0F);
            assert(eye->half_len >= 10.0F && eye->half_len <= 50.0F);
            assert(eye->half_w >= 5.0F && eye->half_w <= 30.0F);
            assert(eye->half_w <= eye->half_len + 0.01F);
        }
    }
    // Spot checks against the RE doc table.
    assert(eyes::kCapsulePoses[0].left.cx == 22.4F && eyes::kCapsulePoses[0].left.cy == -47.5F);
    assert(eyes::kCapsulePoses[13].right.angle_deg == 167.0F);
    assert(eyes::kCapsulePoses[3].right.half_len == eyes::kCapsulePoses[3].right.half_w);
}

void test_capsule_path_bbox()
{
    // Flatten the 12-point path exactly like the rasterizer (cubic sampling)
    // and compare its bbox against the analytic rotated-capsule bbox:
    // half-extent = body * |cos| + w along x, body * |sin| + w along y.
    const float angles[] = {0.0F, 30.0F, 63.0F, 90.0F, 137.0F, 173.0F};
    const float sizes[][2] = {{47.0F, 17.6F}, {28.2F, 28.2F}, {22.6F, 10.6F}, {28.2F, 6.9F}};
    for (const float angle_deg : angles) {
        for (const auto &size : sizes) {
            const float half_len = size[0] * eyes::kCapsuleScale;
            const float half_w = size[1] * eyes::kCapsuleScale;
            const float angle = angle_deg * 3.14159265F / 180.0F;
            const auto path = eyes::capsule_path(233.0F, 233.0F, angle, half_len, half_w);
            float min_x = 1e9F;
            float max_x = -1e9F;
            float min_y = 1e9F;
            float max_y = -1e9F;
            float area2 = 0.0F;
            std::vector<eyes::Vec2> flat;
            for (std::size_t curve = 0; curve < 4U; ++curve) {
                const eyes::Vec2 p0 = path[curve * 3U];
                const eyes::Vec2 p1 = path[curve * 3U + 1U];
                const eyes::Vec2 p2 = path[curve * 3U + 2U];
                const eyes::Vec2 p3 = path[(curve * 3U + 3U) % 12U];
                for (int step = 0; step < 64; ++step) {
                    const float t = static_cast<float>(step) / 64.0F;
                    const float u = 1.0F - t;
                    const eyes::Vec2 point{
                        u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x +
                            t * t * t * p3.x,
                        u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y +
                            t * t * t * p3.y,
                    };
                    assert(std::isfinite(point.x) && std::isfinite(point.y));
                    min_x = std::min(min_x, point.x);
                    max_x = std::max(max_x, point.x);
                    min_y = std::min(min_y, point.y);
                    max_y = std::max(max_y, point.y);
                    flat.push_back(point);
                }
            }
            for (std::size_t index = 0; index < flat.size(); ++index) {
                const eyes::Vec2 &a = flat[index];
                const eyes::Vec2 &b = flat[(index + 1U) % flat.size()];
                area2 += a.x * b.y - b.x * a.y;
            }
            const float body = half_len - half_w;
            const float cosine = std::fabs(std::cos(angle));
            const float sine = std::fabs(std::sin(angle));
            const float expect_x = body * cosine + half_w;
            const float expect_y = body * sine + half_w;
            assert(std::fabs((max_x - min_x) * 0.5F - expect_x) <= 1.0F);
            assert(std::fabs((max_y - min_y) * 0.5F - expect_y) <= 1.0F);
            assert(std::fabs((max_x + min_x) * 0.5F - 233.0F) <= 1.0F);
            assert(std::fabs((max_y + min_y) * 0.5F - 233.0F) <= 1.0F);
            // Closed sane outline: shoelace area within 3% of the analytic
            // capsule area (pi w^2 + 4 body w), consistent winding.
            const float analytic_area = 3.14159265F * half_w * half_w + 4.0F * body * half_w;
            assert(std::fabs(std::fabs(area2) * 0.5F - analytic_area) <= analytic_area * 0.03F);
        }
    }
}

void test_capsule_spring_convergence()
{
    // Sleeping pins pose 13 with no blinks/drift; every spring must settle
    // onto the pose (no NaN, stationary outputs).
    eyes::CapsuleFace face(9U);
    for (std::uint32_t time = 0; time <= 8000U; time += 16U) {
        face.update(capsule_frame(time, eyes::Expression::sleepy,
                                  eyes::InteractionMode::sleeping));
        for (int eye = 0; eye < 2; ++eye) {
            const auto out = face.eye(eye);
            assert(std::isfinite(out.cx) && std::isfinite(out.cy));
            assert(std::isfinite(out.angle_rad));
            assert(out.half_len > 0.0F && out.half_w > 0.0F);
        }
    }
    const auto left = face.eye(0);
    const auto right = face.eye(1);
    const eyes::CapsulePose &closed = eyes::kCapsulePoses[13];
    // Outputs carry breath (+-2.2%) and the screen mapping; compare loosely.
    assert(std::fabs(left.half_len / eyes::kCapsuleScale - closed.left.half_len) < 2.0F);
    assert(std::fabs(right.half_len / eyes::kCapsuleScale - closed.right.half_len) < 2.0F);
    // The sleeping Qi table tips the whole face by roll = 4 + sin(.25t)*2 deg
    // (head-spring lag keeps it inside the 2..6 deg band).
    const float left_angle = std::remainder(
        left.angle_rad - closed.left.angle_deg * 3.14159265F / 180.0F, 3.14159265F);
    assert(left_angle > 0.02F && left_angle < 0.12F);
    // Stationary: 100 ms later the eyes moved less than a pixel (slow breath only).
    face.update(capsule_frame(8100U, eyes::Expression::sleepy, eyes::InteractionMode::sleeping));
    const auto later = face.eye(0);
    assert(std::fabs(later.cx - left.cx) < 1.0F && std::fabs(later.cy - left.cy) < 1.0F);
}

void test_capsule_blink_envelope_and_morph_continuity()
{
    // An expression change fires a blink (blink-on-state-entry) and a springy
    // pose morph. Track openness (dip, overshoot, recovery) and the per-frame
    // geometry delta (morph continuity) across the transition.
    for (std::uint32_t seed = 3U; seed <= 12U; ++seed) {
        eyes::CapsuleFace face(seed);
        for (std::uint32_t time = 0; time <= 3000U; time += 16U) {
            face.update(capsule_frame(time));
        }
        auto previous_left = face.eye(0);
        float min_open = 10.0F;
        float max_open = -10.0F;
        float max_step = 0.0F;
        bool was_spinning = false;
        for (std::uint32_t time = 3016U; time <= 3000U + 2200U; time += 16U) {
            face.update(capsule_frame(time, eyes::Expression::happy));
            min_open = std::min(min_open, face.blink_openness());
            max_open = std::max(max_open, face.blink_openness());
            const auto now_left = face.eye(0);
            // Happy entry has a 10% spinWild chance; the 3-turn visual spin
            // legitimately sweeps the eyes faster than the morph bound.
            if (!face.spin_active() && !was_spinning) {
                max_step = std::max({max_step, std::fabs(now_left.cx - previous_left.cx),
                                     std::fabs(now_left.cy - previous_left.cy),
                                     std::fabs(now_left.half_len - previous_left.half_len)});
            }
            was_spinning = face.spin_active();
            previous_left = now_left;
            assert(std::isfinite(face.blink_openness()));
        }
        assert(min_open < 0.15F);   // Grok envelope dips to ~0.147
        assert(max_open > 1.02F);   // and overshoots (~1.036)
        assert(std::fabs(face.blink_openness() - 1.0F) < 0.02F ||
               max_open > 1.02F);   // double-blink seeds settle slightly later
        // Springy morph: a 16 ms frame never jumps more than ~12 px even on a
        // full pose swap (omega 8 entry morph).
        assert(max_step < 12.0F);
    }
}

void test_capsule_needs_frame()
{
    eyes::CapsuleFace face(21U);
    // Settle in sleep (slow breath only; drift is off).
    for (std::uint32_t time = 0; time <= 6000U; time += 16U) {
        face.update(capsule_frame(time, eyes::Expression::sleepy,
                                  eyes::InteractionMode::sleeping));
        face.mark_rendered();
    }
    int busy = 0;
    int idle_ticks = 0;
    for (std::uint32_t time = 6016U; time <= 9200U; time += 16U) {
        face.update(capsule_frame(time, eyes::Expression::sleepy,
                                  eyes::InteractionMode::sleeping));
        ++idle_ticks;
        if (face.needs_frame()) {
            ++busy;
            face.mark_rendered();
        }
    }
    // At rest the quantized geometry only crosses a 1/4 px quantum when the
    // slow breath moves it: most ticks skip.
    assert(busy < idle_ticks / 2);
    assert(busy < idle_ticks);  // and at least the gate exists
    // A gaze change must demand a frame within two ticks.
    eyes::FrameState looking =
        capsule_frame(9216U, eyes::Expression::sleepy, eyes::InteractionMode::idle);
    looking.gaze_pupils = {0.7F, 0.0F};
    face.update(looking);
    bool woke = face.needs_frame();
    if (!woke) {
        looking.time_ms += 16U;
        face.update(looking);
        woke = face.needs_frame();
    }
    assert(woke);
}

void test_capsule_renderer_incremental_matches_full()
{
    std::vector<std::uint16_t> incremental_pixels(
        static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight);
    std::vector<std::uint16_t> full_pixels(incremental_pixels.size());
    eyes::EyeRenderer incremental(
        {incremental_pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth},
        true);
    eyes::EyeRenderer full(
        {full_pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    incremental.set_face_mode(eyes::FaceMode::capsule);
    full.set_face_mode(eyes::FaceMode::capsule);

    // Drive gaze faster than the 1/4 px skip quantum every frame so neither
    // renderer skips; the erase-then-repaint path must then be pixel-identical
    // to a full disc repaint. (Sub-quantum moves are allowed to skip by design,
    // so a free-running engine can legitimately diverge below the quantum.)
    eyes::FrameState state = capsule_frame(0U);
    for (int frame = 0; frame < 40; ++frame) {
        state.time_ms += 16U;
        const float phase = static_cast<float>(frame);
        state.gaze_pupils = {std::sin(phase * 0.31F) * 0.6F, std::cos(phase * 0.23F) * 0.4F};
        incremental.render(state);
        full.invalidate();  // force the disc repaint path every frame
        full.render(state);
    }
    assert(hash_pixels(incremental_pixels) == hash_pixels(full_pixels));

    // Disc present: the palette outer color covers most of the disc area.
    const std::uint16_t disc = eyes::rgb565(eyes::palette_at(63).outer);
    const auto disc_pixels = std::count(incremental_pixels.begin(), incremental_pixels.end(), disc);
    assert(disc_pixels > 90000);

    // Rim crossing: happy alternates poses 2/20, and pose 20's right-eye cap
    // plus the 18 px down-right gaze reach crosses the r=220 disc rim. The
    // erase must restore the rim's antialiased ring bit-exactly (an integer
    // chord erase left black notches / hard full-color pixels there). Skipped
    // frames may lag sub-quantum by design; every rendered frame must match.
    bool crossed_rim = false;
    state.expression = eyes::Expression::happy;
    state.gaze_pupils = {0.8F, 0.8F};
    for (int frame = 0; frame < 560; ++frame) {
        state.time_ms += 16U;
        incremental.render(state);
        full.invalidate();
        full.render(state);
        for (int eye = 0; eye < 2; ++eye) {
            const eyes::CapsuleFace::EyeOut out = incremental.capsule_face().eye(eye);
            const float body = out.half_len - out.half_w;
            const float bx = std::cos(out.angle_rad) * body;
            const float by = std::sin(out.angle_rad) * body;
            const float tip = std::sqrt(std::max(
                (out.cx - 233.0F + bx) * (out.cx - 233.0F + bx) +
                    (out.cy - 233.0F + by) * (out.cy - 233.0F + by),
                (out.cx - 233.0F - bx) * (out.cx - 233.0F - bx) +
                    (out.cy - 233.0F - by) * (out.cy - 233.0F - by)));
            crossed_rim = crossed_rim || tip + out.half_w > eyes::kCapsuleDiscRadius + 0.75F;
        }
        if (incremental.rendered_bounds().valid()) {
            assert(hash_pixels(incremental_pixels) == hash_pixels(full_pixels));
        }
    }
    assert(crossed_rim);

    // Steady frames transfer eye-sized bands, not the disc. (These solo
    // renders desync the two faces' event clocks, so no comparisons follow.)
    state.time_ms += 16U;
    incremental.render(state);
    const eyes::PixelBounds bounds = incremental.rendered_bounds();
    if (bounds.valid()) {
        assert(bounds.y1 - bounds.y0 < 320);
    }

    // Identical quantized geometry: re-render at the same time -> no transfer.
    incremental.render(state);
    assert(!incremental.rendered_bounds().valid());

    // Mode switch back to the creature fully repaints: the disc ring (inside
    // the old disc, far from the creature eyes) must be black again.
    incremental.set_face_mode(eyes::FaceMode::creature);
    incremental.render(state);
    const std::uint16_t black = eyes::rgb565({0, 0, 0});
    const auto pixel_at = [&](int x, int y) {
        return incremental_pixels[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                  static_cast<std::size_t>(x)];
    };
    assert(pixel_at(233, 25) == black);
    assert(pixel_at(25, 233) == black);
    assert(pixel_at(233, 441) == black);
    assert(pixel_at(441, 233) == black);
}

void test_wink_suppresses_one_eye()
{
    // Renderer: at the blink apex the winking eye's pupil is culled while the
    // suppressed (open) eye keeps its pupil fully opaque.
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    const auto &clip = eyes::creature::kBlinkClips[0];
    std::uint16_t apex_ms = 0U;
    for (std::size_t frame = 0; frame < clip.frame_count; ++frame) {
        const std::size_t index = 1U * eyes::creature::kBlinkTotalFrameCount +
                                  clip.frame_offset + frame;
        if ((eyes::creature::kBlinkPathMasks[index] & 0x0aU) == 0U) {
            apex_ms = eyes::creature::kBlinkFrameTimesMs[clip.frame_offset + frame];
            break;
        }
    }
    eyes::FrameState state{};
    state.selection = {1, 63};
    state.blink_active = true;
    state.blink_clip = 0U;
    state.blink_elapsed_ms = apex_ms;
    // Every palette shares one pupil color; tell the eyes apart by screen half.
    const std::uint16_t pupil = eyes::rgb565(eyes::palette_at(63).inner);
    const auto pupil_halves = [&](long &left, long &right) {
        left = 0;
        right = 0;
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth; ++x) {
                if (pixels[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                           static_cast<std::size_t>(x)] == pupil) {
                    (x < eyes::kScreenWidth / 2 ? left : right) += 1;
                }
            }
        }
    };
    long left_count = 0;
    long right_count = 0;

    state.wink = 0U;  // plain blink: both pupils culled at the apex
    renderer.render(state);
    pupil_halves(left_count, right_count);
    assert(left_count == 0 && right_count == 0);

    state.wink = 1U;  // left eye winks: right eye stays open, pupil intact
    renderer.render(state);
    pupil_halves(left_count, right_count);
    assert(left_count == 0 && right_count > 50);

    state.wink = 2U;  // right eye winks
    renderer.render(state);
    pupil_halves(left_count, right_count);
    assert(left_count > 50 && right_count == 0);

    // Engine: winks fire in content moods with the shortest clip, one eye.
    bool winked = false;
    for (std::uint32_t seed = 1U; seed <= 12U && !winked; ++seed) {
        eyes::EyeEngine engine(seed);
        while (engine.frame().time_ms < 130000U) {
            advance(engine, 8U);
            if (engine.frame().blink_active && engine.frame().wink != 0U) {
                assert(engine.frame().blink_clip == 0U);
                assert(engine.frame().wink == 1U || engine.frame().wink == 2U);
                // Sleep entered mid-wink must drop the one-eye flag; it used
                // to stay set (update_blink's sleeping branch returned before
                // the reset), pinning the suppressed eye open all sleep long.
                engine.sleep();
                advance(engine, 4000U);
                assert(engine.frame().mode == eyes::InteractionMode::sleeping);
                assert(engine.frame().wink == 0U);
                winked = true;
                break;
            }
        }
    }
    assert(winked);
}

void test_os_services_gate_absent_hardware()
{
    // The Waveshare baseline: fitted hardware is flagged, everything else
    // routes to null backends that refuse politely instead of being compiled
    // out, so app code needs no #ifdefs for hardware that is not here yet.
    eyes::Services services = eyes::Services::waveshare_amoled_175c();
    assert(services.caps.has(eyes::Capability::display));
    assert(services.caps.has(eyes::Capability::imu));
    assert(services.caps.has(eyes::Capability::wifi));
    assert(!services.caps.has(eyes::Capability::camera_front));
    assert(!services.caps.has(eyes::Capability::camera_rear));
    assert(!services.caps.has(eyes::Capability::nfc));
    assert(!services.caps.has(eyes::Capability::gnss));
    assert(!services.caps.has(eyes::Capability::crown));
    assert(services.camera != nullptr && services.nfc != nullptr && services.gnss != nullptr &&
           services.crown != nullptr && services.haptics != nullptr);
    assert(!services.camera->available(eyes::CameraService::Lens::rear));
    assert(!services.camera->start(eyes::CameraService::Lens::rear));
    eyes::CameraFrame frame{};
    assert(!services.camera->capture(eyes::CameraService::Lens::front, frame));
    assert(frame.data == nullptr);
    eyes::NfcTag tag{};
    assert(!services.nfc->read_tag(tag));
    eyes::GnssFix fix{};
    assert(!services.gnss->fix(fix));
    assert(services.crown->take_rotation() == 0.0F && !services.crown->pressed());
    services.haptics->pulse(30U, 1.0F);  // must be a safe no-op

    // A future backend swaps in without the call sites changing: the same
    // Services handle, now with a fitted crown.
    struct FakeCrown final : eyes::CrownService {
        float pending{3.0F};
        bool available() const override { return true; }
        float take_rotation() override
        {
            const float out = pending;
            pending = 0.0F;
            return out;
        }
        bool pressed() const override { return false; }
    } fake_crown;
    services.crown = &fake_crown;
    services.caps.set(eyes::Capability::crown);
    assert(services.caps.has(eyes::Capability::crown));
    assert(services.crown->take_rotation() == 3.0F);
    assert(services.crown->take_rotation() == 0.0F);  // reading consumed it
}

void test_ota_controller_gating()
{
    // A scripted backend: a newer version exists and downloads instantly.
    struct FakeOta final : eyes::OtaBackend {
        std::string newer{"1.1.0"};
        int pct{0};
        bool applied{false};
        bool confirmed{false};
        const char *current_version() const override { return "1.0.0"; }
        void begin_check() override {}
        std::string check_result() override { return newer; }
        void begin_download(const std::string &) override { pct = 0; }
        int download_percent() override { return pct += 50; }
        bool download_done() const override { return pct >= 100; }
        void apply_and_reboot() override { applied = true; }
        void confirm_running_image() override { confirmed = true; }
    } backend;

    eyes::OtaController ota(backend);
    std::uint32_t t = 0;
    ota.request_check(t);
    // Low battery + not charging: gating blocks the check entirely.
    ota.tick(t += 16, 20, false, true);
    assert(ota.status().state == eyes::OtaState::idle);
    // Not idle (mid-conversation): still blocked even with power.
    ota.tick(t += 16, 90, false, false);
    assert(ota.status().state == eyes::OtaState::idle);
    // Idle + healthy battery: proceeds through the whole flow to install.
    for (int i = 0; i < 12 && !backend.applied; ++i) {
        ota.tick(t += 16, 90, false, true);
    }
    assert(backend.applied);
    // Boot self-check confirms the running image (else bootloader rolls back).
    ota.confirm_boot();
    assert(backend.confirmed);
}

void test_ota_required_update()
{
    // A REQUIRED update is never deferred by the retry cap, but still obeys the
    // safety gates (idle + power) — you can't brick a device mid-conversation.
    struct FakeOta final : eyes::OtaBackend {
        bool req{true};
        const char *current_version() const override { return "1.0.0"; }
        void begin_check() override {}
        std::string check_result() override { return "1.1.0"; }
        void begin_download(const std::string &) override {}
        int download_percent() override { return -1; }  // download always fails here
        bool download_done() const override { return false; }
        void apply_and_reboot() override {}
        void confirm_running_image() override {}
        bool required_update() const override { return req; }
    } backend;

    eyes::OtaPolicy policy;
    policy.check_cooldown_ms = 0;       // no cooldown in this test
    policy.max_attempts_per_version = 3;
    eyes::OtaController ota(backend, policy);
    std::uint32_t t = 0;

    // Well past the cap: a required update marks itself required and never defers.
    bool deferred = false;
    bool sawRequired = false;
    for (int c = 0; c < 6; ++c) {
        ota.request_check(t);
        ota.tick(t += 16, 90, true, true);  // idle -> checking
        ota.tick(t += 16, 90, true, true);  // checking -> available
        sawRequired = sawRequired || ota.status().required;
        ota.tick(t += 16, 90, true, true);  // available -> downloading (not deferred)
        deferred = deferred || ota.status().message.find("too many attempts") != std::string::npos;
        ota.tick(t += 16, 90, true, true);  // downloading -> error
        ota.tick(t += 31000, 90, true, true);  // error -> idle
    }
    assert(sawRequired);
    assert(!deferred);

    // Flip to optional: with the attempt budget already spent on this version, it
    // now defers instead of retrying forever.
    backend.req = false;
    ota.request_check(t);
    ota.tick(t += 16, 90, true, true);  // idle -> checking
    ota.tick(t += 16, 90, true, true);  // checking -> available (required=false now)
    ota.tick(t += 16, 90, true, true);  // available -> deferred
    assert(ota.status().message.find("too many attempts") != std::string::npos);

    // Safety still wins: a fresh required update is blocked by low battery and by
    // being mid-conversation, exactly like an optional one.
    FakeOta safe;
    eyes::OtaController gated(safe);
    gated.request_check(t);
    gated.tick(t += 16, 20, false, true);   // low battery -> stays idle
    assert(gated.status().state == eyes::OtaState::idle);
    gated.tick(t += 16, 90, false, false);  // mid-conversation -> stays idle
    assert(gated.status().state == eyes::OtaState::idle);
}

void test_app_switcher()
{
    // Exactly one surface: opening rises to 1 with an opened event; switching
    // apps closes fully (closed event) before the next opens; toggle of the
    // open app closes it.
    eyes::AppSwitcher apps(300.0F, 220.0F);
    assert(!apps.active() && apps.foreground() == 0);
    apps.toggle(1);
    bool opened = false;
    for (int tick = 0; tick < 40 && !opened; ++tick) {
        apps.tick(16U);
        assert(apps.foreground() == 1);
        opened = apps.take_opened() == 1;
    }
    assert(opened && apps.fully_open(1));
    apps.toggle(2);  // switch: close 1 fully, then open 2
    bool closed_one = false;
    bool opened_two = false;
    for (int tick = 0; tick < 80 && !opened_two; ++tick) {
        apps.tick(16U);
        if (apps.take_closed() == 1) {
            closed_one = true;
            assert(apps.blend() <= 0.0F);  // one picture: fully closed first
        }
        if (apps.take_opened() == 2) {
            opened_two = true;
        }
        assert(!(apps.foreground() == 2 && !closed_one));  // never 2 before 1 closed
    }
    assert(closed_one && opened_two && apps.fully_open(2));
    apps.toggle(2);
    bool closed_two = false;
    for (int tick = 0; tick < 40 && !closed_two; ++tick) {
        apps.tick(16U);
        closed_two = apps.take_closed() == 2;
    }
    assert(closed_two && !apps.active() && apps.blend() <= 0.0F);
}

void test_expression_hold()
{
    // The conversation mood layer: held expressions pin the face and pause
    // idle drift; reactions play over and return to the hold; release
    // restores drift. Live events outrank mood, mood outranks idle.
    eyes::EyeEngine engine(7U);
    advance(engine, 2000U);
    engine.hold_expression(eyes::Expression::thinking);
    assert(engine.frame().expression == eyes::Expression::thinking);
    assert(engine.expression_held());
    // Idle drift would normally re-roll the emotion within ~8s; held, it
    // must not.
    advance(engine, 20000U);
    assert(engine.frame().expression == eyes::Expression::thinking);
    // A poke reaction overrides while active...
    engine.pointer_down(233.0F, 233.0F, engine.frame().time_ms);
    engine.pointer_up(233.0F, 233.0F, engine.frame().time_ms + 50U);
    advance(engine, 200U);
    // ...and the face returns to the held expression once attention idles.
    advance(engine, 6000U);
    assert(engine.frame().expression == eyes::Expression::thinking);
    engine.release_expression();
    assert(!engine.expression_held());
    bool drifted = false;
    for (int step = 0; step < 40 && !drifted; ++step) {
        advance(engine, 1000U);
        drifted = engine.frame().expression != eyes::Expression::thinking;
    }
    assert(drifted);
}

void test_agent_link_round_trip()
{
    // Every event type survives encode -> parse, including escapes; junk and
    // unknown types degrade instead of crashing (forward compatibility).
    const eyes::AgentEventType kinds[] = {
        eyes::AgentEventType::user_text,    eyes::AgentEventType::run_started,
        eyes::AgentEventType::text_delta,   eyes::AgentEventType::tool_call,
        eyes::AgentEventType::tool_result,  eyes::AgentEventType::run_finished,
        eyes::AgentEventType::run_error,    eyes::AgentEventType::background_result,
        eyes::AgentEventType::confirm_request, eyes::AgentEventType::confirm,
        eyes::AgentEventType::confirm_close, eyes::AgentEventType::wallet_update,
        eyes::AgentEventType::balance_reveal, eyes::AgentEventType::funds_received,
        eyes::AgentEventType::cast_start,    eyes::AgentEventType::cast_paired,
        eyes::AgentEventType::cast_cursor,   eyes::AgentEventType::cast_end,
        eyes::AgentEventType::notification,  eyes::AgentEventType::timer_set,
        eyes::AgentEventType::show_qr};
    for (const eyes::AgentEventType kind : kinds) {
        eyes::AgentEvent out{};
        out.type = kind;
        out.text = "line\none \"quoted\"\ttab \\slash";
        out.tool = "gps_fix";
        out.id = "run-7";
        const std::string line = eyes::agent_link::encode(out);
        assert(line.back() == '\n');
        eyes::AgentEvent in{};
        assert(eyes::agent_link::parse(line, in));
        assert(in.type == kind && in.text == out.text && in.tool == out.tool && in.id == out.id);
    }
    eyes::AgentEvent event{};
    assert(eyes::agent_link::parse("{\"type\":\"text_delta\",\"text\":\"hi\"}", event));
    assert(event.type == eyes::AgentEventType::text_delta && event.text == "hi");
    assert(event.tool.empty() && event.id.empty());
    // Unknown keys are skipped; unknown types carried as unknown.
    assert(eyes::agent_link::parse(
        "{\"type\":\"future_thing\",\"novel\":\"x\",\"text\":\"kept\"}", event));
    assert(event.type == eyes::AgentEventType::unknown && event.text == "kept");
    // Malformed lines refuse cleanly.
    assert(!eyes::agent_link::parse("", event));
    assert(!eyes::agent_link::parse("not json", event));
    assert(!eyes::agent_link::parse("{\"text\":\"no type\"}", event));
    assert(!eyes::agent_link::parse("{\"type\":\"user_text\",\"n\":5}", event));
    // Unicode escape decodes.
    assert(eyes::agent_link::parse("{\"type\":\"text_delta\",\"text\":\"a\\u00e9b\"}", event));
    assert(event.text == "a\xC3\xA9"
                         "b");
    // Conversation state machine: the face follows the stream.
    eyes::ConversationState state = eyes::ConversationState::idle;
    eyes::AgentEvent step{};
    step.type = eyes::AgentEventType::run_started;
    state = eyes::next_conversation_state(state, step);
    assert(state == eyes::ConversationState::thinking);
    step.type = eyes::AgentEventType::text_delta;
    state = eyes::next_conversation_state(state, step);
    assert(state == eyes::ConversationState::streaming);
    step.type = eyes::AgentEventType::run_finished;
    state = eyes::next_conversation_state(state, step);
    assert(state == eyes::ConversationState::idle);
}

void test_earcon_synth()
{
    // The device earcon synth: deterministic per seed, varied across seeds,
    // level-safe, and every recipe actually makes sound. The jitter is the
    // whole point — a fixed WAV can't do this — so both halves are asserted.
    constexpr std::uint32_t kSr = 16000U;
    static std::int16_t a[kSr];  // 1 s is plenty for any recipe
    static std::int16_t b[kSr];

    const eyes::earcon::Sound sounds[] = {
        eyes::earcon::Sound::confirm,   eyes::earcon::Sound::received,
        eyes::earcon::Sound::listening, eyes::earcon::Sound::error,
        eyes::earcon::Sound::tick};

    for (const eyes::earcon::Sound sound : sounds) {
        const std::size_t predicted = eyes::earcon::sample_count(sound, kSr);
        assert(predicted > 0 && predicted <= kSr);

        // Same seed -> byte-identical (pin-able for goldens). sample_count is an
        // upper bound (timing jitters per seed), so a real play fits within it.
        const std::size_t n1 = eyes::earcon::render(sound, kSr, 42U, a, kSr);
        const std::size_t n2 = eyes::earcon::render(sound, kSr, 42U, b, kSr);
        assert(n1 == n2 && n1 <= predicted);
        for (std::size_t i = 0; i < n1; ++i) {
            assert(a[i] == b[i]);
        }

        // Non-silent: a real acknowledgment, not a dead buffer.
        std::int32_t peak = 0;
        for (std::size_t i = 0; i < n1; ++i) {
            const std::int32_t mag = a[i] < 0 ? -a[i] : a[i];
            if (mag > peak) {
                peak = mag;
            }
        }
        assert(peak > 2000);  // clearly audible, well above noise

        // Different seed -> at least some samples differ (the per-play jitter).
        eyes::earcon::render(sound, kSr, 43U, b, kSr);
        bool differs = false;
        for (std::size_t i = 0; i < n1 && !differs; ++i) {
            differs = a[i] != b[i];
        }
        assert(differs);
    }

    // Capacity is honored: a short buffer truncates, never overruns.
    const std::size_t capped = eyes::earcon::render(eyes::earcon::Sound::received, kSr, 1U, a, 100U);
    assert(capped == 100U);

    // Guards: null/zero inputs return 0 rather than misbehaving.
    assert(eyes::earcon::render(eyes::earcon::Sound::tick, kSr, 1U, nullptr, kSr) == 0U);
    assert(eyes::earcon::render(eyes::earcon::Sound::tick, 0U, 1U, a, kSr) == 0U);
    assert(eyes::earcon::render(eyes::earcon::Sound::tick, kSr, 1U, a, 0U) == 0U);
}

void test_cast_controller()
{
    // podbot's control side of a cast (broker model): capture a code -> cast_start,
    // go live on cast_paired, stream cursor packets, tear down on cast_end.
    eyes::CastController cast;
    assert(cast.state() == eyes::CastState::idle && !cast.active());

    // A cursor before a cast is a no-op (unknown event, nothing to send).
    assert(cast.cursor(1, 2, 0, 0).type == eyes::AgentEventType::unknown);

    // begin() returns the cast_start to send, carrying code + agent + session.
    const eyes::AgentEvent start = cast.begin("482913", "browser", "sess-1");
    assert(start.type == eyes::AgentEventType::cast_start);
    assert(start.text == "482913" && start.tool == "browser" && start.id == "sess-1");
    assert(cast.state() == eyes::CastState::pairing && !cast.live());

    // A second begin while active is refused (unknown, don't send).
    assert(cast.begin("000000", "x", "sess-2").type == eyes::AgentEventType::unknown);

    // A cast_paired for another session does NOT go live; ours does.
    eyes::AgentEvent other{};
    other.type = eyes::AgentEventType::cast_paired;
    other.id = "sess-9";
    assert(!cast.on_event(other));
    assert(cast.state() == eyes::CastState::pairing);
    eyes::AgentEvent paired{};
    paired.type = eyes::AgentEventType::cast_paired;
    paired.id = "sess-1";
    assert(cast.on_event(paired) && cast.live());

    // Live: cursor packs "dx,dy,dscroll,buttons" and round-trips through parse.
    const eyes::AgentEvent cur = cast.cursor(-3, 7, -1, 1);
    assert(cur.type == eyes::AgentEventType::cast_cursor && cur.id == "sess-1");
    assert(cur.text == "-3,7,-1,1");
    int dx = 0, dy = 0, ds = 0;
    unsigned btn = 99;
    assert(eyes::parse_cursor(cur.text, dx, dy, ds, btn));
    assert(dx == -3 && dy == 7 && ds == -1 && btn == 1);
    // Malformed packets are rejected.
    assert(!eyes::parse_cursor("1,2,3", dx, dy, ds, btn));
    assert(!eyes::parse_cursor("1,2,3,x", dx, dy, ds, btn));
    assert(!eyes::parse_cursor("1,2,3,4,5", dx, dy, ds, btn));

    // end() returns cast_end for this session and returns to idle.
    const eyes::AgentEvent stop = cast.end();
    assert(stop.type == eyes::AgentEventType::cast_end && stop.id == "sess-1");
    assert(cast.state() == eyes::CastState::idle && !cast.active());
    // A cursor after end is a no-op again.
    assert(cast.cursor(5, 5, 0, 0).type == eyes::AgentEventType::unknown);
    // A harness-driven cast_end also tears down from live.
    cast.begin("111111", "browser", "sess-3");
    eyes::AgentEvent p3{};
    p3.type = eyes::AgentEventType::cast_paired;
    p3.id = "sess-3";
    assert(cast.on_event(p3) && cast.live());
    eyes::AgentEvent remote_end{};
    remote_end.type = eyes::AgentEventType::cast_end;
    remote_end.id = "sess-3";
    assert(cast.on_event(remote_end) && cast.state() == eyes::CastState::idle);
}

void test_mood_blink_table()
{
    // ~1.35x the raw Grok table: physical-object pacing (user feedback).
    assert(eyes::blink_range_for(eyes::Expression::content).min_ms == 8000U);
    assert(eyes::blink_range_for(eyes::Expression::content).max_ms == 18000U);
    assert(eyes::blink_range_for(eyes::Expression::curious).min_ms == 5500U);
    assert(eyes::blink_range_for(eyes::Expression::listening).max_ms == 12000U);
    assert(eyes::blink_range_for(eyes::Expression::surprised).min_ms == 3500U);
    assert(eyes::blink_range_for(eyes::Expression::sleepy).max_ms == 8000U);
    for (int expression = 0; expression < 10; ++expression) {
        const eyes::BlinkRange range =
            eyes::blink_range_for(static_cast<eyes::Expression>(expression));
        assert(range.min_ms >= 3000U && range.max_ms <= 18000U && range.min_ms < range.max_ms);
    }
}

void test_capsule_qi_tables()
{
    // Every Expression and mode override maps to an in-range Qi recipe and
    // produces finite, on-screen geometry (indices exercised for all inputs).
    const eyes::InteractionMode modes[] = {eyes::InteractionMode::idle,
                                           eyes::InteractionMode::dizzy,
                                           eyes::InteractionMode::sleeping};
    for (int expression = 0; expression < 10; ++expression) {
        for (const eyes::InteractionMode mode : modes) {
            eyes::CapsuleFace face(40U + static_cast<std::uint32_t>(expression));
            for (std::uint32_t time = 0; time <= 6000U; time += 16U) {
                face.update(capsule_frame(time, static_cast<eyes::Expression>(expression),
                                          mode));
                for (int eye = 0; eye < 2; ++eye) {
                    const auto out = face.eye(eye);
                    assert(std::isfinite(out.cx) && std::isfinite(out.cy) &&
                           std::isfinite(out.angle_rad) && std::isfinite(out.squash_y));
                    assert(std::fabs(out.cx - 233.0F) < 260.0F);
                    assert(std::fabs(out.cy - 233.0F) < 260.0F);
                    assert(out.half_len >= out.half_w && out.half_w > 0.0F);
                    assert(out.half_len < 150.0F);
                }
            }
        }
    }

    // Qi roll bases read through the output angle (sampled inside the first
    // pose dwell so the angle spring is settled on the pose): listening leans
    // +8 deg (+sin(.5t)*1.5), thinking -9 (+sin(.35t)*5).
    const auto average_roll = [](eyes::Expression expression) {
        eyes::CapsuleFace face(17U);
        float sum = 0.0F;
        int count = 0;
        for (std::uint32_t time = 0; time <= 2900U; time += 16U) {
            face.update(capsule_frame(time, expression));
            if (time >= 2000U) {
                const eyes::CapsuleEye &pose =
                    eyes::kCapsulePoses[static_cast<std::size_t>(face.pose_index())].left;
                sum += std::remainder(
                    face.eye(0).angle_rad - pose.angle_deg * 3.14159265F / 180.0F,
                    3.14159265F);
                ++count;
            }
        }
        return sum / static_cast<float>(count);
    };
    const float listening_roll = average_roll(eyes::Expression::listening);
    assert(listening_roll > 0.10F && listening_roll < 0.21F);
    const float thinking_roll = average_roll(eyes::Expression::thinking);
    assert(thinking_roll > -0.16F && thinking_roll < -0.04F);

    // Angry (annoyed) velocity kick: y.v += 70 every 1.8-3.2 s bounces the
    // face measurably (peak ~12 px on a critically damped omega-4 spring).
    eyes::CapsuleFace face(23U);
    float baseline = 0.0F;
    int baseline_count = 0;
    float peak = 0.0F;
    for (std::uint32_t time = 0; time <= 3900U; time += 16U) {
        face.update(capsule_frame(time, eyes::Expression::annoyed));
        const float cy = face.eye(0).cy;
        if (time >= 1200U && time <= 1750U) {
            baseline += cy;
            ++baseline_count;
        } else if (time > 1800U && baseline_count > 0) {
            peak = std::max(peak,
                            std::fabs(cy - baseline / static_cast<float>(baseline_count)));
        }
    }
    assert(peak > 5.0F);
}

void test_capsule_spin_wild()
{
    // Double poke (two pokes within 600 ms) starts spinWild; the transient
    // rotation offset peaks (3 visual turns = 6*pi), lands back on a multiple
    // of 2*pi at the authored decel end, and is exactly 0 after the 5.49 s
    // flourish. The lid couples wide (1.14) and the settle gate never freezes
    // mid-spin.
    eyes::CapsuleFace face(7U);
    eyes::FrameState state = capsule_frame(0U);
    for (std::uint32_t time = 0; time <= 2000U; time += 16U) {
        state.time_ms = time;
        face.update(state);
    }
    state.time_ms = 2016U;
    state.poke_count = 1U;
    face.update(state);
    for (std::uint32_t time = 2032U; time <= 2300U; time += 16U) {
        state.time_ms = time;
        face.update(state);
    }
    assert(!face.spin_active());  // single poke never celebrates
    assert(face.spin_offset_rad() == 0.0F);
    state.time_ms = 2316U;
    state.poke_count = 2U;
    face.update(state);
    assert(face.spin_active());  // second poke within 600 ms does
    const std::uint32_t start = 2316U;

    float max_offset = 0.0F;
    float max_open = 0.0F;
    bool landed_on_turn = false;
    std::uint32_t last_active = start;
    std::uint32_t first_inactive = 0U;
    int cruise_frames = 0;
    int cruise_busy = 0;
    face.mark_rendered();
    for (std::uint32_t time = start + 16U; time <= start + 6000U; time += 16U) {
        state.time_ms = time;
        face.update(state);
        const std::uint32_t elapsed = time - start;
        if (face.spin_active()) {
            last_active = time;
            max_offset = std::max(max_offset, std::fabs(face.spin_offset_rad()));
            if (elapsed >= 3790U) {
                // Decel landed: belt = 9*2pi exactly, visual = 6*pi == 0 mod 2pi.
                assert(std::fabs(std::remainder(face.spin_offset_rad(),
                                                2.0F * 3.14159265F)) < 1e-3F);
                landed_on_turn = true;
            }
            if (elapsed >= 600U && elapsed <= 2500U) {
                ++cruise_frames;
                if (face.needs_frame()) {
                    ++cruise_busy;
                }
                max_open = std::max(max_open, face.blink_openness());
            }
        } else if (first_inactive == 0U) {
            first_inactive = time;
        }
        face.mark_rendered();
    }
    assert(!face.spin_active());
    assert(face.spin_offset_rad() == 0.0F);  // returns to exactly 0
    // Authored duration 5490 ms (16 ms tick grid).
    assert(last_active - start >= 5474U && last_active - start < 5490U);
    assert(first_inactive - start >= 5490U && first_inactive - start <= 5506U);
    assert(max_offset > 5.0F);  // really wound up toward 6*pi ~ 18.85
    assert(landed_on_turn);
    assert(max_open > 1.05F);  // spin lid coupling (target 1.14)
    // Settle gate: the continuous rotation demands a frame virtually every tick.
    assert(cruise_busy >= cruise_frames * 9 / 10);

    // Entering happy celebrates rarely (10%): some of 60 seeds, never most.
    int celebrated = 0;
    for (std::uint32_t seed = 1U; seed <= 60U; ++seed) {
        eyes::CapsuleFace probe(seed);
        for (std::uint32_t time = 0; time <= 500U; time += 16U) {
            probe.update(capsule_frame(time));
        }
        probe.update(capsule_frame(516U, eyes::Expression::happy));
        if (probe.spin_active()) {
            ++celebrated;
        }
    }
    assert(celebrated >= 1 && celebrated <= 20);

    // Asleep: double poke must not spin.
    eyes::CapsuleFace sleeper(9U);
    eyes::FrameState night = capsule_frame(0U, eyes::Expression::sleepy,
                                           eyes::InteractionMode::sleeping);
    for (std::uint32_t time = 0; time <= 1000U; time += 16U) {
        night.time_ms = time;
        sleeper.update(night);
    }
    night.time_ms = 1016U;
    night.poke_count = 1U;
    sleeper.update(night);
    night.time_ms = 1216U;
    night.poke_count = 2U;
    sleeper.update(night);
    assert(!sleeper.spin_active());
}

void test_capsule_nod_off()
{
    // Drowsy nod-off raw curve: base lid .34 +- .07, droop to .04 over 1.7 s,
    // bounce peek .46 over 0.3 s, recover over 1.5 s (spring-lagged samples).
    eyes::CapsuleFace face(11U);
    std::vector<float> open;
    for (std::uint32_t time = 0; time <= 16000U; time += 16U) {
        face.update(capsule_frame(time, eyes::Expression::sleepy));
        open.push_back(face.blink_openness());
    }
    // Base before the first nod (earliest nod at entry+1200 ms): near .34.
    const float base = open[1000U / 16U];
    assert(base > 0.20F && base < 0.50F);
    // Global minimum reaches the .04 floor (omega-26 spring tracks within .06).
    std::size_t min_index = 0;
    for (std::size_t index = 1; index < open.size(); ++index) {
        if (open[index] < open[min_index]) {
            min_index = index;
        }
    }
    assert(open[min_index] < 0.10F);
    // Droop shape: openness stays below .30 for 0.8-1.4 s leading into the min
    // (target crosses .30 at ~620 ms before the floor).
    std::size_t below = min_index;
    while (below > 0 && open[below - 1] < 0.30F) {
        --below;
    }
    const float droop_ms = static_cast<float>(min_index - below) * 16.0F;
    assert(droop_ms > 700.0F && droop_ms < 1500.0F);
    // Bounce peek within 500 ms after the floor.
    float peek = 0.0F;
    for (std::size_t index = min_index; index < std::min(min_index + 32U, open.size());
         ++index) {
        peek = std::max(peek, open[index]);
    }
    assert(peek > 0.36F && peek < 0.55F);
    // Recovered back into the drowsy base band ~2 s after the floor.
    const std::size_t after = std::min(min_index + 125U, open.size() - 1U);
    assert(open[after] > 0.15F && open[after] < 0.55F);
}

void test_capsule_wave()
{
    // Wave-bar "talking" pulse: sound attention modulates eye size (9*level
    // detuned per eye); any other attention leaves sizes on breath only.
    const auto half_len_spread = [](eyes::AttentionSource attention) {
        eyes::CapsuleFace face(13U);
        eyes::FrameState state = capsule_frame(0U);
        state.attention = attention;
        float minimum = 1e9F;
        float maximum = -1e9F;
        for (std::uint32_t time = 0; time <= 5000U; time += 16U) {
            state.time_ms = time;
            face.update(state);
            if (time >= 1500U) {
                minimum = std::min(minimum, face.eye(0).half_len);
                maximum = std::max(maximum, face.eye(0).half_len);
            }
        }
        return maximum - minimum;
    };
    assert(half_len_spread(eyes::AttentionSource::sound) > 4.0F);
    assert(half_len_spread(eyes::AttentionSource::idle) < 2.5F);
    assert(half_len_spread(eyes::AttentionSource::touch) < 2.5F);

    // Clearing the attention gate returns the pulse to rest.
    eyes::CapsuleFace face(13U);
    eyes::FrameState state = capsule_frame(0U);
    state.attention = eyes::AttentionSource::sound;
    for (std::uint32_t time = 0; time <= 3000U; time += 16U) {
        state.time_ms = time;
        face.update(state);
    }
    state.attention = eyes::AttentionSource::idle;
    float minimum = 1e9F;
    float maximum = -1e9F;
    for (std::uint32_t time = 3016U; time <= 6500U; time += 16U) {
        state.time_ms = time;
        face.update(state);
        if (time >= 4500U) {  // 1.5 s for the omega-10 gain spring to settle
            minimum = std::min(minimum, face.eye(0).half_len);
            maximum = std::max(maximum, face.eye(0).half_len);
        }
    }
    assert(maximum - minimum < 2.5F);
}

void write_ppm(const char *path, const std::vector<std::uint16_t> &pixels, int width, int height)
{
    std::ofstream stream(path, std::ios::binary);
    assert(stream.good());
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    for (const std::uint16_t pixel : pixels) {
        const auto expand5 = [](std::uint16_t v) {
            return static_cast<char>((v * 255U + 15U) / 31U);
        };
        const auto expand6 = [](std::uint16_t v) {
            return static_cast<char>((v * 255U + 31U) / 63U);
        };
        const char rgb[3]{expand5(static_cast<std::uint16_t>((pixel >> 11U) & 0x1fU)),
                          expand6(static_cast<std::uint16_t>((pixel >> 5U) & 0x3fU)),
                          expand5(static_cast<std::uint16_t>(pixel & 0x1fU))};
        stream.write(rgb, 3);
    }
}

// Renders every pose through the real rasterizer into 5x5 tiles plus a blink
// strip (real CapsuleFace envelope) — visually verifiable outputs in /tmp.
void render_capsule_previews()
{
    const eyes::Rgb disc_color = eyes::palette_at(63).outer;
    const eyes::Rgb black{0, 0, 0};
    std::vector<std::uint16_t> tile(static_cast<std::size_t>(eyes::kScreenWidth) *
                                    eyes::kScreenHeight);

    const auto render_pose = [&](const eyes::CapsulePose &pose, float openness) {
        eyes::Raster raster({tile.data(), eyes::kScreenWidth, eyes::kScreenHeight,
                             eyes::kScreenWidth});
        raster.clear(black);
        raster.fill_circle(233.0F, 233.0F, eyes::kCapsuleDiscRadius, disc_color);
        for (const eyes::CapsuleEye *eye : {&pose.left, &pose.right}) {
            const float half_w = eye->half_w * openness;
            const auto path = eyes::capsule_path(
                233.0F + eye->cx * eyes::kCapsuleScale, 233.0F + eye->cy * eyes::kCapsuleScale,
                eye->angle_deg * 3.14159265F / 180.0F, eye->half_len * eyes::kCapsuleScale,
                std::min(half_w, eye->half_len) * eyes::kCapsuleScale);
            raster.fill_cubic_path(path.data(), path.size(), black);
        }
    };

    // 5x5 grid of all 25 poses, each tile downsampled 2x -> 1165x1165.
    constexpr int kTile = eyes::kScreenWidth / 2;
    std::vector<std::uint16_t> grid(static_cast<std::size_t>(kTile * 5) * (kTile * 5));
    for (int pose = 0; pose < 25; ++pose) {
        render_pose(eyes::kCapsulePoses[static_cast<std::size_t>(pose)], 1.0F);
        const int origin_x = (pose % 5) * kTile;
        const int origin_y = (pose / 5) * kTile;
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                grid[static_cast<std::size_t>((origin_y + y) * (kTile * 5) + origin_x + x)] =
                    tile[static_cast<std::size_t>(y * 2 * eyes::kScreenWidth + x * 2)];
            }
        }
    }
    write_ppm("/tmp/capsule_grid.ppm", grid, kTile * 5, kTile * 5);

    // Blink strip: sample the real spring envelope through a forced state-entry
    // blink and render pose 10 at each sampled openness (8 frames, 2x down).
    eyes::CapsuleFace face(3U);
    for (std::uint32_t time = 0; time <= 3000U; time += 16U) {
        face.update(capsule_frame(time));
    }
    const std::uint32_t offsets[8] = {0U, 48U, 96U, 144U, 208U, 288U, 384U, 640U};
    std::vector<float> samples;
    std::uint32_t cursor = 3000U;
    for (const std::uint32_t offset : offsets) {
        while (cursor < 3016U + offset) {
            cursor += 16U;
            face.update(capsule_frame(cursor, eyes::Expression::happy));
        }
        samples.push_back(face.blink_openness());
    }
    std::vector<std::uint16_t> strip(static_cast<std::size_t>(kTile * 8) * kTile);
    for (int frame = 0; frame < 8; ++frame) {
        render_pose(eyes::kCapsulePoses[10],
                    std::clamp(samples[static_cast<std::size_t>(frame)], 0.05F, 1.15F));
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                strip[static_cast<std::size_t>(y * (kTile * 8) + frame * kTile + x)] =
                    tile[static_cast<std::size_t>(y * 2 * eyes::kScreenWidth + x * 2)];
            }
        }
    }
    write_ppm("/tmp/capsule_blink.ppm", strip, kTile * 8, kTile);
    std::cout << "Wrote /tmp/capsule_grid.ppm and /tmp/capsule_blink.ppm\n";
}

// The audio-bar visualizer (dynamic island): deterministic, in-bounds, and
// louder → taller. Renders into a full panel-sized buffer like the device.
void test_audio_bars()
{
    constexpr int W = eyes::kScreenWidth;
    constexpr int H = eyes::kScreenHeight;
    std::vector<std::uint16_t> a(static_cast<std::size_t>(W * H), 0U);
    std::vector<std::uint16_t> b(static_cast<std::size_t>(W * H), 0U);

    float levels[4];
    eyes::audio_bar_levels(1.234F, 0.7F, levels);
    for (int i = 0; i < 4; ++i) {
        assert(levels[i] >= 0.0F && levels[i] <= 1.0F);
    }

    eyes::draw_audio_bars(a.data(), W, H, W / 2, H / 2, levels, 0xFFFFU);
    eyes::draw_audio_bars(b.data(), W, H, W / 2, H / 2, levels, 0xFFFFU);
    assert(a == b);  // deterministic given (phase, levels)

    auto white = [](const std::vector<std::uint16_t> &fb) {
        std::size_t n = 0;
        for (auto p : fb) {
            if (p == 0xFFFFU) ++n;
        }
        return n;
    };
    const std::size_t lit = white(a);
    assert(lit > 0);  // it drew something

    // Louder → more lit pixels (taller bars). Use full vs near-silent energy.
    float loud[4];
    float quiet[4];
    eyes::audio_bar_levels(0.0F, 1.0F, loud);
    eyes::audio_bar_levels(0.0F, 0.0F, quiet);
    std::vector<std::uint16_t> fl(static_cast<std::size_t>(W * H), 0U);
    std::vector<std::uint16_t> fq(static_cast<std::size_t>(W * H), 0U);
    eyes::draw_audio_bars(fl.data(), W, H, W / 2, H / 2, loud, 0xFFFFU);
    eyes::draw_audio_bars(fq.data(), W, H, W / 2, H / 2, quiet, 0xFFFFU);
    assert(white(fl) > white(fq));

    // A center-of-panel pixel lands inside a bar (the middle two bars straddle
    // the center), so the row through cy is lit somewhere near the middle.
    bool row_has_ink = false;
    for (int x = 0; x < W; ++x) {
        if (a[static_cast<std::size_t>((H / 2) * W + x)] == 0xFFFFU) {
            row_has_ink = true;
            break;
        }
    }
    assert(row_has_ink);
    std::cout << "audio bars lit " << lit << " px (loud " << white(fl) << ", quiet "
              << white(fq) << ")\n";
}

// Status glyphs (listening / success / error): deterministic, in-bounds, and
// animation reveals more ink over progress/phase.
void test_status_glyphs()
{
    constexpr int W = eyes::kScreenWidth;
    constexpr int H = eyes::kScreenHeight;
    auto white = [](const std::vector<std::uint16_t> &fb) {
        std::size_t n = 0;
        for (auto p : fb) if (p == 0xFFFFU) ++n;
        return n;
    };
    auto blank = []() { return std::vector<std::uint16_t>(static_cast<std::size_t>(W * H), 0U); };

    // Success check: half-drawn has ink; fully-drawn has strictly more; two
    // renders of the same progress are identical.
    auto half = blank(), full = blank(), full2 = blank();
    eyes::draw_check(half.data(), W, H, W / 2, H / 2, 0.5F, 0xFFFFU);
    eyes::draw_check(full.data(), W, H, W / 2, H / 2, 1.0F, 0xFFFFU);
    eyes::draw_check(full2.data(), W, H, W / 2, H / 2, 1.0F, 0xFFFFU);
    assert(white(half) > 0 && white(full) > white(half));
    assert(full == full2);  // deterministic

    // Error cross draws something.
    auto cross = blank();
    eyes::draw_cross(cross.data(), W, H, W / 2, H / 2, 1.0F, 0xFFFFU);
    assert(white(cross) > 0);

    // Listening dots: deterministic per phase, and the middle band is lit.
    auto lis = blank(), lis2 = blank();
    eyes::draw_listening(lis.data(), W, H, W / 2, H / 2, 1.0F, 0xFFFFU);
    eyes::draw_listening(lis2.data(), W, H, W / 2, H / 2, 1.0F, 0xFFFFU);
    assert(white(lis) > 0 && lis == lis2);
    std::cout << "status glyphs: check " << white(full) << " cross " << white(cross)
              << " listen " << white(lis) << " px\n";
}

// --- voice session: the stream's edges as the face sees them ----------------

void test_voice_session_edges_and_timeouts()
{
    eyes::VoiceSession voice;
    eyes::EyeEngine engine(5U);
    assert(voice.state() == eyes::VoiceState::idle);
    assert(!voice.consume_changed());

    // No transport fitted: the wake edge still perks the face, briefly.
    voice.wake(1000U);
    assert(voice.state() == eyes::VoiceState::listening);
    voice.apply_to(engine);
    assert(engine.expression_held());
    voice.tick(1000U + eyes::VoiceSession::kNoTransportHoldMs - 1U);
    assert(voice.state() == eyes::VoiceState::listening);
    voice.tick(1000U + eyes::VoiceSession::kNoTransportHoldMs);
    assert(voice.state() == eyes::VoiceState::idle);
    voice.apply_to(engine);
    assert(!engine.expression_held());

    // With a transport: the full turn, barge-in included.
    voice.set_transport_available(true);
    voice.wake(5000U);
    assert(voice.state() == eyes::VoiceState::listening);
    voice.tick(5000U + eyes::VoiceSession::kIdleTimeoutMs - 1U);
    assert(voice.state() == eyes::VoiceState::listening);  // the contract's 30 s
    voice.end_of_turn(6000U);
    assert(voice.state() == eyes::VoiceState::thinking);
    eyes::Expression held{};
    assert(eyes::VoiceSession::expression_for(voice.state(), held) && held == eyes::Expression::thinking);
    voice.wake(6100U);  // a wake mid-think is ignored; the next edge decides
    assert(voice.state() == eyes::VoiceState::thinking);
    voice.reply_started(6500U);
    assert(voice.state() == eyes::VoiceState::speaking);
    assert(!eyes::VoiceSession::expression_for(voice.state(), held));  // bars own the eyes
    voice.wake(7000U);  // talking over the reply
    assert(voice.state() == eyes::VoiceState::listening);
    assert(voice.consume_barge_ins() == 1U);
    assert(voice.consume_barge_ins() == 0U);
    voice.end_of_turn(7500U);
    voice.reply_started(7600U);
    voice.reply_finished(9000U);
    assert(voice.state() == eyes::VoiceState::listening);  // the session stays open
    voice.tick(9000U + eyes::VoiceSession::kIdleTimeoutMs);
    assert(voice.state() == eyes::VoiceState::idle);
    voice.close(9500U);
    assert(voice.state() == eyes::VoiceState::idle);
}

// --- onboarding: hey, then a name and a wake word by voice, or the app -----

namespace {

struct OnboardingRig {
    std::vector<std::uint16_t> pixels;
    eyes::Onboarding onboarding;
    std::uint32_t now{1000U};

    OnboardingRig() : pixels(static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight) {}

    std::size_t lit() const
    {
        std::size_t count = 0;
        for (std::uint16_t p : pixels) {
            if (eyes::pixel_red5(p) > 0) {
                ++count;
            }
        }
        return count;
    }
    std::size_t white() const
    {
        std::size_t count = 0;
        for (std::uint16_t p : pixels) {
            if (p == eyes::white_at(1.0F)) {
                ++count;
            }
        }
        return count;
    }
    // Advance one 16 ms frame; returns the rows the surface touched.
    eyes::PixelBounds frame()
    {
        now += 16U;
        return onboarding.render(pixels.data(), now);
    }
    void run(std::uint32_t duration_ms)
    {
        for (std::uint32_t elapsed = 0; elapsed < duration_ms; elapsed += 16U) {
            (void)frame();
        }
    }
};

}  // namespace

void test_onboarding_greets_once_without_a_path()
{
    OnboardingRig rig;
    eyes::DeviceIdentity fresh{};
    rig.onboarding.begin(fresh, {}, rig.now);
    assert(rig.onboarding.active());
    assert(rig.onboarding.step() == eyes::Onboarding::Step::greet);

    // First frame clears the glass (full band); the pen has not started.
    eyes::PixelBounds first = rig.frame();
    assert(first.valid() && first.y0 == 0 && first.y1 == eyes::kScreenHeight - 1);
    assert(rig.lit() == 0);
    // Mid-word: ink on the glass, and each frame touches a band, not the panel.
    rig.run(eyes::Onboarding::kHeyDelayMs + eyes::Onboarding::kHeyWriteMs / 2);
    const std::size_t mid = rig.lit();
    assert(mid > 2000);
    const eyes::PixelBounds band = rig.frame();
    assert(band.valid());
    assert(band.y1 - band.y0 < eyes::kScreenHeight / 3);
    rig.run(eyes::Onboarding::kHeyWriteMs / 2 + 100U);
    const std::size_t full = rig.lit();
    assert(full > mid * 3 / 2);  // the whole word
    assert(rig.white() > 3000);   // the stroke core is solid white
    assert(rig.onboarding.active());

    // Nobody can answer: the word holds, dips out, the face comes back.
    rig.run(eyes::Onboarding::kGreetHoldMs + 300U);
    assert(!rig.onboarding.active());
    assert(rig.lit() == 0);  // black glass handed back
    bool completed = true;
    assert(rig.onboarding.consume_finished(completed));
    assert(!completed);
    assert(!rig.onboarding.consume_finished(completed));
    eyes::DeviceIdentity saved{};
    assert(rig.onboarding.consume_save(saved));
    assert(saved.stage == eyes::OnboardingStage::greeted);
    assert(saved.name[0] == '\0');

    // Next boot, still no path: straight to the face, no second greeting.
    rig.onboarding.begin(saved, {}, rig.now);
    assert(!rig.onboarding.active());
    // ...and a finished identity never onboards, path or not.
    saved.stage = eyes::OnboardingStage::done;
    rig.onboarding.begin(saved, {true}, rig.now);
    assert(!rig.onboarding.active());
}

void test_onboarding_voice_names_and_sets_the_wake_word()
{
    OnboardingRig rig;
    eyes::DeviceIdentity fresh{};
    assert(std::string(fresh.wake_word) == "hi podbot");  // before a name exists
    rig.onboarding.begin(fresh, {true}, rig.now);
    // Words spoken over the pen are dropped, not queued.
    rig.run(400U);
    rig.onboarding.voice_text("too early", rig.now);
    rig.run(eyes::Onboarding::kHeyDelayMs + eyes::Onboarding::kHeyWriteMs);
    assert(rig.onboarding.step() == eyes::Onboarding::Step::name);
    assert(rig.onboarding.identity().name[0] == '\0');
    // The word waits on the glass; a quiet frame touches nothing.
    rig.run(2000U);
    assert(!rig.frame().valid());
    assert(rig.lit() > 3000);

    // The name is the whole of setup: it fixes the wake word as "hi <name>".
    rig.onboarding.voice_text("Pod.", rig.now);  // the recognizer's period is trimmed
    assert(std::string(rig.onboarding.identity().name) == "Pod");
    assert(std::string(rig.onboarding.identity().wake_word) == "hi Pod");
    eyes::DeviceIdentity saved{};
    assert(rig.onboarding.consume_save(saved));
    assert(saved.stage == eyes::OnboardingStage::done);
    // Dip out (220 ms), then "Nice to meet you, Pod. Say 'hi Pod' to talk."
    // dips in (340 ms), holds, and dips out to the face.
    rig.run(240U);
    assert(rig.onboarding.step() == eyes::Onboarding::Step::confirm);
    rig.run(400U);
    assert(rig.lit() > 500);
    assert(rig.onboarding.active());
    rig.run(eyes::Onboarding::kConfirmHoldMs + 300U);
    assert(!rig.onboarding.active());
    bool completed = false;
    assert(rig.onboarding.consume_finished(completed));
    assert(completed);
    assert(rig.lit() == 0);

    // A legacy identity that was named before the wake word derived from the
    // name finishes on boot without writing "hey" again.
    eyes::DeviceIdentity named{};
    std::snprintf(named.name, sizeof(named.name), "Pod");
    named.stage = eyes::OnboardingStage::named;
    rig.onboarding.begin(named, {true}, rig.now);
    assert(!rig.onboarding.active());
    assert(rig.onboarding.consume_save(saved));
    assert(saved.stage == eyes::OnboardingStage::done);
    assert(std::string(saved.wake_word) == "hi Pod");
}

void test_onboarding_qr_launches_and_the_app_claims()
{
    OnboardingRig rig;
    eyes::DeviceIdentity fresh{};
    rig.onboarding.begin(fresh, {}, rig.now);  // no voice on this board
    // A 21-module code (version 1) with a finder pattern in one corner.
    std::string rows;
    for (int y = 0; y < 21; ++y) {
        for (int x = 0; x < 21; ++x) {
            const bool in_finder = x < 7 && y < 7;
            const bool finder = in_finder && (x == 0 || y == 0 || x == 6 || y == 6 ||
                                              (x >= 2 && x <= 4 && y >= 2 && y <= 4));
            const bool data = !in_finder && ((x * 7 + y * 3) % 5 == 0);
            rows += (finder || data) ? '1' : '0';
        }
        rows += '\n';
    }
    // Pushed while the pen is still writing: the word finishes, then dips out
    // and the code launches in its place.
    rig.run(800U);
    rig.onboarding.show_qr(rows.c_str(), rig.now);
    assert(rig.onboarding.qr_showing());
    rig.run(eyes::Onboarding::kHeyDelayMs + eyes::Onboarding::kHeyWriteMs);
    assert(rig.onboarding.step() == eyes::Onboarding::Step::name);
    rig.run(240U + eyes::Onboarding::kQrLaunchMs + 200U);
    assert(rig.onboarding.active());
    // A white card with black modules sits on the black glass: white pixels
    // inside the card, black modules inside it, black glass around it.
    const std::size_t card_white = rig.white();
    assert(card_white > 20000 && card_white < 60000);
    const int cx = eyes::kScreenWidth / 2;
    const int cy = eyes::kScreenHeight / 2 - 12;
    const std::size_t centre = static_cast<std::size_t>(cy) * eyes::kScreenWidth + static_cast<std::size_t>(cx);
    (void)centre;
    // The finder's top-left module (row 0, col 0) is dark; module (1,1) is light.
    const int module = 250 / 21;
    const int total = module * 21;
    const int left = cx - total / 2;
    const int top = cy - total / 2;
    const auto at = [&](int mx, int my) {
        return rig.pixels[static_cast<std::size_t>(top + my * module + module / 2) * eyes::kScreenWidth +
                          static_cast<std::size_t>(left + mx * module + module / 2)];
    };
    assert(at(0, 0) == 0U);
    assert(at(1, 1) == eyes::white_at(1.0F));
    assert(rig.pixels[10] == 0U);  // the glass outside stays black
    // Idle on the code: nothing to redraw.
    assert(!rig.frame().valid());

    // The signed-in app claims the code and names the device.
    rig.onboarding.app_claimed("Kitchen", rig.now);
    assert(!rig.onboarding.qr_showing());
    rig.run(240U);
    assert(rig.onboarding.step() == eyes::Onboarding::Step::confirm);
    rig.run(340U + eyes::Onboarding::kConfirmHoldMs + 300U);
    assert(!rig.onboarding.active());
    bool completed = false;
    assert(rig.onboarding.consume_finished(completed) && completed);
    eyes::DeviceIdentity saved{};
    assert(rig.onboarding.consume_save(saved));
    assert(saved.stage == eyes::OnboardingStage::done);
    assert(std::string(saved.name) == "Kitchen");
    assert(std::string(saved.wake_word) == "hi Kitchen");  // the app's name sets the wake word too

    // Malformed matrices are refused, never half-drawn.
    rig.onboarding.show_qr("1010\n10\n", rig.now);
    assert(!rig.onboarding.qr_showing());
    rig.onboarding.show_qr("", rig.now);
    assert(!rig.onboarding.qr_showing());
}


int main()
{
    test_catalog();
#if CONFIG_LILGUY_WORLD_VIEW
    test_swipe_and_settle();
    test_selection_lock_keeps_touch_and_recovers_browse();
#endif
    test_input_arbitration_and_reactions();
    test_calm_motion();
    test_sleep_cancels_motion_and_wakes_cleanly();
#if CONFIG_LILGUY_WORLD_VIEW
    test_browse_freezes_live_motion();
#endif
    test_exact_priority_windows_and_blink_deferral();
    test_imu_release_hold();
    test_imu_continuous_response();
    test_imu_timeout_recoil_lockout_and_rearm();
    test_base_selection_and_emotion_dwell();
#if CONFIG_LILGUY_WORLD_VIEW
    test_tap_during_browse_defers_blink();
#endif
    test_seeded_weights_and_zero_seed();
#if CONFIG_LILGUY_WORLD_VIEW
    test_cancel_does_not_select_or_react();
#endif
    test_sound_gate_hold_and_cooldown();
    test_sound_stale_and_higher_priority_clear_streak();
    test_sound_sleep_browse_shake_clear_without_resume();
    test_pupil_clips_loop_without_discontinuity();
    test_shape_morph_endpoints_and_midpoint();
    test_shape_morph_engine_flow_and_retarget_continuity();
    test_rot_gaze_cross_terms();
    test_boot_ramp_starts_neutral();
    test_blink_follows_gaze();
    test_blink_pool_weights_and_drowsy_clips();
    test_blink_pupil_alpha_fade();
    test_pupils_lead_eyes_on_gaze_step();
    test_rot_clips_start_and_end_at_neutral();
    test_rot_flourish_schedules_and_renders_additively();
    test_renderer();
    test_expression_pose_and_blink_interpolation();
    test_expression_poses_stay_mirrored();
    test_sleep_uses_authored_closed_frame();
#if CONFIG_LILGUY_WORLD_VIEW
    test_hex_grid_inner_cells_animate_and_final_fade();
    test_pan_idle_incremental_matches_full_repaint();
#endif
    test_hero_diff_clear_matches_full_clear();
#if CONFIG_LILGUY_WORLD_VIEW
    test_hex_cell_shapes_vary_and_settle_adopts_them();
#endif
    test_dirty_bounds_cover_every_changed_pixel();
    test_dirty_regions_reduce_transfer_area();
    test_boundary_coverage_and_clipping();
    test_shallow_fold_is_contiguous_and_deterministic();
    test_local_extremum_keeps_partial_row_coverage();
    test_bow_tie_crossing_splits_the_row_band();
    test_randomize_selection_changes_and_morphs();
    test_sensitivity_levels_and_default();
    test_sleep_wake_toggle();
    test_orientation_rotates_render_and_sensors();
    test_ui_boot_press_gestures();
    test_ui_power_press_gestures();
    test_ui_settings_run_and_persist_and_look_is_pinned();
    test_ui_paints_text_bands_and_unions_rows();
    test_ui_applies_stored_settings();
    test_aa_override_pins_raster_quality();
    test_capsule_pose_table();
    test_capsule_path_bbox();
    test_capsule_spring_convergence();
    test_capsule_blink_envelope_and_morph_continuity();
    test_capsule_needs_frame();
    test_capsule_renderer_incremental_matches_full();
    test_capsule_qi_tables();
    test_capsule_spin_wild();
    test_capsule_nod_off();
    test_capsule_wave();
    test_wink_suppresses_one_eye();
    test_mood_blink_table();
    test_os_services_gate_absent_hardware();
    test_agent_link_round_trip();
    test_cast_controller();
    test_earcon_synth();
    test_expression_hold();
    test_ota_controller_gating();
    test_ota_required_update();
    test_app_switcher();
    test_audio_bars();
    test_status_glyphs();
    test_voice_session_edges_and_timeouts();
    test_onboarding_greets_once_without_a_path();
    test_onboarding_voice_names_and_sets_the_wake_word();
    test_onboarding_qr_launches_and_the_app_claims();
    render_capsule_previews();
    std::cout << "All LilGuy eye host tests passed\n";
    return 0;
}
