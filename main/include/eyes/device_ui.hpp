#pragma once

// The device's framebuffer text UI: the stats page, the status overlay and
// the BOOT/PWR press-gesture state machines. Split out of app_main.cpp so the
// desktop simulator drives the same press semantics as the board instead of a
// lookalike that drifts.
//
// There is no touch menu: the eyes are the interface and touch is petting
// (owner's call, 2026-09-30). The two settings a buttonless device still
// needs (tilt sensitivity, rotation reset) are plain calls the simulator's
// panel and, later, the phone app drive; the panel never grows tap rows.
//
// Everything here is host-compilable. The things it cannot do off-device
// (persist settings, read the battery gauge, ask the IMU task to recalibrate,
// open the voice stream) go through DeviceUiHost.

#include <cstdint>

#include "eyes/eye_engine.hpp"
#include "eyes/eye_renderer.hpp"
#include "eyes/types.hpp"

namespace eyes {

// Framebuffer text UI pages (BOOT double press). Diagnostics only.
enum class UiPage : std::uint8_t { none, stats };

// The settings the simulator panel (and later the app) can run. Each maps to
// exactly what the old options-menu row did, minus the row.
enum class UiSetting : std::uint8_t { tilt, reset_rotation, count };
constexpr int kUiSettingCount = static_cast<int>(UiSetting::count);

constexpr int kTextScale = 2;  // 16px tall glyphs
constexpr int kOverlayRowFirst = 372;
constexpr int kOverlayRowLast = 419;  // two lines + padding
constexpr int kDebugRowFirst = 48;
constexpr int kDebugRowLast = 71;  // one stats line
// Stats page band: eight 24 px text rows in the lower two thirds of the
// panel. Text is screen-aligned even when the face is rotated by the
// two-finger gesture (documented limitation of the framebuffer UI).
constexpr int kUiRowFirst = 140;
constexpr int kStatsRowPitch = 24;
constexpr int kStatsRowCount = 8;
constexpr int kUiRowLast = kUiRowFirst + 8 + kStatsRowCount * kStatsRowPitch + 7;  // 347
constexpr std::uint32_t kOverlayDurationMs = 1500U;
constexpr std::uint32_t kDoublePressMs = 350U;
// BOOT hold thresholds: below kBootDebounceMs is noise, then a short press,
// then an IMU recalibrate, then the debug overlay toggle.
constexpr std::uint32_t kBootDebounceMs = 35U;
constexpr std::uint32_t kBootCalibrateHoldMs = 850U;
constexpr std::uint32_t kBootDebugHoldMs = 3000U;
// Menu LOW/MED/HIGH -> EyeEngine tilt sensitivity (engine default is MED).
constexpr float kImuLevelValues[3] = {0.8F, 1.3F, 1.9F};
constexpr const char *kImuLevelNames[3] = {"LOW", "MED", "HIGH"};

// Wrap-safe deadline compare (shared with the engine's own scheduler).
inline bool ui_time_reached(std::uint32_t now, std::uint32_t deadline)
{
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

// Uppercase names for the stats page.
const char *ui_attention_name(AttentionSource source);

// What the fuel gauge reports. absent and error render differently on the
// stats page, matching the AXP2101's ESP_ERR_NOT_FOUND vs any other failure.
struct BatteryStatus {
    enum class State : std::uint8_t { ok, absent, error };

    State state{State::error};
    std::uint16_t millivolts{0};
    std::uint8_t percent{0};
    bool charging{false};
};

// Platform services the UI needs. The device implements these over NVS, the
// AXP2101 fuel gauge and the IMU task; the simulator fakes them.
class DeviceUiHost {
  public:
    virtual ~DeviceUiHost() = default;
    virtual void save_settings(const Settings &settings) = 0;
    virtual void request_imu_calibration() = 0;
    virtual BatteryStatus read_battery() = 0;
    // PWR single press: open the voice stream (the same thing the wake word
    // does). The device hands it to its voice session; the simulator starts a
    // recognition.
    virtual void request_listen() = 0;
};

// Frame-timing numbers the stats and debug lines report.
struct UiPerf {
    std::uint32_t fps{0};
    std::uint32_t render_us{0};
    std::uint32_t te_us{0};
    std::uint32_t blit_us{0};
};

class DeviceUi {
  public:
    DeviceUi(EyeEngine &engine, EyeRenderer &renderer, DeviceUiHost &host, Settings settings)
        : engine_(engine), renderer_(renderer), host_(host), settings_(settings)
    {
    }

    // Push the current Settings into the engine and renderer. Called at boot
    // and by every setting that changes one. The renderer is always pinned to
    // the capsule face with inverted ink: the look is fixed.
    void apply_settings();

    // --- physical buttons, polled the way the device timers poll them ---
    // BOOT is a GPIO read every 20 ms: single press = nothing (reserved),
    // double = stats page, 850 ms-3 s hold = IMU recalibrate, >=3 s hold =
    // debug overlay.
    void poll_boot_button(bool down, std::uint32_t now_ms);
    // PWR only surfaces short-press events from the AXP2101: single = listen
    // (open the voice stream), double = sleep/wake toggle.
    void poll_power_button(bool short_press_event, std::uint32_t now_ms);

    // --- touch ---
    // Touch never reaches the UI: there is no page that takes taps. The enum
    // stays as the shared vocabulary for the platforms' touch routing.
    enum class TouchPhase : std::uint8_t { pressed, pressing, released, press_lost, other };

    // --- per frame ---
    // Deadlines: pending single-presses, menu idle close, stats and debug text
    // refresh, selection-overlay expiry.
    void tick(std::uint32_t now_ms);
    void set_perf(const UiPerf &perf) { perf_ = perf; }
    void set_last_sound(const SoundPacket &packet) { last_sound_ = packet; }
    // prefix == nullptr shows the shape and palette names; a prefix may carry
    // two lines separated by '\n'.
    void show_selection(std::uint32_t now_ms, const char *prefix = nullptr);

    // Two-finger rotate drives the engine live and reports the angle here; the
    // gesture end persists it.
    void note_orientation(float radians) { settings_.orientation = radians; }
    void save_settings() { host_.save_settings(settings_); }

    // --- paint and band push ---
    // A band "pushes" while its text is visible, plus one frame after it goes
    // away so the rows get erased.
    bool overlay_push() const { return overlay_active_ || overlay_dirty_; }
    bool debug_push() const { return debug_overlay_ || debug_dirty_; }
    bool ui_push() const { return page_ != UiPage::none || ui_dirty_; }
    bool any_push() const { return overlay_push() || debug_push() || ui_push(); }
    // Paints whichever bands are pushing, in the device's order (overlay,
    // debug, then the menu/stats band on top).
    void paint(std::uint16_t *framebuffer);
    // Widens [y0, y1] to cover every pushing band.
    void union_push_rows(int &y0, int &y1) const;
    void clear_dirty();

    UiPage page() const { return page_; }
    bool debug_overlay() const { return debug_overlay_; }
    const Settings &settings() const { return settings_; }
    // Runs one setting (cycle tilt sensitivity, reset the rotation) and
    // persists it: what the simulator's panel and the phone app drive.
    void run_setting(UiSetting setting);
    // Setting labels with their current value, for the panel.
    void setting_text(UiSetting setting, char *out, std::size_t size) const;

  private:
    struct PressGesture {
        std::uint32_t deadline_ms{0};
        bool pending{false};
    };

    void open_page(UiPage page, std::uint32_t now_ms);
    void boot_single(std::uint32_t now_ms);
    void boot_double(std::uint32_t now_ms);
    void power_single();
    void power_double();
    void finish_single_press(PressGesture &gesture, std::uint32_t now_ms, bool boot);
    void short_press(PressGesture &gesture, std::uint32_t now_ms, bool boot);
    void compose_stats();
    void compose_debug();
    void paint_overlay(std::uint16_t *framebuffer) const;
    void paint_debug(std::uint16_t *framebuffer) const;
    void paint_ui(std::uint16_t *framebuffer) const;

    EyeEngine &engine_;
    EyeRenderer &renderer_;
    DeviceUiHost &host_;
    Settings settings_{};
    UiPerf perf_{};
    SoundPacket last_sound_{};

    UiPage page_{UiPage::none};
    bool ui_dirty_{false};
    std::uint32_t stats_next_ms_{0};
    char stats_lines_[kStatsRowCount][40]{};

    char overlay_line1_[48]{};
    char overlay_line2_[48]{};
    bool overlay_active_{false};
    bool overlay_dirty_{false};
    std::uint32_t overlay_until_ms_{0};

    bool debug_overlay_{false};
    bool debug_dirty_{false};
    char debug_line_[48]{};
    std::uint32_t debug_next_ms_{0};

    PressGesture boot_press_{};
    PressGesture power_press_{};
    std::uint32_t button_down_ms_{0};
    bool button_was_down_{false};
};

}  // namespace eyes
