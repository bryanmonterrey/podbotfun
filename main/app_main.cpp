#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "eyes/catalog.hpp"
#include "eyes/device_ui.hpp"
#include "eyes/eye_engine.hpp"
#include "eyes/eye_renderer.hpp"
#include "eyes/imu_service.hpp"
#include "eyes/os/onboarding.hpp"
#include "eyes/os/voice_session.hpp"
#include "eyes/preferences.hpp"

namespace {

constexpr char kTag[] = "lilguy";
constexpr gpio_num_t kBootButton = GPIO_NUM_0;
constexpr std::uint32_t kFocusFramePeriodMs = 16U;
constexpr std::uint32_t kBrowseFramePeriodMs = 16U;
constexpr std::uint32_t kSleepFramePeriodMs = 250U;
// 48kHz stereo, 20ms windows. The two MEMS mics sit ~30mm apart along the
// board's left edge, so the inter-channel cross-correlation lag (max ~87us =
// ~4.2 samples at 48kHz) gives a usable top-vs-bottom arrival direction.
constexpr std::uint32_t kAudioSampleRate = 48000U;
constexpr std::size_t kAudioFramesPerWindow = 960U;
constexpr int kAudioMaxLag = 6;                 // +-125us search, > physical max
constexpr float kAudioFullScaleLag = 4.2F;      // 30mm / 343m/s at 48kHz
constexpr std::uint64_t kAudioDirectionGate = 400000ULL;  // min energy for TDOA

std::uint32_t now_ms()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

lv_area_t transfer_area(eyes::PixelBounds bounds)
{
    if (!bounds.valid()) {
        return {0, 0, eyes::kScreenWidth - 1, eyes::kScreenHeight - 1};
    }
    // Meet the CO5300 even-start/odd-end bounds without joining adjacent dirty regions.
    int x0 = std::clamp(bounds.x0, 0, eyes::kScreenWidth - 1) & ~1;
    int y0 = std::clamp(bounds.y0, 0, eyes::kScreenHeight - 1) & ~1;
    int x1 = std::clamp(bounds.x1, 0, eyes::kScreenWidth - 1) | 1;
    int y1 = std::clamp(bounds.y1, 0, eyes::kScreenHeight - 1) | 1;
    x1 = std::min(x1, eyes::kScreenWidth - 1);
    y1 = std::min(y1, eyes::kScreenHeight - 1);
    return {x0, y0, x1, y1};
}

struct Application : eyes::DeviceUiHost {
    eyes::EyeEngine engine{0x73746172U};
    eyes::Preferences preferences{};
    eyes::ImuService imu{};
    eyes::EyeRenderer *renderer{nullptr};
    // Stats page, status overlay and the BOOT/PWR press gestures; shared
    // verbatim with the host simulator.
    // The voice stream's state machine. No transport is fitted on this board
    // yet (docs/VOICE_AGENT.md "The device"): the PWR key still opens it so
    // the listening face is real, and it lets go on its own.
    eyes::VoiceSession voice{};
    // First-boot onboarding: "hey" by hand, then a name and wake word. This
    // board has no voice transport and no pairing client yet, so it greets
    // once and hands the glass to the face; the stage persists in NVS and
    // the flow resumes when a path exists.
    eyes::Onboarding onboarding{};
    eyes::DeviceIdentity identity{};
    eyes::DeviceUi *ui{nullptr};
    QueueHandle_t imu_queue{nullptr};
    QueueHandle_t calibration_queue{nullptr};
    QueueHandle_t sound_queue{nullptr};
    esp_codec_dev_handle_t microphone{nullptr};
    std::uint16_t *framebuffer{nullptr};
    lv_display_t *display{nullptr};
    lv_obj_t *canvas{nullptr};
    std::uint32_t debug_fps{0};
    std::uint32_t last_render_us{0};
    std::uint32_t last_te_us{0};
    std::uint32_t last_blit_us{0};
    std::uint32_t pending_render_us{0};
    std::uint32_t stat_report_ms{0};
    std::uint32_t stat_frames{0};
    std::uint64_t stat_render_us{0};
    std::uint64_t stat_te_us{0};
    std::uint64_t stat_blit_us{0};
    std::uint32_t stat_render_max_us{0};
    std::uint32_t last_frame_ms{0};
    std::uint32_t last_render_ms{0};
    bool first_frame{true};
    bool power_poll_failed{false};
    bool dummy_draw{false};
    bool te_timeout_warned{false};
    // Two-finger rotate gesture (anchor orientation).
    bool rotate_active{false};
    bool pointer_blocked{false};
    float rotate_last_angle{0.0F};

