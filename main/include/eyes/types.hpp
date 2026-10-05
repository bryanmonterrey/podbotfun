#pragma once

#include <cstdint>

namespace eyes {

constexpr int kScreenWidth = 466;
constexpr int kScreenHeight = 466;
constexpr float kGridPitch = 272.0F;

struct Vec2 {
    float x{0.0F};
    float y{0.0F};
};

struct Rgb {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
};

struct Selection {
    int shape{0};
    int palette{0};

    bool operator==(const Selection &other) const
    {
        return shape == other.shape && palette == other.palette;
    }

    bool operator!=(const Selection &other) const { return !(*this == other); }
};

struct MotionSample {
    float accel_x{0.0F};
    float accel_y{0.0F};
    float accel_z{0.0F};
    float accel_magnitude{9.80665F};
    float gyro_x{0.0F};
    float gyro_y{0.0F};
    float gyro_z{0.0F};
    std::uint32_t timestamp_ms{0};
};

struct SoundPacket {
    std::uint64_t energy{0};
    std::uint32_t timestamp_ms{0};
    std::uint32_t feature_us{0};
    // Arrival-direction estimate along the mic axis, -1..+1 (the two MEMS mics
    // sit ~30mm apart along the board's left edge; cross-correlation lag gives
    // top-vs-bottom). Only meaningful when direction_valid.
    float direction{0.0F};
    bool direction_valid{false};
};

struct Calibration {
    float neutral_accel_x{0.0F};
    float neutral_accel_y{0.0F};
    float neutral_accel_z{9.80665F};
    float gyro_bias_x{0.0F};
    float gyro_bias_y{0.0F};
    float gyro_bias_z{0.0F};
    bool valid{false};
};

// User-tunable settings (settings rows / gestures). Stored as one NVS blob
// that may grow: older, shorter blobs load with defaults for the new tail.
struct Settings {
    std::uint8_t imu_level{1};  // 0=LOW 1=MED 2=HIGH -> tilt sensitivity 0.8/1.3/1.9
    float orientation{0.0F};    // anchored face rotation, radians in [-pi, pi)
    // The look is fixed (owner's call, 2026-09-30): white capsule eyes on a
    // black field, no switching for now. These two fields stay in the blob so
    // its layout does not change; DeviceUi::apply_settings ignores them and
    // pins the renderer to capsule + inverted ink whatever they hold.
    std::uint8_t face_mode{1};  // 0=CREATURE 1=CAPSULE (Grok capsule face)
    std::uint8_t capsule_invert{1};  // 0=palette disc+black eyes 1=black field+palette eyes
};

enum class Geometry : std::uint8_t {
    ellipse,
    capsule,
    boxy,
    diamond,
};

}  // namespace eyes
