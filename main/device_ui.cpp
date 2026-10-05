#include "eyes/device_ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "eyes/catalog.hpp"
#include "eyes/font8x8.hpp"
#include "eyes/raster.hpp"

namespace eyes {

namespace {

void fill_rows(std::uint16_t *framebuffer, int first_row, int last_row, std::uint16_t color)
{
    std::uint16_t *begin = framebuffer + static_cast<std::size_t>(first_row) * kScreenWidth;
    std::uint16_t *end = framebuffer + static_cast<std::size_t>(last_row + 1) * kScreenWidth;
    for (std::uint16_t *pixel = begin; pixel != end; ++pixel) {
        *pixel = color;
    }
}

// Centered 8x8 glyph run drawn straight into the framebuffer (panel byte order
// via rgb565), so the display pipeline never has to leave dummy-draw mode.
void draw_text_line(std::uint16_t *framebuffer, const char *text, int y)
{
    const std::uint16_t white = rgb565({255, 255, 255});
    const int width = static_cast<int>(std::strlen(text)) * 8 * kTextScale;
    int x = (kScreenWidth - width) / 2;
    for (const char *c = text; *c != '\0'; ++c, x += 8 * kTextScale) {
        if (*c < 32 || *c > 126) {
            continue;
        }
        const std::uint8_t *glyph = kFont8x8[*c - 32];
        for (int row = 0; row < 8; ++row) {
            for (int bit = 0; bit < 8; ++bit) {
                if ((glyph[row] & (1U << bit)) == 0U) {
                    continue;
                }
                for (int dy = 0; dy < kTextScale; ++dy) {
                    for (int dx = 0; dx < kTextScale; ++dx) {
                        const int px = x + bit * kTextScale + dx;
                        const int py = y + row * kTextScale + dy;
                        if (px >= 0 && px < kScreenWidth && py >= 0 && py < kScreenHeight) {
                            framebuffer[static_cast<std::size_t>(py) * kScreenWidth +
                                        static_cast<std::size_t>(px)] = white;
                        }
                    }
                }
            }
        }
    }
}

}  // namespace

const char *ui_attention_name(AttentionSource source)
{
    switch (source) {
        case AttentionSource::imu:
            return "IMU";
        case AttentionSource::sound:
            return "SOUND";
        case AttentionSource::shake:
            return "SHAKE";
        case AttentionSource::touch:
            return "TOUCH";
        case AttentionSource::browse:
            return "BROWSE";
        case AttentionSource::sleep:
            return "SLEEP";
        case AttentionSource::idle:
        default:
            return "IDLE";
    }
}

void DeviceUi::apply_settings()
{
    engine_.set_imu_sensitivity(kImuLevelValues[settings_.imu_level % 3U]);
    engine_.set_orientation(settings_.orientation);
    // The look is fixed: white capsule eyes on a black field. Whatever an
    // older settings blob stored for face/ink is ignored, not migrated, so
    // switching can come back later without a blob version bump.
    renderer_.set_face_mode(FaceMode::capsule);
    renderer_.set_capsule_invert(true);
}

// --- press gestures ---------------------------------------------------------

void DeviceUi::finish_single_press(PressGesture &gesture, std::uint32_t now_ms, bool boot)
{
    if (gesture.pending && ui_time_reached(now_ms, gesture.deadline_ms)) {
        gesture.pending = false;
        if (boot) {
            boot_single(now_ms);
        } else {
            power_single();
        }
    }
}

void DeviceUi::short_press(PressGesture &gesture, std::uint32_t now_ms, bool boot)
{
    if (gesture.pending && !ui_time_reached(now_ms, gesture.deadline_ms)) {
        gesture.pending = false;
        if (boot) {
            boot_double(now_ms);
        } else {
            power_double();
        }
        return;
    }
    finish_single_press(gesture, now_ms, boot);
    gesture.pending = true;
    gesture.deadline_ms = now_ms + kDoublePressMs;
}

void DeviceUi::boot_single(std::uint32_t now_ms)
{
    // Reserved. The options menu it used to open was touch UI; the eyes are
    // the interface now and setup lives in voice and the phone app.
    (void)now_ms;
}

void DeviceUi::boot_double(std::uint32_t now_ms) { open_page(UiPage::stats, now_ms); }

void DeviceUi::power_single()
{
    // Listen: the button is the wake word for anyone who would rather press
    // than speak. The host opens the voice stream and holds the listening face.
    host_.request_listen();
}

void DeviceUi::power_double()
{
    // Sleep/wake toggle. Palm-cover sleep stays independent: it only ever
    // calls sleep() when awake, so the toggle cannot fight it.
    if (engine_.frame().mode == InteractionMode::sleeping) {
        engine_.wake();
    } else {
        engine_.sleep();
    }
}

void DeviceUi::poll_boot_button(bool down, std::uint32_t now_ms)
{
    if (!down) {
        finish_single_press(boot_press_, now_ms, true);
    }
    if (down && !button_was_down_) {
        button_down_ms_ = now_ms;
    } else if (!down && button_was_down_) {
        const std::uint32_t held_ms = now_ms - button_down_ms_;
        if (held_ms >= kBootDebugHoldMs) {
            // Very long hold: toggle the on-device debug stats overlay.
            boot_press_.pending = false;
            debug_overlay_ = !debug_overlay_;
            debug_dirty_ = true;
            debug_next_ms_ = 0U;
        } else if (held_ms >= kBootCalibrateHoldMs) {
            boot_press_.pending = false;
            host_.request_imu_calibration();
            show_selection(now_ms, "HOLD STILL\nCALIBRATING IMU");
        } else if (held_ms >= kBootDebounceMs) {
            short_press(boot_press_, now_ms, true);
        }
    }
    button_was_down_ = down;
}

void DeviceUi::poll_power_button(bool short_press_event, std::uint32_t now_ms)
{
    finish_single_press(power_press_, now_ms, false);
    if (short_press_event) {
        short_press(power_press_, now_ms, false);
    }
}

// --- pages ------------------------------------------------------------------

void DeviceUi::open_page(UiPage page, std::uint32_t now_ms)
{
    (void)now_ms;
    page_ = page_ == page ? UiPage::none : page;  // toggle
    ui_dirty_ = true;
    stats_next_ms_ = 0U;
    // The band overlaps the status overlay rows; drop the overlay so the two
    // never interleave.
    overlay_active_ = false;
    overlay_dirty_ = true;
}

void DeviceUi::run_setting(UiSetting setting)
{
    switch (setting) {
        case UiSetting::tilt:  // LOW -> MED -> HIGH
            settings_.imu_level = static_cast<std::uint8_t>((settings_.imu_level + 1U) % 3U);
            engine_.set_imu_sensitivity(kImuLevelValues[settings_.imu_level]);
            save_settings();
            break;
        case UiSetting::reset_rotation:
            settings_.orientation = 0.0F;
            engine_.set_orientation(0.0F);
            save_settings();
            break;
        case UiSetting::count:
            break;
    }
}

void DeviceUi::tick(std::uint32_t now_ms)
{
    if (page_ == UiPage::stats && ui_time_reached(now_ms, stats_next_ms_)) {
        compose_stats();
        stats_next_ms_ = now_ms + 500U;
    }
    if (debug_overlay_ && ui_time_reached(now_ms, debug_next_ms_)) {
        compose_debug();
        debug_next_ms_ = now_ms + 500U;
        debug_dirty_ = true;
    }
#if CONFIG_LILGUY_SHOW_SELECTION_OVERLAY
    if (overlay_active_ && ui_time_reached(now_ms, overlay_until_ms_)) {
        overlay_active_ = false;
        overlay_dirty_ = true;  // one more push to erase the text rows
    }
#endif
}

void DeviceUi::show_selection(std::uint32_t now_ms, const char *prefix)
{
#if CONFIG_LILGUY_SHOW_SELECTION_OVERLAY
    if (page_ != UiPage::none) {
        return;  // the stats band overlaps the overlay rows
    }
    const Selection selected = engine_.selection();
    const ShapePreset &shape = shape_at(selected.shape);
    const Palette &palette = palette_at(selected.palette);
    if (prefix != nullptr) {
        std::snprintf(overlay_line1_, sizeof(overlay_line1_), "%s", prefix);
        overlay_line2_[0] = '\0';
        // A prefix may carry two lines separated by '\n'.
        if (char *split = std::strchr(overlay_line1_, '\n')) {
            *split = '\0';
            std::snprintf(overlay_line2_, sizeof(overlay_line2_), "%s", split + 1);
        }
    } else {
        std::snprintf(overlay_line1_, sizeof(overlay_line1_), "%s  %.1f%%", shape.name,
                      static_cast<double>(shape.rarity_percent));
        std::snprintf(overlay_line2_, sizeof(overlay_line2_), "%s  %.1f%%", palette.name,
                      static_cast<double>(palette.rarity_percent));
    }
    overlay_until_ms_ = now_ms + kOverlayDurationMs;
    overlay_active_ = true;
    overlay_dirty_ = true;
#else
    (void)now_ms;
    (void)prefix;
#endif
}

// --- text composition -------------------------------------------------------

void DeviceUi::compose_debug()
{
    std::snprintf(debug_line_, sizeof(debug_line_), "FPS%2u R%2u W%2u B%2u",
                  static_cast<unsigned>(perf_.fps),
                  static_cast<unsigned>(perf_.render_us / 1000U),
                  static_cast<unsigned>(perf_.te_us / 1000U),
                  static_cast<unsigned>(perf_.blit_us / 1000U));
}

// Refreshed at 2 Hz while the stats page is open.
void DeviceUi::compose_stats()
{
    const FrameState &frame = engine_.frame();
    const BatteryStatus battery = host_.read_battery();
    if (battery.state == BatteryStatus::State::ok) {
        std::snprintf(stats_lines_[0], sizeof(stats_lines_[0]), "BAT %umV %u%% %s",
                      static_cast<unsigned>(battery.millivolts),
                      static_cast<unsigned>(battery.percent), battery.charging ? "CHG" : "DIS");
    } else {
        std::snprintf(stats_lines_[0], sizeof(stats_lines_[0]), "BAT %s",
                      battery.state == BatteryStatus::State::absent ? "NONE" : "ERR");
    }
    std::snprintf(stats_lines_[1], sizeof(stats_lines_[1]), "FPS%2u R%2u W%2u B%2u",
                  static_cast<unsigned>(perf_.fps),
                  static_cast<unsigned>(perf_.render_us / 1000U),
                  static_cast<unsigned>(perf_.te_us / 1000U),
                  static_cast<unsigned>(perf_.blit_us / 1000U));
    std::snprintf(stats_lines_[2], sizeof(stats_lines_[2]), "ATT %s",
                  ui_attention_name(frame.attention));
    std::snprintf(stats_lines_[3], sizeof(stats_lines_[3]), "GAZE %+.2f %+.2f",
                  static_cast<double>(frame.gaze.x), static_cast<double>(frame.gaze.y));
    std::snprintf(stats_lines_[4], sizeof(stats_lines_[4]), "PUPL %+.2f %+.2f",
                  static_cast<double>(frame.gaze_pupils.x),
                  static_cast<double>(frame.gaze_pupils.y));
    const Vec2 imu_target = engine_.imu_gaze_target();
    std::snprintf(stats_lines_[5], sizeof(stats_lines_[5]), "IMU %+.2f %+.2f",
                  static_cast<double>(imu_target.x), static_cast<double>(imu_target.y));
    std::snprintf(stats_lines_[6], sizeof(stats_lines_[6]), "SND %lu %+.2f",
                  static_cast<unsigned long>(last_sound_.energy / 1000U),
                  static_cast<double>(last_sound_.direction_valid ? last_sound_.direction : 0.0F));
    std::snprintf(stats_lines_[7], sizeof(stats_lines_[7]), "ROT %+d DEG",
                  static_cast<int>(std::lround(static_cast<double>(engine_.orientation()) * 180.0 /
                                               3.14159265)));
}

void DeviceUi::setting_text(UiSetting setting, char *out, std::size_t size) const
{
    switch (setting) {
        case UiSetting::tilt:
            std::snprintf(out, size, "TILT FB: %s", kImuLevelNames[settings_.imu_level % 3U]);
            break;
        case UiSetting::reset_rotation:
            std::snprintf(out, size, "RESET ROTATION");
            break;
        case UiSetting::count:
            out[0] = '\0';
            break;
    }
}

// --- painting ---------------------------------------------------------------

void DeviceUi::paint_overlay(std::uint16_t *framebuffer) const
{
    fill_rows(framebuffer, kOverlayRowFirst, kOverlayRowLast, rgb565({0, 0, 0}));
    if (overlay_active_) {
        draw_text_line(framebuffer, overlay_line1_, kOverlayRowFirst + 6);
        if (overlay_line2_[0] != '\0') {
            draw_text_line(framebuffer, overlay_line2_, kOverlayRowFirst + 6 + 20);
        }
    }
}

void DeviceUi::paint_debug(std::uint16_t *framebuffer) const
{
    fill_rows(framebuffer, kDebugRowFirst, kDebugRowLast, rgb565({0, 0, 0}));
    if (debug_overlay_) {
        draw_text_line(framebuffer, debug_line_, kDebugRowFirst + 4);
    }
}

// Painted after the eye render every frame, so the text always wins the band.
void DeviceUi::paint_ui(std::uint16_t *framebuffer) const
{
    fill_rows(framebuffer, kUiRowFirst, kUiRowLast, rgb565({0, 0, 0}));
    if (page_ == UiPage::stats) {
        for (int row = 0; row < kStatsRowCount; ++row) {
            if (stats_lines_[row][0] != '\0') {
                draw_text_line(framebuffer, stats_lines_[row],
                               kUiRowFirst + 8 + row * kStatsRowPitch);
            }
        }
    }
}

void DeviceUi::paint(std::uint16_t *framebuffer)
{
#if CONFIG_LILGUY_SHOW_SELECTION_OVERLAY
    if (overlay_push()) {
        paint_overlay(framebuffer);
    }
#endif
    if (debug_push()) {
        paint_debug(framebuffer);
    }
    if (ui_push()) {
        paint_ui(framebuffer);
    }
}

void DeviceUi::union_push_rows(int &y0, int &y1) const
{
    const auto widen = [&y0, &y1](int first, int last) {
        y0 = y0 < first ? y0 : first;
        y1 = y1 > last ? y1 : last;
    };
    if (overlay_push()) {
        widen(kOverlayRowFirst, kOverlayRowLast);
    }
    if (debug_push()) {
        widen(kDebugRowFirst, kDebugRowLast);
    }
    if (ui_push()) {
        widen(kUiRowFirst, kUiRowLast);
    }
}

void DeviceUi::clear_dirty()
{
    overlay_dirty_ = false;
    debug_dirty_ = false;
    ui_dirty_ = false;
}

}  // namespace eyes