    // --- eyes::DeviceUiHost: the three services the shared UI cannot itself do
    void save_settings(const eyes::Settings &settings) override
    {
        const esp_err_t result = preferences.save_settings(settings);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "Could not save settings: %s", esp_err_to_name(result));
        }
    }

    void request_imu_calibration() override { imu.request_calibration(); }

    void request_listen() override
    {
        voice.wake(now_ms());
        if (!voice.transport_available()) {
            ESP_LOGI(kTag, "listen: no voice transport on this board yet");
        }
    }

    eyes::BatteryStatus read_battery() override
    {
        std::uint16_t millivolts = 0;
        std::uint8_t percent = 0;
        bool charging = false;
        const esp_err_t result = bsp_battery_read(&millivolts, &percent, &charging);
        if (result == ESP_OK) {
            return {eyes::BatteryStatus::State::ok, millivolts, percent, charging};
        }
        eyes::BatteryStatus status{};
        status.state = result == ESP_ERR_NOT_FOUND ? eyes::BatteryStatus::State::absent
                                                   : eyes::BatteryStatus::State::error;
        return status;
    }
};

// TE vsync: the CO5300 raises GPIO13 at the start of each vertical blank. A band
// write started right after that edge outruns the scan beam, so it can never tear.
StaticSemaphore_t te_semaphore_storage;
SemaphoreHandle_t te_semaphore;

// TE line diagnostics (ANYEDGE): rise-to-rise = frame period, high time = porch.
volatile std::int64_t te_last_rise_us = 0;
volatile std::uint32_t te_period_us = 0;
volatile std::uint32_t te_high_us = 0;

void IRAM_ATTR te_isr(void *)
{
    const std::int64_t now = esp_timer_get_time();
    if (gpio_get_level(BSP_LCD_TE) != 0) {
        if (te_last_rise_us != 0) {
            te_period_us = static_cast<std::uint32_t>(now - te_last_rise_us);
        }
        te_last_rise_us = now;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(te_semaphore, &woken);
        portYIELD_FROM_ISR(woken);
    } else if (te_last_rise_us != 0) {
        te_high_us = static_cast<std::uint32_t>(now - te_last_rise_us);
    }
}

esp_err_t install_te_vsync()
{
    te_semaphore = xSemaphoreCreateBinaryStatic(&te_semaphore_storage);
    const gpio_config_t te_config{
        .pin_bit_mask = 1ULL << static_cast<unsigned>(BSP_LCD_TE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&te_config), kTag, "TE gpio_config");
    const esp_err_t isr_service = gpio_install_isr_service(0);
    if (isr_service != ESP_OK && isr_service != ESP_ERR_INVALID_STATE) {
        return isr_service;
    }
    return gpio_isr_handler_add(BSP_LCD_TE, te_isr, nullptr);
}

void wait_for_vsync(Application &app)
{
    xSemaphoreTake(te_semaphore, 0);  // drop a stale edge; wait for a fresh one
    if (xSemaphoreTake(te_semaphore, pdMS_TO_TICKS(40)) != pdTRUE && !app.te_timeout_warned) {
        ESP_LOGW(kTag, "TE edge missing; writing unsynced");
        app.te_timeout_warned = true;
    }
}

// The SPI driver bounce-copies PSRAM buffers into freshly heap-allocated internal
// DMA memory for every chunk; that churn fragments the heap until allocations fail
// and transfers truncate mid-band. Instead, slice the PSRAM framebuffer through
// four preallocated internal DMA buffers: zero runtime allocations, DMA always
// reads internal RAM. Four buffers because up to three transactions are in flight
// (trans_queue_depth), so slot i-4 is guaranteed recycled before reuse.
constexpr int kSliceRows = 16;
constexpr int kSliceBuffers = 4;
std::uint16_t *slice_buffers[kSliceBuffers];

bool allocate_slice_buffers()
{
    for (int index = 0; index < kSliceBuffers; ++index) {
        slice_buffers[index] = static_cast<std::uint16_t *>(heap_caps_aligned_alloc(
            64U, static_cast<std::size_t>(kSliceRows) * eyes::kScreenWidth * sizeof(std::uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (slice_buffers[index] == nullptr) {
            return false;
        }
    }
    return true;
}

esp_err_t blit_band(Application &app, int y0, int y1)
{
    int slice = 0;
    for (int y = y0; y <= y1; y += kSliceRows, ++slice) {
        const int rows = std::min(kSliceRows, y1 - y + 1);
        std::uint16_t *bounce = slice_buffers[slice % kSliceBuffers];
        std::memcpy(bounce,
                    app.framebuffer + static_cast<std::size_t>(y) * eyes::kScreenWidth,
                    static_cast<std::size_t>(rows) * eyes::kScreenWidth * sizeof(std::uint16_t));
        const bool last = y + rows > y1;
        const esp_err_t result = esp_lv_adapter_dummy_draw_blit(
            app.display, 0, y, eyes::kScreenWidth, y + rows, bounce, last);
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

// Measured on this panel: TE rises at vblank start, beam scans ~628us later,
// one row every ~35us (16.9ms frame). QSPI writes a row in ~23us plus feed
// overhead; call it 26us.
constexpr std::int64_t kTeBlankUs = 628;
constexpr std::int64_t kBeamRowUs = 35;
constexpr std::int64_t kWriteRowUs = 26;

std::int64_t read_te_rise()
{
    std::int64_t value = te_last_rise_us;
    std::int64_t check = te_last_rise_us;
    while (value != check) {  // volatile int64 on a 32-bit core can tear
        value = te_last_rise_us;
        check = te_last_rise_us;
    }
    return value;
}

void delay_us_cooperative(std::int64_t amount)
{
    if (amount > 2000) {
        vTaskDelay(pdMS_TO_TICKS(amount / 1000));
    } else if (amount > 0) {
        esp_rom_delay_us(static_cast<std::uint32_t>(amount));
    }
}

// Start the band write so the beam can never meet it. The TE ISR tracks beam
// phase continuously, so start immediately whenever we are inside the safe
// window: beam already past the band bottom AND enough time before it wraps
// back into the band top. Tall bands have no such window; they start at vblank
// and rely on the write (~43 rows/ms) outrunning the beam (~29 rows/ms).
void wait_for_beam_clear(Application &app, int y0, int y1)
{
    const std::int64_t write_us =
        static_cast<std::int64_t>(y1 - y0 + 1) * kWriteRowUs + 500;
    for (int guard = 0; guard < 6; ++guard) {
        const std::int64_t rise = read_te_rise();
        if (rise == 0) {
            wait_for_vsync(app);
            return;
        }
        const std::int64_t period = te_period_us != 0U ? te_period_us : 16880;
        const std::int64_t phase = esp_timer_get_time() - rise;
        if (phase > 3 * period) {
            if (!app.te_timeout_warned) {
                ESP_LOGW(kTag, "TE edge missing; writing unsynced");
                app.te_timeout_warned = true;
            }
            return;
        }
        // Early window: the beam is still ABOVE the band top. The write starts at
        // y0 with a head start and moves faster than the beam (~26 vs ~35 us/row),
        // so the beam can never catch it — start immediately.
        const std::int64_t early_close_us =
            kTeBlankUs + static_cast<std::int64_t>(y0 - 6) * kBeamRowUs;
        if (phase <= early_close_us) {
            return;
        }
        // Late window: the beam has passed the band bottom, and the write finishes
        // before it wraps back into the band top.
        const std::int64_t open_us =
            kTeBlankUs + static_cast<std::int64_t>(y1 + 6) * kBeamRowUs;
        const std::int64_t close_us = period + kTeBlankUs +
                                      static_cast<std::int64_t>(y0 - 6) * kBeamRowUs -
                                      write_us;
        if (phase >= open_us && phase <= std::max(close_us, open_us)) {
            return;
        }
        if (close_us <= open_us && phase > open_us) {
            wait_for_vsync(app);  // tall band, beam mid-band: restart at vblank
            return;
        }
        delay_us_cooperative(phase < open_us ? open_us - phase : period - phase + early_close_us);
    }
}

void audio_capture_task(void *context)
{
    auto *app = static_cast<Application *>(context);
    std::int16_t samples[kAudioFramesPerWindow * 2U]{};
    std::uint32_t windows = 0U;
    std::uint32_t max_feature_us = 0U;
    std::uint32_t report_ms = now_ms();

    while (true) {
        const int result = esp_codec_dev_read(app->microphone, samples, sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(kTag, "Audio read failed: %d", result);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const std::int64_t feature_start_us = esp_timer_get_time();
        std::uint64_t mic1 = 0U;
        std::uint64_t mic2 = 0U;
        for (std::size_t frame = 0; frame < kAudioFramesPerWindow; ++frame) {
            const std::int64_t first = samples[frame * 2U];
            const std::int64_t second = samples[frame * 2U + 1U];
            mic1 += static_cast<std::uint64_t>(first * first);
            mic2 += static_cast<std::uint64_t>(second * second);
        }
        // TDOA on loud windows: cross-correlate the channels over small lags,
        // refine the peak parabolically to sub-sample precision.
        float direction = 0.0F;
        bool direction_valid = false;
        if (mic1 + mic2 >= kAudioDirectionGate) {
            std::int64_t correlation[2 * kAudioMaxLag + 1];
            for (int lag = -kAudioMaxLag; lag <= kAudioMaxLag; ++lag) {
                std::int64_t sum = 0;
                // frame in [maxLag, N-maxLag) keeps frame+lag in bounds for all lags
                for (int frame = kAudioMaxLag;
                     frame < static_cast<int>(kAudioFramesPerWindow) - kAudioMaxLag; ++frame) {
                    sum += static_cast<std::int64_t>(samples[frame * 2]) *
                           samples[(frame + lag) * 2 + 1];
                }
                correlation[lag + kAudioMaxLag] = sum;
            }
            int peak = 0;
            for (int index = 1; index < 2 * kAudioMaxLag + 1; ++index) {
                if (correlation[index] > correlation[peak]) {
                    peak = index;
                }
            }
            float refined = static_cast<float>(peak - kAudioMaxLag);
            if (peak > 0 && peak < 2 * kAudioMaxLag) {
                const float left = static_cast<float>(correlation[peak - 1]);
                const float mid = static_cast<float>(correlation[peak]);
                const float right = static_cast<float>(correlation[peak + 1]);
                const float denominator = left - 2.0F * mid + right;
                if (denominator < -1.0F) {
                    refined += 0.5F * (left - right) / denominator;
                }
            }
            direction = std::clamp(refined / kAudioFullScaleLag, -1.0F, 1.0F);
            direction_valid = true;
        }
        const std::uint32_t feature_us = static_cast<std::uint32_t>(
            esp_timer_get_time() - feature_start_us);
        const eyes::SoundPacket packet{
            .energy = mic1 + mic2,
            .timestamp_ms = now_ms(),
            .feature_us = feature_us,
            .direction = direction,
            .direction_valid = direction_valid,
        };
        xQueueOverwrite(app->sound_queue, &packet);
        ++windows;
        max_feature_us = std::max(max_feature_us, feature_us);
        const std::uint32_t current_ms = packet.timestamp_ms;
        if (current_ms - report_ms >= 1000U) {
            ESP_LOGI(kTag, "Audio windows=%u feature_max=%uus",
                     static_cast<unsigned>(windows), static_cast<unsigned>(max_feature_us));
            windows = 0U;
            max_feature_us = 0U;
            report_ms = current_ms;
        }
    }
}

bool initialize_audio(Application &app)
{
    const i2s_std_config_t config{
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kAudioSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {},
        },
    };
    const esp_err_t audio_result = bsp_audio_init(&config);
    if (audio_result != ESP_OK) {
        ESP_LOGE(kTag, "Could not open 16 kHz stereo I2S: %s", esp_err_to_name(audio_result));
        return false;
    }
    app.microphone = bsp_audio_codec_microphone_init();
    if (app.microphone == nullptr) {
        ESP_LOGE(kTag, "Could not initialize ES7210 microphones");
        return false;
    }
    esp_codec_dev_sample_info_t format{
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = kAudioSampleRate,
        .mclk_multiple = 0,
    };
    const int open_result = esp_codec_dev_open(app.microphone, &format);
    if (open_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "Could not open ES7210 stereo input: %d", open_result);
        return false;
    }
    if (xTaskCreate(audio_capture_task, "audio_capture", 8192U, &app,
                    tskIDLE_PRIORITY + 1U, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Could not create audio capture task");
        esp_codec_dev_close(app.microphone);
        return false;
    }
    return true;
}

void touch_event(lv_event_t *event)
{
    auto *app = static_cast<Application *>(lv_event_get_user_data(event));
    lv_indev_t *input = lv_indev_active();
    if (app == nullptr || input == nullptr) {
        return;
    }
    lv_point_t point{};
    lv_indev_get_point(input, &point);
    const std::uint32_t timestamp = now_ms();
    const lv_event_code_t code = lv_event_get_code(event);
    // Two-finger rotate owns the pointer: swallow single-point events until
    // the gesture (and the trailing finger) is fully released.
    if (app->rotate_active || app->pointer_blocked) {
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            app->pointer_blocked = false;
        }
        return;
    }
    using Phase = eyes::DeviceUi::TouchPhase;
    Phase phase = Phase::other;
    switch (code) {
        case LV_EVENT_PRESSED:
            phase = Phase::pressed;
            break;
        case LV_EVENT_PRESSING:
            phase = Phase::pressing;
            break;
        case LV_EVENT_RELEASED:
            phase = Phase::released;
            break;
        case LV_EVENT_PRESS_LOST:
            phase = Phase::press_lost;
            break;
        default:
            break;
    }
    // Touch is petting: every event goes to the engine. No UI page takes taps.
    if (phase == Phase::pressed || phase == Phase::released) {
        // Diagnostic: attribute gaze jerks — phantom touches show up here with
        // coordinates while nobody touches the panel (CST9217 throws I2C errors).
        ESP_LOGI(kTag, "touch %s x=%d y=%d", phase == Phase::pressed ? "down" : "up",
                 static_cast<int>(point.x), static_cast<int>(point.y));
    }
    switch (phase) {
        case Phase::pressed:
            app->engine.pointer_down(static_cast<float>(point.x), static_cast<float>(point.y), timestamp);
            break;
        case Phase::pressing:
            app->engine.pointer_move(static_cast<float>(point.x), static_cast<float>(point.y), timestamp);
            break;
        case Phase::released:
            app->engine.pointer_up(static_cast<float>(point.x), static_cast<float>(point.y), timestamp);
            break;
        case Phase::press_lost:
            app->engine.pointer_cancel();
            break;
        case Phase::other:
            break;
    }
}

void frame_timer(lv_timer_t *timer)
{
    auto *app = static_cast<Application *>(lv_timer_get_user_data(timer));
    const std::uint32_t current_ms = now_ms();
    const std::uint32_t elapsed_ms = std::clamp<std::uint32_t>(current_ms - app->last_frame_ms, 1U, 100U);
    app->last_frame_ms = current_ms;

    // Two-finger rotate: read the multi-point snapshot the adapter's IRQ-gated
    // poll refreshed within this same LVGL pass. get_coordinates only copies
    // the snapshot (no read_data), so this adds zero I2C traffic and cannot
    // fight the adapter's single-point indev.
    esp_lcd_touch_handle_t touch = bsp_touch_get_handle();
    if (touch != nullptr) {
        std::uint16_t touch_x[2] = {0, 0};
        std::uint16_t touch_y[2] = {0, 0};
        std::uint8_t touch_points = 0;
        const bool touched =
            esp_lcd_touch_get_coordinates(touch, touch_x, touch_y, nullptr, &touch_points, 2);
        if (touched && touch_points >= 2U) {
            const float angle =
                std::atan2(static_cast<float>(touch_y[1]) - static_cast<float>(touch_y[0]),
                           static_cast<float>(touch_x[1]) - static_cast<float>(touch_x[0]));
            if (!app->rotate_active) {
                app->rotate_active = true;
                app->pointer_blocked = true;
                app->engine.pointer_cancel();  // drop the in-flight single-finger touch
            } else {
                float delta = angle - app->rotate_last_angle;
                // The driver reports no track IDs, so treat the finger pair as
                // an undirected line (period pi): an index swap between frames
                // becomes invisible, and nobody rotates 90 deg in one frame.
                while (delta > 1.5707963F) {
                    delta -= 3.1415927F;
                }
                while (delta < -1.5707963F) {
                    delta += 3.1415927F;
                }
                // Live during the gesture; the engine wraps into [-pi, pi).
                app->engine.set_orientation(app->engine.orientation() + delta);
                app->ui->note_orientation(app->engine.orientation());
            }
            app->rotate_last_angle = angle;
        } else if (app->rotate_active) {
            app->rotate_active = false;
            app->ui->save_settings();  // persist the anchor at gesture end
        }
    }

    eyes::ImuPacket packet{};
    if (xQueueReceive(app->imu_queue, &packet, 0) == pdTRUE) {
        app->engine.motion_sample(packet.sample);
    }
    eyes::SoundPacket sound{};
    if (xQueueReceive(app->sound_queue, &sound, 0) == pdTRUE) {
        app->ui->set_last_sound(sound);
        app->engine.sound_sample(sound, current_ms);
    }
    // Menu idle close, stats/debug text refresh, selection-overlay expiry.
    app->ui->set_perf({app->debug_fps, app->last_render_us, app->last_te_us, app->last_blit_us});
    app->ui->tick(current_ms);
    app->voice.tick(current_ms);
    app->voice.apply_to(app->engine);
    // Diagnostic: log every attention-source and mode transition with gaze so
    // "jerk and return" events can be attributed (flourish/sound/tilt/ghost touch).
    {
        static eyes::AttentionSource last_attention = eyes::AttentionSource::idle;
        static eyes::InteractionMode last_mode = eyes::InteractionMode::idle;
        static bool last_rot = false;
        const eyes::FrameState &probe = app->engine.frame();
        if (probe.attention != last_attention || probe.mode != last_mode ||
            probe.rot_active != last_rot) {
            ESP_LOGI(kTag, "state attn=%d mode=%d rot=%d gaze=%.2f,%.2f",
                     static_cast<int>(probe.attention), static_cast<int>(probe.mode),
                     probe.rot_active ? 1 : 0, static_cast<double>(probe.gaze.x),
                     static_cast<double>(probe.gaze.y));
            last_attention = probe.attention;
            last_mode = probe.mode;
            last_rot = probe.rot_active;
        }
    }
    eyes::Calibration completed_calibration{};
    if (xQueueReceive(app->calibration_queue, &completed_calibration, 0) == pdTRUE) {
        const esp_err_t save_result = app->preferences.save_calibration(completed_calibration);
        if (save_result != ESP_OK) {
            ESP_LOGW(kTag, "Could not save IMU calibration: %s", esp_err_to_name(save_result));
        }
        app->ui->show_selection(current_ms, "IMU CALIBRATED");
    }
    if (bsp_touch_take_palm_event() &&
        app->engine.frame().mode != eyes::InteractionMode::sleeping) {
        ESP_LOGI(kTag, "Palm cover detected; sleeping");
        app->engine.sleep();
    }

    app->engine.update(elapsed_ms);
    const eyes::FrameState &frame = app->engine.frame();
    const bool sleeping = frame.mode == eyes::InteractionMode::sleeping;
    const bool browsing = frame.mode == eyes::InteractionMode::dragging ||
                          frame.mode == eyes::InteractionMode::settling;
    // Capsule mode keeps the full tick even while sleeping: its breath/sway are
    // continuous (4Hz steps read as chop) and its settle gate already skips
    // identical frames, so the idle cost stays near zero.
    // Onboarding owns the glass while it runs: the surface draws into the
    // framebuffer incrementally and only its rows go to the panel. The eyes
    // resume with a full repaint when it dismisses.
    if (app->onboarding.active()) {
        const eyes::PixelBounds bounds = app->onboarding.render(app->framebuffer, current_ms);
        eyes::DeviceIdentity identity{};
        if (app->onboarding.consume_save(identity)) {
            app->identity = identity;
            const esp_err_t save_result = app->preferences.save_identity(identity);
            if (save_result != ESP_OK) {
                ESP_LOGW(kTag, "Could not save identity: %s", esp_err_to_name(save_result));
            }
        }
        bool completed = false;
        if (app->onboarding.consume_finished(completed)) {
            app->renderer->invalidate();
            app->first_frame = true;
            app->engine.request_blink();
            if (completed) {
                app->ui->show_selection(current_ms, "READY");
            }
            ESP_LOGI(kTag, "onboarding %s (stage %d)", completed ? "complete" : "greeted",
                     static_cast<int>(app->identity.stage));
        }
        if (bounds.valid()) {
            if (app->dummy_draw) {
                const int y0 = std::max(0, bounds.y0 & ~1);
                const int y1 = std::min(eyes::kScreenHeight - 1, bounds.y1 | 1);
                wait_for_beam_clear(*app, y0, y1);
                const esp_err_t blit_result = blit_band(*app, y0, y1);
                if (blit_result != ESP_OK) {
                    ESP_LOGW(kTag, "onboarding blit failed: %s", esp_err_to_name(blit_result));
                }
            } else {
                const lv_area_t dirty = transfer_area(bounds);
                lv_obj_invalidate_area(app->canvas, &dirty);
            }
        }
        app->last_render_ms = current_ms;
        if (app->onboarding.active()) {
            return;
        }
    }
    const bool capsule_mode = app->renderer->face_mode() == eyes::FaceMode::capsule;
    const std::uint32_t render_period_ms =
        (sleeping && !capsule_mode) ? kSleepFramePeriodMs
                                    : browsing ? kBrowseFramePeriodMs : kFocusFramePeriodMs;
    if (app->first_frame || current_ms - app->last_render_ms >= render_period_ms) {
        const std::int64_t render_start_us = esp_timer_get_time();
        app->renderer->render(frame);
        app->pending_render_us =
            static_cast<std::uint32_t>(esp_timer_get_time() - render_start_us);
        app->last_render_ms = current_ms;
        const eyes::PixelBounds bounds = app->renderer->rendered_bounds();
        // Selection overlay, debug line and menu/stats rows are repainted
        // after the eyes every frame; grid painting may cross the text rows, so
        // the text always goes on top. Their bands are unioned into the
        // transfer below.
        const bool text_push = app->ui->any_push();
        app->ui->paint(app->framebuffer);
        if (app->dummy_draw) {
            // Direct path: one continuous full-width band, gated on the TE edge.
            if (text_push) {
                // Text bands punch black holes into the capsule disc; make the
                // next capsule frame repaint it fully (no-op in creature mode).
                app->renderer->invalidate();
            }
            if (app->first_frame || bounds.valid() || text_push) {
                int y0 = eyes::kScreenHeight - 1;
                int y1 = 0;
                if (app->first_frame) {
                    y0 = 0;
                    y1 = eyes::kScreenHeight - 1;
                } else {
                    if (bounds.valid()) {
                        y0 = std::max(0, bounds.y0 & ~1);
                        y1 = std::min(eyes::kScreenHeight - 1, bounds.y1 | 1);
                    }
                    app->ui->union_push_rows(y0, y1);
                }
                app->ui->clear_dirty();
                const std::int64_t blit_wait_start_us = esp_timer_get_time();
                wait_for_beam_clear(*app, y0, y1);
                const std::int64_t blit_start_us = esp_timer_get_time();
                const esp_err_t blit_result = blit_band(*app, y0, y1);
                const std::int64_t blit_end_us = esp_timer_get_time();
                if (blit_result != ESP_OK) {
                    ESP_LOGW(kTag, "Band blit failed: %s", esp_err_to_name(blit_result));
                    app->first_frame = true;  // window pointer desynced: repaint everything
                }
                const std::uint32_t render_us = app->pending_render_us;
                app->last_render_us = render_us;
                app->last_te_us = static_cast<std::uint32_t>(blit_start_us - blit_wait_start_us);
                app->last_blit_us = static_cast<std::uint32_t>(blit_end_us - blit_start_us);
                ++app->stat_frames;
                app->stat_render_us += render_us;
                app->stat_render_max_us = std::max(app->stat_render_max_us, render_us);
                app->stat_te_us += static_cast<std::uint64_t>(blit_start_us - blit_wait_start_us);
                app->stat_blit_us += static_cast<std::uint64_t>(blit_end_us - blit_start_us);
                if (current_ms - app->stat_report_ms >= 2000U) {
                    if (app->stat_report_ms != 0U && app->stat_frames > 0U) {
                        const std::uint32_t frames = app->stat_frames;
                        app->debug_fps = frames * 1000U / (current_ms - app->stat_report_ms);
                        ESP_LOGI(kTag,
                                 "fps=%u render=%u/%uus te_wait=%uus blit=%uus te_period=%uus te_high=%uus clear=%u paths=%u mask=%u",
                                 static_cast<unsigned>(frames * 1000U /
                                                       (current_ms - app->stat_report_ms)),
                                 static_cast<unsigned>(app->stat_render_us / frames),
                                 static_cast<unsigned>(app->stat_render_max_us),
                                 static_cast<unsigned>(app->stat_te_us / frames),
                                 static_cast<unsigned>(app->stat_blit_us / frames),
                                 static_cast<unsigned>(te_period_us),
                                 static_cast<unsigned>(te_high_us),
                                 static_cast<unsigned>(app->renderer->phase_us[0]),
                                 static_cast<unsigned>(app->renderer->phase_us[1]),
                                 static_cast<unsigned>(app->renderer->phase_us[2]));
                    }
                    app->stat_report_ms = current_ms;
                    app->stat_frames = 0U;
                    app->stat_render_us = 0U;
                    app->stat_te_us = 0U;
                    app->stat_blit_us = 0U;
                    app->stat_render_max_us = 0U;
                }
            }
        } else {
            // Overlay fallback: LVGL composites canvas + label through the adapter.
            if (app->first_frame) {
                const lv_area_t dirty{0, 0, eyes::kScreenWidth - 1, eyes::kScreenHeight - 1};
                lv_obj_invalidate_area(app->canvas, &dirty);
            } else if (bounds.valid()) {
                const lv_area_t dirty = transfer_area(bounds);
                lv_obj_invalidate_area(app->canvas, &dirty);
            }
        }
        app->first_frame = false;
    }
}

// BOOT: single = reserved, double = stats page, 850ms-3s hold = IMU
// calibrate, >=3s hold = debug overlay line.
void button_timer(lv_timer_t *timer)
{
    auto *app = static_cast<Application *>(lv_timer_get_user_data(timer));
    app->ui->poll_boot_button(gpio_get_level(kBootButton) == 0, now_ms());
}

// PWR: single = listen (open the voice stream), double = sleep/wake toggle,
// hold (the AXP2101's long-press IRQ, ~1.5 s) = power off. The PMIC's own
// native power-off needs a ~6 s hold, which read as "the off button doesn't
// work" on the first board (owner, 2026-10-04); the firmware now takes the
// PMIC's long-press event and cuts the rails itself. A short press powers on.
void power_button_timer(lv_timer_t *timer)
{
    auto *app = static_cast<Application *>(lv_timer_get_user_data(timer));
    bsp_power_button_event_t event{};
    const esp_err_t result = bsp_power_button_poll(&event);
    if (result != ESP_OK) {
        if (!app->power_poll_failed) {
            ESP_LOGW(kTag, "Could not poll AXP2101 power key: %s", esp_err_to_name(result));
            app->power_poll_failed = true;
        }
        app->ui->poll_power_button(false, now_ms());
        return;
    }
    app->power_poll_failed = false;
    if (event.long_press) {
        ESP_LOGI(kTag, "PWR held: powering off");
        const esp_err_t off = bsp_power_off();
        if (off != ESP_OK) {
            ESP_LOGW(kTag, "Power off failed: %s", esp_err_to_name(off));
        }
        return;
    }
    app->ui->poll_power_button(event.short_press, now_ms());
}

bool initialize_display(Application &app)
{
    bsp_display_cfg_t display_config{};
    display_config.lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    display_config.lv_adapter_cfg.task_core_id = 0;  // IMU owns core 1
    display_config.rotation = ESP_LV_ADAPTER_ROTATE_0;
    // NONE releases GPIO13: the app owns the TE edge and gates its own band blits.
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.touch_flags.mirror_x = 1;
    display_config.touch_flags.mirror_y = 1;
    app.display = bsp_display_start_with_config(&display_config);
    if (app.display == nullptr) {
        ESP_LOGE(kTag, "Display initialization failed");
        return false;
    }
    // The vendored BSP starts dark; this is a defensive repeat for future BSP changes.
    // Panel IO is not thread-safe: hold the LVGL lock so the brightness command never
    // interleaves with an in-flight LVGL flush.
    ESP_ERROR_CHECK(bsp_display_lock(-1));
    ESP_ERROR_CHECK(bsp_display_backlight_off());
    bsp_display_unlock();
    constexpr std::size_t framebuffer_bytes =
        static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight * sizeof(std::uint16_t);
    app.framebuffer = static_cast<std::uint16_t *>(heap_caps_aligned_alloc(
        64U, framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (app.framebuffer == nullptr) {
        ESP_LOGE(kTag, "Could not allocate the %u-byte RGB565 framebuffer in PSRAM",
                 static_cast<unsigned>(framebuffer_bytes));
        return false;
    }
    app.renderer = new (std::nothrow) eyes::EyeRenderer(
        {app.framebuffer, eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth},
#if CONFIG_LILGUY_ROUND_VIEWPORT
        true
#else
        false
#endif
    );
    if (app.renderer == nullptr) {
        ESP_LOGE(kTag, "Could not allocate the eye renderer");
        return false;
    }
    app.ui = new (std::nothrow) eyes::DeviceUi(app.engine, *app.renderer, app,
                                              app.preferences.load_settings());
    if (app.ui == nullptr) {
        ESP_LOGE(kTag, "Could not allocate the device UI");
        return false;
    }
    // Stored tilt sensitivity, face rotation, face mode and ink into the
    // engine and renderer.
    app.ui->apply_settings();
    ESP_LOGI(kTag, "init: first render");
    app.renderer->render(app.engine.frame());
    ESP_LOGI(kTag, "init: first render done, locking display");

    ESP_ERROR_CHECK(bsp_display_lock(-1));
    ESP_LOGI(kTag, "init: display locked");
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);
    app.canvas = lv_canvas_create(lv_screen_active());
    // The rasterizer stores panel byte order (EYES_RGB565_SWAPPED), so the canvas
    // must declare SWAPPED for the LVGL overlay fallback path to composite correctly.
    lv_canvas_set_buffer(app.canvas, app.framebuffer, eyes::kScreenWidth, eyes::kScreenHeight,
                         LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_obj_set_pos(app.canvas, 0, 0);
    lv_obj_add_flag(app.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(app.canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(app.canvas, touch_event, LV_EVENT_ALL, &app);

    bsp_display_unlock();

    if (install_te_vsync() != ESP_OK) {
        ESP_LOGW(kTag, "TE vsync unavailable; band writes run unsynced");
    }
    if (!allocate_slice_buffers()) {
        ESP_LOGE(kTag, "Could not allocate internal DMA slice buffers");
        return false;
    }
    ESP_LOGI(kTag, "init: enabling dummy draw");
    ESP_ERROR_CHECK(esp_lv_adapter_set_dummy_draw(app.display, true));
    app.dummy_draw = true;
    ESP_LOGI(kTag, "init: dummy draw on, waiting TE");
    // Paint the whole panel once before turning emission on. Locked: panel IO is
    // not thread-safe against the LVGL task.
    ESP_ERROR_CHECK(bsp_display_lock(-1));
    wait_for_vsync(app);
    ESP_LOGI(kTag, "init: TE ok, first blit");
    const esp_err_t first_blit = blit_band(app, 0, eyes::kScreenHeight - 1);
    ESP_LOGI(kTag, "init: first blit done: %s", esp_err_to_name(first_blit));
    if (first_blit == ESP_OK) {
        app.first_frame = false;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    bsp_display_unlock();
    ESP_LOGI(kTag, "init: backlight on");
    return true;
}

}  // namespace

extern "C" void app_main(void)
{
    static Application app;
    ESP_ERROR_CHECK(app.preferences.init());
    // The look is fixed: shape 1 ("circle") x palette 35 ("og") = white pill
    // eyes on black under the capsule renderer. NVS is not consulted, so a
    // look saved by an older build cannot bring switching back by accident.
    app.engine.set_selection({1, 35});
    app.identity = app.preferences.load_identity();

    // Builds the renderer and the DeviceUi, and applies the stored settings.
    if (!initialize_display(app)) {
        return;
    }

    app.imu_queue = xQueueCreate(1, sizeof(eyes::ImuPacket));
    app.calibration_queue = xQueueCreate(1, sizeof(eyes::Calibration));
    app.sound_queue = xQueueCreate(1, sizeof(eyes::SoundPacket));
    if (app.imu_queue == nullptr || app.calibration_queue == nullptr || app.sound_queue == nullptr) {
        ESP_LOGE(kTag, "Could not create sensor mailboxes");
        return;
    }
    if (!initialize_audio(app)) {
        ESP_LOGW(kTag, "Sound attention disabled");
    }
    const eyes::Calibration calibration = app.preferences.load_calibration();
    if (!app.imu.start(app.imu_queue, app.calibration_queue, calibration,
#if CONFIG_LILGUY_IMU_AUTOCALIBRATE
                       true
#else
                       false
#endif
                       )) {
        ESP_LOGW(kTag, "IMU task did not start; autonomous and touch gaze remain available");
    }

    const gpio_config_t button_config{
        .pin_bit_mask = 1ULL << static_cast<unsigned>(kBootButton),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
    const esp_err_t power_button_result = bsp_power_button_init();
    if (power_button_result != ESP_OK) {
        ESP_LOGW(kTag, "PWR input disabled: %s", esp_err_to_name(power_button_result));
    }

    ESP_ERROR_CHECK(bsp_display_lock(-1));
    app.last_frame_ms = now_ms();
    app.last_render_ms = app.last_frame_ms;
    const std::uint32_t frame_period_ms =
        std::max<std::uint32_t>(1U, 1000U / static_cast<std::uint32_t>(CONFIG_LILGUY_FRAME_RATE));
    // Nobody can answer on this board yet (no mic transport, no pairing
    // client): a fresh device writes "hey" once, then shows the face.
    app.onboarding.begin(app.identity, eyes::OnboardingPaths{false}, now_ms());
    lv_timer_create(frame_timer, frame_period_ms, &app);
    lv_timer_create(button_timer, 20U, &app);
    if (power_button_result == ESP_OK) {
        lv_timer_create(power_button_timer, 80U, &app);
    }
    app.ui->show_selection(now_ms());
    bsp_display_unlock();

    ESP_LOGI(kTag, "Started %ux%u cubic eye rig with %u looks",
             eyes::kScreenWidth, eyes::kScreenHeight, static_cast<unsigned>(eyes::kLookCount));
}
