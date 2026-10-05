// Desktop simulator: the real EyeEngine, EyeRenderer and DeviceUi in an SDL2
// window, with a control panel whose buttons stand in for the board's physical
// inputs. Only the hardware layer is faked, so the menu, the press gestures and
// every reaction are the firmware's own code paths.
//
// The device screen is on the left; clicking it is a finger on the panel. When
// the options menu is up, clicks land on its real tap rows.
//
// Panel controls are labelled on screen. Keyboard equivalents:
//   Left mouse (screen)  touch: click, drag, release
//   Right mouse (screen) tilt the board; the tilt holds until FLAT
//   B / N (hold)         BOOT / PWR
//   Arrows               palette (left/right), shape (up/down)
//   R C V                random look, face mode, capsule ink
//   S P X                sleep-wake, palm cover, shake
//   Space (hold)         loud sound into the mics
//   1 2 3                tilt sensitivity LOW / MED / HIGH
//   Q E 0                rotate the anchored face -15 / +15 / reset
//   W                    webcam viewfinder through CameraService (macOS)
//   M                    now-playing surface (real Spotify / Apple Music state)
//   A (hold)             push-to-talk to the agent harness; reply is spoken (macOS)
//   consent cards are answered by VOICE: hold A, say the passphrase (or "cancel")
//   Scroll wheel / J     digital crown: rotate to browse looks, press (J) for random
//   T                    cycle the transition style: zoom / eyes / iris / slide / fade
//   Y                    blink-to-shutter (Vision landmarks; blink to snap a photo)
//   Click the red circle take a photo -> host/photo_NN.ppm
//   G Tab Esc            capture a PPM, hide the panel, quit
//
// Headless, for CI or a machine with no display:
//   ./host/eyes_simulator --headless --seconds 6 --out /tmp/sim
//   ./host/eyes_simulator --headless --composite --seconds 1 --out /tmp/panel

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "eyes/catalog.hpp"
#include "eyes/device_ui.hpp"
#include "eyes/eye_renderer.hpp"
#include "eyes/font8x8.hpp"
#include "eyes/font_geist.hpp"
#include "eyes/os/app.hpp"
#include "eyes/os/services.hpp"
#include "eyes/geist_text.hpp"
#include "eyes/os/onboarding.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifndef EYES_SIM_HEADLESS_ONLY
#include <SDL.h>
#include <SDL_syswm.h>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "eyes/os/agent_link.hpp"
#include "eyes/os/cast_session.hpp"
#include "eyes/audio_bars.hpp"
#include "eyes/status_glyphs.hpp"
#endif

#ifdef EYES_SIM_MAC
// host/mac_window.mm
extern "C" void eyes_mac_prepare_window(void *handle);
extern "C" void eyes_mac_set_click_through(void *handle, int enabled);
// host/mac_camera.mm
extern "C" int eyes_mac_camera_start(void);
extern "C" void eyes_mac_camera_stop(void);
extern "C" int eyes_mac_camera_latest(unsigned char *dst, int dst_capacity, int *width,
                                      int *height);
extern "C" int eyes_mac_camera_take_blinks(void);
// host/mac_speech.mm
extern "C" int eyes_mac_speech_begin(void);
extern "C" void eyes_mac_speech_end(void);
extern "C" int eyes_mac_speech_take_transcript(char *out, int capacity);
extern "C" int eyes_mac_wake_enable(const char *wake_word);
extern "C" void eyes_mac_wake_disable(void);
extern "C" int eyes_mac_wake_take_command(char *out, int capacity);
extern "C" int eyes_mac_wake_is_awake(void);
#endif

namespace {

// Device frame pacing (main/app_main.cpp): focus and browse both tick at 16 ms,
// sleep drops to 250 ms unless the capsule face is up (its breath is continuous).
constexpr std::uint32_t kFocusFramePeriodMs = 16U;
constexpr std::uint32_t kBrowseFramePeriodMs = 16U;
constexpr std::uint32_t kSleepFramePeriodMs = 250U;
constexpr float kGravity = 9.80665F;
constexpr float kPi = 3.14159265F;
// The device polls BOOT every 20 ms and PWR every 80 ms.
constexpr std::uint32_t kBootPollMs = 20U;
constexpr std::uint32_t kPowerPollMs = 80U;
// The IMU task takes about this long to finish a calibration and report back.
constexpr std::uint32_t kCalibrationMs = 1500U;

// Mic window cadence and levels. The engine averages its first 50 windows into
// a noise floor and opens the gate on 3 consecutive windows at >= 4x floor, so
// the quiet baseline must be non-zero and the loud level well above it.
constexpr std::uint32_t kSoundWindowMs = 32U;
constexpr std::uint64_t kSoundQuietEnergy = 900000U;
constexpr std::uint64_t kSoundLoudEnergy = 90000000U;

// --- panel geometry (window pixels; the panel never scales) -----------------
constexpr int kPanelWidth = 360;
constexpr int kPanelPad = 12;
constexpr int kRowHeight = 26;
constexpr int kRowGap = 4;
constexpr int kHeaderHeight = 20;
constexpr int kTiltPadSize = 88;

// --- panel palette ----------------------------------------------------------
constexpr std::uint32_t kColorPanel = 0xFF14161AU;
constexpr std::uint32_t kColorButton = 0xFF23272EU;
constexpr std::uint32_t kColorButtonHot = 0xFF2E7D5BU;
constexpr std::uint32_t kColorButtonHeld = 0xFF3A7BD5U;
constexpr std::uint32_t kColorEdge = 0xFF343A44U;
constexpr std::uint32_t kColorLabel = 0xFFE6E9EFU;
constexpr std::uint32_t kColorHeader = 0xFF7F8B9EU;
constexpr std::uint32_t kColorHint = 0xFF69748AU;
constexpr std::uint32_t kColorValue = 0xFF6FD3A3U;

std::uint8_t expand5(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value * 255U + 15U) / 31U);
}

std::uint8_t expand6(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value * 255U + 31U) / 63U);
}

// Channel expansion runs once per displayed pixel per frame, so the divisions
// live in two tiny tables instead of the inner loop.
struct ChannelTables {
    std::uint32_t red[32]{};
    std::uint32_t green[64]{};
    std::uint32_t blue[32]{};

    ChannelTables()
    {
        for (std::uint32_t index = 0; index < 32U; ++index) {
            const std::uint32_t value = expand5(static_cast<std::uint16_t>(index));
            red[index] = value << 16U;
            blue[index] = value;
        }
        for (std::uint32_t index = 0; index < 64U; ++index) {
            green[index] = static_cast<std::uint32_t>(expand6(static_cast<std::uint16_t>(index)))
                           << 8U;
        }
    }
};

const ChannelTables kChannels{};

std::uint32_t rgb565_to_argb(std::uint16_t pixel)
{
    return 0xFF000000U | kChannels.red[(pixel >> 11U) & 0x1fU] |
           kChannels.green[(pixel >> 5U) & 0x3fU] | kChannels.blue[pixel & 0x1fU];
}

const char *mode_name(eyes::InteractionMode mode)
{
    switch (mode) {
        case eyes::InteractionMode::idle: return "IDLE";
        case eyes::InteractionMode::touching: return "TOUCHING";
        case eyes::InteractionMode::dragging: return "DRAGGING";
        case eyes::InteractionMode::settling: return "SETTLING";
        case eyes::InteractionMode::dizzy: return "DIZZY";
        case eyes::InteractionMode::sleeping: return "SLEEPING";
    }
    return "?";
}

const char *expression_name(eyes::Expression expression)
{
    switch (expression) {
        case eyes::Expression::content: return "CONTENT";
        case eyes::Expression::curious: return "CURIOUS";
        case eyes::Expression::playful: return "PLAYFUL";
        case eyes::Expression::happy: return "HAPPY";
        case eyes::Expression::surprised: return "SURPRISED";
        case eyes::Expression::sleepy: return "SLEEPY";
        case eyes::Expression::annoyed: return "ANNOYED";
        case eyes::Expression::shy: return "SHY";
        case eyes::Expression::listening: return "LISTENING";
        case eyes::Expression::thinking: return "THINKING";
    }
    return "?";
}

const char *page_name(eyes::UiPage page)
{
    switch (page) {
        case eyes::UiPage::none: return "NONE";
        case eyes::UiPage::stats: return "STATS";
    }
    return "?";
}

// --- fake hardware ----------------------------------------------------------

// In place of NVS, the AXP2101 fuel gauge and the IMU task.
class SimHost : public eyes::DeviceUiHost {
  public:
    void save_settings(const eyes::Settings &settings) override
    {
        stored_ = settings;  // an in-memory stand-in for the NVS blob
        ++saves;
    }

    void request_imu_calibration() override { calibration_requested = true; }

    eyes::BatteryStatus read_battery() override
    {
        // No gauge on the host: report a plausible steady pack so the stats
        // page has something to draw.
        return {eyes::BatteryStatus::State::ok, 4021U, 87U, false};
    }

    // PWR single press asks for the voice stream; the window loop turns it
    // into a Mac recognition, the selftest just counts it.
    void request_listen() override
    {
        listen_requested = true;
        ++listen_requests;
    }

    const eyes::Settings &stored() const { return stored_; }

    bool calibration_requested{false};
    bool listen_requested{false};
    int listen_requests{0};
    int saves{0};

  private:
    eyes::Settings stored_{};
};

// Accelerometer, mic pair and shake impulse, pumped in on the device's cadence.
struct FakeSensors {
    eyes::Vec2 tilt{};  // gaze-space tilt, -1..1 per axis; holds until reset
    bool loud{false};
    // The engine arms its dizzy reaction on three consecutive shake samples.
    int shake_samples{0};
    std::uint32_t next_sound_ms{0};

    void pump(eyes::EyeEngine &engine, eyes::DeviceUi &ui, std::uint32_t now_ms)
    {
        eyes::MotionSample sample{};
        // Device accel arrives neutral-subtracted; the engine reads
        // (accel_y, accel_x) as the gaze-space tilt vector.
        sample.accel_y = tilt.x * kGravity;
        sample.accel_x = tilt.y * kGravity;
        sample.accel_z = kGravity;
        sample.accel_magnitude = kGravity;
        sample.timestamp_ms = now_ms;
        if (shake_samples > 0) {
            --shake_samples;
            sample.accel_magnitude = kGravity + 12.0F;  // engine gate is >7 off gravity
            sample.gyro_z = 14.0F;                      // ...or >8 rad/s of rotation
        }
        engine.motion_sample(sample);

        if (now_ms >= next_sound_ms) {
            next_sound_ms = now_ms + kSoundWindowMs;
            eyes::SoundPacket packet{};
            packet.energy = loud ? kSoundLoudEnergy : kSoundQuietEnergy;
            packet.timestamp_ms = now_ms;
            packet.direction = loud ? 0.6F : 0.0F;
            packet.direction_valid = loud;
            ui.set_last_sound(packet);
            engine.sound_sample(packet, now_ms);
        }
    }
};

// --- scripted touch gestures ------------------------------------------------
// A panel button cannot press a finger down, wait and drag, so the gesture
// buttons queue timed pointer events and let them play out over real frames.
// That way swipes carry genuine velocity into the engine's fling logic.

struct GestureStep {
    enum class Kind : std::uint8_t { down, move, up };

    std::uint32_t offset_ms;
    Kind kind;
    float x;
    float y;
};

class GestureRunner {
  public:
    void start(std::vector<GestureStep> steps, std::uint32_t now_ms)
    {
        steps_ = std::move(steps);
        next_ = 0;
        start_ms_ = now_ms;
        active_ = !steps_.empty();
    }

    bool active() const { return active_; }
    void cancel() { active_ = false; }

    // Returns the steps due by now; the caller delivers them through the same
    // path a real touch takes.
    template <typename Deliver>
    void pump(std::uint32_t now_ms, Deliver deliver)
    {
        while (active_ && next_ < steps_.size() &&
               now_ms - start_ms_ >= steps_[next_].offset_ms) {
            const GestureStep &step = steps_[next_];
            deliver(step);
            ++next_;
        }
        if (next_ >= steps_.size()) {
            active_ = false;
        }
    }

  private:
    std::vector<GestureStep> steps_{};
    std::size_t next_{0};
    std::uint32_t start_ms_{0};
    bool active_{false};
};

// A swipe across the panel centre: ~230 px in 200 ms is a deliberate flick,
// fast enough for the engine to read it as a browse rather than a gaze drag.
std::vector<GestureStep> swipe_gesture(float dx, float dy)
{
    constexpr float centre = static_cast<float>(eyes::kScreenWidth) * 0.5F;
    constexpr float reach = 117.0F;
    constexpr int steps = 8;
    constexpr std::uint32_t duration_ms = 200U;
    std::vector<GestureStep> gesture;
    gesture.push_back({0U, GestureStep::Kind::down, centre - dx * reach, centre - dy * reach});
    for (int index = 1; index <= steps; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(steps);
        const std::uint32_t at = static_cast<std::uint32_t>(
            static_cast<float>(duration_ms) * t);
        gesture.push_back({at, GestureStep::Kind::move, centre + dx * reach * (2.0F * t - 1.0F),
                           centre + dy * reach * (2.0F * t - 1.0F)});
    }
    gesture.push_back({duration_ms + 10U, GestureStep::Kind::up, centre + dx * reach,
                       centre + dy * reach});
    return gesture;
}

std::vector<GestureStep> tap_gesture()
{
    constexpr float centre = static_cast<float>(eyes::kScreenWidth) * 0.5F;
    return {{0U, GestureStep::Kind::down, centre, centre},
            {60U, GestureStep::Kind::up, centre, centre}};
}

// A long press: held well past a tap, released. The firmware treats the
// release as a poke; nothing in it sleeps on a held touch.
std::vector<GestureStep> long_press_gesture()
{
    constexpr float centre = static_cast<float>(eyes::kScreenWidth) * 0.5F;
    return {{0U, GestureStep::Kind::down, centre, centre},
            {400U, GestureStep::Kind::move, centre, centre},
            {820U, GestureStep::Kind::up, centre, centre}};
}

// --- panel drawing ----------------------------------------------------------

struct Rect {
    int x{0};
    int y{0};
    int w{0};
    int h{0};

    bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// --- physical device model --------------------------------------------------
// Waveshare ESP32-S3-Touch-AMOLED-1.75C, from the dimension drawing. The
// 43.76 mm active circle is our 466 px framebuffer, so everything else follows
// from that one ratio.
constexpr float kDisplayMm = 43.76F;    // active AMOLED circle
constexpr float kGlassMm = 48.96F;      // cover glass / black bezel
constexpr float kBodyMm = 55.00F;       // widest point of the metal shell
constexpr float kButtonArcMm = 7.0F;     // side button length along the rim
constexpr float kButtonPocketMm = 1.9F;  // depth of the recess cut into the rim
constexpr float kButtonProudMm = 0.35F;  // the key barely clears its pocket
constexpr float kMicHoleMm = 1.1F;
// Both side buttons sit on the right edge, about 1:30 and 4:30. The two MEMS
// mics face the left edge, ~30 mm apart (see the audio notes in app_main).
constexpr float kBootButtonAngle = 0.82F;   // radians, +y is down
constexpr float kPowerButtonAngle = -0.82F;
constexpr float kMicAngleTop = -2.46F;
constexpr float kMicAngleBottom = 2.46F;

struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

Vec3 normalize(Vec3 value)
{
    const float length =
        std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.0F) {
        return {0.0F, 0.0F, 1.0F};
    }
    return {value.x / length, value.y / length, value.z / length};
}

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

float clamp01(float value) { return value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value); }

float smoothstep01(float edge0, float edge1, float value)
{
    const float t = clamp01((value - edge0) / (edge1 - edge0));
    return t * t * (3.0F - 2.0F * t);
}

// Key light from the upper left, slightly toward the viewer.
const Vec3 kKeyLight = normalize({-0.46F, -0.72F, 0.52F});

std::uint32_t pack(float r, float g, float b, float a)
{
    const auto channel = [](float value) {
        const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
        return static_cast<std::uint32_t>(clamped * 255.0F + 0.5F);
    };
    return (channel(a) << 24U) | (channel(r) << 16U) | (channel(g) << 8U) | channel(b);
}

// Hash-based value noise. The shell is baked once, so this is sampled in the
// shell layer's own pixel space and never swims.
float hash_noise(int x, int y)
{
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393U +
                      static_cast<std::uint32_t>(y) * 668265263U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    h ^= h >> 16U;
    return static_cast<float>(h & 0xFFFFFFU) / 16777215.0F;
}

float value_noise(float x, float y)
{
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    const float tx = x - fx;
    const float ty = y - fy;
    // Smoothstep interpolation between the four lattice corners.
    const float ux = tx * tx * (3.0F - 2.0F * tx);
    const float uy = ty * ty * (3.0F - 2.0F * ty);
    const float a = hash_noise(ix, iy);
    const float b = hash_noise(ix + 1, iy);
    const float c = hash_noise(ix, iy + 1);
    const float d = hash_noise(ix + 1, iy + 1);
    return (a + (b - a) * ux) + ((c - a) + (d - c) * ux) * uy;
}

// Two octaves of fine speckle plus one coarse octave: the orange-peel of a
// powder coat rather than the fine lines of a brushed finish.
float powder_grain(float x, float y)
{
    return 0.56F * (value_noise(x * 1.9F, y * 1.9F) - 0.5F) +
           0.30F * (value_noise(x * 0.62F, y * 0.62F) - 0.5F) +
           0.14F * (value_noise(x * 5.5F, y * 5.5F) - 0.5F);
}

// Powder-coated aluminium alloy. A powder coat is a matte, slightly rough
// finish: it scatters light rather than mirroring it, so this leans on diffuse
// and a broad soft sheen instead of the tight reflections of bare metal, and
// carries a fine orange-peel grain. `t` runs 0 at the glass edge to 1 at the
// outer silhouette, where the surface has rolled a quarter turn away.
std::uint32_t shell_color(float angle, float t, float brightness, float grain)
{
    const float roll = t * 1.5707963F;
    const float radial = std::sin(roll);
    const float nz = std::cos(roll);
    // The grain perturbs the surface normal a little, which is what makes the
    // texture read as roughness rather than as noise painted on top.
    const float perturbed = clamp01(t + grain * 0.10F);
    const float bumped_roll = perturbed * 1.5707963F;
    const Vec3 normal{std::cos(angle) * std::sin(bumped_roll),
                      std::sin(angle) * std::sin(bumped_roll), std::cos(bumped_roll)};

    const float diffuse = std::max(0.0F, dot(normal, kKeyLight));
    // Wrapped diffuse: rough surfaces stay lit well past the terminator.
    const float wrapped = std::max(0.0F, (dot(normal, kKeyLight) + 0.45F) / 1.45F);

    // A broad, blurry environment. Matte, so the bands are soft and low
    // contrast compared with polished metal.
    const float reflect_y = 2.0F * nz * (std::sin(angle) * radial);
    const float overhead = smoothstep01(-0.30F, 0.95F, -reflect_y);
    const float bounce = smoothstep01(-0.10F, 0.90F, reflect_y);
    const float env = 0.30F * overhead + 0.16F * bounce;

    // Wide, weak sheen instead of a specular pinpoint.
    const Vec3 half = normalize({kKeyLight.x, kKeyLight.y, kKeyLight.z + 1.0F});
    const float sheen = std::pow(std::max(0.0F, dot(normal, half)), 6.0F) * 0.12F;

    const float edge = 1.0F - 0.34F * smoothstep01(0.76F, 1.0F, t);

    float intensity = (0.34F + 0.30F * wrapped + 0.16F * diffuse + env) * edge;
    intensity += sheen + grain * 0.085F;
    intensity = intensity * brightness;

    // A coated alloy shell: neutral grey with the faintest cool cast.
    return pack(intensity * 1.0F, intensity * 1.005F, intensity * 1.035F, 1.0F);
}

// An ARGB layer with real alpha, so the device can be composited over the
// backdrop at a moving offset.
class Layer {
  public:
    Layer() = default;
    Layer(int width, int height) : width_(width), height_(height)
    {
        pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    }

    int width() const { return width_; }
    int height() const { return height_; }
    std::uint32_t at(int x, int y) const
    {
        return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                       static_cast<std::size_t>(x)];
    }
    void set(int x, int y, std::uint32_t argb)
    {
        pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                static_cast<std::size_t>(x)] = argb;
    }

  private:
    int width_{0};
    int height_{0};
    std::vector<std::uint32_t> pixels_{};
};


// Everything the window shows is composed into one ARGB buffer, so a headless
// run can dump the exact same image the window presents.
class Canvas {
  public:
    Canvas(int width, int height) : width_(width), height_(height)
    {
        pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    }

    int width() const { return width_; }
    int height() const { return height_; }
    const std::uint32_t *pixels() const { return pixels_.data(); }
    int pitch() const { return width_ * static_cast<int>(sizeof(std::uint32_t)); }

    void clear(std::uint32_t color)
    {
        std::fill(pixels_.begin(), pixels_.end(), color);
    }

    // Restore a prerendered background in one go.
    void copy_from(const Canvas &other)
    {
        std::copy(other.pixels_.begin(), other.pixels_.end(), pixels_.begin());
    }

    void fill(const Rect &rect, std::uint32_t color)
    {
        const int x0 = std::max(0, rect.x);
        const int y0 = std::max(0, rect.y);
        const int x1 = std::min(width_, rect.x + rect.w);
        const int y1 = std::min(height_, rect.y + rect.h);
        for (int y = y0; y < y1; ++y) {
            std::uint32_t *row =
                pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
            for (int x = x0; x < x1; ++x) {
                row[x] = color;
            }
        }
    }

    void outline(const Rect &rect, std::uint32_t color)
    {
        fill({rect.x, rect.y, rect.w, 1}, color);
        fill({rect.x, rect.y + rect.h - 1, rect.w, 1}, color);
        fill({rect.x, rect.y, 1, rect.h}, color);
        fill({rect.x + rect.w - 1, rect.y, 1, rect.h}, color);
    }

    static int text_width(const char *text, int scale)
    {
        return static_cast<int>(std::strlen(text)) * 8 * scale;
    }

    // The device's own 8x8 font, so panel text matches the panel's text.
    void text(int x, int y, const char *string, std::uint32_t color, int scale)
    {
        for (const char *c = string; *c != '\0'; ++c, x += 8 * scale) {
            if (*c < 32 || *c > 126) {
                continue;
            }
            const std::uint8_t *glyph = eyes::kFont8x8[*c - 32];
            for (int row = 0; row < 8; ++row) {
                for (int bit = 0; bit < 8; ++bit) {
                    if ((glyph[row] & (1U << bit)) == 0U) {
                        continue;
                    }
                    fill({x + bit * scale, y + row * scale, scale, scale}, color);
                }
            }
        }
    }

    void text_centered(const Rect &rect, const char *string, std::uint32_t color, int scale)
    {
        const int x = rect.x + (rect.w - text_width(string, scale)) / 2;
        const int y = rect.y + (rect.h - 8 * scale) / 2;
        text(x, y, string, color, scale);
    }

    // Nearest-neighbour blit of the RGB565 device framebuffer.
    void blit_screen(const std::uint16_t *framebuffer, const Rect &rect, int scale)
    {
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            for (int x = 0; x < eyes::kScreenWidth; ++x) {
                const std::uint32_t argb = rgb565_to_argb(
                    framebuffer[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                static_cast<std::size_t>(x)]);
                fill({rect.x + x * scale, rect.y + y * scale, scale, scale}, argb);
            }
        }
    }

    // The active AMOLED circle: the framebuffer sampled into a disc of
    // arbitrary size, with a soft edge so the glass meets it cleanly.
    void blit_display_disc(const std::uint16_t *framebuffer, float cx, float cy, float radius,
                           bool smooth)
    {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)) - 1);
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)) - 1);
        const int x1 = std::min(width_ - 1, static_cast<int>(std::ceil(cx + radius)) + 1);
        const int y1 = std::min(height_ - 1, static_cast<int>(std::ceil(cy + radius)) + 1);
        const float source_scale = static_cast<float>(eyes::kScreenWidth) / (radius * 2.0F);
        const float inner = std::max(0.0F, radius - 1.5F);
        const float inner_squared = inner * inner;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = static_cast<float>(x) + 0.5F - cx;
                const float dy = static_cast<float>(y) + 0.5F - cy;
                const float squared = dx * dx + dy * dy;
                float coverage = 1.0F;
                if (squared > inner_squared) {  // only the rim needs the root
                    coverage = clamp01(radius + 0.5F - std::sqrt(squared));
                    if (coverage <= 0.0F) {
                        continue;
                    }
                }
                const float fx = (dx + radius) * source_scale;
                const float fy = (dy + radius) * source_scale;
                std::uint32_t argb = 0U;
                if (smooth && source_scale > 1.25F) {
                    // Minified disc: one bilinear tap skips source pixels and
                    // the eyes shimmer in motion; average the whole footprint.
                    argb = sample_box(framebuffer, fx, fy, source_scale);
                } else if (smooth) {
                    // The real panel is ~270 PPI, far past the eye; magnified
                    // on a monitor, point sampling shows panel pixels the
                    // device never would. Bilinear is the closer likeness.
                    argb = sample_bilinear(framebuffer, fx - 0.5F, fy - 0.5F);
                } else {
                    const int sx = std::clamp(static_cast<int>(fx), 0, eyes::kScreenWidth - 1);
                    const int sy = std::clamp(static_cast<int>(fy), 0, eyes::kScreenHeight - 1);
                    argb = rgb565_to_argb(
                        framebuffer[static_cast<std::size_t>(sy) * eyes::kScreenWidth +
                                    static_cast<std::size_t>(sx)]);
                }
                blend(x, y, argb, coverage);
            }
        }
    }

    // Equal-weight average over the source footprint of one destination
    // pixel. Correct minification filter where bilinear under-samples.
    static std::uint32_t sample_box(const std::uint16_t *framebuffer, float fx, float fy,
                                    float footprint)
    {
        const float half = footprint * 0.5F;
        const int x0 = std::clamp(static_cast<int>(fx - half), 0, eyes::kScreenWidth - 1);
        const int y0 = std::clamp(static_cast<int>(fy - half), 0, eyes::kScreenHeight - 1);
        const int x1 = std::clamp(static_cast<int>(fx + half), x0, eyes::kScreenWidth - 1);
        const int y1 = std::clamp(static_cast<int>(fy + half), y0, eyes::kScreenHeight - 1);
        std::uint32_t r = 0U;
        std::uint32_t g = 0U;
        std::uint32_t b = 0U;
        std::uint32_t count = 0U;
        for (int y = y0; y <= y1; ++y) {
            const std::uint16_t *row = framebuffer + static_cast<std::size_t>(y) * eyes::kScreenWidth;
            for (int x = x0; x <= x1; ++x) {
                const std::uint32_t argb = rgb565_to_argb(row[x]);
                r += (argb >> 16) & 0xFFU;
                g += (argb >> 8) & 0xFFU;
                b += argb & 0xFFU;
                ++count;
            }
        }
        r = (r + count / 2U) / count;
        g = (g + count / 2U) / count;
        b = (b + count / 2U) / count;
        return 0xFF000000U | (r << 16) | (g << 8) | b;
    }

    // Bilinear tap into the RGB565 framebuffer.
    static std::uint32_t sample_bilinear(const std::uint16_t *framebuffer, float fx, float fy)
    {
        const float clamped_x = std::clamp(fx, 0.0F, static_cast<float>(eyes::kScreenWidth - 1));
        const float clamped_y = std::clamp(fy, 0.0F, static_cast<float>(eyes::kScreenHeight - 1));
        const int x0 = static_cast<int>(clamped_x);
        const int y0 = static_cast<int>(clamped_y);
        const int x1 = std::min(x0 + 1, eyes::kScreenWidth - 1);
        const int y1 = std::min(y0 + 1, eyes::kScreenHeight - 1);
        const float tx = clamped_x - static_cast<float>(x0);
        const float ty = clamped_y - static_cast<float>(y0);
        const auto tap = [&](int x, int y) {
            return rgb565_to_argb(framebuffer[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                              static_cast<std::size_t>(x)]);
        };
        const std::uint32_t a = tap(x0, y0);
        const std::uint32_t b = tap(x1, y0);
        const std::uint32_t c = tap(x0, y1);
        const std::uint32_t d = tap(x1, y1);
        const auto channel = [&](int shift) {
            const float top = static_cast<float>((a >> shift) & 0xFFU) +
                              (static_cast<float>((b >> shift) & 0xFFU) -
                               static_cast<float>((a >> shift) & 0xFFU)) * tx;
            const float bottom = static_cast<float>((c >> shift) & 0xFFU) +
                                 (static_cast<float>((d >> shift) & 0xFFU) -
                                  static_cast<float>((c >> shift) & 0xFFU)) * tx;
            return static_cast<std::uint32_t>(top + (bottom - top) * ty + 0.5F);
        };
        return 0xFF000000U | (channel(16) << 16U) | (channel(8) << 8U) | channel(0);
    }

    // Source-over blend that carries alpha, so the window can be transparent
    // everywhere the device and panel are not.
    void blend(int x, int y, std::uint32_t argb, float alpha)
    {
        if (x < 0 || y < 0 || x >= width_ || y >= height_ || alpha <= 0.0F) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(width_) +
                                  static_cast<std::size_t>(x);
        const std::uint32_t rgb = argb & 0x00FFFFFFU;
        if (alpha >= 1.0F) {
            pixels_[index] = 0xFF000000U | rgb;  // fully covers whatever was there
            return;
        }
        const std::uint32_t dst = pixels_[index];
        const std::uint32_t dst_alpha = dst >> 24U;
        if (dst_alpha == 0U) {  // the common case: first paint onto empty space
            pixels_[index] =
                (static_cast<std::uint32_t>(alpha * 255.0F + 0.5F) << 24U) | rgb;
            return;
        }
        const float da = static_cast<float>(dst_alpha) / 255.0F;
        const float out_alpha = alpha + da * (1.0F - alpha);
        const auto mix = [&](int shift) {
            const float source = static_cast<float>((rgb >> shift) & 0xFFU);
            const float destination = static_cast<float>((dst >> shift) & 0xFFU);
            const float value =
                (source * alpha + destination * da * (1.0F - alpha)) / out_alpha;
            return static_cast<std::uint32_t>(std::min(255.0F, value + 0.5F));
        };
        pixels_[index] = (static_cast<std::uint32_t>(out_alpha * 255.0F + 0.5F) << 24U) |
                         (mix(16) << 16U) | (mix(8) << 8U) | mix(0);
    }

    std::uint8_t alpha_at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) {
            return 0U;
        }
        return static_cast<std::uint8_t>(
            pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                    static_cast<std::size_t>(x)] >>
            24U);
    }

    // Composite an ARGB layer with per-pixel alpha at an integer offset.
    void blit_layer(const Layer &layer, int origin_x, int origin_y)
    {
        for (int y = 0; y < layer.height(); ++y) {
            const int dy = origin_y + y;
            if (dy < 0 || dy >= height_) {
                continue;
            }
            for (int x = 0; x < layer.width(); ++x) {
                const std::uint32_t argb = layer.at(x, y);
                const std::uint32_t alpha = argb >> 24U;
                if (alpha == 0U) {
                    continue;
                }
                blend(origin_x + x, dy, argb & 0x00FFFFFFU,
                      static_cast<float>(alpha) / 255.0F);
            }
        }
    }

    // Anti-aliased filled disc.
    void disc(float cx, float cy, float radius, std::uint32_t color, float alpha = 1.0F)
    {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)) - 1);
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)) - 1);
        const int x1 = std::min(width_ - 1, static_cast<int>(std::ceil(cx + radius)) + 1);
        const int y1 = std::min(height_ - 1, static_cast<int>(std::ceil(cy + radius)) + 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = static_cast<float>(x) + 0.5F - cx;
                const float dy = static_cast<float>(y) + 0.5F - cy;
                const float coverage =
                    clamp01(radius + 0.5F - std::sqrt(dx * dx + dy * dy));
                blend(x, y, color, coverage * alpha);
            }
        }
    }

    // Soft elliptical contact shadow: the device is lit from above, so what
    // lands on the backdrop is wide, offset down, and very diffuse.
    void soft_shadow(float cx, float cy, float radius_x, float radius_y, float strength)
    {
        const int x0 = std::max(0, static_cast<int>(cx - radius_x) - 1);
        const int y0 = std::max(0, static_cast<int>(cy - radius_y) - 1);
        const int x1 = std::min(width_ - 1, static_cast<int>(cx + radius_x) + 1);
        const int y1 = std::min(height_ - 1, static_cast<int>(cy + radius_y) + 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5F - cx) / radius_x;
                const float dy = (static_cast<float>(y) + 0.5F - cy) / radius_y;
                const float distance = std::sqrt(dx * dx + dy * dy);
                if (distance >= 1.0F) {
                    continue;
                }
                const float falloff = (1.0F - distance) * (1.0F - distance);
                blend(x, y, 0x000000U, falloff * strength);
            }
        }
    }

    // Anti-aliased rounded rectangle, for the floating panel.
    void rounded_rect(const Rect &rect, float corner, std::uint32_t color, float alpha = 1.0F)
    {
        const float left = static_cast<float>(rect.x) + corner;
        const float right = static_cast<float>(rect.x + rect.w) - corner;
        const float top = static_cast<float>(rect.y) + corner;
        const float bottom = static_cast<float>(rect.y + rect.h) - corner;
        const int corner_px = static_cast<int>(std::ceil(corner));
        for (int y = std::max(0, rect.y); y < std::min(height_, rect.y + rect.h); ++y) {
            const bool corner_row =
                y < rect.y + corner_px || y >= rect.y + rect.h - corner_px;
            for (int x = std::max(0, rect.x); x < std::min(width_, rect.x + rect.w); ++x) {
                if (!corner_row) {  // the straight middle band needs no maths
                    blend(x, y, color, alpha);
                    continue;
                }
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                const float dx = std::max({left - px, 0.0F, px - right});
                const float dy = std::max({top - py, 0.0F, py - bottom});
                if (dx == 0.0F && dy == 0.0F) {
                    blend(x, y, color, alpha);
                    continue;
                }
                const float distance = std::sqrt(dx * dx + dy * dy);
                blend(x, y, color, clamp01(corner + 0.5F - distance) * alpha);
            }
        }
    }

  private:
    int width_{0};
    int height_{0};
    std::vector<std::uint32_t> pixels_{};
};


// Geometry of the rendered device, all derived from the display size.
struct DeviceMetrics {
    float display_radius{0.0F};
    float glass_radius{0.0F};
    float body_radius{0.0F};
    float px_per_mm{0.0F};
    int tile_size{0};   // the cached shell layer is square
    float tile_center{0.0F};

    static DeviceMetrics from_display(float display_px)
    {
        DeviceMetrics metrics{};
        metrics.px_per_mm = display_px / kDisplayMm;
        metrics.display_radius = display_px * 0.5F;
        metrics.glass_radius = kGlassMm * 0.5F * metrics.px_per_mm;
        metrics.body_radius = kBodyMm * 0.5F * metrics.px_per_mm;
        // Room for the side buttons and the rim's outer anti-aliasing.
        const float reach = metrics.body_radius + kButtonProudMm * metrics.px_per_mm + 4.0F;
        metrics.tile_size = static_cast<int>(std::ceil(reach * 2.0F));
        // Keep it even so tile_center is a whole pixel: the shell and the
        // separately baked keys are both blitted at integer offsets, and a
        // half-pixel centre would slide the keys out of their pockets.
        if ((metrics.tile_size & 1) != 0) {
            ++metrics.tile_size;
        }
        metrics.tile_center = static_cast<float>(metrics.tile_size) * 0.5F;
        return metrics;
    }
};

// Radial band and arc the pocket occupies, shared by the shell bake, the live
// key and hit testing so the three can never disagree.
struct ButtonSlot {
    float inner{0.0F};
    float outer{0.0F};
    float half_arc{0.0F};
};

ButtonSlot button_slot(const DeviceMetrics &metrics)
{
    ButtonSlot slot{};
    slot.outer = metrics.body_radius + kButtonProudMm * metrics.px_per_mm;
    slot.inner = slot.outer - kButtonPocketMm * metrics.px_per_mm;
    slot.half_arc = kButtonArcMm * 0.5F * metrics.px_per_mm;
    return slot;
}

// Signed distance from a point to the rounded slot, in pixels: negative
// inside. Works in (arc length, radius) space, which is near enough to
// Cartesian over a span this short.
float slot_distance(const DeviceMetrics &metrics, const ButtonSlot &slot, float angle, float dx,
                    float dy, float shrink)
{
    const float distance = std::sqrt(dx * dx + dy * dy);
    float delta = std::atan2(dy, dx) - angle;
    while (delta > kPi) {
        delta -= 2.0F * kPi;
    }
    while (delta < -kPi) {
        delta += 2.0F * kPi;
    }
    const float along = std::fabs(delta) * metrics.body_radius;
    const float radius = (slot.outer - slot.inner) * 0.5F - shrink;
    const float centre = (slot.inner + slot.outer) * 0.5F;
    const float over_arc = std::max(0.0F, along - (slot.half_arc - shrink) + radius);
    const float over_radial = std::fabs(distance - centre);
    return std::sqrt(over_arc * over_arc + over_radial * over_radial) - radius;
}

bool hits_button(const DeviceMetrics &metrics, float angle, float dx, float dy)
{
    return slot_distance(metrics, button_slot(metrics), angle, dx, dy, 0.0F) <= 1.0F;
}

// One shaded point on the shell, in tile coordinates. Everything the shell is
// made of lives here so the supersampler can average all of it at once.
struct Shaded {
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
    float a{0.0F};
};

Shaded shell_pixel(const DeviceMetrics &metrics, const ButtonSlot &slot, float tx, float ty)
{
    const float centre = metrics.tile_center;
    const float dx = tx - centre;
    const float dy = ty - centre;
    const float distance = std::sqrt(dx * dx + dy * dy);
    Shaded out{};
    if (distance > metrics.body_radius + 1.5F) {
        // Outside the body, but a pocket lip may still bulge past it.
        bool near_pocket = false;
        for (const float angle : {kBootButtonAngle, kPowerButtonAngle}) {
            if (slot_distance(metrics, slot, angle, dx, dy, 0.0F) <= 1.5F) {
                near_pocket = true;
            }
        }
        if (!near_pocket) {
            return out;
        }
    }
    const float angle = std::atan2(dy, dx);
    const float rim_span = metrics.body_radius - metrics.glass_radius;

    if (distance > metrics.glass_radius) {
        // Rounded, powder-coated rim.
        const float t = clamp01((distance - metrics.glass_radius) / rim_span);
        const float lip = 1.0F + 0.22F * (1.0F - smoothstep01(0.0F, 0.16F, t));
        const float grain = powder_grain(tx, ty);
        const std::uint32_t color = shell_color(angle, t, 0.92F * lip, grain);
        out.r = static_cast<float>((color >> 16U) & 0xFFU) / 255.0F;
        out.g = static_cast<float>((color >> 8U) & 0xFFU) / 255.0F;
        out.b = static_cast<float>(color & 0xFFU) / 255.0F;
        out.a = clamp01(metrics.body_radius + 0.5F - distance);
    } else {
        // Cover glass: near-black, with the rim darkening it inward and a soft
        // reflection sweeping the upper left.
        float shade = 0.055F + 0.02F * smoothstep01(metrics.display_radius, metrics.glass_radius,
                                                    distance);
        const float sheen_x = dx * 0.82F + dy * 0.57F;
        const float sheen_y = -dx * 0.57F + dy * 0.82F;
        const float glass_sq = metrics.glass_radius * metrics.glass_radius;
        const float sheen =
            std::exp(-(sheen_x * sheen_x) / (0.62F * glass_sq) -
                     (sheen_y + metrics.glass_radius * 0.62F) *
                         (sheen_y + metrics.glass_radius * 0.62F) / (0.20F * glass_sq));
        shade += 0.13F * sheen;
        // A thin dark seam where the glass meets the metal.
        shade *= 1.0F - 0.55F * smoothstep01(metrics.glass_radius - 2.5F, metrics.glass_radius,
                                             distance);
        out.r = shade;
        out.g = shade;
        out.b = shade * 1.06F;
        out.a = clamp01(metrics.glass_radius + 0.5F - distance);
    }

    // Two mic ports on the left edge, ~30 mm apart.
    const float hole_radius = kMicHoleMm * 0.5F * metrics.px_per_mm;
    const float seat = metrics.body_radius - 1.6F * metrics.px_per_mm;
    for (const float hole_angle : {kMicAngleTop, kMicAngleBottom}) {
        const float hx = centre + std::cos(hole_angle) * seat;
        const float hy = centre + std::sin(hole_angle) * seat;
        const float hdx = tx - hx;
        const float hdy = ty - hy;
        const float coverage =
            clamp01(hole_radius + 0.5F - std::sqrt(hdx * hdx + hdy * hdy));
        if (coverage > 0.0F && out.a > 0.0F) {
            out.r += (0.060F - out.r) * coverage;
            out.g += (0.062F - out.g) * coverage;
            out.b += (0.070F - out.b) * coverage;
        }
    }

    // Pockets machined into the rim. The key sits down inside one of these, so
    // the shell always overlaps its edges and it reads as part of the body.
    for (const float pocket_angle : {kBootButtonAngle, kPowerButtonAngle}) {
        const float signed_distance = slot_distance(metrics, slot, pocket_angle, dx, dy, 0.0F);
        if (signed_distance > 1.5F) {
            continue;
        }
        const float depth = clamp01(-signed_distance / 3.0F);
        const float floor_shade = 0.045F + 0.075F * depth;
        const float coverage = clamp01(0.5F - signed_distance);
        if (coverage <= 0.0F) {
            continue;
        }
        out.r += (floor_shade - out.r) * coverage;
        out.g += (floor_shade - out.g) * coverage;
        out.b += (floor_shade * 1.05F - out.b) * coverage;
        out.a = std::max(out.a, coverage);
    }
    return out;
}

// The key seated in its pocket, clipped inside the walls so the shell frames it.
Shaded key_pixel(const DeviceMetrics &metrics, const ButtonSlot &slot, float angle, bool pressed,
                 float dx, float dy)
{
    Shaded out{};
    const float sink = pressed ? 1.3F : 0.0F;
    const float signed_distance =
        slot_distance(metrics, slot, angle, dx, dy, 1.4F + sink * 0.35F);
    const float coverage = clamp01(0.5F - signed_distance);
    if (coverage <= 0.0F) {
        return out;
    }
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float pixel_angle = std::atan2(dy, dx);
    const float across = clamp01((distance - slot.inner) / (slot.outer - slot.inner));
    const float crown = 0.62F + 0.38F * std::sin(across * kPi);
    const float grain = powder_grain(dx * 1.6F, dy * 1.6F);
    const float lit = pressed ? 0.74F : 1.0F;
    const std::uint32_t color =
        shell_color(pixel_angle, 0.26F + across * 0.46F, crown * lit, grain);
    out.r = static_cast<float>((color >> 16U) & 0xFFU) / 255.0F;
    out.g = static_cast<float>((color >> 8U) & 0xFFU) / 255.0F;
    out.b = static_cast<float>(color & 0xFFU) / 255.0F;
    // Contact shadow cast by the pocket wall along the key's inner edge.
    const float wall = clamp01(1.0F - (distance - slot.inner) / 3.0F) * 0.35F;
    out.r *= 1.0F - wall;
    out.g *= 1.0F - wall;
    out.b *= 1.0F - wall;
    out.a = coverage;
    return out;
}

// Supersampling factor for the baked layers. These are built once at startup,
// so the shell's edges, its pocket walls and the mic ports can afford proper
// averaging rather than one coverage sample per pixel.
constexpr int kShellSupersample = 3;

// Average an SSxSS block, weighting colour by alpha so transparent samples do
// not drag the edge toward black.
std::uint32_t resolve_block(const Shaded *samples, int count)
{
    float alpha = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    for (int index = 0; index < count; ++index) {
        const Shaded &s = samples[index];
        alpha += s.a;
        r += s.r * s.a;
        g += s.g * s.a;
        b += s.b * s.a;
    }
    if (alpha <= 0.0F) {
        return 0U;
    }
    const std::uint32_t out_a =
        static_cast<std::uint32_t>(clamp01(alpha / static_cast<float>(count)) * 255.0F + 0.5F);
    const auto channel = [&](float sum) {
        return static_cast<std::uint32_t>(clamp01(sum / alpha) * 255.0F + 0.5F);
    };
    return (out_a << 24U) | (channel(r) << 16U) | (channel(g) << 8U) | channel(b);
}

void build_shell_layer(Layer &layer, const DeviceMetrics &metrics)
{
    layer = Layer(metrics.tile_size, metrics.tile_size);
    const ButtonSlot slot = button_slot(metrics);
    constexpr int ss = kShellSupersample;
    std::vector<Shaded> block(static_cast<std::size_t>(ss * ss));
    for (int y = 0; y < metrics.tile_size; ++y) {
        for (int x = 0; x < metrics.tile_size; ++x) {
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float tx = static_cast<float>(x) +
                                     (static_cast<float>(sx) + 0.5F) / static_cast<float>(ss);
                    const float ty = static_cast<float>(y) +
                                     (static_cast<float>(sy) + 0.5F) / static_cast<float>(ss);
                    block[static_cast<std::size_t>(sy * ss + sx)] =
                        shell_pixel(metrics, slot, tx, ty);
                }
            }
            layer.set(x, y, resolve_block(block.data(), ss * ss));
        }
    }
}

// Both key states are baked too: they are small, they never change shape, and
// baking them keeps the per-frame cost to a blit.
void build_key_layer(Layer &layer, const DeviceMetrics &metrics, float angle, bool pressed,
                     int &offset_x, int &offset_y)
{
    const ButtonSlot slot = button_slot(metrics);
    const float mid_radius = (slot.inner + slot.outer) * 0.5F;
    const float key_cx = std::cos(angle) * mid_radius;
    const float key_cy = std::sin(angle) * mid_radius;
    const float extent = slot.half_arc + (slot.outer - slot.inner) * 0.5F + 4.0F;
    offset_x = static_cast<int>(std::floor(key_cx - extent));
    offset_y = static_cast<int>(std::floor(key_cy - extent));
    const int size = static_cast<int>(std::ceil(extent * 2.0F)) + 2;
    layer = Layer(size, size);

    constexpr int ss = kShellSupersample;
    std::vector<Shaded> block(static_cast<std::size_t>(ss * ss));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float dx = static_cast<float>(offset_x + x) +
                                     (static_cast<float>(sx) + 0.5F) / static_cast<float>(ss);
                    const float dy = static_cast<float>(offset_y + y) +
                                     (static_cast<float>(sy) + 0.5F) / static_cast<float>(ss);
                    block[static_cast<std::size_t>(sy * ss + sx)] =
                        key_pixel(metrics, slot, angle, pressed, dx, dy);
                }
            }
            layer.set(x, y, resolve_block(block.data(), ss * ss));
        }
    }
}

// Only the hover caption is still drawn live; the key itself is a blit.
void draw_key_caption(Canvas &canvas, const DeviceMetrics &metrics, float cx, float cy,
                      float angle, bool pressed, const char *label)
{
    const ButtonSlot slot = button_slot(metrics);
    const float text_radius = slot.outer + 20.0F;
    const int width = Canvas::text_width(label, 1) + 14;
    const Rect chip{static_cast<int>(cx + std::cos(angle) * text_radius),
                    static_cast<int>(cy + std::sin(angle) * text_radius) - 9, width, 18};
    canvas.rounded_rect(chip, 9.0F, 0xFF181C22U, 0.92F);
    canvas.text(chip.x + 7, chip.y + 5, label, pressed ? kColorValue : kColorLabel, 1);
}

// Nothing but the shadow the device casts. The window itself is transparent,
// so the puck sits on whatever is behind it.
void render_shadow(Canvas &canvas, const DeviceMetrics &metrics, float cx, float cy)
{
    canvas.clear(0x00000000U);
    canvas.soft_shadow(cx, cy + metrics.body_radius * 0.30F, metrics.body_radius * 1.22F,
                       metrics.body_radius * 0.54F, 0.46F);
    canvas.soft_shadow(cx, cy + metrics.body_radius * 0.14F, metrics.body_radius * 1.03F,
                       metrics.body_radius * 0.92F, 0.34F);
}

// Where the device, its chip and the debug panel sit in the window. The chip
// is anchored to the puck, and the panel spawns out to its side, so the layout
// is derived from the device outward.
struct Stage {
    int width{0};
    int height{0};
    float device_cx{0.0F};
    float device_cy{0.0F};
    int tooltip_x{0};
    int tooltip_y{0};
    int panel_x{0};
    int panel_y{0};

    static constexpr int kTooltipWidth = 116;
    static constexpr int kTooltipHeight = 28;
    static constexpr float kTooltipGap = 34.0F;   // clear air between chip and shell
    static constexpr float kTooltipAngle = 0.62F;

    static Stage lay_out(const DeviceMetrics &metrics, int panel_height)
    {
        Stage stage{};
        const int margin = 52;
        stage.device_cx = metrics.body_radius + static_cast<float>(margin);
        const float reach = metrics.body_radius + kTooltipGap;
        stage.tooltip_x = static_cast<int>(stage.device_cx + std::cos(kTooltipAngle) * reach);
        // The panel spawns to the right of the chip, never touching the shell.
        stage.panel_x = stage.tooltip_x + kTooltipWidth + 18;
        stage.width = stage.panel_x + kPanelWidth + margin / 2;
        stage.height = std::max(static_cast<int>(metrics.body_radius * 2.0F) + margin * 2,
                                panel_height + 48);
        stage.device_cy = static_cast<float>(stage.height) * 0.5F;
        stage.tooltip_y =
            static_cast<int>(stage.device_cy + std::sin(kTooltipAngle) * reach);
        stage.panel_y = (stage.height - panel_height) / 2;
        return stage;
    }

    // --max: the stage IS the display. Device centred, panel against the
    // right edge (it may overlap the shell on a narrow screen; it is a debug
    // tool and opens on demand).
    static Stage lay_out_fill(const DeviceMetrics &metrics, int panel_height, int width,
                              int height)
    {
        Stage stage{};
        stage.width = width;
        stage.height = height;
        stage.device_cx = static_cast<float>(width) * 0.5F;
        stage.device_cy = static_cast<float>(height) * 0.5F;
        const float reach = metrics.body_radius + kTooltipGap;
        stage.tooltip_x = static_cast<int>(stage.device_cx + std::cos(kTooltipAngle) * reach);
        stage.tooltip_y = static_cast<int>(stage.device_cy + std::sin(kTooltipAngle) * reach);
        stage.panel_x = std::max(0, width - kPanelWidth - 26);
        stage.panel_y = std::max(0, (height - panel_height) / 2);
        return stage;
    }
};

// A small floating chip parked near the device. It is deliberately detached
// from the shell, with clear air around it, so nobody reads it as part of the
// product geometry. Clicking it spawns the debug panel.
struct Tooltip {
    Rect rect{};
    bool open{false};

    void place(const Stage &stage)
    {
        rect = {stage.tooltip_x, stage.tooltip_y, Stage::kTooltipWidth, Stage::kTooltipHeight};
    }

    // A faint tether to the panel it spawned, drawn only between the two
    // floating elements: it never reaches the shell.
    void draw_tether(Canvas &canvas, int panel_x, int panel_y, int panel_height) const
    {
        const int y0 = rect.y + rect.h / 2;
        const int y1 = panel_y + panel_height / 2;
        for (int x = rect.x + rect.w + 2; x < panel_x - 1; x += 4) {
            canvas.fill({x, y0, 2, 1}, 0x66FFFFFFU);
        }
        const int step = y1 > y0 ? 4 : -4;
        for (int y = y0; step > 0 ? y < y1 : y > y1; y += step) {
            canvas.fill({panel_x - 2, y, 1, 2}, 0x44FFFFFFU);
        }
    }

    void draw(Canvas &canvas, bool hovered) const
    {
        canvas.soft_shadow(static_cast<float>(rect.x + rect.w / 2),
                           static_cast<float>(rect.y + rect.h / 2) + 5.0F,
                           static_cast<float>(rect.w) * 0.72F,
                           static_cast<float>(rect.h) * 0.95F, 0.42F);
        canvas.rounded_rect(rect, static_cast<float>(rect.h) * 0.5F,
                            hovered ? 0xFF232A34U : 0xFF181C22U, 0.94F);
        // A status dot, lit while the panel is up.
        canvas.disc(static_cast<float>(rect.x) + 15.0F,
                    static_cast<float>(rect.y + rect.h / 2), 4.0F,
                    open ? kColorValue : 0xFF5A6577U);
        canvas.text(rect.x + 26, rect.y + rect.h / 2 - 4, open ? "DEBUG ON" : "DEBUG",
                    open ? kColorLabel : kColorHint, 1);
    }
};

// Every panel control. Most map to one device input; kMenuRow runs the real
// options-menu row action.
enum class Control : std::uint8_t {
    none,
    boot,
    power,
    tap,
    long_press,
    palm,
    swipe_left,
    swipe_right,
    swipe_up,
    swipe_down,
    rotate_ccw,
    rotate_reset,
    rotate_cw,
    shake,
    sound,
    tilt_flat,
    tilt_pad,
    setting,
    aa_mode,
    display_filter,
    capture,
};

struct Button {
    Control control{Control::none};
    int row{0};  // eyes::UiSetting index for Control::setting
    Rect rect{};
    const char *label{""};
    bool is_hold{false};  // press and hold, rather than fire on click
    // Fixed per button so a row never mixes glyph sizes; the menu rows carry
    // long dynamic labels and use the small font.
    int text_scale{2};
};

// Lays the panel out once; positions stay fixed so hit-testing is trivial.
class Panel {
  public:
    void build()
    {
        buttons_.clear();
        int y = 26 + kPanelPad;  // below the drag bar
        const int width = kPanelWidth - kPanelPad * 2;

        y = header(y, "PHYSICAL BUTTONS");
        // Hold these like the real thing: the press duration decides what
        // happens, so a long BOOT press really does recalibrate.
        y = row(y, {{Control::boot, 0, {}, "BOOT", true}, {Control::power, 0, {}, "PWR", true}},
                width);
        y = hint(y, "HOLD. BOOT TAP=MENU X2=STATS 850MS=CAL");
        y = hint(y, "3S=DEBUG. PWR TAP=RANDOM X2=SLEEP");

        y = header(y, "TOUCH PANEL");
        y = row(y, {{Control::tap, 0, {}, "TAP", false}, {Control::palm, 0, {}, "PALM", false}},
                width);
        y = row(y, {{Control::long_press, 0, {}, "HOLD 800MS", false}}, width);
        y = row(y,
                {{Control::swipe_left, 0, {}, "SWIPE <", false},
                 {Control::swipe_right, 0, {}, "SWIPE >", false}},
                width);
        y = row(y,
                {{Control::swipe_up, 0, {}, "SWIPE ^", false},
                 {Control::swipe_down, 0, {}, "SWIPE v", false}},
                width);
        y = hint(y, "OR TOUCH THE SCREEN DIRECTLY");

        y = header(y, "MOTION AND SOUND");
        // The tilt pad sits left, its readout and controls stack to the right.
        const Rect pad{kPanelPad, y, kTiltPadSize, kTiltPadSize};
        buttons_.push_back({Control::tilt_pad, 0, pad, "", false});
        tilt_pad_ = pad;
        const int side_x = pad.x + pad.w + kRowGap;
        const int side_w = kPanelWidth - kPanelPad - side_x;
        int side_y = y;
        buttons_.push_back({Control::tilt_flat, 0, {side_x, side_y, side_w, kRowHeight}, "FLAT",
                            false});
        side_y += kRowHeight + kRowGap;
        buttons_.push_back({Control::shake, 0, {side_x, side_y, side_w, kRowHeight}, "SHAKE",
                            false});
        side_y += kRowHeight + kRowGap;
        buttons_.push_back({Control::sound, 0, {side_x, side_y, side_w, kRowHeight},
                            "SOUND (HOLD)", true});
        y += kTiltPadSize + kRowGap;
        y = hint(y, "DRAG THE PAD TO TILT; IT HOLDS");

        y = header(y, "ANCHORED FACE ROTATION");
        y = row(y,
                {{Control::rotate_ccw, 0, {}, "ROT-", false},
                 {Control::rotate_reset, 0, {}, "ROT 0", false},
                 {Control::rotate_cw, 0, {}, "ROT+", false}},
                width);
        y = hint(y, "STEPS OF 15 DEGREES");

        y = header(y, "RASTER ANTIALIASING");
        y = row(y,
                {{Control::aa_mode, 0, {}, "AUTO", false},
                 {Control::aa_mode, 2, {}, "2X", false},
                 {Control::aa_mode, 4, {}, "4X", false},
                 {Control::aa_mode, 8, {}, "8X", false}},
                width);
        y = hint(y, "VERTICAL SUBSAMPLES PER PATH");
        y = row(y, {{Control::display_filter, 0, {}, "", false, 1}}, width);
        y = hint(y, "HOW THIS WINDOW MAGNIFIES THE PANEL");

        y = header(y, "SETTINGS");
        // These run the firmware's own settings, labelled from the firmware's
        // own strings. The device has no menu for them: this panel (and later
        // the phone app) is where they live.
        {
            std::vector<Button> pair;
            for (int setting = 0; setting < eyes::kUiSettingCount; ++setting) {
                pair.push_back({Control::setting, setting, {}, "", false, 1});
            }
            y = row(y, pair, width);
        }

        y = header(y, "SESSION");
        y = row(y, {{Control::capture, 0, {}, "CAPTURE PPM", false}}, width);

        state_y_ = y + kRowGap;
        // "STATE" caption plus ten readout lines.
        height_ = state_y_ + 12 + 10 * 10 + kPanelPad;
    }

    const std::vector<Button> &buttons() const { return buttons_; }
    const Rect &tilt_pad() const { return tilt_pad_; }
    int state_y() const { return state_y_; }
    int height() const { return height_; }

  private:
    int header(int y, const char *title)
    {
        headers_.push_back({y, title});
        return y + kHeaderHeight;
    }

    int hint(int y, const char *text)
    {
        hints_.push_back({y, text});
        return y + 10;
    }

    int row(int y, const std::vector<Button> &entries, int width)
    {
        const int count = static_cast<int>(entries.size());
        if (count == 0) {
            return y;
        }
        const int each = (width - (count - 1) * kRowGap) / count;
        for (int index = 0; index < count; ++index) {
            Button button = entries[static_cast<std::size_t>(index)];
            button.rect = {kPanelPad + index * (each + kRowGap), y, each, kRowHeight};
            buttons_.push_back(button);
        }
        return y + kRowHeight + kRowGap;
    }

  public:
    struct Caption {
        int y;
        const char *text;
    };

    const std::vector<Caption> &headers() const { return headers_; }
    const std::vector<Caption> &hints() const { return hints_; }

  private:
    std::vector<Button> buttons_{};
    std::vector<Caption> headers_{};
    std::vector<Caption> hints_{};
    Rect tilt_pad_{};
    int state_y_{0};
    int height_{0};
};

// What the panel needs to label and highlight itself.
struct PanelState {
    Control held{Control::none};
    int held_row{-1};
    bool boot_down{false};
    bool power_down{false};
    bool sound_down{false};
    eyes::Vec2 tilt{};
    int fps{0};
    int captures{0};
};

// The panel's drop shadow, baked once into a transparent layer and composited
// wherever the card currently sits.
void build_panel_shadow(Layer &layer, int card_width, int card_height, int margin)
{
    const int width = card_width + margin * 2;
    const int height = card_height + margin * 2;
    layer = Layer(width, height);
    Canvas scratch(width, height);
    scratch.clear(0x00000000U);
    scratch.soft_shadow(static_cast<float>(width) * 0.5F,
                        static_cast<float>(height) * 0.5F + 16.0F,
                        static_cast<float>(card_width) * 0.78F,
                        static_cast<float>(card_height) * 0.62F, 0.55F);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            layer.set(x, y, scratch.pixels()[static_cast<std::size_t>(y) *
                                                 static_cast<std::size_t>(width) +
                                             static_cast<std::size_t>(x)]);
        }
    }
}

void draw_panel(Canvas &canvas, const Panel &panel, int origin_x, int origin_y,
                const eyes::DeviceUi &ui, const eyes::EyeEngine &engine,
                const eyes::EyeRenderer &renderer, const PanelState &state, int saves,
                bool smooth)
{
    // A floating card. Its drop shadow is prerendered by the caller, since it
    // depends only on the card's size and would otherwise be the single most
    // expensive thing drawn each frame.
    const Rect card{origin_x, origin_y, kPanelWidth, panel.height()};
    canvas.rounded_rect(card, 14.0F, kColorPanel, 0.96F);
    canvas.rounded_rect({card.x, card.y, card.w, 26}, 14.0F, 0xFF1B1F26U, 0.96F);
    canvas.text(card.x + kPanelPad, card.y + 9, "DEBUG PANEL  (DRAG THIS BAR, TAB HIDES)",
                kColorHint, 1);

    for (const Panel::Caption &caption : panel.headers()) {
        canvas.text(origin_x + kPanelPad, origin_y + caption.y + 6, caption.text, kColorHeader, 1);
        canvas.fill({origin_x + kPanelPad, origin_y + caption.y + 18,
                     kPanelWidth - kPanelPad * 2, 1},
                    kColorEdge);
    }
    for (const Panel::Caption &caption : panel.hints()) {
        canvas.text(origin_x + kPanelPad, origin_y + caption.y, caption.text, kColorHint, 1);
    }

    char label[48];
    for (const Button &button : panel.buttons()) {
        Rect rect = button.rect;
        rect.x += origin_x;
        rect.y += origin_y;
        if (button.control == Control::tilt_pad) {
            canvas.fill(rect, 0xFF0E1013U);
            canvas.outline(rect, kColorEdge);
            // Crosshair, then the current gravity vector as a dot.
            canvas.fill({rect.x + rect.w / 2, rect.y + 4, 1, rect.h - 8}, kColorEdge);
            canvas.fill({rect.x + 4, rect.y + rect.h / 2, rect.w - 8, 1}, kColorEdge);
            const int cx = rect.x + rect.w / 2 +
                           static_cast<int>(state.tilt.x * static_cast<float>(rect.w / 2 - 6));
            const int cy = rect.y + rect.h / 2 +
                           static_cast<int>(state.tilt.y * static_cast<float>(rect.h / 2 - 6));
            canvas.fill({cx - 4, cy - 4, 9, 9}, kColorValue);
            canvas.text(rect.x + 5, rect.y + 4, "TILT", kColorHint, 1);
            continue;
        }

        const char *text = button.label;
        if (button.control == Control::setting) {
            ui.setting_text(static_cast<eyes::UiSetting>(button.row), label, sizeof(label));
            text = label;
        }
        if (button.control == Control::display_filter) {
            std::snprintf(label, sizeof(label), "MAGNIFY: %s", smooth ? "SMOOTH" : "PANEL PIXELS");
            text = label;
        }

        bool held = false;
        if (button.control == Control::boot) {
            held = state.boot_down;
        } else if (button.control == Control::power) {
            held = state.power_down;
        } else if (button.control == Control::sound) {
            held = state.sound_down;
        } else if (button.control == Control::display_filter) {
            held = smooth;
        } else if (button.control == Control::aa_mode) {
            held = renderer.aa_override() == button.row;
        } else if (button.control == state.held) {
            held = button.control != Control::setting || button.row == state.held_row;
        }
        canvas.fill(rect, held ? (button.is_hold ? kColorButtonHeld : kColorButtonHot)
                               : kColorButton);
        canvas.outline(rect, kColorEdge);
        const int scale =
            Canvas::text_width(text, button.text_scale) <= rect.w - 8 ? button.text_scale : 1;
        canvas.text_centered(rect, text, kColorLabel, scale);
    }

    // --- live state readout --------------------------------------------------
    const eyes::FrameState &frame = engine.frame();
    const eyes::Selection selection = engine.selection();
    int y = origin_y + panel.state_y();
    char buffer[80];
    const auto put = [&](const char *text) {
        canvas.text(origin_x + kPanelPad, y, text, kColorValue, 1);
        y += 10;
    };
    canvas.text(origin_x + kPanelPad, y, "STATE", kColorHeader, 1);
    y += 12;
    std::snprintf(buffer, sizeof(buffer), "MODE %s  EXPR %s", mode_name(frame.mode),
                  expression_name(frame.expression));
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "ATTN %s  PAGE %s",
                  eyes::ui_attention_name(frame.attention), page_name(ui.page()));
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "LOOK %s / %s", eyes::shape_at(selection.shape).name,
                  eyes::palette_at(selection.palette).name);
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "FACE %s  INK %s",
                  renderer.face_mode() == eyes::FaceMode::capsule ? "CAPSULE" : "CREATURE",
                  renderer.capsule_invert() ? "EYES" : "DISC");
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "GAZE %+.2f %+.2f  PUPL %+.2f %+.2f",
                  static_cast<double>(frame.gaze.x), static_cast<double>(frame.gaze.y),
                  static_cast<double>(frame.gaze_pupils.x),
                  static_cast<double>(frame.gaze_pupils.y));
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "TILT %+.2f %+.2f  IMU %+.2f %+.2f",
                  static_cast<double>(state.tilt.x), static_cast<double>(state.tilt.y),
                  static_cast<double>(engine.imu_gaze_target().x),
                  static_cast<double>(engine.imu_gaze_target().y));
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "SENS %s  ROT %+d DEG",
                  eyes::kImuLevelNames[ui.settings().imu_level % 3U],
                  static_cast<int>(std::lround(static_cast<double>(engine.orientation()) * 180.0 /
                                               3.14159265)));
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "BLINK %.2f  POKE %.2f  %s",
                  static_cast<double>(frame.blink_open), static_cast<double>(frame.poke),
                  frame.morph_active ? "MORPHING" : "");
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "AA %s  ASKED %dX  LINES/ROW %d",
                  renderer.aa_override() == 0 ? "AUTO" : "PINNED", renderer.last_aa_subsamples(),
                  renderer.last_aa_lines());
    put(buffer);
    std::snprintf(buffer, sizeof(buffer), "%d FPS  SAVES %d", state.fps, saves);
    put(buffer);
}

// --- shared simulation ------------------------------------------------------

bool write_ppm(const std::string &path, const std::vector<std::uint16_t> &pixels)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream << "P6\n" << eyes::kScreenWidth << ' ' << eyes::kScreenHeight << "\n255\n";
    for (const std::uint16_t pixel : pixels) {
        const char rgb[3]{
            static_cast<char>(expand5(static_cast<std::uint16_t>((pixel >> 11U) & 0x1fU))),
            static_cast<char>(expand6(static_cast<std::uint16_t>((pixel >> 5U) & 0x3fU))),
            static_cast<char>(expand5(static_cast<std::uint16_t>(pixel & 0x1fU))),
        };
        stream.write(rgb, 3);
    }
    return true;
}

bool write_canvas_ppm(const std::string &path, const Canvas &canvas)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream << "P6\n" << canvas.width() << ' ' << canvas.height() << "\n255\n";
    // PPM has no alpha channel, so composite over a checkerboard: it makes the
    // window's transparency visible in a still.
    const std::uint32_t *pixels = canvas.pixels();
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const std::uint32_t argb =
                pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width()) +
                       static_cast<std::size_t>(x)];
            const float alpha = static_cast<float>(argb >> 24U) / 255.0F;
            const bool light = (((x / 16) + (y / 16)) & 1) != 0;
            const float back = light ? 96.0F : 72.0F;
            const auto channel = [&](int shift) {
                const float source = static_cast<float>((argb >> shift) & 0xFFU);
                return static_cast<char>(source * alpha + back * (1.0F - alpha));
            };
            const char rgb[3]{channel(16), channel(8), channel(0)};
            stream.write(rgb, 3);
        }
    }
    return true;
}

// The digital crown (Track B hardware) demonstrated now via a sim fake, the
// way the webcam stands in for the future cameras: the mouse scroll wheel and
// a press key feed the real CrownService interface, so the crown UX is built
// and proven before the TMAG3001/PixArt part exists. Thread-safe-enough for
// the single-threaded sim loop (fed and read on the same thread).
class SimCrownService final : public eyes::CrownService {
public:
    bool available() const override { return true; }
    float take_rotation() override
    {
        const float r = accum_;
        accum_ = 0.0F;
        return r;
    }
    bool pressed() const override { return pressed_; }

    // Called from the SDL event loop.
    void add_detents(float detents) { accum_ += detents; }
    void set_pressed(bool down) { pressed_ = down; }

private:
    float accum_{0.0F};
    bool pressed_{false};
};

// The product's LRA/ERM motor standing in as a brief window shake, so haptic
// feedback is designed and timed against the real HapticsService interface
// before the motor exists. duration_ms/strength drive how long and how far the
// window jolts; the loop reads take_offset() each frame and nudges SDL's window
// position, decaying back to rest. A no-op when SDL can't move the window
// (headless) — the interface contract still holds.
class SimHapticsService final : public eyes::HapticsService {
public:
    bool available() const override { return true; }
    void pulse(std::uint32_t duration_ms, float strength) override
    {
        // Clamp to the range a real LRA can actually express; longer buzzes
        // read as "stuck" on the wrist, so cap at a firm double-tap length.
        const std::uint32_t d = duration_ms > 220U ? 220U : duration_ms;
        const float s = strength < 0.0F ? 0.0F : (strength > 1.0F ? 1.0F : strength);
        remaining_ms_ = d;
        amplitude_px_ = 1.0F + s * 5.0F; // 1..6 px throw, matched to a felt tap
    }

    // Called once per rendered frame with the elapsed time; returns the pixel
    // offset to apply to the window this frame (0,0 at rest). Alternates sign
    // per call so the window buzzes rather than drifts.
    void take_offset(std::uint32_t dt_ms, int &dx, int &dy)
    {
        if (remaining_ms_ == 0U) { dx = 0; dy = 0; return; }
        remaining_ms_ = dt_ms >= remaining_ms_ ? 0U : remaining_ms_ - dt_ms;
        phase_ = !phase_;
        const int mag = static_cast<int>(amplitude_px_);
        dx = phase_ ? mag : -mag;
        dy = phase_ ? -mag : mag;
        if (remaining_ms_ == 0U) { dx = 0; dy = 0; } // settle exactly at rest
    }
    bool buzzing() const { return remaining_ms_ > 0U; }

private:
    std::uint32_t remaining_ms_{0U};
    float amplitude_px_{0.0F};
    bool phase_{false};
};

#ifdef EYES_SIM_MAC
// The Mac's webcam standing in for both of the product's lenses through the
// real CameraService interface, so camera apps can be written against
// hardware that does not exist yet. Delivers panel-sized RGB565 frames;
// center-crops the largest square and mirrors the front lens like every
// selfie view.
class WebcamCameraService final : public eyes::CameraService {
public:
    bool available(Lens) const override { return !failed_; }

    bool start(Lens) override
    {
        if (failed_) {
            return false;
        }
        if (!started_) {
            started_ = eyes_mac_camera_start() != 0;
            failed_ = !started_;
            if (failed_) {
                std::printf("webcam unavailable (no device, or camera access denied)\n");
            }
        }
        return started_;
    }

    void stop(Lens) override
    {
        if (started_) {
            eyes_mac_camera_stop();
            started_ = false;
        }
    }

    bool capture(Lens lens, eyes::CameraFrame &frame) override
    {
        if (!start(lens)) {
            return false;
        }
        bgra_.resize(kMaxSourceBytes);
        int width = 0;
        int height = 0;
        if (eyes_mac_camera_latest(bgra_.data(), static_cast<int>(bgra_.size()), &width,
                                   &height) == 0) {
            return false;
        }
        const bool mirror = lens == Lens::front;
        const int side = std::min(width, height);
        const int x0 = (width - side) / 2;
        const int y0 = (height - side) / 2;
        rgb565_.resize(static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight);
        for (int y = 0; y < eyes::kScreenHeight; ++y) {
            const int sy = y0 + y * side / eyes::kScreenHeight;
            const unsigned char *row =
                bgra_.data() + static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) * 4U;
            for (int x = 0; x < eyes::kScreenWidth; ++x) {
                const int column = mirror ? eyes::kScreenWidth - 1 - x : x;
                const int sx = x0 + column * side / eyes::kScreenWidth;
                const unsigned char *px = row + static_cast<std::size_t>(sx) * 4U;  // BGRA
                const unsigned red = px[2];
                const unsigned green = px[1];
                const unsigned blue = px[0];
                rgb565_[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                        static_cast<std::size_t>(x)] =
                    static_cast<std::uint16_t>(((red >> 3U) << 11U) | ((green >> 2U) << 5U) |
                                               (blue >> 3U));
            }
        }
        frame.data = reinterpret_cast<const std::uint8_t *>(rgb565_.data());
        frame.width = eyes::kScreenWidth;
        frame.height = eyes::kScreenHeight;
        frame.format = eyes::CameraFrame::Format::rgb565;
        return true;
    }

private:
    // The session asks for 640x480; sized so any webcam's frames still fit.
    static constexpr int kMaxSourceBytes = 1920 * 1080 * 4;
    bool started_{false};
    bool failed_{false};
    std::vector<unsigned char> bgra_;
    std::vector<std::uint16_t> rgb565_;
};
#endif

// State-transition styles for the viewfinder (and, later, app switches).
// All read as "incoming view over the face"; blend is the raw 0..1 position,
// the ease lives inside each style. T cycles them in the window.
// Apple-style timing asymmetry: opens settle in (~300 ms decelerating),
// closes get out of the way (~220 ms accelerating).
constexpr float kCameraOpenMs = 300.0F;
constexpr float kCameraCloseMs = 220.0F;

enum class TransitionStyle : std::uint8_t { zoom, eyes, iris, slide, fade, count };

constexpr const char *kTransitionNames[] = {"ZOOM", "EYES", "IRIS", "SLIDE", "FADE"};

// Apple-camera shutter: red inner on a one-color liquid-glass puck (the
// camera frame blurred through it) with a single border ring, bottom centre.
constexpr float kShutterX = 233.0F;
constexpr float kShutterY = 352.0F;
constexpr float kShutterRadius = 33.0F;
constexpr float kShutterRingOuter = 44.0F;
// The photo well sits beside the shutter on the same row, iOS camera style.
// The "white border" is really the well's background showing as 2 px padding.
constexpr float kWellX = 330.0F;
constexpr float kWellY = 352.0F;
constexpr float kWellHalf = 26.0F;
constexpr float kWellCorner = 9.0F;
constexpr float kWellPad = 2.0F;

// liquid-taffy's HOUSE spring (zeta 0.434, omega 22.46 rad/s over ~0.30 s),
// normalized to t in [0,1]: overshoots ~22% and rings once before settling.
// The house bounce for everything that arrives.
inline float house_spring(float t)
{
    constexpr float kZeta = 0.434F;
    constexpr float kOmega = 6.74F;  // 22.46 rad/s * 0.30 s
    const float root = std::sqrt(1.0F - kZeta * kZeta);
    const float damped = kOmega * root;
    return 1.0F - std::exp(-kZeta * kOmega * t) *
                      (std::cos(damped * t) + (kZeta / root) * std::sin(damped * t));
}

inline std::uint16_t lerp565(std::uint16_t under, std::uint16_t over, float amount)
{
    const int ur = (under >> 11) & 31;
    const int ug = (under >> 5) & 63;
    const int ub = under & 31;
    const int red = ur + static_cast<int>(static_cast<float>(((over >> 11) & 31) - ur) * amount);
    const int green = ug + static_cast<int>(static_cast<float>(((over >> 5) & 63) - ug) * amount);
    const int blue = ub + static_cast<int>(static_cast<float>((over & 31) - ub) * amount);
    return static_cast<std::uint16_t>((red << 11) | (green << 5) | blue);
}

void overlay_camera_transition(std::uint16_t *dst, const std::uint16_t *src, float blend,
                               TransitionStyle style, bool opening)
{
    // Opening is a two-beat launch: ooze (a slow bulge to ~35%, surface
    // tension) then the house spring fires the rest — overshoots slightly,
    // rings once, settles. Closing anticipates nothing and accelerates away
    // (ease-in quadratic). Arriving has mass, leaving gets out of the way.
    float eased;
    if (opening) {
        constexpr float kOozeEnd = 0.38F;   // first beat's share of the move
        constexpr float kOozeRise = 0.35F;  // how far the bulge gets
        if (blend < kOozeEnd) {
            const float u = blend / kOozeEnd;
            eased = kOozeRise * u * u * (3.0F - 2.0F * u);
        } else {
            const float u = (blend - kOozeEnd) / (1.0F - kOozeEnd);
            eased = kOozeRise + (1.0F - kOozeRise) * house_spring(u);
        }
    } else {
        eased = blend * blend;
    }
    const float cx = static_cast<float>(eyes::kScreenWidth) * 0.5F;
    const float cy = static_cast<float>(eyes::kScreenHeight) * 0.5F;
    switch (style) {
        case TransitionStyle::iris: {
            // Aperture circle opening from the centre: camera language and
            // eye language at once, feathered at the rim.
            const float full = std::sqrt(cx * cx + cy * cy) + 2.0F;
            const float radius = eased * full;
            const float feather = 6.0F;
            const float inner = std::max(0.0F, radius - feather);
            const float inner_squared = inner * inner;
            const float radius_squared = radius * radius;
            for (int y = 0; y < eyes::kScreenHeight; ++y) {
                const float dy = static_cast<float>(y) + 0.5F - cy;
                for (int x = 0; x < eyes::kScreenWidth; ++x) {
                    const float dx = static_cast<float>(x) + 0.5F - cx;
                    const float distance_squared = dx * dx + dy * dy;
                    if (distance_squared >= radius_squared) {
                        continue;
                    }
                    const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                              static_cast<std::size_t>(x);
                    if (distance_squared <= inner_squared) {
                        dst[index] = src[index];
                    } else {
                        const float coverage =
                            (radius - std::sqrt(distance_squared)) / feather;
                        dst[index] = lerp565(dst[index], src[index], coverage);
                    }
                }
            }
            break;
        }
        case TransitionStyle::zoom: {
            // The watchOS app-launch feel: the incoming view grows from the
            // centre as a DISC (the panel is round; a square overlay shows
            // its corners), fading with the same blend curve both directions
            // so opening and closing are mirror images.
            // The spring rings past 1; the disc clips at the panel while the
            // content zoom carries the visible bounce. Alpha never overshoots.
            const float alpha = std::clamp(eased, 0.0F, 1.0F);
            if (alpha <= 0.0F) {
                break;
            }
            const float disc_radius = (0.55F + 0.45F * eased) * cx;  // panel radius, scaled
            // The content starts punched in ~8% and relaxes to 1:1 as it
            // arrives — the iOS camera-open settle. sample scale maps a dst
            // offset into the (cropped) source: larger zoom = tighter crop.
            const float content_zoom = 1.08F - 0.08F * eased;
            const float scale = disc_radius * content_zoom / cx;
            const float feather = 4.0F;
            const float inner = std::max(0.0F, disc_radius - feather);
            const float inner_squared = inner * inner;
            const float radius_squared = disc_radius * disc_radius;
            for (int y = 0; y < eyes::kScreenHeight; ++y) {
                const float dy = static_cast<float>(y) + 0.5F - cy;
                const float sy = cy + dy / scale;
                if (sy < 0.0F || sy >= static_cast<float>(eyes::kScreenHeight)) {
                    continue;
                }
                const std::size_t src_row = static_cast<std::size_t>(sy) * eyes::kScreenWidth;
                for (int x = 0; x < eyes::kScreenWidth; ++x) {
                    const float dx = static_cast<float>(x) + 0.5F - cx;
                    const float distance_squared = dx * dx + dy * dy;
                    if (distance_squared >= radius_squared) {
                        continue;
                    }
                    const float sx = cx + dx / scale;
                    if (sx < 0.0F || sx >= static_cast<float>(eyes::kScreenWidth)) {
                        continue;
                    }
                    float coverage = alpha;
                    if (distance_squared > inner_squared) {
                        coverage *= (disc_radius - std::sqrt(distance_squared)) / feather;
                    }
                    const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                              static_cast<std::size_t>(x);
                    dst[index] =
                        lerp565(dst[index], src[src_row + static_cast<std::size_t>(sx)], coverage);
                }
            }
            break;
        }
        case TransitionStyle::eyes: {
            // The view opens out of the creature itself: two discs growing
            // from the eye positions until they merge and cover the panel.
            const float eye_x[2] = {cx - 72.0F, cx + 72.0F};
            const float radius = eased * 330.0F;
            const float feather = 6.0F;
            const float inner = std::max(0.0F, radius - feather);
            const float inner_squared = inner * inner;
            const float radius_squared = radius * radius;
            for (int y = 0; y < eyes::kScreenHeight; ++y) {
                const float dy = static_cast<float>(y) + 0.5F - cy;
                for (int x = 0; x < eyes::kScreenWidth; ++x) {
                    const float dx0 = static_cast<float>(x) + 0.5F - eye_x[0];
                    const float dx1 = static_cast<float>(x) + 0.5F - eye_x[1];
                    const float distance_squared =
                        std::min(dx0 * dx0 + dy * dy, dx1 * dx1 + dy * dy);
                    if (distance_squared >= radius_squared) {
                        continue;
                    }
                    const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                              static_cast<std::size_t>(x);
                    if (distance_squared <= inner_squared) {
                        dst[index] = src[index];
                    } else {
                        const float coverage = (radius - std::sqrt(distance_squared)) / feather;
                        dst[index] = lerp565(dst[index], src[index], coverage);
                    }
                }
            }
            break;
        }
        case TransitionStyle::slide: {
            // Cover slide from the right, the face-swipe language.
            const int edge =
                std::max(0, static_cast<int>((1.0F - std::min(1.0F, eased)) *
                                             static_cast<float>(eyes::kScreenWidth)));
            for (int y = 0; y < eyes::kScreenHeight; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * eyes::kScreenWidth;
                for (int x = edge; x < eyes::kScreenWidth; ++x) {
                    dst[row + static_cast<std::size_t>(x)] =
                        src[row + static_cast<std::size_t>(x - edge)];
                }
            }
            break;
        }
        case TransitionStyle::fade: {
            // Plain dissolve.
            const float amount = std::clamp(eased, 0.0F, 1.0F);
            const std::size_t total =
                static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight;
            for (std::size_t index = 0; index < total; ++index) {
                dst[index] = lerp565(dst[index], src[index], amount);
            }
            break;
        }
        case TransitionStyle::count:
            break;
    }
}

// The viewfinder chrome, iOS-camera style. The puck is real liquid glass on
// a budget: the camera image under it is box-blurred (5-tap) and tinted one
// flat color, with a single border ring. The red inner is a rounded-rect
// distance field that morphs circle -> rounded-square while a capture is
// absorbed. inner_scale carries the press bounce (squash, then house spring).
void draw_camera_shutter(std::uint16_t *dst, float inner_scale, float morph)
{
    const int x0 = std::max(0, static_cast<int>(kShutterX - kShutterRingOuter) - 4);
    const int x1 = std::min(eyes::kScreenWidth - 1,
                            static_cast<int>(kShutterX + kShutterRingOuter) + 4);
    const int y0 = std::max(0, static_cast<int>(kShutterY - kShutterRingOuter) - 4);
    const int y1 = std::min(eyes::kScreenHeight - 1,
                            static_cast<int>(kShutterY + kShutterRingOuter) + 4);
    constexpr std::uint16_t kRed = 0xF9C6U;   // Apple systemRed #FF3B30 in RGB565
    constexpr std::uint16_t kWhite = 0xFFFFU;
    // Blur must read the unmodified underlay, so snapshot the puck's bbox.
    constexpr int kSnapSize = 104;
    static std::uint16_t snapshot[kSnapSize * kSnapSize];
    const int snap_w = x1 - x0 + 1;
    const int snap_h = y1 - y0 + 1;
    for (int y = 0; y < snap_h; ++y) {
        std::memcpy(snapshot + static_cast<std::size_t>(y) * kSnapSize,
                    dst + static_cast<std::size_t>(y0 + y) * eyes::kScreenWidth + x0,
                    static_cast<std::size_t>(snap_w) * sizeof(std::uint16_t));
    }
    const auto snap = [&](int x, int y) {
        const int sx = std::clamp(x - x0, 0, snap_w - 1);
        const int sy = std::clamp(y - y0, 0, snap_h - 1);
        return snapshot[static_cast<std::size_t>(sy) * kSnapSize + static_cast<std::size_t>(sx)];
    };
    const float half = kShutterRadius * inner_scale * (1.0F - 0.10F * morph);
    const float corner = half * (1.0F - 0.62F * morph);
    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5F - kShutterY;
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - kShutterX;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float glass = kShutterRingOuter + 0.5F - distance;
            if (glass <= 0.0F) {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(y) * eyes::kScreenWidth + static_cast<std::size_t>(x);
            // Refraction (the Aave displacement-map trick, done directly):
            // sample the underlay at bent coordinates — near-1 at the centre,
            // pulling harder toward the rim — so the glass bends the world
            // instead of just frosting it. The red and blue channels bend
            // slightly differently for the faint chromatic fringe.
            const float norm = distance / kShutterRingOuter;
            const float bend = 1.0F - 0.16F * norm * norm * norm;
            const auto refracted = [&](float channel_bend) {
                const int rx = static_cast<int>(kShutterX + dx * channel_bend);
                const int ry = static_cast<int>(kShutterY + dy * channel_bend);
                const std::uint16_t taps[5] = {snap(rx, ry), snap(rx - 3, ry), snap(rx + 3, ry),
                                               snap(rx, ry - 3), snap(rx, ry + 3)};
                unsigned red_sum = 0;
                unsigned green_sum = 0;
                unsigned blue_sum = 0;
                for (const std::uint16_t tap : taps) {
                    red_sum += (tap >> 11) & 31U;
                    green_sum += (tap >> 5) & 63U;
                    blue_sum += tap & 31U;
                }
                return static_cast<std::uint16_t>(((red_sum / 5U) << 11) |
                                                  ((green_sum / 5U) << 5) | (blue_sum / 5U));
            };
            const std::uint16_t bent = refracted(bend);
            const std::uint16_t bent_red = refracted(bend - 0.025F * norm);
            const std::uint16_t bent_blue = refracted(bend + 0.025F * norm);
            const std::uint16_t blurred = static_cast<std::uint16_t>(
                (bent_red & 0xF800U) | (bent & 0x07E0U) | (bent_blue & 0x001FU));
            // One flat tint over the refraction, edge-antialiased, plus a
            // specular kiss on the upper-left for legibility.
            const float coverage = std::min(1.0F, glass);
            std::uint16_t glass_color = lerp565(blurred, kWhite, 0.16F);
            const float specular = -(dx + dy) / (2.0F * kShutterRingOuter);
            if (specular > 0.0F && norm > 0.55F) {
                glass_color =
                    lerp565(glass_color, kWhite, specular * (norm - 0.55F) * 0.9F);
            }
            // Single border ring, one color.
            const float border =
                std::min(coverage, distance - (kShutterRingOuter - 2.0F));
            if (border > 0.0F) {
                glass_color = lerp565(glass_color, kWhite, std::min(1.0F, border) * 0.65F);
            }
            dst[index] = lerp565(dst[index], glass_color, coverage);
            // Red inner via a rounded-rect distance field.
            const float qx = std::max(std::fabs(dx) - (half - corner), 0.0F);
            const float qy = std::max(std::fabs(dy) - (half - corner), 0.0F);
            const float field = std::sqrt(qx * qx + qy * qy) - corner;
            const float inner = 0.5F - field;
            if (inner > 0.0F) {
                dst[index] = lerp565(dst[index], kRed, std::min(1.0F, inner));
            }
        }
    }
}

// The captured photo flying into the well, and the resting thumbnail: a
// squircle whose "white border" is the well background showing as padding.
// progress 0 = full screen, 1 = seated. splat_seconds < 0 means no splat;
// otherwise the well absorbs the landing (stretch, counter-slosh, spring) —
// the doctrine's "whatever lands gets absorbed".
void draw_photo_well(std::uint16_t *dst, const std::uint16_t *photo, float progress,
                     float splat_seconds)
{
    float squash_x = 1.0F;
    float squash_y = 1.0F;
    if (splat_seconds >= 0.0F && splat_seconds < 0.45F) {
        if (splat_seconds < 0.07F) {
            const float u = splat_seconds / 0.07F;
            squash_x = 1.0F + 0.18F * u;
            squash_y = 1.0F - 0.16F * u;
        } else if (splat_seconds < 0.11F) {
            const float u = (splat_seconds - 0.07F) / 0.04F;
            squash_x = 1.18F - 0.23F * u;
            squash_y = 0.84F + 0.22F * u;
        } else {
            const float settle = house_spring((splat_seconds - 0.11F) / 0.34F);
            squash_x = 0.95F + 0.05F * settle;
            squash_y = 1.06F - 0.06F * settle;
        }
    }
    const float eased = progress * progress * (3.0F - 2.0F * progress);
    const float center_x = 233.0F + (kWellX - 233.0F) * eased;
    const float center_y = 233.0F + (kWellY - 233.0F) * eased;
    const float half_x = (233.0F + (kWellHalf - 233.0F) * eased) * squash_x;
    const float half_y = (233.0F + (kWellHalf - 233.0F) * eased) * squash_y;
    const float half = std::max(half_x, half_y);
    const float corner = 2.0F + (kWellCorner - 2.0F) * eased;
    const float pad = kWellPad * eased;
    const float frame_half = half + pad;
    const int x0 = std::max(0, static_cast<int>(center_x - frame_half) - 1);
    const int x1 = std::min(eyes::kScreenWidth - 1, static_cast<int>(center_x + frame_half) + 1);
    const int y0 = std::max(0, static_cast<int>(center_y - frame_half) - 1);
    const int y1 = std::min(eyes::kScreenHeight - 1, static_cast<int>(center_y + frame_half) + 1);
    constexpr std::uint16_t kWhite = 0xFFFFU;
    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5F - center_y;
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - center_x;
            // Frame (the "border" = background with padding).
            const float fqx = std::max(std::fabs(dx) - (half_x + pad - corner), 0.0F);
            const float fqy = std::max(std::fabs(dy) - (half_y + pad - corner), 0.0F);
            const float frame_field = 0.5F - (std::sqrt(fqx * fqx + fqy * fqy) - corner);
            if (frame_field <= 0.0F) {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(y) * eyes::kScreenWidth + static_cast<std::size_t>(x);
            dst[index] = lerp565(dst[index], kWhite, std::min(1.0F, frame_field));
            // Photo inset by the padding, sampled nearest from the capture.
            const float iqx = std::max(std::fabs(dx) - (half_x - corner), 0.0F);
            const float iqy = std::max(std::fabs(dy) - (half_y - corner), 0.0F);
            const float image_field = 0.5F - (std::sqrt(iqx * iqx + iqy * iqy) - corner);
            if (image_field > 0.0F) {
                const int sx = std::clamp(
                    static_cast<int>((dx / (half_x * 2.0F) + 0.5F) *
                                     static_cast<float>(eyes::kScreenWidth)),
                    0, eyes::kScreenWidth - 1);
                const int sy = std::clamp(
                    static_cast<int>((dy / (half_y * 2.0F) + 0.5F) *
                                     static_cast<float>(eyes::kScreenHeight)),
                    0, eyes::kScreenHeight - 1);
                const std::uint16_t sample =
                    photo[static_cast<std::size_t>(sy) * eyes::kScreenWidth +
                          static_cast<std::size_t>(sx)];
                dst[index] = lerp565(dst[index], sample, std::min(1.0F, image_field));
            }
        }
    }
}

// A small refracting glass lens, reusable chrome (the Aave slider-thumb
// recipe): rounded-rect SDF, underlay sampled at bent coordinates with a
// touch of blur, one flat tint, single border.
void draw_glass_lens(std::uint16_t *dst, float cx, float cy, float half_w, float half_h,
                     float corner)
{
    const int x0 = std::max(0, static_cast<int>(cx - half_w) - 4);
    const int x1 = std::min(eyes::kScreenWidth - 1, static_cast<int>(cx + half_w) + 4);
    const int y0 = std::max(0, static_cast<int>(cy - half_h) - 4);
    const int y1 = std::min(eyes::kScreenHeight - 1, static_cast<int>(cy + half_h) + 4);
    constexpr int kSnapSize = 160;
    static std::uint16_t snapshot[kSnapSize * kSnapSize];
    const int snap_w = std::min(x1 - x0 + 1, kSnapSize);
    const int snap_h = std::min(y1 - y0 + 1, kSnapSize);
    for (int y = 0; y < snap_h; ++y) {
        std::memcpy(snapshot + static_cast<std::size_t>(y) * kSnapSize,
                    dst + static_cast<std::size_t>(y0 + y) * eyes::kScreenWidth + x0,
                    static_cast<std::size_t>(snap_w) * sizeof(std::uint16_t));
    }
    const auto snap = [&](int x, int y) {
        const int sx = std::clamp(x - x0, 0, snap_w - 1);
        const int sy = std::clamp(y - y0, 0, snap_h - 1);
        return snapshot[static_cast<std::size_t>(sy) * kSnapSize + static_cast<std::size_t>(sx)];
    };
    constexpr std::uint16_t kWhite = 0xFFFFU;
    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5F - cy;
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float qx = std::max(std::fabs(dx) - (half_w - corner), 0.0F);
            const float qy = std::max(std::fabs(dy) - (half_h - corner), 0.0F);
            const float field = std::sqrt(qx * qx + qy * qy) - corner;
            const float coverage = std::min(1.0F, 0.5F - field);
            if (coverage <= 0.0F) {
                continue;
            }
            const float norm = std::min(
                1.0F, std::max(std::fabs(dx) / half_w, std::fabs(dy) / half_h));
            const float bend = 1.0F - 0.14F * norm * norm * norm;
            const int rx = static_cast<int>(cx + dx * bend);
            const int ry = static_cast<int>(cy + dy * bend);
            const std::uint16_t taps[5] = {snap(rx, ry), snap(rx - 2, ry), snap(rx + 2, ry),
                                           snap(rx, ry - 2), snap(rx, ry + 2)};
            unsigned red_sum = 0;
            unsigned green_sum = 0;
            unsigned blue_sum = 0;
            for (const std::uint16_t tap : taps) {
                red_sum += (tap >> 11) & 31U;
                green_sum += (tap >> 5) & 63U;
                blue_sum += tap & 31U;
            }
            std::uint16_t glass = static_cast<std::uint16_t>(
                ((red_sum / 5U) << 11) | ((green_sum / 5U) << 5) | (blue_sum / 5U));
            glass = lerp565(glass, kWhite, 0.14F);
            if (field > -1.6F) {  // single border band at the rim
                glass = lerp565(glass, kWhite, 0.5F);
            }
            const std::size_t index =
                static_cast<std::size_t>(y) * eyes::kScreenWidth + static_cast<std::size_t>(x);
            dst[index] = lerp565(dst[index], glass, coverage);
        }
    }
}

// Geist text lives in the core now (eyes/geist_text.hpp) so the onboarding
// surface draws the same copy on the panel; the simulator's surfaces use it
// unqualified as before.
using eyes::draw_geist;
using eyes::draw_geist_centered;
using eyes::geist_width;


// Photo | Video mode tab UNDER the shutter row, in the wide band above the
// panel's bottom edge: a glass strip with a travelling liquid pill (squashes
// along its travel, elastic recovery — the taffy PillTabs recipe). pill_pos
// is 0 (photo) .. 1 (video); pill_squash grows while the pill is in flight.
constexpr float kTabY = 428.0F;
constexpr float kTabHalfW = 88.0F;
constexpr float kTabHalfH = 20.0F;

void draw_mode_tab(std::uint16_t *dst, float pill_pos, float pill_squash)
{
    constexpr std::uint16_t kWhite = 0xFFFFU;
    constexpr std::uint16_t kBlack = 0x0000U;
    const float cx = kShutterX;
    // Glass strip: darkened one-color pill with a faint border.
    const int x0 = static_cast<int>(cx - kTabHalfW) - 1;
    const int x1 = static_cast<int>(cx + kTabHalfW) + 1;
    const int y0 = static_cast<int>(kTabY - kTabHalfH) - 1;
    const int y1 = static_cast<int>(kTabY + kTabHalfH) + 1;
    const float strip_corner = kTabHalfH;
    for (int y = std::max(0, y0); y <= std::min(eyes::kScreenHeight - 1, y1); ++y) {
        const float dy = static_cast<float>(y) + 0.5F - kTabY;
        for (int x = std::max(0, x0); x <= std::min(eyes::kScreenWidth - 1, x1); ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float qx = std::max(std::fabs(dx) - (kTabHalfW - strip_corner), 0.0F);
            const float qy = std::max(std::fabs(dy) - (kTabHalfH - strip_corner), 0.0F);
            const float field = 0.5F - (std::sqrt(qx * qx + qy * qy) - strip_corner);
            if (field <= 0.0F) {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(y) * eyes::kScreenWidth + static_cast<std::size_t>(x);
            dst[index] = lerp565(dst[index], kBlack, std::min(1.0F, field) * 0.45F);
        }
    }
    // The travelling pill: squashes flat while in flight, elastic on arrival.
    const float pill_cx = cx - kTabHalfW * 0.5F + kTabHalfW * pill_pos;
    const float pill_half_w = 42.0F * (1.0F + 0.22F * pill_squash);
    const float pill_half_h = 16.0F * (1.0F - 0.18F * pill_squash);
    const float pill_corner = pill_half_h;
    const int px0 = static_cast<int>(pill_cx - pill_half_w) - 1;
    const int px1 = static_cast<int>(pill_cx + pill_half_w) + 1;
    for (int y = std::max(0, y0); y <= std::min(eyes::kScreenHeight - 1, y1); ++y) {
        const float dy = static_cast<float>(y) + 0.5F - kTabY;
        for (int x = std::max(0, px0); x <= std::min(eyes::kScreenWidth - 1, px1); ++x) {
            const float dx = static_cast<float>(x) + 0.5F - pill_cx;
            const float qx = std::max(std::fabs(dx) - (pill_half_w - pill_corner), 0.0F);
            const float qy = std::max(std::fabs(dy) - (pill_half_h - pill_corner), 0.0F);
            const float field = 0.5F - (std::sqrt(qx * qx + qy * qy) - pill_corner);
            if (field > 0.0F) {
                const std::size_t index =
                    static_cast<std::size_t>(y) * eyes::kScreenWidth +
                    static_cast<std::size_t>(x);
                dst[index] = lerp565(dst[index], kWhite, std::min(1.0F, field) * 0.30F);
            }
        }
    }
    // Geist labels, regular case, centred in each half; active side brighter.
    const float photo_active = 1.0F - pill_pos;
    draw_geist_centered(dst, static_cast<int>(cx - kTabHalfW * 0.5F), static_cast<int>(kTabY),
                        "Photo", kWhite, 0.5F + 0.5F * photo_active, false);
    draw_geist_centered(dst, static_cast<int>(cx + kTabHalfW * 0.5F), static_cast<int>(kTabY),
                        "Video", kWhite, 0.5F + 0.5F * pill_pos, false);
}

#ifdef EYES_SIM_MAC
struct MediaSnapshot {
    bool active{false};
    bool playing{false};
    bool spotify{false};
    std::string title;
    std::string artist;
    float position{0.0F};
    float duration{0.0F};
    std::chrono::steady_clock::time_point taken{};
};

// Real now-playing data: polls Spotify and Apple Music through AppleScript on
// a background thread once a second; transport commands go out fire-and-
// forget the same way. The thread starts lazily on first use.
class MediaPoller {
public:
    ~MediaPoller()
    {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void ensure_started()
    {
        if (!thread_.joinable()) {
            thread_ = std::thread([this] { loop(); });
        }
    }

    MediaSnapshot snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    void play_pause(bool spotify)
    {
        run_async(spotify ? "tell application \"Spotify\" to playpause"
                          : "tell application \"Music\" to playpause");
    }

    void seek(bool spotify, float seconds)
    {
        char script[160];
        std::snprintf(script, sizeof(script),
                      "tell application \"%s\" to set player position to %.1f",
                      spotify ? "Spotify" : "Music", static_cast<double>(seconds));
        run_async(script);
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.position = seconds;
        latest_.taken = std::chrono::steady_clock::now();
    }

private:
    static void run_async(std::string script)
    {
        std::thread([moved = std::move(script)] {
            const std::string command = "osascript -e '" + moved + "' >/dev/null 2>&1";
            (void)std::system(command.c_str());
        }).detach();
    }

    static bool query(const char *app, bool duration_in_ms, MediaSnapshot &out)
    {
        char command[640];
        std::snprintf(
            command, sizeof(command),
            "osascript -e 'try' -e 'tell application \"%s\"' -e 'if it is running then' "
            "-e 'return (player state as text) & tab & name of current track & tab & "
            "artist of current track & tab & (player position as text) & tab & "
            "(duration of current track as text)' -e 'end if' -e 'end tell' -e 'end try' "
            "2>/dev/null",
            app);
        FILE *pipe = popen(command, "r");
        if (pipe == nullptr) {
            return false;
        }
        char line[512] = {0};
        const bool got = std::fgets(line, sizeof(line), pipe) != nullptr;
        pclose(pipe);
        if (!got) {
            return false;
        }
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        std::array<std::string, 5> fields;
        std::size_t start = 0;
        for (int field = 0; field < 5; ++field) {
            const std::size_t tab = text.find('\t', start);
            if (tab == std::string::npos) {
                if (field < 4) {
                    return false;
                }
                fields[static_cast<std::size_t>(field)] = text.substr(start);
                break;
            }
            fields[static_cast<std::size_t>(field)] = text.substr(start, tab - start);
            start = tab + 1;
        }
        out.playing = fields[0] == "playing";
        if (!out.playing && fields[0] != "paused") {
            return false;
        }
        out.title = fields[1];
        out.artist = fields[2];
        out.position = std::strtof(fields[3].c_str(), nullptr);
        out.duration = std::strtof(fields[4].c_str(), nullptr);
        if (duration_in_ms) {
            out.duration /= 1000.0F;
        }
        out.active = out.duration > 0.0F;
        return out.active;
    }

    void loop()
    {
        while (!stop_) {
            MediaSnapshot spotify_snap{};
            MediaSnapshot music_snap{};
            const bool have_spotify = query("Spotify", true, spotify_snap);
            const bool have_music = query("Music", false, music_snap);
            MediaSnapshot chosen{};
            if (have_spotify && (spotify_snap.playing || !have_music || !music_snap.playing)) {
                chosen = spotify_snap;
                chosen.spotify = true;
            } else if (have_music) {
                chosen = music_snap;
            }
            chosen.taken = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_ = chosen;
            }
            for (int tick = 0; tick < 10 && !stop_; ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    std::thread thread_;
    std::mutex mutex_;
    MediaSnapshot latest_{};
    std::atomic<bool> stop_{false};
};
#endif

// Owner-consent card: a glass panel with the request and two buttons. Drawn
// over whatever is on screen; the shell owns Approve (green) / Decline.
// Geometry of the swipe-to-confirm capsule (shared by the renderer + touch).
constexpr float kSwipeCx = 233.0F;
constexpr float kSwipeY = 300.0F;
constexpr float kSwipeHalfW = 132.0F;
constexpr float kSwipeHalfH = 26.0F;
constexpr float kSwipeTravelL = kSwipeCx - kSwipeHalfW + kSwipeHalfH;
constexpr float kSwipeTravelR = kSwipeCx + kSwipeHalfW - kSwipeHalfH;
constexpr float kSwipeApprove = 0.9F; // fraction of travel that counts as "sent"
constexpr float kConfirmCancelY = 348.0F;

void draw_confirm_card(std::uint16_t *dst, const std::string &summary, float swipe)
{
    // Dim the backdrop so the card reads as modal.
    const std::size_t total =
        static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight;
    for (std::size_t i = 0; i < total; ++i) {
        dst[i] = lerp565(dst[i], 0x0000U, 0.55F);
    }
    // Wrap the summary to ~22 chars/line, small Geist, centred around y=190.
    std::vector<std::string> lines;
    std::string current;
    std::string word;
    const auto flush_word = [&] {
        if (word.empty()) {
            return;
        }
        if (current.empty()) {
            current = word;
        } else if (current.size() + 1 + word.size() <= 22) {
            current += " " + word;
        } else {
            lines.push_back(current);
            current = word;
        }
        word.clear();
    };
    for (const char c : summary) {
        if (c == ' ') {
            flush_word();
        } else {
            word += c;
        }
    }
    flush_word();
    if (!current.empty()) {
        lines.push_back(current);
    }
    int y = 150 - static_cast<int>(lines.size()) * 12;
    for (const std::string &line : lines) {
        draw_geist_centered(dst, 233, y, line.c_str(), 0xFFFFU, 1.0F, true);
        y += 40;
    }

    // Swipe-to-confirm: a glass capsule you drag right to send. A tap can't fire
    // it — deliberate motion only (money doesn't move by accident). Voice ("say
    // confirm") and gaze/blink remain the hands-free primary; this is the touch
    // fallback. visionOS-style translucent glass, filled behind the thumb.
    const float clamped = swipe < 0.0F ? 0.0F : (swipe > 1.0F ? 1.0F : swipe);
    const float thumbX = kSwipeTravelL + clamped * (kSwipeTravelR - kSwipeTravelL);
    const float left = kSwipeCx - kSwipeHalfW;
    const float right = kSwipeCx + kSwipeHalfW;
    const int W = eyes::kScreenWidth;
    const int H = eyes::kScreenHeight;
    for (int py = static_cast<int>(kSwipeY - kSwipeHalfH); py <= static_cast<int>(kSwipeY + kSwipeHalfH); ++py) {
        if (py < 0 || py >= H) continue;
        for (int px = static_cast<int>(left); px <= static_cast<int>(right); ++px) {
            if (px < 0 || px >= W) continue;
            float dx = 0.0F;
            if (static_cast<float>(px) < left + kSwipeHalfH) dx = (left + kSwipeHalfH) - static_cast<float>(px);
            else if (static_cast<float>(px) > right - kSwipeHalfH) dx = static_cast<float>(px) - (right - kSwipeHalfH);
            const float dy = static_cast<float>(py) - kSwipeY;
            if (dx * dx + dy * dy > kSwipeHalfH * kSwipeHalfH) continue; // rounded ends
            const float tint = static_cast<float>(px) <= thumbX ? 0.30F : 0.13F; // filled trail brighter
            const std::size_t idx = static_cast<std::size_t>(py) * static_cast<std::size_t>(W) + static_cast<std::size_t>(px);
            dst[idx] = lerp565(dst[idx], 0xFFFFU, tint);
        }
    }
    // The thumb (solid white circle).
    for (int py = static_cast<int>(kSwipeY - kSwipeHalfH); py <= static_cast<int>(kSwipeY + kSwipeHalfH); ++py) {
        if (py < 0 || py >= H) continue;
        for (int px = static_cast<int>(thumbX - kSwipeHalfH); px <= static_cast<int>(thumbX + kSwipeHalfH); ++px) {
            if (px < 0 || px >= W) continue;
            const float dx = static_cast<float>(px) - thumbX;
            const float dy = static_cast<float>(py) - kSwipeY;
            if (dx * dx + dy * dy > (kSwipeHalfH - 2.0F) * (kSwipeHalfH - 2.0F)) continue;
            dst[static_cast<std::size_t>(py) * static_cast<std::size_t>(W) + static_cast<std::size_t>(px)] = 0xFFFFU;
        }
    }
    // Label fades out as you drag; once past the threshold, it reads "Release".
    if (clamped >= kSwipeApprove) {
        draw_geist_centered(dst, 233, static_cast<int>(kSwipeY) - 6, "Release to send", 0xFFFFU, 1.0F, false);
    } else if (clamped < 0.6F) {
        draw_geist_centered(dst, 233, static_cast<int>(kSwipeY) - 6, "Swipe to confirm  ›", 0x9FF3U, 1.0F, false);
    }
    // Hands-free primary + a touch decline.
    draw_geist_centered(dst, 233, static_cast<int>(kConfirmCancelY), "or say confirm  ·  Cancel", 0x8410U, 1.0F, false);
}

// A notification banner sliding down from the top: a glass pill with the
// message. progress 0 = fully off-screen above, 1 = seated near the top.
void draw_notification_banner(std::uint16_t *dst, const std::string &text, float progress)
{
    const float eased = progress * progress * (3.0F - 2.0F * progress);
    const float cy = -20.0F + eased * 78.0F;  // slides in from above
    const float half_w = 180.0F;
    const float half_h = 30.0F;
    draw_glass_lens(dst, 233.0F, cy, half_w, half_h, half_h);
    // Trim to fit the pill.
    std::string line = text;
    while (line.size() > 4 && geist_width(line.c_str(), false) > 330) {
        line.resize(line.size() - 4);
        line += "...";
    }
    draw_geist_centered(dst, 233, static_cast<int>(cy), line.c_str(), 0xFFFFU, eased, false);
}

// Transient balance card: the privacy-first reveal. A glass pill seats from
// just above with a small spring, holds long enough to read, then fades — the
// number is never a resident element, so looking away takes it away. phase in
// 0..1 is the fade envelope (0 off-screen/gone, 1 fully seated).
void draw_balance_card(std::uint16_t *dst, const std::string &line,
                       const std::string &sub, float phase)
{
    const float eased = phase * phase * (3.0F - 2.0F * phase);
    const float cy = 233.0F - (1.0F - eased) * 22.0F;  // springs down into place
    draw_glass_lens(dst, 233.0F, cy, 176.0F, 74.0F, 40.0F);
    // Trim the number to the pill rather than wrap — a balance is one glance.
    std::string big = line;
    while (big.size() > 3 && geist_width(big.c_str(), true) > 300) {
        big.resize(big.size() - 1);
    }
    draw_geist_centered(dst, 233, static_cast<int>(cy) - (sub.empty() ? 0 : 14), big.c_str(),
                        0xFFFFU, eased, true);
    if (!sub.empty()) {
        draw_geist_centered(dst, 233, static_cast<int>(cy) + 30, sub.c_str(), 0x9FF3U,
                            eased * 0.75F, false);
    }
}

// Received: a soft mint ring pushes out from the centre while a green "+$"
// rises and fades — the face is the receipt. t in 0..1 is the animation clock.
// One ring, one number, gone in ~1.5 s (motion doctrine: subtle, authored exit).
void draw_funds_received(std::uint16_t *dst, const std::string &amount,
                         const std::string &from, float t)
{
    constexpr std::uint16_t kMint = 0x6794U;  // light green — money-positive
    const float ease = 1.0F - (1.0F - t) * (1.0F - t);  // ease-out expansion
    const float radius = 40.0F + ease * 150.0F;
    const float ring_alpha = std::max(0.0F, 0.55F * (1.0F - t));
    if (ring_alpha > 0.01F) {
        constexpr float thickness = 10.0F;
        const int x0 = std::max(0, static_cast<int>(233.0F - radius - thickness));
        const int x1 =
            std::min(eyes::kScreenWidth - 1, static_cast<int>(233.0F + radius + thickness));
        const int y0 = std::max(0, static_cast<int>(233.0F - radius - thickness));
        const int y1 =
            std::min(eyes::kScreenHeight - 1, static_cast<int>(233.0F + radius + thickness));
        for (int y = y0; y <= y1; ++y) {
            const float dy = static_cast<float>(y) + 0.5F - 233.0F;
            for (int x = x0; x <= x1; ++x) {
                const float dx = static_cast<float>(x) + 0.5F - 233.0F;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float edge = std::fabs(d - radius);
                if (edge > thickness) {
                    continue;
                }
                const float band = (1.0F - edge / thickness) * ring_alpha;
                const std::size_t index = static_cast<std::size_t>(y) * eyes::kScreenWidth +
                                          static_cast<std::size_t>(x);
                dst[index] = lerp565(dst[index], kMint, band);
            }
        }
    }
    // Amount rises ~30 px and fades over the tail; a quick lead-in so it pops.
    const float rise = t * 30.0F;
    const float txt_alpha =
        t < 0.15F ? t / 0.15F : std::max(0.0F, 1.0F - (t - 0.15F) / 0.85F);
    const int cy = static_cast<int>(210.0F - rise);
    draw_geist_centered(dst, 233, cy, amount.c_str(), kMint, txt_alpha, true);
    if (!from.empty()) {
        draw_geist_centered(dst, 233, cy + 34, from.c_str(), 0x9FF3U, txt_alpha * 0.8F, false);
    }
}

// Engine, renderer, shared device UI and the faked hardware around them.
struct Simulation {
    std::vector<std::uint16_t> pixels;
    eyes::EyeRenderer renderer;
    eyes::EyeEngine engine{};
    SimHost host{};
    eyes::DeviceUi ui;
    FakeSensors sensors{};
    GestureRunner gesture{};
    // The OS service handle: null backends for hardware the board lacks; the
    // window path swaps in the webcam-backed camera on macOS.
    eyes::Services services = eyes::Services::waveshare_amoled_175c();
    // First-boot onboarding (eyes/os/onboarding.hpp, shared with the device):
    // "hey" by hand, then a name and a wake word by voice, or the app claims
    // the pairing QR. Persisted to ~/.lilguy/device.json; a finished identity
    // skips it. begin() waits for the first frame so the pen runs on the
    // loop's clock.
    eyes::Onboarding onboarding;
    eyes::DeviceIdentity identity;
    bool onboarding_begin_pending{false};
    bool force_onboarding{false};  // LILGUY_SIM_ONBOARDING=1: greet regardless of the file
    std::string device_name;
    std::string wake_word{"hi podbot"};  // always "hi <name>"; onboarding derives it
    // Settings (G): a botOS home for the handful of things a buttonless device
    // still needs a switch for. Crown scrolls the rows, crown-press toggles the
    // selected one; all persisted next to name/wake in device.json. Voice and
    // wake mirror the env/keys but the toggle wins once the owner sets it.
    bool set_voice{false};    // device speaks replies aloud (LILGUY_DEVICE_TTS default)
    bool set_wake{true};      // hands-free wake-word listening (voice-activated by default)
    bool set_haptics{true};   // the motor buzzes on alerts/confirm/crown
    int set_bright{2};        // 0 dim, 1 mid, 2 bright — drives backlight/dim
    int set_tilt{1};          // 0 low, 1 mid, 2 high — gaze/tilt sensitivity
    // The action trigger the owner picks: what fires a click/shutter/confirm.
    // 0 blink, 1 wink, 2 snap, 3 voice. Shared with the Mac gesture watcher
    // (mac-mcp reads it from device.json) so "blink is the click" is settable.
    int set_trigger{0};
    // Anti-spoof: require a live face (camera) to approve sensitive actions.
    bool set_liveness{false};
    // Speaker-ID: require the owner's voice to match to approve (cloud/local).
    bool set_voiceid{false};
    // Customizable trigger hold length in ms (Face-ID-style, changeable anytime):
    // how long a blink must be held to count. Index into a preset table.
    int set_hold{1};          // 0 quick(200) 1 medium(350) 2 long(550) 3 hold(800)
    int settings_sel{0};      // highlighted row
    static constexpr int kSettingsRows = 9;
    static constexpr int kHoldMs[4] = {200, 350, 550, 800};
    bool wake_listening{false};  // hands-free wake mode is live (set_wake, once speech is up)
    // Notifications: the creature is the notification UI. A banner slides down
    // from the top, the face glances, an earcon plays; queued, auto-dismiss.
    std::vector<std::string> notif_queue;
    std::string notif_text;
    std::uint32_t notif_shown_ms{0};  // when the current banner appeared
    // Audio-visualizer takeover: when the device is speaking/playing, the eyes
    // become the four-bar EQ (the dynamic island). Driven by the speaking edge.
    bool audio_playing{false};
    float audio_energy{0.7F};
    // Status glyphs (the rest of the dynamic-island vocabulary): a listening
    // pulse while the mic is open, a success check when an action lands, an
    // error cross when one is refused. success/error auto-clear; listening
    // persists until cleared.
    enum class Glyph : std::uint8_t { none, listening, success, error };
    Glyph glyph{Glyph::none};
    std::uint32_t glyph_start_ms{0};
    void show_glyph(Glyph g, std::uint32_t now_ms)
    {
        glyph = g;
        glyph_start_ms = now_ms == 0U ? 1U : now_ms;
    }
    // Pairing QR: rows of '0'/'1' pushed by the harness (show_qr). The creature
    // gives way to the code for the phone app to scan.
    std::vector<std::string> qr_matrix;
    bool qr_showing{false};
    void set_qr(const std::string &text, std::uint32_t now_ms)
    {
        // During onboarding the code is the app path: the surface owns it.
        if (onboarding.active()) {
            onboarding.show_qr(text.c_str(), now_ms);
            return;
        }
        qr_matrix.clear();
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t nl = text.find('\n', start);
            const std::string row = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!row.empty()) qr_matrix.push_back(row);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        qr_showing = !qr_matrix.empty();
    }
    // Render the QR: black modules on white, centered with a quiet zone, plus a
    // caption. Drawn straight to the framebuffer (a takeover).
    void render_qr(std::uint16_t *fb)
    {
        std::fill(pixels.begin(), pixels.end(), static_cast<std::uint16_t>(0xFFFFU)); // white
        const int n = static_cast<int>(qr_matrix.size());
        if (n == 0) return;
        const int W = eyes::kScreenWidth, H = eyes::kScreenHeight;
        const int span = 330;                 // QR draw area (leaves a quiet zone)
        const int mod = span / n;              // pixels per module
        const int total = mod * n;
        const int ox = (W - total) / 2, oy = (H - total) / 2 - 12;
        for (int my = 0; my < n; ++my) {
            const std::string &row = qr_matrix[static_cast<std::size_t>(my)];
            for (int mx = 0; mx < n && mx < static_cast<int>(row.size()); ++mx) {
                if (row[static_cast<std::size_t>(mx)] != '1') continue;
                for (int dy = 0; dy < mod; ++dy) {
                    std::uint16_t *r = fb + static_cast<std::size_t>((oy + my * mod + dy) * W);
                    for (int dx = 0; dx < mod; ++dx) r[ox + mx * mod + dx] = 0x0000U; // black
                }
            }
        }
        draw_geist_centered(fb, W / 2, oy + total + 22, "Scan with the app", 0x0000U, 1.0F, false);
    }
    // AMOLED care: sleep the face after inactivity to save the panel and
    // battery. Any input wakes it. A real firmware also dims and slow-drifts
    // static content; here we sleep the engine (eyes close, low motion).
    std::uint32_t last_interaction_ms{0};
    bool auto_slept{false};

    void note_interaction(std::uint32_t now_ms)
    {
        last_interaction_ms = now_ms;
        if (auto_slept) {
            engine.wake();
            auto_slept = false;
        }
    }

    void update_auto_sleep(std::uint32_t now_ms)
    {
        constexpr std::uint32_t kIdleSleepMs = 90000U;  // 90 s
        const bool busy = apps.active() || confirm_open || onboarding.active() ||
                          !notif_text.empty() ||
                          engine.frame().mode == eyes::InteractionMode::sleeping;
        if (!busy && !auto_slept && last_interaction_ms != 0U &&
            now_ms - last_interaction_ms > kIdleSleepMs) {
            engine.sleep();
            auto_slept = true;
        }
    }

    static std::string device_config_path()
    {
        const char *home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.lilguy/device.json";
    }

    // Very small JSON read/write (two string fields) — no dependency needed.
    void load_or_begin_onboarding()
    {
        std::ifstream in(device_config_path());
        if (in) {
            std::string blob((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            const auto field = [&](const char *key) -> std::string {
                const std::string needle = std::string("\"") + key + "\":\"";
                const std::size_t at = blob.find(needle);
                if (at == std::string::npos) return "";
                const std::size_t start = at + needle.size();
                const std::size_t end = blob.find('"', start);
                return end == std::string::npos ? "" : blob.substr(start, end - start);
            };
            const std::string n = field("name");
            const std::string stage = field("stage");
            if (!n.empty() || !stage.empty()) {
                device_name = n;
                const std::string w = field("wake");
                if (!w.empty()) wake_word = w;
                std::snprintf(identity.name, sizeof(identity.name), "%s", device_name.c_str());
                std::snprintf(identity.wake_word, sizeof(identity.wake_word), "%s",
                              wake_word.c_str());
                // Files from before stages existed carry a name: that was "done".
                const int st = stage.empty() ? static_cast<int>(eyes::OnboardingStage::done)
                                             : std::atoi(stage.c_str());
                identity.stage = static_cast<eyes::OnboardingStage>(
                    std::clamp(st, 0, static_cast<int>(eyes::OnboardingStage::done)));
                // Settings, if present (older configs simply keep the defaults).
                const auto flag = [&](const char *k, bool def) {
                    const std::string v = field(k);
                    return v.empty() ? def : (v == "1" || v == "true");
                };
                const auto num = [&](const char *k, int def) {
                    const std::string v = field(k);
                    return v.empty() ? def : std::atoi(v.c_str());
                };
                set_voice = flag("voice", set_voice);
                set_wake = flag("wake_on", set_wake);
                set_haptics = flag("haptics", set_haptics);
                set_bright = std::clamp(num("bright", set_bright), 0, 2);
                set_tilt = std::clamp(num("tilt", set_tilt), 0, 2);
                set_trigger = std::clamp(num("trigger", set_trigger), 0, 3);
                set_liveness = flag("liveness", set_liveness);
                set_voiceid = flag("voiceid", set_voiceid);
                const int hm = num("holdMs", kHoldMs[set_hold]);
                for (int i = 0; i < 4; ++i) {
                    if (kHoldMs[i] == hm) set_hold = i;
                }
                onboarding_begin_pending = true;
                return;
            }
        }
        identity = eyes::DeviceIdentity{};  // first boot
        onboarding_begin_pending = true;
    }

    // What can answer the greeting here: on the Mac, the speech recognizer.
    static eyes::OnboardingPaths onboarding_paths()
    {
#ifdef EYES_SIM_MAC
        return eyes::OnboardingPaths{true};
#else
        return eyes::OnboardingPaths{false};
#endif
    }

    void begin_onboarding_if_pending(std::uint32_t now_ms)
    {
        if (!onboarding_begin_pending) {
            return;
        }
        onboarding_begin_pending = false;
        if (force_onboarding) {
            identity.stage = eyes::OnboardingStage::fresh;
            identity.name[0] = '\0';
        }
        onboarding.begin(identity, onboarding_paths(), now_ms);
    }

    // The surface's one-shots: persist what changed, and when it dismisses
    // the face comes back with a blink, READY if setup completed, and the
    // wake word starts listening.
    void service_onboarding(std::uint32_t now_ms)
    {
        eyes::DeviceIdentity changed{};
        if (onboarding.consume_save(changed)) {
            identity = changed;
            device_name = identity.name;
            wake_word = identity.wake_word;
            save_device_config();
        }
        bool completed = false;
        if (onboarding.consume_finished(completed)) {
            renderer.invalidate();
            engine.release_expression();
            engine.request_blink();
            if (completed) {
                ui.show_selection(now_ms, "READY");
            }
            arm_wake_listening();
        }
    }

    void save_device_config()
    {
        const std::string dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                                "/.lilguy";
        (void)std::system(("mkdir -p '" + dir + "'").c_str());
        std::ofstream out(device_config_path());
        out << "{\"name\":\"" << device_name << "\",\"wake\":\"" << wake_word << "\""
            << ",\"stage\":\"" << static_cast<int>(identity.stage) << "\""
            << ",\"voice\":\"" << (set_voice ? 1 : 0) << "\""
            << ",\"wake_on\":\"" << (set_wake ? 1 : 0) << "\""
            << ",\"haptics\":\"" << (set_haptics ? 1 : 0) << "\""
            << ",\"bright\":\"" << set_bright << "\""
            << ",\"tilt\":\"" << set_tilt << "\""
            << ",\"trigger\":\"" << set_trigger << "\""
            << ",\"liveness\":\"" << (set_liveness ? 1 : 0) << "\""
            << ",\"voiceid\":\"" << (set_voiceid ? 1 : 0) << "\""
            << ",\"holdMs\":\"" << kHoldMs[std::clamp(set_hold, 0, 3)] << "\"}\n";
    }

    // A settings row's label + current-value text, for the surface and tests.
    void settings_row(int i, std::string &label, std::string &value) const
    {
        static const char *kLow[] = {"low", "mid", "high"};
        static const char *kDim[] = {"dim", "mid", "bright"};
        switch (i) {
            case 0: label = "Voice";    value = set_voice ? "speaks" : "silent"; break;
            case 1: label = "Wake word"; value = set_wake ? "listening" : "off"; break;
            case 2: label = "Haptics";  value = set_haptics ? "on" : "off"; break;
            case 3: label = "Brightness"; value = kDim[std::clamp(set_bright, 0, 2)]; break;
            case 4: label = "Tilt";     value = kLow[std::clamp(set_tilt, 0, 2)]; break;
            case 5: {
                static const char *kTrig[] = {"blink", "wink", "snap", "voice"};
                label = "Trigger"; value = kTrig[std::clamp(set_trigger, 0, 3)]; break;
            }
            case 6: label = "Liveness"; value = set_liveness ? "on" : "off"; break;
            case 7: {
                static const char *kHold[] = {"quick", "medium", "long", "hold"};
                label = "Hold"; value = kHold[std::clamp(set_hold, 0, 3)]; break;
            }
            case 8: label = "Voice ID"; value = set_voiceid ? "on" : "off"; break;
            default: label = ""; value = ""; break;
        }
    }

    // Toggle/cycle the highlighted row; persists. Returns true if it changed.
    bool settings_activate()
    {
        switch (settings_sel) {
            case 0: set_voice = !set_voice; break;
            case 1: set_wake = !set_wake; break;
            case 2: set_haptics = !set_haptics; break;
            case 3: set_bright = (set_bright + 1) % 3; break;
            case 4: set_tilt = (set_tilt + 1) % 3; break;
            case 5: set_trigger = (set_trigger + 1) % 4; break;
            case 6: set_liveness = !set_liveness; break;
            case 7: set_hold = (set_hold + 1) % 4; break;
            case 8: set_voiceid = !set_voiceid; break;
            default: return false;
        }
        save_device_config();
        return true;
    }

    // Called with a spoken transcript while onboarding is active.
    void onboarding_input(const std::string &text, std::uint32_t now_ms)
    {
        onboarding.voice_text(text.c_str(), now_ms);
    }

    // Voice-activated: hands-free wake listening comes up on its own when the
    // owner has it on (the default) and setup is done. H toggles it for the
    // session; the settings row persists the choice.
    void arm_wake_listening()
    {
#ifdef EYES_SIM_MAC
        if (set_wake && !wake_listening && !onboarding.active() &&
            eyes_mac_wake_enable(wake_word.c_str()) != 0) {
            wake_listening = true;
            std::printf("hands-free ON - say \"%s ...\"\n", wake_word.c_str());
            std::fflush(stdout);
        }
#endif
    }

    // Foreground surfaces route through the app framework: camera and music
    // are the first two apps; the cleanup flag buys one extra black-fill
    // frame after a surface closes.
    static constexpr int kAppCamera = 1;
    static constexpr int kAppMusic = 2;
    static constexpr int kAppWallet = 3;
    static constexpr int kAppSettings = 4;
    static constexpr int kAppClock = 5;
    eyes::AppSwitcher apps{kCameraOpenMs, kCameraCloseMs};
    std::vector<std::uint16_t> settings_frame;
    std::vector<std::uint16_t> clock_frame;
    // Timer: a countdown the owner sets by voice ("set a timer for 5 minutes")
    // — the harness relays a timer_set event — or locally for a demo. Tracked in
    // SDL-tick time like everything else; expiry fires the notification banner
    // and a long buzz, because the alert IS the creature. total drives the ring.
    bool timer_running{false};
    std::uint32_t timer_end_ms{0};
    std::uint32_t timer_total_ms{0};

    void start_timer(std::uint32_t seconds, std::uint32_t now_ms)
    {
        if (seconds == 0U) { timer_running = false; return; }
        timer_total_ms = seconds * 1000U;
        timer_end_ms = now_ms + timer_total_ms;
        timer_running = true;
    }
    std::uint32_t timer_remaining_ms(std::uint32_t now_ms) const
    {
        if (!timer_running || now_ms >= timer_end_ms) return 0U;
        return timer_end_ms - now_ms;
    }
    // Call each frame; returns true exactly once when the timer reaches zero.
    bool tick_timer(std::uint32_t now_ms)
    {
        if (timer_running && now_ms >= timer_end_ms) {
            timer_running = false;
            return true;
        }
        return false;
    }
    // Wallet surface (K): balance line pushed by the harness (wallet_update).
    std::string wallet_line{"Ask me to check your balance"};
    std::string wallet_last{""};
    std::vector<std::uint16_t> wallet_frame;
    // Balance is hidden by default (privacy — the number never sits on screen).
    // A voice "show balance" (harness balance_reveal) flashes a transient card
    // that fades on its own; nothing is latched or persisted. 0 = no card.
    std::uint32_t balance_reveal_ms{0};
    std::string balance_reveal_line;  // "$128.40"
    std::string balance_reveal_sub;   // optional sub-line, e.g. "dollar wallet"
    // Funds received: a splash pulse + rising amount when money lands, driven
    // by the harness funds_received event. 0 = not playing.
    std::uint32_t funds_received_ms{0};
    std::string funds_received_line;  // "+$20.00"
    std::string funds_received_from;  // optional "from charlie"
    // Agent casting control (docs/AGENT_CASTING.md, broker model). podbot owns the
    // session; the media flows cloud VM -> the TV's own receiver, never here.
    eyes::CastController cast;
    // Cast trackpad (T2): while a cast is live, the 466x466 panel is a trackpad —
    // touch becomes cast_cursor packets (relative deltas + tap-click). The loop
    // drains pending_cast_events to the agent link; cast_cursor_{x,y} draws the dot.
    std::vector<eyes::AgentEvent> pending_cast_events;
    float cast_cursor_x{233.0F};
    float cast_cursor_y{233.0F};
    float cast_last_x{0.0F};
    float cast_last_y{0.0F};
    float cast_moved{0.0F};
    bool cast_touching{false};
    std::uint32_t cast_press_ms{0};

    void queue_cast_cursor(int dx, int dy, int dscroll, unsigned buttons)
    {
        const eyes::AgentEvent ev = cast.cursor(dx, dy, dscroll, buttons);
        if (ev.type != eyes::AgentEventType::unknown) {
            pending_cast_events.push_back(ev);
        }
    }
    // Panel touch while casting -> cursor packets + the local dot. Returns true if
    // it consumed the touch (cast mode owns the panel).
    bool cast_trackpad_touch(eyes::DeviceUi::TouchPhase phase, float x, float y,
                             std::uint32_t now_ms)
    {
        if (!cast.live()) {
            return false;
        }
        using Phase = eyes::DeviceUi::TouchPhase;
        // Keep the dot inside the disc (radius ~210 around the 233,233 centre).
        const auto clamp_disc = [](float px, float py, float &ox, float &oy) {
            const float dx = px - 233.0F;
            const float dy = py - 233.0F;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float k = r > 210.0F ? 210.0F / r : 1.0F;
            ox = 233.0F + dx * k;
            oy = 233.0F + dy * k;
        };
        switch (phase) {
            case Phase::pressed:
                cast_touching = true;
                cast_last_x = x;
                cast_last_y = y;
                cast_press_ms = now_ms;
                cast_moved = 0.0F;
                clamp_disc(x, y, cast_cursor_x, cast_cursor_y);
                break;
            case Phase::pressing:
                if (cast_touching) {
                    const int dx = static_cast<int>(std::lround(x - cast_last_x));
                    const int dy = static_cast<int>(std::lround(y - cast_last_y));
                    cast_moved += std::fabs(x - cast_last_x) + std::fabs(y - cast_last_y);
                    if (dx != 0 || dy != 0) {
                        queue_cast_cursor(dx, dy, 0, 0);
                    }
                    clamp_disc(x, y, cast_cursor_x, cast_cursor_y);
                    cast_last_x = x;
                    cast_last_y = y;
                }
                break;
            case Phase::released:
            case Phase::press_lost:
                if (cast_touching) {
                    const bool tap =
                        cast_moved < 8.0F && (now_ms - cast_press_ms) < 300U;
                    if (tap && phase == Phase::released) {
                        queue_cast_cursor(0, 0, 0, 1);  // click: press…
                        queue_cast_cursor(0, 0, 0, 0);  // …then release
                    }
                    cast_touching = false;
                }
                break;
            default:
                break;
        }
        return true;
    }
    // Card lifetime: quick spring in, a readable hold, a soft fade — the number
    // is gone before a shoulder-surfer settles on it (motion doctrine: authored
    // exits, nothing lingers). Received is shorter; it is an accent, not a read.
    static constexpr float kBalanceInMs = 220.0F;
    static constexpr float kBalanceHoldMs = 2600.0F;
    static constexpr float kBalanceOutMs = 520.0F;
    static constexpr float kReceivedMs = 1500.0F;

    void reveal_balance(const std::string &line, const std::string &sub, std::uint32_t now_ms)
    {
        balance_reveal_line = line;
        balance_reveal_sub = sub;
        balance_reveal_ms = now_ms == 0U ? 1U : now_ms;  // 0 is the "off" sentinel
    }
    void receive_funds(const std::string &amount, const std::string &from, std::uint32_t now_ms)
    {
        funds_received_line = amount;
        funds_received_from = from;
        funds_received_ms = now_ms == 0U ? 1U : now_ms;
    }
    // 0 while the card is up, climbing to 1 as it finishes — the loop uses this
    // to drop the card and (for balance) keep the number off screen afterward.
    bool balance_card_active(std::uint32_t now_ms) const
    {
        if (balance_reveal_ms == 0U) return false;
        return static_cast<float>(now_ms - balance_reveal_ms) <=
               kBalanceInMs + kBalanceHoldMs + kBalanceOutMs;
    }
    bool funds_anim_active(std::uint32_t now_ms) const
    {
        if (funds_received_ms == 0U) return false;
        return static_cast<float>(now_ms - funds_received_ms) <= kReceivedMs;
    }
    bool camera_cleanup{false};
    // Zoom is the shipped default (the watchOS app-launch feel, user-picked);
    // T still cycles the alternatives for comparison.
    TransitionStyle transition_style{TransitionStyle::zoom};
    // Shutter: tap the red circle, or (with blink_snap on) blink at the
    // camera — Vision face landmarks in mac_camera.mm count real blinks.
    bool blink_snap{false};
    float camera_flash{0.0F};
    std::uint32_t shutter_pop_ms{0};  // press time; drives the squash+spring
    int photo_count{0};
    // The photo well and its viewer.
    std::vector<std::uint16_t> last_photo;
    bool has_photo{false};
    std::uint32_t photo_fly_ms{0};  // capture flying into the well
    bool photo_view{false};         // tap the well to view, tap again to close
    // PHOTO / VIDEO mode tab and recording (10 fps frame sequence, 10 s cap).
    int camera_mode{0};  // 0 photo, 1 video
    float pill_pos{0.0F};
    float record_morph{0.0F};
    // The now-playing surface (M key): real Spotify / Apple Music state via
    // the AppleScript poller, glass-lens scrubber, glass play/pause.
    std::vector<std::uint16_t> music_frame;
    // Landing splat when a capture seats in the photo well (motion doctrine).
    std::uint32_t well_splat_ms{0};
    // Owner-consent card for unsafe agent actions (payments). The link client
    // sends the answer; the shell only draws and hit-tests.
    bool confirm_open{false};
    std::string confirm_id;
    std::string confirm_summary;
    int pending_confirm_answer{-1};  // -1 none, 0 decline, 1 approve; loop drains
    bool confirm_swiping{false};     // finger is dragging the swipe-to-confirm thumb
    float confirm_swipe{0.0F};       // 0..1 progress along the track
#ifdef EYES_SIM_MAC
    MediaPoller media;
#endif

    void render_wallet_surface()
    {
        wallet_frame.assign(pixels.size(), 0x0000U);
        std::uint16_t *frame = wallet_frame.data();
        draw_geist_centered(frame, 233, 120, "Wallet", 0x8410U, 1.0F, false);
        // Balance, wrapped roughly, large.
        std::string line = wallet_line;
        while (line.size() > 4 && geist_width(line.c_str(), true) > 400) {
            line.resize(line.size() - 4);
            line += "...";
        }
        draw_geist_centered(frame, 233, 200, line.c_str(), 0xFFFFU, 1.0F, true);
        if (!wallet_last.empty()) {
            draw_geist_centered(frame, 233, 250, wallet_last.c_str(), 0x9FF3U, 1.0F, false);
        }
        // A glass pill hinting the voice action.
        draw_glass_lens(frame, 233.0F, 330.0F, 150.0F, 26.0F, 26.0F);
        draw_geist_centered(frame, 233, 330, "Hold A: send charlie 2 solana", 0xFFFFU, 0.9F,
                            false);
        draw_geist_centered(frame, 233, 400, "tap top to close", 0x738EU, 1.0F, false);
    }

    // The settings surface: a short list on the round panel. The selected row
    // wears a glass pill; crown scrolls, crown-press toggles. Fits the circle
    // by keeping rows near the vertical center where the width is greatest.
    void render_settings_surface()
    {
        settings_frame.assign(pixels.size(), 0x0000U);
        std::uint16_t *frame = settings_frame.data();
        draw_geist_centered(frame, 233, 96, "Settings", 0x8410U, 1.0F, false);
        constexpr int kTop = 150;
        constexpr int kStep = 46;
        for (int i = 0; i < kSettingsRows; ++i) {
            std::string label, value;
            settings_row(i, label, value);
            const float y = static_cast<float>(kTop + i * kStep);
            const bool on = (i == settings_sel);
            if (on) {
                draw_glass_lens(frame, 233.0F, y, 200.0F, 20.0F, 20.0F);
            }
            // Label left of center, value right — both kept inside the disc.
            // draw_geist takes the text TOP; offset by half the cap height so
            // the row centers in its glass pill.
            const int ty = static_cast<int>(y) - 12;
            draw_geist(frame, 96, ty, label.c_str(),
                       on ? 0xFFFFU : 0xC618U, 1.0F, false);
            const int vw = geist_width(value.c_str(), false);
            draw_geist(frame, 370 - vw, ty, value.c_str(),
                       on ? 0xFFFFU : 0x9FF3U, 1.0F, false);
        }
        draw_geist_centered(frame, 233, 408, "crown scrolls · press toggles", 0x738EU, 1.0F,
                            false);
    }

    // The clock surface: big wall-clock time, weekday/date, and — when a timer
    // is running — a depleting ring with MM:SS in the middle. Wall time is the
    // host clock here; on device it comes from SNTP/RTC. The ring is stroked
    // straight into the frame buffer (these surfaces don't own a rasterizer).
    void render_clock_surface(std::uint32_t now_ms)
    {
        clock_frame.assign(pixels.size(), 0x0000U);
        std::uint16_t *frame = clock_frame.data();
        const int W = eyes::kScreenWidth;
        const int H = eyes::kScreenHeight;

        const std::time_t t = std::time(nullptr);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char hhmm[16];
        char date[32];
        std::strftime(hhmm, sizeof(hhmm), "%H:%M", &lt);
        std::strftime(date, sizeof(date), "%a %b %e", &lt);

        if (timer_running) {
            // Timer takes the stage: ring + countdown, clock shrinks to a header.
            draw_geist_centered(frame, 233, 78, hhmm, 0x8410U, 1.0F, false);
            const std::uint32_t rem = timer_remaining_ms(now_ms);
            const float frac = timer_total_ms == 0U
                                   ? 0.0F
                                   : static_cast<float>(rem) / static_cast<float>(timer_total_ms);
            const float cx = 233.0F, cy = 250.0F, R = 92.0F;
            // Stroke the ring: full dim track, bright arc for the remaining
            // fraction, sweeping clockwise from 12 o'clock.
            const auto put = [&](int px, int py, std::uint16_t c) {
                if (px >= 0 && px < W && py >= 0 && py < H) frame[py * W + px] = c;
            };
            constexpr float kTwoPi = 6.2831853F;
            for (int i = 0; i < 720; ++i) {
                const float a = kTwoPi * static_cast<float>(i) / 720.0F;   // 0..2pi
                const float ang = -1.5707963F + a;                        // start at top
                const bool lit = (a / kTwoPi) <= frac;
                const std::uint16_t c = lit ? 0xFFFFU : 0x2124U;
                for (float dr = -3.0F; dr <= 3.0F; dr += 1.0F) {
                    const float rr = R + dr;
                    put(static_cast<int>(cx + rr * std::cos(ang)),
                        static_cast<int>(cy + rr * std::sin(ang)), c);
                }
            }
            char mmss[16];
            const std::uint32_t secs = (rem + 999U) / 1000U;  // ceil so it hits 0 at end
            std::snprintf(mmss, sizeof(mmss), "%u:%02u", secs / 60U, secs % 60U);
            draw_geist_centered(frame, 233, 250, mmss, 0xFFFFU, 1.0F, true);
            draw_geist_centered(frame, 233, 388, "tap top to close", 0x738EU, 1.0F, false);
        } else {
            // Just the clock: hero time, date under it, name at the foot.
            draw_geist_centered(frame, 233, 210, hhmm, 0xFFFFU, 1.0F, true);
            draw_geist_centered(frame, 233, 268, date, 0x9FF3U, 1.0F, false);
            const std::string who = device_name.empty() ? "podbot" : device_name;
            draw_geist_centered(frame, 233, 330, who.c_str(), 0x738EU, 1.0F, false);
            draw_geist_centered(frame, 233, 388, "ask me to set a timer", 0x738EU, 1.0F, false);
        }
    }

    void render_music_surface()
    {
        music_frame.assign(pixels.size(), 0x0000U);
        std::uint16_t *frame = music_frame.data();
#ifdef EYES_SIM_MAC
        media.ensure_started();
        const MediaSnapshot snap = media.snapshot();
        if (!snap.active) {
            draw_geist_centered(frame, 233, 210, "Nothing playing", 0xFFFFU, 1.0F, true);
            draw_geist_centered(frame, 233, 252, "Play something on Spotify or Music", 0x8410U,
                                1.0F, false);
            return;
        }
        float position = snap.position;
        if (snap.playing) {
            position += static_cast<float>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - snap.taken)
                                               .count()) /
                        1000.0F;
        }
        position = std::clamp(position, 0.0F, snap.duration);
        draw_geist_centered(frame, 233, 122, snap.spotify ? "Spotify" : "Apple Music", 0x8410U,
                            1.0F, false);
        std::string title = snap.title;
        while (title.size() > 4 && geist_width(title.c_str(), true) > 380) {
            title.resize(title.size() - 4);
            title += "...";
        }
        draw_geist_centered(frame, 233, 182, title.c_str(), 0xFFFFU, 1.0F, true);
        draw_geist_centered(frame, 233, 218, snap.artist.c_str(), 0x8410U, 1.0F, false);
        // Scrubber: track line, played fill, glass-lens thumb.
        const float fraction = snap.duration > 0.0F ? position / snap.duration : 0.0F;
        const int fill_x = 90 + static_cast<int>(286.0F * fraction);
        for (int y = 298; y <= 301; ++y) {
            for (int x = 90; x <= 376; ++x) {
                frame[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                      static_cast<std::size_t>(x)] = x <= fill_x ? 0xFFFFU : 0x2965U;
            }
        }
        draw_glass_lens(frame, static_cast<float>(fill_x), 300.0F, 26.0F, 16.0F, 12.0F);
        const auto format_time = [](float seconds, char *out, std::size_t size) {
            const int whole = std::max(0, static_cast<int>(seconds));
            std::snprintf(out, size, "%d:%02d", whole / 60, whole % 60);
        };
        char elapsed[16];
        char total[16];
        format_time(position, elapsed, sizeof(elapsed));
        format_time(snap.duration, total, sizeof(total));
        draw_geist(frame, 90, 318, elapsed, 0x8410U, 1.0F, false);
        draw_geist(frame, 376 - geist_width(total, false), 318, total, 0x8410U, 1.0F, false);
        // Play / pause on a glass puck.
        draw_glass_lens(frame, 233.0F, 396.0F, 32.0F, 32.0F, 32.0F);
        if (snap.playing) {
            for (int y = 384; y <= 408; ++y) {
                for (int x = 222; x <= 229; ++x) {
                    frame[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                          static_cast<std::size_t>(x)] = 0xFFFFU;
                }
                for (int x = 237; x <= 244; ++x) {
                    frame[static_cast<std::size_t>(y) * eyes::kScreenWidth +
                          static_cast<std::size_t>(x)] = 0xFFFFU;
                }
            }
        } else {
            for (int row = -12; row <= 12; ++row) {
                const int span = ((12 - std::abs(row)) * 20) / 12;
                for (int x = 0; x <= span; ++x) {
                    frame[static_cast<std::size_t>(396 + row) * eyes::kScreenWidth +
                          static_cast<std::size_t>(226 + x)] = 0xFFFFU;
                }
            }
        }
#else
        draw_geist_centered(frame, 233, 220, "Now playing needs macOS", 0xFFFFU, 1.0F, false);
#endif
    }
    bool recording{false};
    std::uint32_t last_video_frame_ms{0};
    std::vector<std::vector<std::uint16_t>> video_frames;
    int video_count{0};

    bool capture_frame(std::vector<std::uint16_t> &out)
    {
        eyes::CameraFrame shot{};
        if (!services.camera->capture(eyes::CameraService::Lens::front, shot) ||
            shot.format != eyes::CameraFrame::Format::rgb565 ||
            shot.width != eyes::kScreenWidth || shot.height != eyes::kScreenHeight) {
            return false;
        }
        const std::uint16_t *frame_pixels =
            reinterpret_cast<const std::uint16_t *>(shot.data);
        out.assign(frame_pixels,
                   frame_pixels + static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
        return true;
    }

    void stop_recording()
    {
        recording = false;
        if (video_frames.empty()) {
            return;
        }
        char path[64];
        for (std::size_t frame = 0; frame < video_frames.size(); ++frame) {
            std::snprintf(path, sizeof(path), "host/video_%02d_f%03zu.ppm", video_count, frame);
            write_ppm(path, video_frames[frame]);
        }
        std::printf("video saved: host/video_%02d_f*.ppm (%zu frames, 10 fps)\n", video_count,
                    video_frames.size());
        std::fflush(stdout);
        ++video_count;
        video_frames.clear();
    }

    void capture_video_frame(std::uint32_t now_ms)
    {
        if (now_ms - last_video_frame_ms < 100U) {
            return;
        }
        std::vector<std::uint16_t> frame;
        if (!capture_frame(frame)) {
            return;
        }
        last_video_frame_ms = now_ms;
        video_frames.push_back(std::move(frame));
        if (video_frames.size() >= 100U) {
            stop_recording();  // 10 s cap
        }
    }

    void take_photo(std::uint32_t now_ms)
    {
        if (!capture_frame(last_photo)) {
            std::printf("photo: no camera frame available\n");
            return;
        }
        has_photo = true;
        char path[64];
        std::snprintf(path, sizeof(path), "host/photo_%02d.ppm", photo_count);
        if (write_ppm(path, last_photo)) {
            ++photo_count;
            std::printf("photo saved: %s\n", path);
            std::fflush(stdout);
        }
        camera_flash = 1.0F;
        shutter_pop_ms = now_ms == 0U ? 1U : now_ms;
        // No toast: the capture itself flies into the photo well.
        photo_fly_ms = now_ms == 0U ? 1U : now_ms;
    }
    // How this window magnifies the 466 px panel; a viewing choice, not device
    // behaviour.
    bool smooth_display{true};
    std::uint32_t calibration_done_ms{0};
    std::uint32_t last_boot_poll_ms{0};
    std::uint32_t last_power_poll_ms{0};
    std::uint32_t power_click_ms{0};
    bool power_click_pending{false};

    explicit Simulation(const eyes::Settings &settings)
        : pixels(static_cast<std::size_t>(eyes::kScreenWidth) * eyes::kScreenHeight),
          renderer({pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth},
                   true),
          ui(engine, renderer, host, settings)
    {
    }

    // The one path a touch takes, whether it came from the mouse or a scripted
    // gesture: the UI gets first refusal, exactly as in app_main.
    void deliver_touch(eyes::DeviceUi::TouchPhase phase, float x, float y, std::uint32_t now_ms)
    {
        note_interaction(now_ms);
        // A tap dismisses the pairing QR.
        if (qr_showing && phase == eyes::DeviceUi::TouchPhase::pressed) {
            qr_showing = false;
            return;
        }
        // A tap on the notification banner dismisses it early.
        if (!notif_text.empty() && phase == eyes::DeviceUi::TouchPhase::pressed && y < 110.0F) {
            notif_text.clear();
            engine.release_expression();
            (void)x;
            return;
        }
        // The consent card is modal: it owns every touch while up. Approve is a
        // deliberate swipe (a tap can't send money); decline is a tap on Cancel.
        if (confirm_open) {
            const float progressAt = [&] {
                const float p = (x - kSwipeTravelL) / (kSwipeTravelR - kSwipeTravelL);
                return p < 0.0F ? 0.0F : (p > 1.0F ? 1.0F : p);
            }();
            const bool onTrack = std::fabs(y - kSwipeY) <= kSwipeHalfH + 10.0F &&
                                 x >= kSwipeCx - kSwipeHalfW - 6.0F && x <= kSwipeCx + kSwipeHalfW + 6.0F;
            switch (phase) {
                case eyes::DeviceUi::TouchPhase::pressed:
                    if (onTrack) {
                        confirm_swiping = true;
                        confirm_swipe = progressAt;
                    } else if (std::fabs(y - kConfirmCancelY) <= 22.0F) {
                        pending_confirm_answer = 0; // tap Cancel to decline
                    }
                    break;
                case eyes::DeviceUi::TouchPhase::pressing:
                    if (confirm_swiping) confirm_swipe = progressAt;
                    break;
                case eyes::DeviceUi::TouchPhase::released:
                case eyes::DeviceUi::TouchPhase::press_lost:
                    if (confirm_swiping) {
                        if (phase == eyes::DeviceUi::TouchPhase::released) confirm_swipe = progressAt; // release position counts
                        if (confirm_swipe >= kSwipeApprove) pending_confirm_answer = 1; // sent
                        confirm_swiping = false;
                        confirm_swipe = 0.0F; // snap back if not carried to the end
                    }
                    break;
                default:
                    break;
            }
            return;
        }
        // While casting, the panel is the trackpad (below a modal confirm, above
        // the app surfaces). It owns the touch and emits cursor packets.
        if (cast_trackpad_touch(phase, x, y, now_ms)) {
            return;
        }
        // Clock: tap the top to close.
        if (apps.fully_open(kAppClock) && phase == eyes::DeviceUi::TouchPhase::pressed) {
            if (y < 160.0F) { apps.close_all(); }
            return;
        }
        // Settings: tap the top to close; tap a row to select + toggle it.
        if (apps.fully_open(kAppSettings) && phase == eyes::DeviceUi::TouchPhase::pressed) {
            if (y < 128.0F) {
                apps.close_all();
                return;
            }
            constexpr float kTop = 150.0F;
            constexpr float kStep = 46.0F;
            const int row = static_cast<int>((y - (kTop - kStep / 2.0F)) / kStep);
            if (row >= 0 && row < kSettingsRows) {
                settings_sel = row;
                if (settings_activate() && set_haptics) { services.haptics->pulse(16, 0.6F); }
            }
            return;
        }
        // The wallet surface: tap the top to close.
        if (apps.fully_open(kAppWallet) && phase == eyes::DeviceUi::TouchPhase::pressed) {
            if (y < 160.0F) {
                apps.close_all();
            }
            return;
        }
        // The now-playing surface owns all touches while open.
        if (apps.fully_open(kAppMusic) && phase == eyes::DeviceUi::TouchPhase::pressed) {
#ifdef EYES_SIM_MAC
            const MediaSnapshot snap = media.snapshot();
            if (snap.active && y >= 270.0F && y <= 334.0F && x >= 80.0F && x <= 386.0F) {
                const float fraction = std::clamp((x - 90.0F) / 286.0F, 0.0F, 1.0F);
                media.seek(snap.spotify, fraction * snap.duration);
                return;
            }
            const float button_dx = x - 233.0F;
            const float button_dy = y - 396.0F;
            if (snap.active && button_dx * button_dx + button_dy * button_dy <= 40.0F * 40.0F) {
                media.play_pause(snap.spotify);
                return;
            }
#endif
            if (y < 160.0F) {
                apps.close_all();  // tap the top to leave
            }
            return;
        }
        // With the viewfinder fully open, the camera chrome owns its regions.
        if (apps.fully_open(kAppCamera) && phase == eyes::DeviceUi::TouchPhase::pressed) {
            if (photo_view) {
                photo_view = false;  // any tap closes the viewer
                return;
            }
            const float dx = x - kShutterX;
            const float dy = y - kShutterY;
            if (dx * dx + dy * dy <= kShutterRingOuter * kShutterRingOuter) {
                if (camera_mode == 0) {
                    take_photo(now_ms);
                } else {
                    shutter_pop_ms = now_ms == 0U ? 1U : now_ms;
                    if (recording) {
                        stop_recording();
                    } else {
                        video_frames.clear();
                        last_video_frame_ms = 0U;
                        recording = true;
                    }
                }
                return;
            }
            if (has_photo && std::fabs(x - kWellX) <= kWellHalf + kWellPad &&
                std::fabs(y - kWellY) <= kWellHalf + kWellPad) {
                photo_view = true;
                return;
            }
            if (std::fabs(x - kShutterX) <= kTabHalfW &&
                std::fabs(y - kTabY) <= kTabHalfH + 6.0F) {
                camera_mode = x < kShutterX ? 0 : 1;
                if (camera_mode == 0 && recording) {
                    stop_recording();
                }
                return;
            }
        }
        // Touch is petting: the engine gets every event.
        switch (phase) {
            case eyes::DeviceUi::TouchPhase::pressed:
                engine.pointer_down(x, y, now_ms);
                break;
            case eyes::DeviceUi::TouchPhase::pressing:
                engine.pointer_move(x, y, now_ms);
                break;
            case eyes::DeviceUi::TouchPhase::released:
                engine.pointer_up(x, y, now_ms);
                break;
            case eyes::DeviceUi::TouchPhase::press_lost:
                engine.pointer_cancel();
                break;
            case eyes::DeviceUi::TouchPhase::other:
                break;
        }
    }

    void pump_gesture(std::uint32_t now_ms)
    {
        gesture.pump(now_ms, [&](const GestureStep &step) {
            using Phase = eyes::DeviceUi::TouchPhase;
            const Phase phase = step.kind == GestureStep::Kind::down
                                    ? Phase::pressed
                                    : (step.kind == GestureStep::Kind::move ? Phase::pressing
                                                                            : Phase::released);
            deliver_touch(phase, step.x, step.y, now_ms);
        });
    }

    // Mirrors the device's button_timer and power_button_timer cadences.
    void poll_buttons(bool boot_down, std::uint32_t now_ms)
    {
        if (now_ms - last_boot_poll_ms >= kBootPollMs) {
            last_boot_poll_ms = now_ms;
            ui.poll_boot_button(boot_down, now_ms);
        }
        if (now_ms - last_power_poll_ms >= kPowerPollMs) {
            last_power_poll_ms = now_ms;
            const bool event = power_click_pending;
            power_click_pending = false;
            ui.poll_power_button(event, now_ms);
        }
    }

    // The IMU task answers a calibration request after a beat.
    void service_calibration(std::uint32_t now_ms)
    {
        if (host.calibration_requested) {
            host.calibration_requested = false;
            calibration_done_ms = now_ms + kCalibrationMs;
        }
        if (calibration_done_ms != 0U && now_ms >= calibration_done_ms) {
            calibration_done_ms = 0U;
            ui.show_selection(now_ms, "IMU CALIBRATED");
        }
    }

    // The device's render gate, then the text bands over the top.
    bool render_frame(std::uint32_t now_ms, std::uint32_t &last_render_ms, bool first_frame)
    {
        const eyes::FrameState &frame = engine.frame();
        const bool sleeping = frame.mode == eyes::InteractionMode::sleeping;
        const bool browsing = frame.mode == eyes::InteractionMode::dragging ||
                              frame.mode == eyes::InteractionMode::settling;
        const bool capsule_mode = renderer.face_mode() == eyes::FaceMode::capsule;
        const std::uint32_t period_ms = (sleeping && !capsule_mode)
                                            ? kSleepFramePeriodMs
                                            : (browsing ? kBrowseFramePeriodMs
                                                        : kFocusFramePeriodMs);
        if (!first_frame && now_ms - last_render_ms < period_ms) {
            return false;
        }
        const std::uint32_t dt_ms =
            first_frame ? 16U : std::min<std::uint32_t>(now_ms - last_render_ms, 100U);
        last_render_ms = now_ms;
        const bool text_push = ui.any_push();
        apps.tick(dt_ms);
        const bool overlay_active = apps.active();
        if (overlay_active || camera_cleanup) {
            // The overlay stomps pixels neither renderer tracks; start every
            // such frame (and one after) from black with a forced full
            // repaint, so the face underneath stays consistent.
            std::fill(pixels.begin(), pixels.end(), std::uint16_t{0});
            renderer.invalidate();
            camera_cleanup = overlay_active;
        }
        if (!onboarding.active()) {
            renderer.render(frame);
        }
        // Close-side effects, once, when a surface finishes closing: the
        // face returns with a blink, hardware winds down.
        switch (apps.take_closed()) {
            case kAppCamera:
                if (recording) {
                    stop_recording();
                }
                photo_view = false;
                services.camera->stop(eyes::CameraService::Lens::front);
                engine.request_blink();
                break;
            case kAppMusic:
                engine.request_blink();
                break;
            default:
                break;
        }
        if (apps.foreground() == kAppMusic) {
            render_music_surface();
            overlay_camera_transition(pixels.data(), music_frame.data(), apps.blend(),
                                      transition_style, apps.opening());
        }
        if (apps.foreground() == kAppWallet) {
            render_wallet_surface();
            overlay_camera_transition(pixels.data(), wallet_frame.data(), apps.blend(),
                                      transition_style, apps.opening());
        }
        if (apps.foreground() == kAppSettings) {
            render_settings_surface();
            overlay_camera_transition(pixels.data(), settings_frame.data(), apps.blend(),
                                      transition_style, apps.opening());
        }
        if (apps.foreground() == kAppClock) {
            render_clock_surface(now_ms);
            overlay_camera_transition(pixels.data(), clock_frame.data(), apps.blend(),
                                      transition_style, apps.opening());
        }
        if (apps.foreground() == kAppCamera) {
            eyes::CameraFrame webcam{};
            if (apps.blend() > 0.0F &&
                services.camera->capture(eyes::CameraService::Lens::front, webcam) &&
                webcam.format == eyes::CameraFrame::Format::rgb565 &&
                webcam.width == eyes::kScreenWidth && webcam.height == eyes::kScreenHeight) {
                overlay_camera_transition(pixels.data(),
                                          reinterpret_cast<const std::uint16_t *>(webcam.data),
                                          apps.blend(), transition_style, apps.opening());
            }
            if (apps.fully_open(kAppCamera) && photo_view && has_photo) {
                // The viewer: the well's photo, full screen; any tap closes.
                std::memcpy(pixels.data(), last_photo.data(),
                            last_photo.size() * sizeof(std::uint16_t));
            } else if (apps.fully_open(kAppCamera)) {
                if (recording) {
                    capture_video_frame(now_ms);
                }
                // Mode tab: the pill travels on smoothing and squashes along
                // its flight (remaining travel drives the squash).
                const float pill_target = camera_mode == 1 ? 1.0F : 0.0F;
                pill_pos += (pill_target - pill_pos) *
                            (1.0F - std::exp(-static_cast<float>(dt_ms) / 70.0F));
                const float pill_squash =
                    std::min(1.0F, std::fabs(pill_target - pill_pos) * 3.0F);
                draw_mode_tab(pixels.data(), pill_pos, pill_squash);
                // Press bounce per the motion doctrine: quick squash to 0.85,
                // then the house spring rings it back (~3% visible overshoot).
                float inner_scale = 1.0F;
                if (shutter_pop_ms != 0U) {
                    const float seconds =
                        static_cast<float>(now_ms - shutter_pop_ms) / 1000.0F;
                    if (seconds < 0.08F) {
                        inner_scale = 1.0F - 0.15F * (seconds / 0.08F);
                    } else if (seconds < 0.45F) {
                        inner_scale = 0.85F + 0.15F * house_spring((seconds - 0.08F) / 0.37F);
                    } else {
                        shutter_pop_ms = 0U;
                    }
                }
                // The red inner morphs toward a rounded square while a photo
                // is absorbed or a recording runs (iOS state language).
                const float morph_target =
                    recording ? 1.0F : std::min(1.0F, camera_flash * 1.2F);
                record_morph += (morph_target - record_morph) *
                                (1.0F - std::exp(-static_cast<float>(dt_ms) / 90.0F));
                draw_camera_shutter(pixels.data(), inner_scale, record_morph);
                if (has_photo) {
                    float progress = 1.0F;
                    if (photo_fly_ms != 0U) {
                        progress = std::min(
                            1.0F, static_cast<float>(now_ms - photo_fly_ms) / 450.0F);
                        if (progress >= 1.0F) {
                            photo_fly_ms = 0U;
                            // The well absorbs the landing.
                            well_splat_ms = now_ms == 0U ? 1U : now_ms;
                        }
                    }
                    float splat_seconds = -1.0F;
                    if (well_splat_ms != 0U) {
                        splat_seconds =
                            static_cast<float>(now_ms - well_splat_ms) / 1000.0F;
                        if (splat_seconds >= 0.45F) {
                            well_splat_ms = 0U;
                            splat_seconds = -1.0F;
                        }
                    }
                    draw_photo_well(pixels.data(), last_photo.data(), progress,
                                    splat_seconds);
                }
#ifdef EYES_SIM_MAC
                if (camera_mode == 0 && blink_snap && eyes_mac_camera_take_blinks() > 0) {
                    take_photo(now_ms);
                }
#endif
            }
            if (camera_flash > 0.0F) {
                // Shutter flash: a white pop decaying over ~180 ms.
                const float flash = std::min(1.0F, camera_flash) * 0.85F;
                for (std::uint16_t &pixel : pixels) {
                    pixel = lerp565(pixel, 0xFFFFU, flash);
                }
                camera_flash =
                    std::max(0.0F, camera_flash - static_cast<float>(dt_ms) / 180.0F);
            }
        }
        // First-boot onboarding owns the glass until setup completes (or the
        // greeting ends with nobody to answer); it draws incrementally into
        // the persistent frame exactly as the device does.
        begin_onboarding_if_pending(now_ms);
        if (onboarding.active()) {
            (void)onboarding.render(pixels.data(), now_ms);
        }
        service_onboarding(now_ms);
        ui.paint(pixels.data());
        // Pairing QR takes over the whole screen until dismissed (tap).
        if (qr_showing && !apps.active() && !onboarding.active() && !confirm_open) {
            render_qr(pixels.data());
            renderer.invalidate();
        } else
        // Dynamic-island takeover: while audio is playing the eyes give way to
        // the four-bar visualizer (nothing else on screen — voice out, no
        // chrome). Suppressed under an app/onboarding/confirm so those own it.
        if (audio_playing && !apps.active() && !onboarding.active() && !confirm_open) {
            std::fill(pixels.begin(), pixels.end(), static_cast<std::uint16_t>(0x0000U));
            const float phase = static_cast<float>(now_ms) * 0.008F;
            float levels[4];
            eyes::audio_bar_levels(phase, audio_energy, levels);
            eyes::draw_audio_bars(pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight,
                                  eyes::kScreenWidth / 2, eyes::kScreenHeight / 2, levels,
                                  static_cast<std::uint16_t>(0xFFFFU));
            renderer.invalidate();
        } else if (glyph != Glyph::none && !apps.active() && !onboarding.active() &&
                   !confirm_open) {
            // Status-glyph takeover (below speaking, which owns the bars).
            const float age = static_cast<float>(now_ms - glyph_start_ms);
            const int cx = eyes::kScreenWidth / 2, cy = eyes::kScreenHeight / 2;
            std::fill(pixels.begin(), pixels.end(), static_cast<std::uint16_t>(0x0000U));
            if (glyph == Glyph::listening) {
                eyes::draw_listening(pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight,
                                     cx, cy, static_cast<float>(now_ms) * 0.006F, 0xFFFFU);
            } else {
                // success/error: ~380ms draw-in, hold, then clear at ~1200ms.
                const float prog = std::min(1.0F, age / 380.0F);
                if (glyph == Glyph::success) {
                    eyes::draw_check(pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, cx, cy, prog, 0xFFFFU);
                } else {
                    eyes::draw_cross(pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, cx, cy, prog, 0xFFFFU);
                }
                if (age > 1200.0F) glyph = Glyph::none;
            }
            renderer.invalidate();
        }
        // Notification banner: advance the queue, then draw the current one
        // sliding in and out (below any modal card/onboarding).
        if (notif_text.empty() && !notif_queue.empty() && !confirm_open &&
            !onboarding.active()) {
            notif_text = notif_queue.front();
            notif_queue.erase(notif_queue.begin());
            notif_shown_ms = now_ms == 0U ? 1U : now_ms;
            engine.hold_expression(eyes::Expression::surprised);
            // The buzz IS the alert on a buttonless device — a double-tap the
            // wrist feels before the eyes even glance up.
            if (set_haptics) { services.haptics->pulse(140, 0.85F); }
        }
        if (!notif_text.empty()) {
            const float age = static_cast<float>(now_ms - notif_shown_ms);
            constexpr float kInMs = 260.0F;
            constexpr float kBannerHoldMs = 3600.0F;
            constexpr float kOutMs = 260.0F;
            float progress = 1.0F;
            if (age < kInMs) {
                progress = age / kInMs;
            } else if (age > kInMs + kBannerHoldMs) {
                progress = std::max(0.0F, 1.0F - (age - kInMs - kBannerHoldMs) / kOutMs);
            }
            draw_notification_banner(pixels.data(), notif_text, progress);
            renderer.invalidate();
            if (age > kInMs + kBannerHoldMs + kOutMs) {
                notif_text.clear();
                engine.release_expression();
                engine.request_blink();
            }
        }
        // Transient balance card + received animation: harness-pushed wallet
        // moments, drawn above the face but below any modal confirm card. The
        // balance fades on its own so the number is never resident (privacy);
        // received is a brief accent when money lands.
        if (balance_reveal_ms != 0U) {
            const float age = static_cast<float>(now_ms - balance_reveal_ms);
            float phase = 1.0F;
            if (age < kBalanceInMs) {
                phase = age / kBalanceInMs;
            } else if (age > kBalanceInMs + kBalanceHoldMs) {
                phase = std::max(0.0F,
                                 1.0F - (age - kBalanceInMs - kBalanceHoldMs) / kBalanceOutMs);
            }
            draw_balance_card(pixels.data(), balance_reveal_line, balance_reveal_sub, phase);
            renderer.invalidate();
            if (age > kBalanceInMs + kBalanceHoldMs + kBalanceOutMs) {
                balance_reveal_ms = 0U;  // gone; nothing latched or persisted
            }
        }
        if (funds_received_ms != 0U) {
            const float t = static_cast<float>(now_ms - funds_received_ms) / kReceivedMs;
            if (t >= 1.0F) {
                funds_received_ms = 0U;
            } else {
                draw_funds_received(pixels.data(), funds_received_line, funds_received_from, t);
                renderer.invalidate();
            }
        }
        if (cast.live()) {
            // Cast mode: a "Casting" pill up top and the trackpad cursor dot the
            // panel is steering. (Eye-tracking of the cursor is later polish.)
            draw_glass_lens(pixels.data(), 233.0F, 60.0F, 118.0F, 24.0F, 24.0F);
            const std::string line = "Casting \xC2\xB7 " + cast.agent();  // middot
            draw_geist_centered(pixels.data(), 233, 60, line.c_str(), 0xFFFFU, 1.0F, false);
            const int ccx = static_cast<int>(cast_cursor_x);
            const int ccy = static_cast<int>(cast_cursor_y);
            for (int yy = ccy - 8; yy <= ccy + 8; ++yy) {
                if (yy < 0 || yy >= eyes::kScreenHeight) continue;
                for (int xx = ccx - 8; xx <= ccx + 8; ++xx) {
                    if (xx < 0 || xx >= eyes::kScreenWidth) continue;
                    const float ddx = static_cast<float>(xx - ccx);
                    const float ddy = static_cast<float>(yy - ccy);
                    const float rr = std::sqrt(ddx * ddx + ddy * ddy);
                    // White core with a dark rim so it reads on the white eyes AND
                    // the black ground.
                    std::uint16_t color = 0xFFFFU;
                    float a = 0.0F;
                    if (rr <= 4.0F) {
                        color = 0xFFFFU;
                        a = 1.0F;
                    } else if (rr <= 7.0F) {
                        color = 0x2104U;  // dark rim
                        a = 0.9F;
                    }
                    if (a > 0.0F) {
                        const std::size_t idx = static_cast<std::size_t>(yy) *
                                                    eyes::kScreenWidth +
                                                static_cast<std::size_t>(xx);
                        pixels[idx] = lerp565(pixels[idx], color, a);
                    }
                }
            }
            renderer.invalidate();
        }
        if (confirm_open) {
            // Modal consent card, drawn last so it sits above everything.
            draw_confirm_card(pixels.data(), confirm_summary, confirm_swipe);
            renderer.invalidate();  // full repaint when it closes
        }
        if (text_push) {
            // Text bands punch black holes into the capsule disc; make the next
            // capsule frame repaint it fully (no-op in creature mode).
            renderer.invalidate();
        }
        ui.clear_dirty();
        return true;
    }
};

// One place where a panel control turns into a device input, shared by the
// window's buttons and the headless self-test.
void apply_control(Simulation &sim, Control control, int row, std::uint32_t now_ms,
                   int &captures)
{
    switch (control) {
        case Control::tap:
            sim.gesture.start(tap_gesture(), now_ms);
            break;
        case Control::long_press:
            sim.gesture.start(long_press_gesture(), now_ms);
            break;
        case Control::palm:
            // The CST9217 palm signal only ever sleeps an awake device.
            if (sim.engine.frame().mode != eyes::InteractionMode::sleeping) {
                sim.engine.sleep();
            }
            break;
        case Control::swipe_left:
            sim.gesture.start(swipe_gesture(-1.0F, 0.0F), now_ms);
            break;
        case Control::swipe_right:
            sim.gesture.start(swipe_gesture(1.0F, 0.0F), now_ms);
            break;
        case Control::swipe_up:
            sim.gesture.start(swipe_gesture(0.0F, -1.0F), now_ms);
            break;
        case Control::swipe_down:
            sim.gesture.start(swipe_gesture(0.0F, 1.0F), now_ms);
            break;
        case Control::rotate_ccw:
            sim.engine.set_orientation(sim.engine.orientation() - kPi / 12.0F);
            sim.ui.note_orientation(sim.engine.orientation());
            sim.ui.save_settings();
            break;
        case Control::rotate_cw:
            sim.engine.set_orientation(sim.engine.orientation() + kPi / 12.0F);
            sim.ui.note_orientation(sim.engine.orientation());
            sim.ui.save_settings();
            break;
        case Control::rotate_reset:
            sim.ui.run_setting(eyes::UiSetting::reset_rotation);
            break;
        case Control::shake:
            sim.sensors.shake_samples = 3;
            break;
        case Control::tilt_flat:
            sim.sensors.tilt = {};
            break;
        case Control::setting:
            sim.ui.run_setting(static_cast<eyes::UiSetting>(row));
            break;
        case Control::capture: {
            char path[256];
            std::snprintf(path, sizeof(path), "host/sim_capture_%02d.ppm", captures);
            if (write_ppm(path, sim.pixels)) {
                ++captures;
                std::printf("wrote %s\n", path);
            } else {
                std::printf("could not write %s\n", path);
            }
            std::fflush(stdout);
            break;
        }
        case Control::aa_mode:
            // 0 restores the renderer's own adaptive choice.
            sim.renderer.set_aa_override(row);
            break;
        case Control::display_filter:
            sim.smooth_display = !sim.smooth_display;
            break;
        case Control::none:
        case Control::boot:
        case Control::power:
        case Control::sound:
        case Control::tilt_pad:
            break;  // held, or handled by the pointer directly
    }
}

// One simulated frame: hardware in, engine forward, text bands over the top.
void step(Simulation &sim, std::uint32_t now_ms, std::uint32_t delta_ms,
          std::uint32_t &last_render_ms, bool first_frame, bool boot_down)
{
    sim.sensors.pump(sim.engine, sim.ui, now_ms);
    sim.pump_gesture(now_ms);
    sim.poll_buttons(boot_down, now_ms);
    sim.service_calibration(now_ms);
    sim.ui.set_perf({60U, 0U, 0U, 0U});
    sim.ui.tick(now_ms);
    sim.engine.update(delta_ms);
    if (sim.engine.consume_selection_changed()) {
        sim.ui.show_selection(now_ms);
    }
    sim.render_frame(now_ms, last_render_ms, first_frame);
}

struct Options {
    bool headless{false};
    bool selftest{false};
    bool composite{false};
    // Multiplies the 466 px active circle; the shell scales with it.
    float scale{1.15F};
    // The bob moved the composited shell in whole-pixel steps, which reads as
    // the face jittering the case. Opt in with --bob.
    bool bob{false};
    // Show the panel's own pixels instead of a smooth magnification.
    bool panel_pixels{false};
    // A transparent, borderless window needs the Cocoa helper; everywhere else
    // falls back to an ordinary decorated one.
#ifdef EYES_SIM_MAC
    bool framed{false};
#else
    bool framed{true};
#endif
    float seconds{0.0F};
    std::string out{};
    int shape{1};
    int palette{35};
    bool capsule{false};
    bool ink{false};
    // Fill the whole display: a fullscreen window with the device scaled to
    // its height, centred, the debug panel parked at the right edge.
    bool max{false};
};

Options parse_options(int argc, char **argv)
{
    Options options{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&](float fallback) {
            return index + 1 < argc ? std::strtof(argv[++index], nullptr) : fallback;
        };
        if (argument == "--selftest") {
            options.selftest = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--composite") {
            options.composite = true;
        } else if (argument == "--capsule") {
            options.capsule = true;
        } else if (argument == "--ink") {
            options.ink = true;
        } else if (argument == "--scale") {
            options.scale = std::max(0.5F, next(1.15F));
        } else if (argument == "--bob") {
            options.bob = true;
        } else if (argument == "--framed") {
            options.framed = true;
        } else if (argument == "--max" || argument == "--fullscreen") {
            options.max = true;
            options.framed = true;
        } else if (argument == "--panel-pixels") {
            options.panel_pixels = true;
        } else if (argument == "--seconds") {
            options.seconds = next(6.0F);
        } else if (argument == "--shape") {
            options.shape = static_cast<int>(next(1.0F));
        } else if (argument == "--palette") {
            options.palette = static_cast<int>(next(35.0F));
        } else if (argument == "--out") {
            options.out = index + 1 < argc ? argv[++index] : "";
        } else if (argument == "--help" || argument == "-h") {
            std::printf(
                "usage: eyes_simulator [--scale N] [--headless [--composite] --seconds S --out PREFIX]\n"
                "                     [--selftest] [--framed] [--max] [--no-bob]\n"
                "  --shape/--palette/--capsule/--ink are accepted and ignored: the look is fixed\n"
                "  (white capsule eyes on black), the same as the device.\n");
            options.seconds = -1.0F;
            return options;
        }
    }
    if (options.headless && options.seconds <= 0.0F) {
        options.seconds = 6.0F;
    }
    return options;
}

eyes::Settings start_settings(const Options &options)
{
    // Fresh Settings are the fixed look (capsule + ink); the flags that used
    // to pick a face are parsed for compatibility and ignored, so a bare
    // ./host/eyes_simulator starts exactly like the device.
    (void)options;
    return eyes::Settings{};
}

void apply_start_options(Simulation &sim, const Options &options)
{
    sim.smooth_display = !options.panel_pixels;
    if (options.shape != 1 || options.palette != 35 || !options.capsule || !options.ink) {
        std::printf("note: the look is fixed (circle x og, capsule, ink); flags ignored\n");
    }
    sim.engine.set_selection({1, 35});
    sim.ui.apply_settings();
}

// Drives every panel control through apply_control and checks it reached the
// firmware. Covers the wiring the SDL event loop cannot be tested through.
int run_selftest()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char *what) {
        std::printf("  %-44s %s\n", what, condition ? "ok" : "FAILED");
        if (!condition) {
            ++failures;
        }
    };

    std::uint32_t now = 0U;
    std::uint32_t last_render = 0U;
    int captures = 0;
    // Fresh settings are the fixed look; the selftest runs exactly what the
    // device boots into.
    Simulation sim(eyes::Settings{});
    sim.ui.apply_settings();
    check(sim.renderer.face_mode() == eyes::FaceMode::capsule && sim.renderer.capsule_invert(),
          "boots into the capsule face with inverted ink");
    check(sim.engine.selection() == eyes::Selection{1, 35}, "boots into circle x og");

    // Run the sim forward, optionally watching for a mode along the way.
    const auto run = [&](std::uint32_t duration_ms, eyes::InteractionMode *seen) {
        for (std::uint32_t elapsed = 0; elapsed < duration_ms; elapsed += kFocusFramePeriodMs) {
            now += kFocusFramePeriodMs;
            step(sim, now, kFocusFramePeriodMs, last_render, now == kFocusFramePeriodMs, false);
            if (seen != nullptr && sim.engine.frame().mode == *seen) {
                seen = nullptr;  // latched
            }
        }
    };
    const auto ran_through = [&](std::uint32_t duration_ms, eyes::InteractionMode mode) {
        bool hit = false;
        for (std::uint32_t elapsed = 0; elapsed < duration_ms; elapsed += kFocusFramePeriodMs) {
            now += kFocusFramePeriodMs;
            step(sim, now, kFocusFramePeriodMs, last_render, false, false);
            hit = hit || sim.engine.frame().mode == mode;
        }
        return hit;
    };

    std::printf("panel control wiring:\n");
    run(1200U, nullptr);  // settle past the boot ramp and sound calibration

    // TAP: the scripted press reaches the engine and pokes it.
    apply_control(sim, Control::tap, 0, now, captures);
    check(ran_through(200U, eyes::InteractionMode::touching), "TAP reaches the engine as a touch");

    // Swipes reach the engine as a touch. Browse is locked by default (and not
    // compiled in on the device at all), so a drag is pure gaze-follow: the
    // gaze must track the finger without the selection changing.
    const eyes::Selection locked_before = sim.engine.selection();
    apply_control(sim, Control::swipe_left, 0, now, captures);
    check(ran_through(300U, eyes::InteractionMode::touching), "SWIPE < reaches the engine");
    run(1500U, nullptr);
    check(sim.engine.selection() == locked_before, "SWIPE < does not browse while locked");

    run(600U, nullptr);

    // HOLD 800MS stays a touch for the whole press and pokes on release.
    apply_control(sim, Control::long_press, 0, now, captures);
    check(ran_through(700U, eyes::InteractionMode::touching), "HOLD 800MS holds a touch");
    run(400U, nullptr);
    check(sim.engine.frame().poke > 0.0F, "HOLD 800MS pokes on release");
    run(800U, nullptr);

    // PALM: the CST9217 cover signal.
    apply_control(sim, Control::palm, 0, now, captures);
    run(100U, nullptr);
    check(sim.engine.frame().mode == eyes::InteractionMode::sleeping, "PALM sleeps");
    sim.engine.wake();
    run(600U, nullptr);

    // SHAKE: past the engine's gate, into the dizzy reaction.
    apply_control(sim, Control::shake, 0, now, captures);
    check(ran_through(400U, eyes::InteractionMode::dizzy), "SHAKE triggers dizzy");
    run(2200U, nullptr);

    // Rotation, and the menu row that resets it.
    const float rotation_before = sim.engine.orientation();
    apply_control(sim, Control::rotate_cw, 0, now, captures);
    check(std::fabs(sim.engine.orientation() - (rotation_before + kPi / 12.0F)) < 1e-4F,
          "ROT+ turns the anchored face 15 deg");
    check(sim.ui.settings().orientation == sim.engine.orientation(), "ROT+ is persisted");
    apply_control(sim, Control::rotate_reset, 0, now, captures);
    check(std::fabs(sim.engine.orientation()) < 1e-6F, "ROT 0 resets rotation");

    // TILT pad and FLAT.
    sim.sensors.tilt = {0.8F, -0.4F};
    run(400U, nullptr);
    check(std::fabs(sim.engine.imu_gaze_target().x) > 0.05F, "tilt reaches the IMU gaze target");
    apply_control(sim, Control::tilt_flat, 0, now, captures);
    check(sim.sensors.tilt.x == 0.0F && sim.sensors.tilt.y == 0.0F, "FLAT levels the board");

    // SOUND: held loud windows open the listening gate.
    run(600U, nullptr);
    sim.sensors.loud = true;
    bool listened = false;
    for (int frame = 0; frame < 60; ++frame) {
        now += kFocusFramePeriodMs;
        step(sim, now, kFocusFramePeriodMs, last_render, false, false);
        listened = listened || sim.engine.frame().attention == eyes::AttentionSource::sound;
    }
    sim.sensors.loud = false;
    check(listened, "SOUND (HOLD) opens the listening gate");
    run(2000U, nullptr);

    // The settings buttons run the firmware's own settings and persist them.
    const int saves_before = sim.host.saves;
    apply_control(sim, Control::setting, static_cast<int>(eyes::UiSetting::tilt), now, captures);
    check(sim.ui.settings().imu_level == 2U && sim.host.saves == saves_before + 1,
          "setting TILT FB cycles and saves");
    sim.engine.set_orientation(0.5F);
    sim.ui.note_orientation(0.5F);
    apply_control(sim, Control::setting, static_cast<int>(eyes::UiSetting::reset_rotation), now,
                  captures);
    check(sim.engine.orientation() == 0.0F && sim.ui.settings().orientation == 0.0F &&
              sim.host.saves == saves_before + 2,
          "setting RESET ROTATION zeroes and saves");
    // The look cannot be changed from anywhere the panel reaches.
    check(sim.engine.selection() == eyes::Selection{1, 35} &&
              sim.renderer.face_mode() == eyes::FaceMode::capsule && sim.renderer.capsule_invert(),
          "the look is still circle x og, capsule, ink");

    // Raster AA override and the magnification filter.
    apply_control(sim, Control::aa_mode, 2, now, captures);
    run(200U, nullptr);
    check(sim.renderer.aa_override() == 2 && sim.renderer.last_aa_subsamples() == 2,
          "AA 2X pins the raster subsamples");
    apply_control(sim, Control::aa_mode, 8, now, captures);
    run(200U, nullptr);
    check(sim.renderer.last_aa_lines() == 8, "AA 8X gives 8 lines per row");
    apply_control(sim, Control::aa_mode, 0, now, captures);
    run(200U, nullptr);
    check(sim.renderer.aa_override() == 0, "AA AUTO restores the adaptive choice");
    const bool smooth_before = sim.smooth_display;
    apply_control(sim, Control::display_filter, 0, now, captures);
    check(sim.smooth_display != smooth_before, "MAGNIFY toggles the display filter");
    apply_control(sim, Control::display_filter, 0, now, captures);

    // BOOT and PWR are held rather than clicked, so they go through the poll.
    std::printf("physical button wiring:\n");
    for (int poll = 0; poll < 6; ++poll) {  // ~120 ms held
        now += kBootPollMs;
        sim.ui.poll_boot_button(true, now);
    }
    now += kBootPollMs;
    sim.ui.poll_boot_button(false, now);
    run(500U, nullptr);
    check(sim.ui.page() == eyes::UiPage::none, "BOOT tap opens nothing (no touch menu)");

    // A click on the glass is a touch on the creature, never a menu row.
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, 233.0F,
                      static_cast<float>(eyes::kUiRowFirst + 5), now);
    check(sim.engine.frame().mode == eyes::InteractionMode::touching,
          "clicking the screen pets the creature");
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::released, 233.0F,
                      static_cast<float>(eyes::kUiRowFirst + 5), now);
    run(1500U, nullptr);

    const int listens_before = sim.host.listen_requests;
    sim.power_click_pending = true;
    run(600U, nullptr);
    check(sim.host.listen_requests == listens_before + 1, "PWR tap asks to listen");
    check(sim.engine.selection() == eyes::Selection{1, 35}, "PWR tap leaves the look alone");

    std::printf("settings surface:\n");
    // Boolean rows flip; the value string tracks it.
    sim.settings_sel = 0;  // Voice
    const bool voice_before = sim.set_voice;
    check(sim.settings_activate() && sim.set_voice != voice_before,
          "settings row VOICE toggles");
    std::string label, value;
    sim.settings_row(0, label, value);
    check(label == "Voice" && value == (sim.set_voice ? "speaks" : "silent"),
          "settings row reports the live value");
    // Enum rows cycle 0->1->2->0.
    sim.settings_sel = 3;  // Brightness
    sim.set_bright = 2;
    check(sim.settings_activate() && sim.set_bright == 0, "settings row BRIGHTNESS wraps 2->0");
    sim.settings_sel = 4;  // Tilt
    sim.set_tilt = 0;
    check(sim.settings_activate() && sim.set_tilt == 1, "settings row TILT cycles 0->1");
    sim.settings_sel = 5;  // Trigger (blink/wink/snap/voice)
    sim.set_trigger = 3;
    check(sim.settings_activate() && sim.set_trigger == 0, "settings row TRIGGER wraps voice->blink");
    sim.settings_sel = 6;  // Liveness (anti-spoof)
    sim.set_liveness = false;
    check(sim.settings_activate() && sim.set_liveness, "settings row LIVENESS toggles on");
    sim.settings_sel = 7;  // Hold length
    sim.set_hold = 0;
    check(sim.settings_activate() && sim.set_hold == 1, "settings row HOLD cycles quick->medium");
    // Persistence round-trips through device.json — under a scratch HOME so
    // the real ~/.lilguy/device.json (the user's actual onboarding) is never
    // touched by the test.
    {
        const char *real_home = std::getenv("HOME");
        const std::string saved_home = real_home ? real_home : "";
        const std::string scratch = "/tmp/lilguy_selftest_home";
        (void)std::system(("rm -rf '" + scratch + "'").c_str());
        setenv("HOME", scratch.c_str(), 1);
        sim.set_haptics = false;
        sim.set_bright = 1;
        sim.set_liveness = true;
        sim.set_hold = 2;  // "long" -> 550ms
        sim.device_name = "selftestbot";
        sim.save_device_config();
        Simulation reloaded(eyes::Settings{});
        reloaded.load_or_begin_onboarding();
        check(reloaded.device_name == "selftestbot" && !reloaded.set_haptics &&
                  reloaded.set_bright == 1 && reloaded.set_liveness && reloaded.set_hold == 2,
              "settings persist and reload from device.json");
        (void)std::system(("rm -rf '" + scratch + "'").c_str());
        if (saved_home.empty()) { unsetenv("HOME"); }
        else { setenv("HOME", saved_home.c_str(), 1); }
    }

    std::printf("clock + timer:\n");
    sim.start_timer(30U, now);
    check(sim.timer_running && sim.timer_remaining_ms(now) == 30000U, "timer starts at 30s");
    check(!sim.tick_timer(now + 5000U) && sim.timer_remaining_ms(now + 5000U) == 25000U,
          "timer counts down, no early fire");
    check(sim.tick_timer(now + 30000U), "timer fires exactly once at zero");
    check(!sim.tick_timer(now + 31000U) && !sim.timer_running, "timer stays fired");

    std::printf("swipe-to-confirm:\n");
    sim.confirm_open = true;
    sim.confirm_id = "t";
    sim.confirm_summary = "Send 0.01 Solana to charlie?";
    sim.confirm_swipe = 0.0F;
    sim.confirm_swiping = false;
    sim.pending_confirm_answer = -1;
    // A short drag released before the threshold must NOT send — it snaps back.
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, 127.0F, 300.0F, now);
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressing, 210.0F, 300.0F, now);
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::released, 210.0F, 300.0F, now);
    check(sim.pending_confirm_answer == -1 && sim.confirm_swipe == 0.0F,
          "a short swipe does not send (snaps back)");
    // A deliberate full drag to the end confirms — the only way money moves by touch.
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, 127.0F, 300.0F, now);
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressing, 280.0F, 300.0F, now);
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::released, 339.0F, 300.0F, now);
    check(sim.pending_confirm_answer == 1, "a full swipe confirms the send");
    // Tapping Cancel declines.
    sim.pending_confirm_answer = -1;
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, 233.0F, 348.0F, now);
    check(sim.pending_confirm_answer == 0, "tapping Cancel declines the send");
    sim.confirm_open = false;
    sim.pending_confirm_answer = -1;

    std::printf("wallet reveal + received:\n");
    // Balance is hidden by default — no card until the harness reveals it.
    check(!sim.balance_card_active(now), "balance hidden by default (no card)");
    sim.reveal_balance("$128.40", "dollar wallet", now);
    check(sim.balance_card_active(now) && sim.balance_reveal_line == "$128.40",
          "show balance flashes the card with the line");
    // It fades on its own; driving the render past the envelope drops it and
    // leaves nothing latched (the number is never resident — privacy).
    const std::uint32_t balance_done =
        now + static_cast<std::uint32_t>(Simulation::kBalanceInMs + Simulation::kBalanceHoldMs +
                                         Simulation::kBalanceOutMs) +
        50U;
    std::uint32_t balance_last_render = 0U;  // large dt so the frame isn't throttled
    sim.render_frame(balance_done, balance_last_render, false);
    check(sim.balance_reveal_ms == 0U && !sim.balance_card_active(balance_done),
          "balance card fades and never persists");
    // Funds received: the animation plays, then clears itself at the tail.
    sim.receive_funds("+$20.00", "from charlie", balance_done);
    check(sim.funds_anim_active(balance_done), "funds received starts the animation");
    const std::uint32_t recv_done =
        balance_done + static_cast<std::uint32_t>(Simulation::kReceivedMs) + 50U;
    std::uint32_t recv_last_render = 0U;  // large dt so the frame isn't throttled
    sim.render_frame(recv_done, recv_last_render, false);
    check(sim.funds_received_ms == 0U && !sim.funds_anim_active(recv_done),
          "received animation ends and clears");

    std::printf("cast session:\n");
    check(!sim.cast.active(), "cast idle by default");
    const eyes::AgentEvent cast_start = sim.cast.begin("482913", "browser", "castsel");
    check(cast_start.type == eyes::AgentEventType::cast_start &&
              sim.cast.state() == eyes::CastState::pairing,
          "begin sends cast_start and pairs");
    eyes::AgentEvent cast_paired{};
    cast_paired.type = eyes::AgentEventType::cast_paired;
    cast_paired.id = "castsel";
    check(sim.cast.on_event(cast_paired) && sim.cast.live(), "cast goes live on paired");
    check(sim.cast.cursor(2, 3, 0, 1).text == "2,3,0,1", "cursor packs while live");
    check(sim.cast.end().type == eyes::AgentEventType::cast_end && !sim.cast.active(),
          "cast_end tears the cast down");

    std::printf("cast trackpad:\n");
    sim.cast.begin("777000", "browser", "cast-tp");
    eyes::AgentEvent tp_paired{};
    tp_paired.type = eyes::AgentEventType::cast_paired;
    tp_paired.id = "cast-tp";
    sim.cast.on_event(tp_paired);
    sim.pending_cast_events.clear();
    // A drag emits a cursor move packet carrying the finger delta.
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, 200.0F, 233.0F, now);
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressing, 210.0F, 233.0F, now);
    check(!sim.pending_cast_events.empty() &&
              sim.pending_cast_events.back().type == eyes::AgentEventType::cast_cursor,
          "drag emits a cast_cursor packet");
    {
        int dx = 0, dy = 0, ds = 0;
        unsigned btn = 9;
        const bool ok = eyes::parse_cursor(sim.pending_cast_events.back().text, dx, dy, ds, btn);
        check(ok && dx == 10 && dy == 0, "cursor packet carries the drag delta");
    }
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::released, 210.0F, 233.0F, now);
    // A tap (press + release, no travel) emits a click: buttons 1 then 0.
    sim.pending_cast_events.clear();
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, 233.0F, 233.0F, now);
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::released, 233.0F, 233.0F, now);
    check(sim.pending_cast_events.size() == 2 &&
              sim.pending_cast_events[0].text == "0,0,0,1" &&
              sim.pending_cast_events[1].text == "0,0,0,0",
          "a tap emits a click (down then up)");
    // A payment confirm still wins over the trackpad while casting.
    sim.confirm_open = true;
    sim.pending_cast_events.clear();
    sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, 233.0F, 233.0F, now);
    check(sim.pending_cast_events.empty(), "a modal confirm outranks the cast trackpad");
    sim.confirm_open = false;
    sim.cast.end();
    sim.pending_cast_events.clear();

    std::printf("%s\n", failures == 0 ? "panel self-test passed" : "panel self-test FAILED");
    return failures == 0 ? 0 : 1;
}

int run_headless(const Options &options)
{
    Simulation sim(start_settings(options));
    apply_start_options(sim, options);
    // LILGUY_SIM_ONBOARDING=1 forces first boot, so --headless --out captures
    // the greeting regardless of ~/.lilguy/device.json.
    if (std::getenv("LILGUY_SIM_ONBOARDING") != nullptr) {
        sim.force_onboarding = true;
        sim.onboarding_begin_pending = true;  // headless never reads device.json
    }

    Panel panel;
    panel.build();
    const float display_px = static_cast<float>(eyes::kScreenWidth) * options.scale;
    const DeviceMetrics metrics = DeviceMetrics::from_display(display_px);
    const Stage stage = Stage::lay_out(metrics, panel.height());
    const int composite_width = stage.width;
    const int composite_height = stage.height;
    const float device_cx = stage.device_cx;
    const float device_cy = stage.device_cy;
    Canvas canvas(options.composite ? composite_width : 1,
                  options.composite ? composite_height : 1);
    Canvas backdrop(options.composite ? composite_width : 1,
                    options.composite ? composite_height : 1);
    Layer shell;
    Layer keys[2];
    int key_offset_x[2] = {0, 0};
    int key_offset_y[2] = {0, 0};
    Layer panel_shadow;
    constexpr int kPanelShadowMargin = 40;
    if (options.composite) {
        render_shadow(backdrop, metrics, device_cx, device_cy);
        build_shell_layer(shell, metrics);
        const float key_angles[2] = {kBootButtonAngle, kPowerButtonAngle};
        for (int key = 0; key < 2; ++key) {
            build_key_layer(keys[key], metrics, key_angles[key], false, key_offset_x[key],
                            key_offset_y[key]);
        }
        build_panel_shadow(panel_shadow, kPanelWidth, panel.height(), kPanelShadowMargin);
    }

    const auto total_ms = static_cast<std::uint32_t>(options.seconds * 1000.0F);
    // Repeatable visual demo of the wallet moments (no harness needed): with
    // LILGUY_DEMO_WALLET set, flash "show balance" early and let money land a
    // couple of seconds later, so an --out capture shows both animations.
    const bool demo_wallet = std::getenv("LILGUY_DEMO_WALLET") != nullptr;
    bool demo_reveal_fired = false;
    bool demo_recv_fired = false;
    const bool demo_cast = std::getenv("LILGUY_DEMO_CAST") != nullptr;
    bool demo_cast_fired = false;
    std::uint32_t last_render_ms = 0;
    int written = 0;
    for (std::uint32_t now_ms = 0; now_ms < total_ms; now_ms += kFocusFramePeriodMs) {
        if (demo_wallet && !demo_reveal_fired && now_ms >= 600U) {
            sim.reveal_balance("$128.40", "dollar wallet", now_ms);
            sim.engine.request_blink();
            demo_reveal_fired = true;
        }
        if (demo_wallet && !demo_recv_fired && now_ms >= 4200U) {
            sim.receive_funds("+$20.00", "from charlie", now_ms);
            sim.engine.request_blink();
            demo_recv_fired = true;
        }
        if (demo_cast && !demo_cast_fired && now_ms >= 600U) {
            sim.cast.begin("482913", "browser", "democast");
            eyes::AgentEvent paired{};
            paired.type = eyes::AgentEventType::cast_paired;
            paired.id = "democast";
            sim.cast.on_event(paired);
            sim.cast_cursor_x = 330.0F;  // park the dot off-centre so it reads
            sim.cast_cursor_y = 300.0F;
            demo_cast_fired = true;
        }
        sim.sensors.pump(sim.engine, sim.ui, now_ms);
        sim.pump_gesture(now_ms);
        sim.poll_buttons(false, now_ms);
        sim.service_calibration(now_ms);
        sim.ui.set_perf({60U, 0U, 0U, 0U});
        sim.ui.tick(now_ms);
        sim.engine.update(kFocusFramePeriodMs);
        if (sim.engine.consume_selection_changed()) {
            sim.ui.show_selection(now_ms);
        }
        sim.render_frame(now_ms, last_render_ms, now_ms == 0U);
        if (options.composite || !options.out.empty()) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s_%04d.ppm", options.out.c_str(), written);
            bool ok = true;
            if (options.composite) {
                // The same composite the window presents, minus the idle bob
                // so frames stay comparable.
                canvas.copy_from(backdrop);
                canvas.blit_layer(shell, static_cast<int>(device_cx - metrics.tile_center),
                                  static_cast<int>(device_cy - metrics.tile_center));
                canvas.blit_display_disc(sim.pixels.data(), device_cx, device_cy,
                                         metrics.display_radius, sim.smooth_display);
                PanelState state{};
                state.tilt = sim.sensors.tilt;
                state.fps = 60;
                // Captions shown in the still so the keys are identifiable.
                for (int key = 0; key < 2; ++key) {
                    canvas.blit_layer(keys[key], static_cast<int>(device_cx) + key_offset_x[key],
                                      static_cast<int>(device_cy) + key_offset_y[key]);
                }
                draw_key_caption(canvas, metrics, device_cx, device_cy, kPowerButtonAngle, false,
                                 "PWR");
                draw_key_caption(canvas, metrics, device_cx, device_cy, kBootButtonAngle, false,
                                 "BOOT");
                Tooltip tooltip{};
                tooltip.place(stage);
                tooltip.open = true;
                tooltip.draw(canvas, false);
                canvas.blit_layer(panel_shadow, stage.panel_x - kPanelShadowMargin,
                                  stage.panel_y - kPanelShadowMargin);
                draw_panel(canvas, panel, stage.panel_x, stage.panel_y, sim.ui, sim.engine,
                           sim.renderer, state, sim.host.saves, sim.smooth_display);
                if (!options.out.empty()) {
                    ok = write_canvas_ppm(path, canvas);
                }
            } else {
                ok = write_ppm(path, sim.pixels);
            }
            if (!ok) {
                std::fprintf(stderr, "Could not write %s\n", path);
                return 1;
            }
        }
        ++written;
    }
    const eyes::FrameState &frame = sim.engine.frame();
    std::printf("headless: %d frames, %.1fs, mode=%s expression=%s attention=%s page=%s\n", written,
                static_cast<double>(options.seconds), mode_name(frame.mode),
                expression_name(frame.expression), eyes::ui_attention_name(frame.attention),
                page_name(sim.ui.page()));
    if (!options.out.empty()) {
        std::printf("%s written to %s_0000.ppm ...\n", options.composite ? "composites" : "frames",
                    options.out.c_str());
    }
    return 0;
}

#ifndef EYES_SIM_HEADLESS_ONLY

#ifdef EYES_SIM_MAC
// Instant acknowledgment before any TTS exists (the apollo earcon pattern):
// a flash-resident system sound, backgrounded so it never blocks a frame.
void play_earcon(const char *name)
{
    char command[128];
    std::snprintf(command, sizeof(command),
                  "afplay /System/Library/Sounds/%s.aiff >/dev/null 2>&1 &", name);
    (void)std::system(command);
}

// The reply half of the voice loop: macOS TTS, pipelined per sentence (the
// apollo segment pattern) — speech starts at the first complete sentence
// while the model is still streaming, and following sentences queue behind
// it. The device's speaker pipeline replaces the `say` backend later; the
// conversation shape doesn't change.
class SpeechQueue {
public:
    ~SpeechQueue()
    {
        stop_ = true;
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void enqueue(std::string text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\n')) {
            text.erase(text.begin());
        }
        if (text.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(text));
        }
        busy_ = true;
        if (!worker_.joinable()) {
            worker_ = std::thread([this] { loop(); });
        }
    }

    bool speaking() const { return busy_; }

private:
    void loop()
    {
        while (!stop_) {
            std::string next;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty()) {
                    next = queue_.front();
                    queue_.erase(queue_.begin());
                }
            }
            if (next.empty()) {
                busy_ = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::string quoted;
            quoted.reserve(next.size() + 16);
            for (const char c : next) {
                if (c == '\'') {
                    quoted += "'\"'\"'";
                } else {
                    quoted += c;
                }
            }
            // The harness owns the voice (ElevenLabs/Grok via tools/tts.mjs),
            // so the sim stays silent unless LILGUY_DEVICE_TTS=1 forces the
            // local macOS `say` fallback (avoids double audio). The queue still
            // runs so speaking() drives the talking-face timing either way.
            if (std::getenv("LILGUY_DEVICE_TTS") == nullptr) {
                continue;
            }
            // Voice + rate are configurable: LILGUY_VOICE (e.g. "Samantha",
            // "Daniel", or an Enhanced Siri voice) and LILGUY_RATE (wpm).
            const char *voice = std::getenv("LILGUY_VOICE");
            const char *rate = std::getenv("LILGUY_RATE");
            std::string command = "say";
            if (voice != nullptr && voice[0] != '\0') {
                command += " -v '" + std::string(voice) + "'";
            }
            if (rate != nullptr && rate[0] != '\0') {
                command += " -r '" + std::string(rate) + "'";
            }
            command += " '" + quoted + "' >/dev/null 2>&1";
            (void)std::system(command.c_str());
        }
    }

    std::vector<std::string> queue_;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_{false};
};

// Splits complete sentences off the front of the streaming buffer.
std::vector<std::string> take_sentences(std::string &buffer)
{
    std::vector<std::string> sentences;
    std::size_t start = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const char c = buffer[i];
        if ((c == '.' || c == '!' || c == '?') &&
            (i + 1 >= buffer.size() || buffer[i + 1] == ' ' || buffer[i + 1] == '\n')) {
            sentences.push_back(buffer.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    buffer.erase(0, start);
    return sentences;
}
#endif

// The device half of the agent link: a reconnecting NDJSON TCP client to the
// harness (tools/harness.mjs). Real conversations drive real face beats; the
// device never learns provider dialects, only the canonical events.
class AgentLinkClient {
public:
    void poll(std::uint32_t now_ms, std::vector<eyes::AgentEvent> &events)
    {
        if (fd_ < 0) {
            if (static_cast<std::int32_t>(now_ms - next_attempt_ms_) < 0) {
                return;
            }
            next_attempt_ms_ = now_ms + 2000U;
            begin_connect();
            return;
        }
        if (connecting_) {
            // A zero-byte send distinguishes "established" from "pending".
            if (send(fd_, "", 0, 0) == 0) {
                connecting_ = false;
                std::printf("agent link connected (127.0.0.1:8791)\n");
                std::fflush(stdout);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOTCONN &&
                       errno != EINPROGRESS) {
                drop();
            }
            return;
        }
        char chunk[2048];
        while (true) {
            const ssize_t got = recv(fd_, chunk, sizeof(chunk), 0);
            if (got > 0) {
                buffer_.append(chunk, static_cast<std::size_t>(got));
                continue;
            }
            if (got == 0) {
                std::printf("agent link disconnected\n");
                std::fflush(stdout);
                drop();
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            drop();
            return;
        }
        std::size_t newline;
        while ((newline = buffer_.find('\n')) != std::string::npos) {
            const std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            eyes::AgentEvent event{};
            if (eyes::agent_link::parse(line, event)) {
                events.push_back(event);
            }
        }
    }

    void send_event(const eyes::AgentEvent &event)
    {
        if (fd_ < 0 || connecting_) {
            return;
        }
        const std::string line = eyes::agent_link::encode(event);
        (void)send(fd_, line.data(), line.size(), 0);
    }

    bool connected() const { return fd_ >= 0 && !connecting_; }

private:
    void begin_connect()
    {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        // With no harness listening, the probe send() on the refused socket
        // raised SIGPIPE and killed the simulator ~2 s after launch. The link
        // is optional; a dead peer must read as "not connected", not exit.
        const int no_sigpipe = 1;
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(8791);
        address.sin_addr.s_addr = htonl(0x7F000001U);  // 127.0.0.1
        const int result = connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        if (result == 0) {
            connecting_ = false;
            std::printf("agent link connected (127.0.0.1:8791)\n");
            std::fflush(stdout);
        } else if (errno == EINPROGRESS) {
            connecting_ = true;
        } else {
            drop();
        }
    }

    void drop()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = -1;
        connecting_ = false;
        buffer_.clear();
    }

    int fd_{-1};
    bool connecting_{false};
    std::uint32_t next_attempt_ms_{0};
    std::string buffer_;
};

int run_window(const Options &options)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    Simulation sim(start_settings(options));
    apply_start_options(sim, options);
#ifdef EYES_SIM_MAC
    // Swap the null camera backend for the webcam: the exact move the custom
    // board makes later with its real drivers, through the same interface.
    static WebcamCameraService webcam_camera;
    sim.services.camera = &webcam_camera;
    sim.services.caps.set(eyes::Capability::camera_front);
    sim.services.caps.set(eyes::Capability::camera_rear);
#endif
    // The digital crown: a sim fake fed by the scroll wheel (Track B hardware
    // proven now, like the webcam). Scroll browses the look; press = random.
    static SimCrownService sim_crown;
    sim.services.crown = &sim_crown;
    sim.services.caps.set(eyes::Capability::crown);
    // The haptic motor: a sim fake that jolts the window, so feedback timing is
    // designed against the real HapticsService before the LRA exists.
    static SimHapticsService sim_haptics;
    sim.services.haptics = &sim_haptics;
    sim.services.caps.set(eyes::Capability::haptics);
    // First boot runs onboarding (name + wake word); a saved config skips it
    // and goes straight to listening for the wake word.
    sim.load_or_begin_onboarding();
    sim.arm_wake_listening();

    Panel panel;
    panel.build();
    // The device is sized from its real proportions: the 43.76 mm active
    // circle is the framebuffer, and the shell follows at 55 mm. Shrink it if
    // the stage would not fit the screen: a clipped borderless window has no
    // title bar to drag it back by.
    float requested_scale = options.scale;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        for (int attempt = 0; attempt < 12; ++attempt) {
            const DeviceMetrics probe = DeviceMetrics::from_display(
                static_cast<float>(eyes::kScreenWidth) * requested_scale);
            const Stage fit = Stage::lay_out(probe, panel.height());
            if (fit.width <= usable.w && fit.height <= usable.h) {
                break;
            }
            requested_scale *= 0.92F;
        }
        if (requested_scale < options.scale) {
            std::printf("scale %.2f -> %.2f so the stage fits %dx%d\n",
                        static_cast<double>(options.scale), static_cast<double>(requested_scale),
                        usable.w, usable.h);
        }
    }
    SDL_DisplayMode desktop{};
    const bool fill = options.max && SDL_GetDesktopDisplayMode(0, &desktop) == 0 &&
                      desktop.w > 0 && desktop.h > 0;
    if (fill) {
        // Largest device whose shell (plus a margin of air) fits the shorter
        // side of the display.
        const float margin = 40.0F;
        const float limit = static_cast<float>(std::min(desktop.w, desktop.h)) - margin * 2.0F;
        requested_scale = limit / static_cast<float>(eyes::kScreenWidth);
        for (int attempt = 0; attempt < 40; ++attempt) {
            const DeviceMetrics probe = DeviceMetrics::from_display(
                static_cast<float>(eyes::kScreenWidth) * requested_scale);
            if (probe.body_radius * 2.0F <= limit) {
                break;
            }
            requested_scale *= 0.97F;
        }
        std::printf("--max: %dx%d display, scale %.2f\n", desktop.w, desktop.h,
                    static_cast<double>(requested_scale));
    } else if (options.max) {
        std::fprintf(stderr, "--max: no display mode (%s); using the normal stage\n",
                     SDL_GetError());
    }
    const float display_px = static_cast<float>(eyes::kScreenWidth) * requested_scale;
    const DeviceMetrics metrics = DeviceMetrics::from_display(display_px);
    const Stage stage = fill ? Stage::lay_out_fill(metrics, panel.height(), desktop.w, desktop.h)
                             : Stage::lay_out(metrics, panel.height());
    bool show_panel = false;
    const int window_width = stage.width;
    const int window_height = stage.height;
    const float device_cx = stage.device_cx;
    const float device_cy = stage.device_cy;
    int panel_x = stage.panel_x;
    int panel_y = stage.panel_y;

    SDL_Window *window = SDL_CreateWindow(
        "LilGuy Eyes simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width,
        window_height,
        SDL_WINDOW_ALLOW_HIGHDPI | (options.framed ? 0U : SDL_WINDOW_BORDERLESS) |
            (fill ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0U));
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer *sdl_renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (sdl_renderer == nullptr) {
        sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (sdl_renderer == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(sdl_renderer, window_width, window_height);
    // Everything outside the device and the panel stays transparent, so the
    // puck simply sits on the desktop.
    SDL_SetRenderDrawBlendMode(sdl_renderer, SDL_BLENDMODE_NONE);
    // On a HiDPI display the backbuffer holds more pixels than the window has
    // points. Composite the device at that full physical resolution rather
    // than letting the GPU stretch a point-resolution texture; the debug
    // chrome stays a point-resolution overlay so its pixel fonts keep their
    // size.
    int output_width = window_width;
    int output_height = window_height;
    SDL_GetRendererOutputSize(sdl_renderer, &output_width, &output_height);
    const int dpi = std::max(1, output_width / window_width);
    if (dpi > 1) {
        std::printf("HiDPI backbuffer %dx%d: device composited at %dx\n", output_width,
                    output_height, dpi);
    }
    void *native_window = nullptr;
#ifdef EYES_SIM_MAC
    if (!options.framed) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info) == SDL_TRUE) {
            native_window = info.info.cocoa.window;
            eyes_mac_prepare_window(native_window);
        }
    }
#endif
    SDL_Texture *texture =
        SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                          window_width * dpi, window_height * dpi);
    SDL_Texture *chrome_texture =
        SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                          window_width, window_height);
    if (texture == nullptr || chrome_texture == nullptr) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(sdl_renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(chrome_texture, SDL_BLENDMODE_BLEND);
    // The device layer bakes and composites at physical resolution. Layout,
    // hit tests and the chrome stay in window points, so only the bakes and
    // the per-frame device blits use the scaled metrics.
    const DeviceMetrics metrics_px =
        dpi == 1 ? metrics : DeviceMetrics::from_display(display_px * static_cast<float>(dpi));
    const float fdpi = static_cast<float>(dpi);
    Canvas canvas(window_width * dpi, window_height * dpi);
    // Backdrop and shell are static, so they are rendered once and composited
    // each frame; only the display, the buttons and the panel are live.
    Canvas backdrop(window_width * dpi, window_height * dpi);
    render_shadow(backdrop, metrics_px, device_cx * fdpi, device_cy * fdpi);
    // The debug chrome (tooltip, key captions, panel) keeps its point-space
    // pixel-art look on its own overlay texture.
    Canvas chrome(window_width, window_height);
    Layer shell;
    build_shell_layer(shell, metrics_px);
    // [key][pressed]
    Layer keys[2][2];
    int key_offset_x[2] = {0, 0};
    int key_offset_y[2] = {0, 0};
    const float key_angles[2] = {kBootButtonAngle, kPowerButtonAngle};
    for (int key = 0; key < 2; ++key) {
        for (int state = 0; state < 2; ++state) {
            int ox = 0;
            int oy = 0;
            build_key_layer(keys[key][state], metrics_px, key_angles[key], state == 1, ox, oy);
            key_offset_x[key] = ox;
            key_offset_y[key] = oy;
        }
    }
    Layer panel_shadow;
    constexpr int kPanelShadowMargin = 40;
    build_panel_shadow(panel_shadow, kPanelWidth, panel.height(), kPanelShadowMargin);

    SDL_Cursor *cursor_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    SDL_Cursor *cursor_move = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
    SDL_Cursor *cursor_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    SDL_Cursor *cursor_current = nullptr;

    Tooltip tooltip{};
    tooltip.place(stage);
    PanelState panel_state{};
    bool running = true;
    bool shell_dragging = false;   // dragging the puck moves the whole window
    int drag_origin_x = 0;
    int drag_origin_y = 0;
    int window_origin_x = 0;
    int window_origin_y = 0;
    bool click_through = false;
    bool screen_dragging = false;
    bool tilt_dragging = false;
    bool panel_dragging = false;
    int panel_grab_x = 0;
    int panel_grab_y = 0;
    bool right_dragging = false;
    eyes::Vec2 tilt_origin{};
    int right_origin_x = 0;
    int right_origin_y = 0;
    std::uint32_t last_frame_ms = SDL_GetTicks();
    std::uint32_t last_render_ms = last_frame_ms;
    std::uint32_t fps_window_ms = last_frame_ms;
    AgentLinkClient agent_link;
    std::vector<eyes::AgentEvent> agent_events;
    eyes::ConversationState conversation_state = eyes::ConversationState::idle;
    std::string reply_text;
#ifdef EYES_SIM_MAC
    SpeechQueue speech;
    std::string speech_pending;  // streamed text awaiting a sentence boundary
    bool spoke_any = false;
    bool was_speaking = false;
#endif
    int fps_frames = 0;
    bool first_frame = true;
    bool crown_was_pressed = false;
    bool audio_preview = false;  // U key: hold the audio-bar visualizer on
    bool onboarding_listening = false;  // onboarding auto-listens (no hold-A)
    bool transparency_reasserted = false;
    {
        int origin_x = 0;
        int origin_y = 0;
        SDL_GetWindowPosition(window, &origin_x, &origin_y);
        std::printf("window %dx%d at %d,%d (%s)\n", window_width, window_height, origin_x,
                    origin_y, options.framed ? "framed" : "transparent");
        std::fflush(stdout);
    }

    // Framebuffer coordinate of a window point, or false if the point is off
    // the glass. Only the active circle counts, exactly like the real panel.
    const auto to_screen = [&](int wx, int wy, float &sx, float &sy) {
        const float dx = static_cast<float>(wx) + 0.5F - device_cx;
        const float dy = static_cast<float>(wy) + 0.5F - device_cy;
        if (dx * dx + dy * dy > metrics.display_radius * metrics.display_radius) {
            return false;
        }
        const float to_frame = static_cast<float>(eyes::kScreenWidth) /
                               (metrics.display_radius * 2.0F);
        sx = (dx + metrics.display_radius) * to_frame;
        sy = (dy + metrics.display_radius) * to_frame;
        return true;
    };

    const auto set_tilt_from_pad = [&](int wx, int wy) {
        const Rect &pad = panel.tilt_pad();
        const float half_w = static_cast<float>(pad.w) * 0.5F;
        const float half_h = static_cast<float>(pad.h) * 0.5F;
        sim.sensors.tilt.x = std::clamp(
            (static_cast<float>(wx - panel_x - pad.x) - half_w) / (half_w - 6.0F), -1.0F, 1.0F);
        sim.sensors.tilt.y = std::clamp(
            (static_cast<float>(wy - panel_y - pad.y) - half_h) / (half_h - 6.0F), -1.0F, 1.0F);
    };

    const auto activate = [&](const Button &button, std::uint32_t now_ms) {
        apply_control(sim, button.control, button.row, now_ms, panel_state.captures);
    };

    while (running) {
        const std::uint32_t now_ms = SDL_GetTicks();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_MOUSEBUTTONDOWN: {
                    float sx = 0.0F;
                    float sy = 0.0F;
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        if (to_screen(event.button.x, event.button.y, sx, sy)) {
                            screen_dragging = true;
                            sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressed, sx, sy, now_ms);
                            break;
                        }
                        // The shell's own side buttons, held like the real ones.
                        const float bdx = static_cast<float>(event.button.x) + 0.5F - device_cx;
                        const float bdy = static_cast<float>(event.button.y) + 0.5F - device_cy;
                        if (hits_button(metrics, kBootButtonAngle, bdx, bdy)) {
                            panel_state.boot_down = true;
                            break;
                        }
                        if (hits_button(metrics, kPowerButtonAngle, bdx, bdy)) {
                            panel_state.power_down = true;
                            break;
                        }
                        if (tooltip.rect.contains(event.button.x, event.button.y)) {
                            show_panel = !show_panel;
                            tooltip.open = show_panel;
                            break;
                        }
                        // Anywhere else on the shell drags the whole window.
                        if (bdx * bdx + bdy * bdy <=
                            metrics.body_radius * metrics.body_radius) {
                            shell_dragging = true;
                            SDL_GetGlobalMouseState(&drag_origin_x, &drag_origin_y);
                            SDL_GetWindowPosition(window, &window_origin_x, &window_origin_y);
                            break;
                        }
                        if (!show_panel) {
                            break;
                        }
                        // Dragging the card's title bar moves the panel.
                        if (event.button.x >= panel_x && event.button.x < panel_x + kPanelWidth &&
                            event.button.y >= panel_y && event.button.y < panel_y + 26) {
                            panel_dragging = true;
                            panel_grab_x = event.button.x - panel_x;
                            panel_grab_y = event.button.y - panel_y;
                            break;
                        }
                        for (const Button &button : panel.buttons()) {
                            Rect rect = button.rect;
                            rect.x += panel_x;
                            rect.y += panel_y;
                            if (!rect.contains(event.button.x, event.button.y)) {
                                continue;
                            }
                            if (button.control == Control::tilt_pad) {
                                tilt_dragging = true;
                                set_tilt_from_pad(event.button.x, event.button.y);
                            } else if (button.control == Control::boot) {
                                panel_state.boot_down = true;
                            } else if (button.control == Control::power) {
                                panel_state.power_down = true;
                            } else if (button.control == Control::sound) {
                                panel_state.sound_down = true;
                                sim.sensors.loud = true;
                            } else {
                                panel_state.held = button.control;
                                panel_state.held_row = button.row;
                                activate(button, now_ms);
                            }
                            break;
                        }
                    } else if (event.button.button == SDL_BUTTON_RIGHT &&
                               to_screen(event.button.x, event.button.y, sx, sy)) {
                        right_dragging = true;
                        right_origin_x = event.button.x;
                        right_origin_y = event.button.y;
                        tilt_origin = sim.sensors.tilt;
                    }
                    break;
                }
                case SDL_MOUSEMOTION: {
                    float sx = 0.0F;
                    float sy = 0.0F;
                    if (screen_dragging && to_screen(event.motion.x, event.motion.y, sx, sy)) {
                        sim.deliver_touch(eyes::DeviceUi::TouchPhase::pressing, sx, sy, now_ms);
                    }
                    if (tilt_dragging) {
                        set_tilt_from_pad(event.motion.x, event.motion.y);
                    }
                    if (shell_dragging) {
                        int global_x = 0;
                        int global_y = 0;
                        SDL_GetGlobalMouseState(&global_x, &global_y);
                        SDL_SetWindowPosition(window,
                                              window_origin_x + global_x - drag_origin_x,
                                              window_origin_y + global_y - drag_origin_y);
                    }
                    if (panel_dragging) {
                        panel_x = std::clamp(event.motion.x - panel_grab_x, -kPanelWidth + 60,
                                             window_width - 60);
                        panel_y = std::clamp(event.motion.y - panel_grab_y,
                                             -panel.height() + 40, window_height - 40);
                    }
                    if (right_dragging) {
                        // Half the panel of drag equals full tilt on that axis;
                        // the tilt holds when the mouse is released.
                        const float span = metrics.display_radius;
                        sim.sensors.tilt.x = std::clamp(
                            tilt_origin.x +
                                static_cast<float>(event.motion.x - right_origin_x) / span,
                            -1.0F, 1.0F);
                        sim.sensors.tilt.y = std::clamp(
                            tilt_origin.y +
                                static_cast<float>(event.motion.y - right_origin_y) / span,
                            -1.0F, 1.0F);
                    }
                    break;
                }
                case SDL_MOUSEBUTTONUP: {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        float sx = 0.0F;
                        float sy = 0.0F;
                        if (screen_dragging) {
                            screen_dragging = false;
                            if (!to_screen(event.button.x, event.button.y, sx, sy)) {
                                sim.deliver_touch(eyes::DeviceUi::TouchPhase::press_lost, 0.0F,
                                                  0.0F, now_ms);
                            } else {
                                sim.deliver_touch(eyes::DeviceUi::TouchPhase::released, sx, sy,
                                                  now_ms);
                            }
                        }
                        tilt_dragging = false;
                        panel_dragging = false;
                        shell_dragging = false;
                        if (panel_state.power_down) {
                            // A PWR click surfaces one short-press event.
                            sim.power_click_pending = true;
                        }
                        if (panel_state.sound_down) {
                            sim.sensors.loud = false;
                        }
                        panel_state.boot_down = false;
                        panel_state.power_down = false;
                        panel_state.sound_down = false;
                        panel_state.held = Control::none;
                        panel_state.held_row = -1;
                    } else if (event.button.button == SDL_BUTTON_RIGHT) {
                        right_dragging = false;
                    }
                    break;
                }
                case SDL_MOUSEWHEEL:
                    // Feed the crown service (the real capability path), not a
                    // direct nudge — the crown poll below turns it into input.
                    sim_crown.add_detents(event.wheel.y > 0 ? 1.0F : -1.0F);
                    sim.note_interaction(SDL_GetTicks());
                    break;
                case SDL_KEYDOWN: {
                    if (event.key.repeat != 0U) {
                        break;
                    }
                    sim.note_interaction(SDL_GetTicks());
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            running = false;
                            break;
                        case SDLK_b:
                            panel_state.boot_down = true;
                            break;
                        case SDLK_n:
                            panel_state.power_down = true;
                            break;
                        case SDLK_s:
                            if (sim.engine.frame().mode == eyes::InteractionMode::sleeping) {
                                sim.engine.wake();
                            } else {
                                sim.engine.sleep();
                            }
                            break;
                        case SDLK_p:
                            if (sim.engine.frame().mode != eyes::InteractionMode::sleeping) {
                                sim.engine.sleep();
                            }
                            break;
                        case SDLK_x:
                            sim.sensors.shake_samples = 3;
                            break;
                        case SDLK_SPACE:
                            sim.sensors.loud = true;
                            panel_state.sound_down = true;
                            break;
                        case SDLK_1:
                        case SDLK_2:
                        case SDLK_3: {
                            // Reach the wanted level through the real menu row.
                            const std::uint8_t want =
                                static_cast<std::uint8_t>(event.key.keysym.sym - SDLK_1);
                            for (int guard = 0; guard < 3; ++guard) {
                                if (sim.ui.settings().imu_level % 3U == want) {
                                    break;
                                }
                                sim.ui.run_setting(eyes::UiSetting::tilt);
                            }
                            break;
                        }
                        case SDLK_q:
                            sim.engine.set_orientation(sim.engine.orientation() - kPi / 12.0F);
                            sim.ui.note_orientation(sim.engine.orientation());
                            break;
                        case SDLK_e:
                            sim.engine.set_orientation(sim.engine.orientation() + kPi / 12.0F);
                            sim.ui.note_orientation(sim.engine.orientation());
                            break;
                        case SDLK_0:
                            sim.ui.run_setting(eyes::UiSetting::reset_rotation);
                            break;
#ifdef EYES_SIM_MAC
                        case SDLK_a:
                            // Push-to-talk: hold A, speak, release. Half
                            // duplex by design (the doctrine): nothing else
                            // listens while the reply speaks.
                            if (eyes_mac_speech_begin() != 0) {
                                sim.engine.hold_expression(eyes::Expression::listening);
                                sim.show_glyph(Simulation::Glyph::listening, now_ms);
                                std::printf("listening... (release A to send)\n");
                            } else {
                                std::printf(
                                    "speech unavailable (mic or recognition denied)\n");
                            }
                            std::fflush(stdout);
                            break;
#endif
#ifdef EYES_SIM_MAC
                        case SDLK_h:
                            // Toggle hands-free wake listening (opt-in).
                            sim.wake_listening = !sim.wake_listening;
                            if (sim.wake_listening) {
                                if (eyes_mac_wake_enable(sim.wake_word.c_str()) != 0) {
                                    std::printf("hands-free ON — say \"%s ...\"\n",
                                                sim.wake_word.c_str());
                                } else {
                                    sim.wake_listening = false;
                                    std::printf("wake mode unavailable (mic denied?)\n");
                                }
                            } else {
                                eyes_mac_wake_disable();
                                std::printf("hands-free off\n");
                            }
                            std::fflush(stdout);
                            break;
#endif
                        case SDLK_j:
                            sim_crown.set_pressed(true);  // crown press (hold J)
                            break;
                        case SDLK_o:
                            // Re-run onboarding (hey, name, wake word).
                            sim.onboarding.restart(now_ms);
                            std::printf("onboarding: just talk when the word is written\n");
                            std::fflush(stdout);
                            break;
                        case SDLK_k:
                            sim.apps.toggle(Simulation::kAppWallet);
                            std::printf("wallet %s\n",
                                        sim.apps.target() == Simulation::kAppWallet ? "ON"
                                                                                    : "off");
                            std::fflush(stdout);
                            break;
                        case SDLK_COMMA:  // the macOS "preferences" key
                            sim.apps.toggle(Simulation::kAppSettings);
                            std::printf("settings %s\n",
                                        sim.apps.target() == Simulation::kAppSettings ? "ON"
                                                                                      : "off");
                            std::fflush(stdout);
                            break;
                        case SDLK_PERIOD:  // sits next to ',' — clock/timer
                            sim.apps.toggle(Simulation::kAppClock);
                            std::printf("clock %s\n",
                                        sim.apps.target() == Simulation::kAppClock ? "ON"
                                                                                   : "off");
                            std::fflush(stdout);
                            break;
                        case SDLK_SEMICOLON:  // demo: start a 10s timer + show it
                            sim.start_timer(10U, now_ms);
                            if (sim.apps.target() != Simulation::kAppClock) {
                                sim.apps.toggle(Simulation::kAppClock);
                            }
                            std::printf("timer: 10s\n");
                            std::fflush(stdout);
                            break;
                        case SDLK_d:  // demo: "show balance" (harness balance_reveal)
                            sim.reveal_balance("$128.40", "dollar wallet", now_ms);
                            sim.engine.request_blink();
                            std::printf("demo: balance revealed\n");
                            std::fflush(stdout);
#ifdef EYES_SIM_MAC
                            play_earcon("Tink");
#endif
                            break;
                        case SDLK_f:  // demo: funds received (harness funds_received)
                            sim.receive_funds("+$20.00", "from charlie", now_ms);
                            sim.engine.request_blink();
                            std::printf("demo: funds received\n");
                            std::fflush(stdout);
#ifdef EYES_SIM_MAC
                            play_earcon("Glass");
                            if (sim.set_haptics) {
                                sim.services.haptics->pulse(120, 0.8F);
                            }
#endif
                            break;
                        case SDLK_l:  // demo: cast an agent to the TV (pair → live → end)
                            if (!sim.cast.active()) {
                                char sid[32];
                                std::snprintf(sid, sizeof(sid), "cast-%u", now_ms);
                                const eyes::AgentEvent ev =
                                    sim.cast.begin("482913", "browser", sid);
                                if (agent_link.connected()) {
                                    agent_link.send_event(ev);
                                }
                                // No real harness in the demo: synthesize the pairing
                                // so the session goes live for the visual.
                                eyes::AgentEvent paired{};
                                paired.type = eyes::AgentEventType::cast_paired;
                                paired.id = sim.cast.session();
                                sim.cast.on_event(paired);
                                std::printf("demo: casting to TV (code 482913)\n");
                            } else {
                                const eyes::AgentEvent ev = sim.cast.end();
                                if (agent_link.connected()) {
                                    agent_link.send_event(ev);
                                }
                                std::printf("demo: cast ended\n");
                            }
                            std::fflush(stdout);
                            break;
                        case SDLK_u:  // preview the audio-bar visualizer (dynamic island)
                            audio_preview = !audio_preview;
                            sim.audio_playing = audio_preview;
                            std::printf("audio bars %s\n", audio_preview ? "ON" : "off");
                            std::fflush(stdout);
                            break;
                        case SDLK_m:
                            sim.apps.toggle(Simulation::kAppMusic);
                            std::printf("now playing %s\n",
                                        sim.apps.target() == Simulation::kAppMusic ? "ON"
                                                                                   : "off");
                            std::fflush(stdout);
                            break;
                        case SDLK_w:
                            // render_frame carries the transition both ways;
                            // closing ends in a blink.
                            sim.apps.toggle(Simulation::kAppCamera);
                            std::printf("camera viewfinder %s (%s transition)\n",
                                        sim.apps.target() == Simulation::kAppCamera
                                            ? "ON (CameraService front lens)"
                                            : "off",
                                        kTransitionNames[static_cast<int>(
                                            sim.transition_style)]);
                            std::fflush(stdout);
                            break;
                        case SDLK_y:
                            sim.blink_snap = !sim.blink_snap;
#ifdef EYES_SIM_MAC
                            if (sim.blink_snap) {
                                (void)eyes_mac_camera_take_blinks();  // drain backlog
                            }
#endif
                            std::printf("blink-to-shutter %s\n",
                                        sim.blink_snap ? "ON (blink at the camera to snap)"
                                                       : "off");
                            std::fflush(stdout);
                            break;
                        case SDLK_t:
                            sim.transition_style = static_cast<TransitionStyle>(
                                (static_cast<int>(sim.transition_style) + 1) %
                                static_cast<int>(TransitionStyle::count));
                            std::printf("transition style: %s\n",
                                        kTransitionNames[static_cast<int>(
                                            sim.transition_style)]);
                            std::fflush(stdout);
                            break;
                        case SDLK_g: {
                            char path[256];
                            std::snprintf(path, sizeof(path), "host/sim_capture_%02d.ppm",
                                          panel_state.captures);
                            if (write_ppm(path, sim.pixels)) {
                                ++panel_state.captures;
                                std::printf("wrote %s\n", path);
                            }
                            std::fflush(stdout);
                            break;
                        }
                        case SDLK_TAB:
                            show_panel = !show_panel;
                            tooltip.open = show_panel;
                            break;
                        default:
                            break;
                    }
                    break;
                }
                case SDL_KEYUP:
                    switch (event.key.keysym.sym) {
                        case SDLK_j:
                            sim_crown.set_pressed(false);
                            break;
#ifdef EYES_SIM_MAC
                        case SDLK_a:
                            eyes_mac_speech_end();
                            if (sim.glyph == Simulation::Glyph::listening) {
                                sim.glyph = Simulation::Glyph::none;
                            }
                            break;
#endif
                        case SDLK_SPACE:
                            sim.sensors.loud = false;
                            panel_state.sound_down = false;
                            break;
                        case SDLK_b:
                            panel_state.boot_down = false;
                            break;
                        case SDLK_n:
                            panel_state.power_down = false;
                            sim.power_click_pending = true;
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
        }

        const std::uint32_t elapsed_ms =
            std::clamp<std::uint32_t>(now_ms - last_frame_ms, 1U, 100U);
        last_frame_ms = now_ms;

        // Digital crown: rotation scrolls the settings list when it is open;
        // press = listen (the same as PWR). The look is fixed, so the crown
        // never browses it.
        if (sim.services.caps.has(eyes::Capability::crown)) {
            const float detents = sim.services.crown->take_rotation();
            const bool in_settings = sim.apps.fully_open(Simulation::kAppSettings);
            if (detents != 0.0F && in_settings) {
                // The crown scrolls the settings list, one row per detent.
                const int step = detents > 0.0F ? 1 : -1;
                int next = sim.settings_sel + step;
                next = std::clamp(next, 0, Simulation::kSettingsRows - 1);
                if (next != sim.settings_sel) {
                    sim.settings_sel = next;
                    if (sim.set_haptics) { sim.services.haptics->pulse(12, 0.5F); } // row tick
                }
            }
            if (sim.services.crown->pressed() && !crown_was_pressed) {
                if (in_settings) {
                    // Press toggles the highlighted setting.
                    if (sim.settings_activate() && sim.set_haptics) {
                        sim.services.haptics->pulse(18, 0.7F);
                    }
                } else if (!sim.apps.active()) {
                    sim.host.request_listen();
                    if (sim.set_haptics) { sim.services.haptics->pulse(18, 0.7F); }
                }
            }
            crown_was_pressed = sim.services.crown->pressed();
        }
        sim.update_auto_sleep(now_ms);
        if (sim.tick_timer(now_ms)) {
            // Timer done: the banner + a long buzz ARE the alarm (buttonless).
            sim.notif_queue.push_back("Timer done");
            if (sim.set_haptics) { sim.services.haptics->pulse(220, 1.0F); }
        }
        sim.sensors.pump(sim.engine, sim.ui, now_ms);
        sim.pump_gesture(now_ms);
        sim.poll_buttons(panel_state.boot_down, now_ms);
        sim.service_calibration(now_ms);
        sim.ui.set_perf({static_cast<std::uint32_t>(panel_state.fps), 0U, 0U, 0U});
        sim.ui.tick(now_ms);

        // The agent link: canonical events in, face beats and text out. The
        // conversation runs on the harness; the creature is the progress UI.
        // PWR or the crown pressed: listen, the same edge as the wake word.
        if (sim.host.listen_requested) {
            sim.host.listen_requested = false;
            sim.note_interaction(now_ms);
#ifdef EYES_SIM_MAC
            if (!sim.onboarding.active() && eyes_mac_speech_begin() != 0) {
                sim.engine.hold_expression(eyes::Expression::listening);
                sim.show_glyph(Simulation::Glyph::listening, now_ms);
                std::printf("listening... (just talk)\n");
            } else if (!sim.onboarding.active()) {
                std::printf("speech unavailable (mic or recognition denied)\n");
            }
#else
            std::printf("listen: no speech backend on this platform\n");
#endif
            std::fflush(stdout);
        }
#ifdef EYES_SIM_MAC
        // Onboarding listens on its own — you just talk, no key. Each step
        // starts a fresh recognition; the transcript handler fills the field
        // and clears the flag so the next step re-arms.
        if (sim.onboarding.active() && !onboarding_listening && !sim.wake_listening) {
            if (eyes_mac_speech_begin() != 0) {
                onboarding_listening = true;
            }
        }
        // Hands-free: a command heard after the wake word becomes a turn (unless
        // a surface/confirm/onboarding owns input). The face shows listening the
        // moment the wake word lands.
        if (sim.wake_listening) {
            // The face perks up the instant the wake word is heard.
            static bool was_awake = false;
            const bool awake = eyes_mac_wake_is_awake() != 0;
            if (awake && !was_awake && conversation_state == eyes::ConversationState::idle) {
                sim.engine.hold_expression(eyes::Expression::listening);
            }
            was_awake = awake;
            char command[1024];
            if (eyes_mac_wake_take_command(command, sizeof(command)) != 0 &&
                command[0] != '\0' && !sim.onboarding.active() && !sim.confirm_open &&
                conversation_state == eyes::ConversationState::idle) {
                std::printf("wake command: %s\n", command);
                std::fflush(stdout);
                if (agent_link.connected()) {
                    eyes::AgentEvent turn{};
                    turn.type = eyes::AgentEventType::user_text;
                    turn.text = command;
                    agent_link.send_event(turn);
                }
            }
        }
        {
            char transcript[1024];
            if (eyes_mac_speech_take_transcript(transcript, sizeof(transcript)) != 0) {
                std::printf("you said: %s\n", transcript);
                std::fflush(stdout);
                sim.note_interaction(now_ms);
                if (sim.onboarding.active()) {
                    // Onboarding captures the name, then the wake word. End the
                    // session so the next step re-arms a fresh recognition.
                    sim.onboarding_input(transcript, now_ms);
                    eyes_mac_speech_end();
                    onboarding_listening = false;
                } else if (sim.confirm_open) {
                    // A consent card is up: the spoken words ARE the approval
                    // passphrase, not a chat turn. The harness verifies it.
                    eyes::AgentEvent answer{};
                    answer.type = eyes::AgentEventType::confirm;
                    answer.id = sim.confirm_id;
                    answer.text = transcript;
                    agent_link.send_event(answer);
                    sim.confirm_open = false;
                    sim.engine.release_expression();
                    sim.engine.request_blink();
                } else if (agent_link.connected()) {
                    eyes::AgentEvent turn{};
                    turn.type = eyes::AgentEventType::user_text;
                    turn.text = transcript;
                    agent_link.send_event(turn);
                    // Listening rolls into thinking when run_started lands.
                } else {
                    sim.engine.release_expression();
                    std::printf("(no harness connected: run node tools/harness.mjs)\n");
                    std::fflush(stdout);
                }
            }
        }
#endif
#ifdef EYES_SIM_MAC
        {
            // The face stays in its talking mood until the voice actually
            // finishes; the release rides speech's falling edge.
            const bool speaking_now = speech.speaking();
            if (was_speaking && !speaking_now &&
                conversation_state == eyes::ConversationState::idle) {
                sim.engine.release_expression();
                sim.engine.request_blink();
            }
            was_speaking = speaking_now;
            // Speaking drives the audio-bar takeover (unless a manual preview is
            // holding it on via the U key).
            if (!audio_preview) sim.audio_playing = speaking_now;
        }
#endif
        // Drain queued cast-cursor packets from the trackpad to the harness/VM.
        if (!sim.pending_cast_events.empty()) {
            if (agent_link.connected()) {
                for (const eyes::AgentEvent &ev : sim.pending_cast_events) {
                    agent_link.send_event(ev);
                }
            }
            sim.pending_cast_events.clear();
        }
        // A tapped consent answer goes back to the harness, then the card
        // closes locally (the harness also sends confirm_close).
        if (sim.pending_confirm_answer >= 0 && sim.confirm_open) {
            eyes::AgentEvent answer{};
            answer.type = eyes::AgentEventType::confirm;
            answer.id = sim.confirm_id;
            answer.text = sim.pending_confirm_answer == 1 ? "yes" : "no";
            agent_link.send_event(answer);
            // Approve gets a warm double-tap you can feel through a pocket;
            // decline gets one short blip. The hand knows the answer landed.
            if (sim.set_haptics) {
                if (sim.pending_confirm_answer == 1) { sim.services.haptics->pulse(200, 0.9F); }
                else { sim.services.haptics->pulse(40, 0.6F); }
            }
            // The face confirms visually too: a check for approve, a cross for
            // decline (the dynamic-island vocabulary).
            sim.show_glyph(sim.pending_confirm_answer == 1 ? Simulation::Glyph::success
                                                           : Simulation::Glyph::error,
                           now_ms);
            sim.confirm_open = false;
            sim.pending_confirm_answer = -1;
            sim.engine.release_expression();
            sim.engine.request_blink();
        }
        agent_events.clear();
        agent_link.poll(now_ms, agent_events);
        for (const eyes::AgentEvent &agent_event : agent_events) {
            const eyes::ConversationState previous = conversation_state;
            conversation_state = eyes::next_conversation_state(conversation_state, agent_event);
            switch (agent_event.type) {
                // The face IS the progress indicator: conversation states map
                // to held expressions (mood layer), not text overlays.
                case eyes::AgentEventType::run_started:
                    reply_text.clear();
                    sim.engine.hold_expression(eyes::Expression::thinking);
#ifdef EYES_SIM_MAC
                    speech_pending.clear();
                    spoke_any = false;
                    // Acknowledgment lands before any TTS exists.
                    play_earcon("Tink");
#endif
                    break;
                case eyes::AgentEventType::text_delta:
                    if (previous != eyes::ConversationState::streaming) {
                        sim.engine.hold_expression(eyes::Expression::happy);
                    }
                    reply_text += agent_event.text;
#ifdef EYES_SIM_MAC
                    // Speak each sentence the moment it closes, while the
                    // model is still streaming (segment pipelining).
                    speech_pending += agent_event.text;
                    for (std::string &sentence : take_sentences(speech_pending)) {
                        speech.enqueue(std::move(sentence));
                        spoke_any = true;
                    }
#endif
                    break;
                case eyes::AgentEventType::run_finished:
                    if (!agent_event.text.empty() && reply_text.empty()) {
                        reply_text = agent_event.text;
                    }
                    std::printf("agent reply: %s\n", reply_text.c_str());
                    std::fflush(stdout);
#ifdef EYES_SIM_MAC
                    if (!speech_pending.empty()) {
                        speech.enqueue(speech_pending);
                        speech_pending.clear();
                        spoke_any = true;
                    }
                    if (!spoke_any && !reply_text.empty()) {
                        speech.enqueue(reply_text);
                        spoke_any = true;
                    }
                    if (!speech.speaking()) {
                        sim.engine.release_expression();
                        sim.engine.request_blink();
                    }
                    // Otherwise the falling edge of speech releases the face.
#else
                    sim.engine.release_expression();
                    sim.engine.request_blink();
#endif
                    break;
                case eyes::AgentEventType::run_error:
                    std::printf("agent error: %s\n", agent_event.text.c_str());
                    std::fflush(stdout);
                    sim.engine.hold_expression(eyes::Expression::annoyed);
                    sim.ui.show_selection(now_ms, "AGENT ERROR");
#ifdef EYES_SIM_MAC
                    play_earcon("Basso");
#endif
                    break;
                // Owner consent: raise the modal card; the answer goes back
                // as a `confirm` event drained from the loop below.
                case eyes::AgentEventType::confirm_request:
                    sim.confirm_open = true;
                    sim.confirm_id = agent_event.id;
                    sim.confirm_summary = agent_event.text;
                    sim.confirm_swipe = 0.0F;
                    sim.confirm_swiping = false;
                    sim.engine.hold_expression(eyes::Expression::curious);
#ifdef EYES_SIM_MAC
                    play_earcon("Funk");
                    // Speak the request; the owner answers with their voice.
                    speech.enqueue(agent_event.text +
                                   " Hold to talk and say confirm to approve, or cancel.");
#endif
                    std::printf("confirm: %s\n", agent_event.text.c_str());
                    std::fflush(stdout);
                    break;
                case eyes::AgentEventType::confirm_close:
                    if (sim.confirm_id == agent_event.id) {
                        sim.confirm_open = false;
                        sim.pending_confirm_answer = -1;
                        sim.confirm_swipe = 0.0F;
                        sim.confirm_swiping = false;
                        sim.engine.release_expression();
                    }
                    break;
                // An offloaded task finished: announce it without disturbing
                // any running conversation state.
                case eyes::AgentEventType::show_qr:
                    // Pairing QR from the harness; the face gives way to it.
                    sim.set_qr(agent_event.text, now_ms);
                    std::printf("pairing QR shown (%zu rows)\n", sim.qr_matrix.size());
                    std::fflush(stdout);
                    break;
                case eyes::AgentEventType::notification:
                    sim.notif_queue.push_back(
                        agent_event.tool.empty() ? agent_event.text
                                                 : agent_event.tool + ": " + agent_event.text);
                    std::printf("notification: %s\n", agent_event.text.c_str());
                    std::fflush(stdout);
#ifdef EYES_SIM_MAC
                    play_earcon("Ping");
#endif
                    break;
                case eyes::AgentEventType::timer_set: {
                    // Voice-set timer: text is the duration in seconds ("0"
                    // cancels). The device owns the countdown so it survives a
                    // dropped link; opening the clock shows it.
                    const long secs = std::strtol(agent_event.text.c_str(), nullptr, 10);
                    if (secs <= 0) {
                        sim.timer_running = false;
                        std::printf("timer: cancelled\n");
                    } else {
                        sim.start_timer(static_cast<std::uint32_t>(secs), now_ms);
                        if (sim.apps.target() != Simulation::kAppClock) {
                            sim.apps.toggle(Simulation::kAppClock);
                        }
                        std::printf("timer: %lds\n", secs);
                    }
                    std::fflush(stdout);
                    break;
                }
                case eyes::AgentEventType::wallet_update:
                    // Harness pushed wallet state; the wallet surface shows it.
                    sim.wallet_line = agent_event.text;
                    if (!agent_event.tool.empty()) {
                        sim.wallet_last = agent_event.tool;  // reuse tool field as sub-line
                    }
                    std::printf("wallet: %s\n", agent_event.text.c_str());
                    std::fflush(stdout);
                    break;
                case eyes::AgentEventType::balance_reveal:
                    // "Show balance": flash the transient card (it fades itself,
                    // so the number never stays on screen) and glance up.
                    sim.reveal_balance(agent_event.text, agent_event.tool, now_ms);
                    sim.engine.request_blink();
                    std::printf("balance revealed: %s\n", agent_event.text.c_str());
                    std::fflush(stdout);
#ifdef EYES_SIM_MAC
                    play_earcon("Tink");
#endif
                    break;
                case eyes::AgentEventType::funds_received:
                    // Money landed: the received animation + a glance + earcon;
                    // a buzz makes it felt before the eyes even move.
                    sim.receive_funds(agent_event.text, agent_event.tool, now_ms);
                    sim.engine.request_blink();
                    std::printf("funds received: %s %s\n", agent_event.text.c_str(),
                                agent_event.tool.c_str());
                    std::fflush(stdout);
#ifdef EYES_SIM_MAC
                    play_earcon("Glass");
                    if (sim.set_haptics) {
                        sim.services.haptics->pulse(120, 0.8F);
                    }
#endif
                    break;
                case eyes::AgentEventType::cast_paired:
                case eyes::AgentEventType::cast_end:
                    // Harness cast events; the controller advances the session.
                    if (sim.cast.on_event(agent_event)) {
                        std::printf("cast: %s (%s)\n",
                                    sim.cast.live() ? "live" : "ended",
                                    sim.cast.agent().c_str());
                        std::fflush(stdout);
                    }
                    break;
                case eyes::AgentEventType::background_result:
                    std::printf("background task %s: %s\n", agent_event.id.c_str(),
                                agent_event.text.c_str());
                    std::fflush(stdout);
                    sim.ui.show_selection(now_ms, "TASK DONE");
                    sim.engine.request_blink();
#ifdef EYES_SIM_MAC
                    play_earcon("Glass");
                    {
                        std::string spoken = "Task finished. " + agent_event.text;
                        if (spoken.size() > 400) {
                            spoken.resize(400);
                        }
                        speech.enqueue(std::move(spoken));
                    }
#endif
                    break;
                // The agent calling the device's own capabilities: execute
                // through the service layer and answer honestly — absent
                // hardware reports "not fitted", never pretends.
                case eyes::AgentEventType::tool_call: {
                    eyes::AgentEvent result{};
                    result.type = eyes::AgentEventType::tool_result;
                    result.id = agent_event.id;
                    result.tool = agent_event.tool;
                    if (agent_event.tool == "device_status") {
                        const eyes::BatteryStatus battery = sim.host.read_battery();
                        const eyes::Selection look = sim.engine.selection();
                        char status[224];
                        std::snprintf(status, sizeof(status),
                                      "Battery %u%% (%umV, %s). Shape %d, palette %d. "
                                      "Conversation mood held: %s. Photos taken: %d.",
                                      battery.percent, battery.millivolts,
                                      battery.charging ? "charging" : "on battery",
                                      look.shape, look.palette,
                                      sim.engine.expression_held() ? "yes" : "no",
                                      sim.photo_count);
                        result.text = status;
                    } else if (agent_event.tool == "take_photo") {
                        const int before = sim.photo_count;
                        sim.take_photo(now_ms);
                        if (sim.photo_count > before) {
                            char saved[64];
                            std::snprintf(saved, sizeof(saved),
                                          "Photo captured and saved as host/photo_%02d.ppm",
                                          sim.photo_count - 1);
                            result.text = saved;
                        } else {
                            result.text = "The camera produced no frame (permission or "
                                          "hardware issue).";
                        }
                    } else if (agent_event.tool == "nfc_read") {
                        eyes::NfcTag tag{};
                        if (sim.services.nfc->read_tag(tag)) {
                            char uid[48];
                            std::snprintf(uid, sizeof(uid), "NFC tag %u bytes",
                                          tag.uid_length);
                            result.text = uid;
                        } else {
                            result.text = "NFC is not fitted on this device "
                                          "(capability gated off).";
                        }
                    } else if (agent_event.tool == "gps_fix") {
                        eyes::GnssFix fix{};
                        if (sim.services.gnss->fix(fix)) {
                            char position[96];
                            std::snprintf(position, sizeof(position),
                                          "lat %.5f, lon %.5f (+/- %.0fm)", fix.latitude_deg,
                                          fix.longitude_deg,
                                          static_cast<double>(fix.horizontal_accuracy_m));
                            result.text = position;
                        } else {
                            result.text = "GNSS hardware is not fitted on this device "
                                          "(capability gated off).";
                        }
                    } else {
                        result.text = "Unknown tool on this device.";
                    }
                    std::printf("tool %s -> %s\n", agent_event.tool.c_str(),
                                result.text.c_str());
                    std::fflush(stdout);
                    agent_link.send_event(result);
                    break;
                }
                default:
                    break;
            }
        }

        sim.engine.update(elapsed_ms);
        if (sim.engine.consume_selection_changed()) {
            sim.ui.show_selection(now_ms);
        }
        sim.render_frame(now_ms, last_render_ms, first_frame);
        first_frame = false;

        // A slow bob so the device reads as floating rather than pinned.
        const float bob = options.bob
                              ? std::sin(static_cast<float>(now_ms) * 0.0011F) * 4.0F
                              : 0.0F;
        const float cy = device_cy + bob;
        canvas.copy_from(backdrop);
        canvas.blit_layer(shell, static_cast<int>(device_cx * fdpi - metrics_px.tile_center),
                          static_cast<int>(cy * fdpi - metrics_px.tile_center));
        canvas.blit_display_disc(sim.pixels.data(), device_cx * fdpi, cy * fdpi,
                                 metrics_px.display_radius, sim.smooth_display);
        chrome.clear(0x00000000U);
        int hover_x = 0;
        int hover_y = 0;
        {
            int global_x = 0;
            int global_y = 0;
            SDL_GetGlobalMouseState(&global_x, &global_y);
            int origin_x = 0;
            int origin_y = 0;
            SDL_GetWindowPosition(window, &origin_x, &origin_y);
            hover_x = global_x - origin_x;
            hover_y = global_y - origin_y;
        }
        const float hover_dx = static_cast<float>(hover_x) + 0.5F - device_cx;
        const float hover_dy = static_cast<float>(hover_y) + 0.5F - cy;
        // The grey shell is the drag handle; the glass and the keys are not.
        const bool over_glass = hover_dx * hover_dx + hover_dy * hover_dy <=
                                metrics.display_radius * metrics.display_radius;
        const bool over_key = hits_button(metrics, kBootButtonAngle, hover_dx, hover_dy) ||
                              hits_button(metrics, kPowerButtonAngle, hover_dx, hover_dy);
        const bool over_shell = !over_glass && !over_key &&
                                hover_dx * hover_dx + hover_dy * hover_dy <=
                                    metrics.body_radius * metrics.body_radius;
        const bool over_click = over_key || tooltip.rect.contains(hover_x, hover_y);
        SDL_Cursor *wanted = over_shell ? cursor_move : (over_click ? cursor_hand : cursor_arrow);
        if (wanted != cursor_current) {
            cursor_current = wanted;
            SDL_SetCursor(wanted);
        }
        const bool key_down[2] = {panel_state.boot_down, panel_state.power_down};
        const bool key_hover[2] = {
            hits_button(metrics, kBootButtonAngle, hover_dx, hover_dy),
            hits_button(metrics, kPowerButtonAngle, hover_dx, hover_dy)};
        const char *key_labels[2] = {"BOOT", "PWR"};
        for (int key = 0; key < 2; ++key) {
            const Layer &art = keys[key][key_down[key] ? 1 : 0];
            canvas.blit_layer(art, static_cast<int>(device_cx * fdpi) + key_offset_x[key],
                              static_cast<int>(cy * fdpi) + key_offset_y[key]);
            if (key_hover[key] || key_down[key]) {
                draw_key_caption(chrome, metrics, device_cx, cy, key_angles[key], key_down[key],
                                 key_labels[key]);
            }
        }
        tooltip.draw(chrome, tooltip.rect.contains(hover_x, hover_y));
        if (show_panel) {
            tooltip.draw_tether(chrome, panel_x, panel_y, panel.height());
            chrome.blit_layer(panel_shadow, panel_x - kPanelShadowMargin,
                              panel_y - kPanelShadowMargin);
            panel_state.tilt = sim.sensors.tilt;
            draw_panel(chrome, panel, panel_x, panel_y, sim.ui, sim.engine, sim.renderer,
                       panel_state, sim.host.saves, sim.smooth_display);
        }

#ifdef EYES_SIM_MAC
        // Let clicks fall through wherever the window is see-through, so the
        // invisible rectangle does not block the desktop behind it.
        if (native_window != nullptr) {
            const bool holding = screen_dragging || shell_dragging || panel_dragging ||
                                 tilt_dragging || right_dragging || panel_state.boot_down ||
                                 panel_state.power_down;
            int global_x = 0;
            int global_y = 0;
            SDL_GetGlobalMouseState(&global_x, &global_y);
            int origin_x = 0;
            int origin_y = 0;
            SDL_GetWindowPosition(window, &origin_x, &origin_y);
            const int local_x = global_x - origin_x;
            const int local_y = global_y - origin_y;
            const bool over_solid = canvas.alpha_at(local_x * dpi, local_y * dpi) > 12U ||
                                    chrome.alpha_at(local_x, local_y) > 12U;
            const bool want_through = !holding && !over_solid;
            if (want_through != click_through) {
                click_through = want_through;
                eyes_mac_set_click_through(native_window, click_through ? 1 : 0);
            }
        }
#endif

        SDL_UpdateTexture(texture, nullptr, canvas.pixels(), canvas.pitch());
        SDL_UpdateTexture(chrome_texture, nullptr, chrome.pixels(), chrome.pitch());
        // Brightness setting drives the panel like a real backlight: the shell
        // chrome stays lit, only the display texture dims. (dim/mid/bright)
        {
            static const Uint8 kLevels[] = {140, 200, 255};
            const Uint8 b = kLevels[std::clamp(sim.set_bright, 0, 2)];
            SDL_SetTextureColorMod(texture, b, b, b);
        }
        SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 0);
        SDL_RenderClear(sdl_renderer);
        // The haptic motor as a felt jolt: the composite shakes a few pixels
        // while the LRA "buzzes", so notification/confirm/crown feedback is
        // visible on host. Offsetting the render rect (not the OS window) keeps
        // it clear of the drag logic and leaves headless golden frames untouched.
        int hdx = 0, hdy = 0;
        sim_haptics.take_offset(elapsed_ms, hdx, hdy);
        if (hdx != 0 || hdy != 0) {
            int rw = 0, rh = 0;
            SDL_GetRendererOutputSize(sdl_renderer, &rw, &rh);
            SDL_Rect dst{hdx, hdy, rw, rh};
            SDL_RenderCopy(sdl_renderer, texture, nullptr, &dst);
            SDL_RenderCopy(sdl_renderer, chrome_texture, nullptr, &dst);
        } else {
            SDL_RenderCopy(sdl_renderer, texture, nullptr, nullptr);
            SDL_RenderCopy(sdl_renderer, chrome_texture, nullptr, nullptr);
        }
        SDL_RenderPresent(sdl_renderer);
#ifdef EYES_SIM_MAC
        if (!transparency_reasserted && native_window != nullptr) {
            // The backing layer exists for certain once a frame has been
            // presented; reassert in case SDL rebuilt it.
            transparency_reasserted = true;
            eyes_mac_prepare_window(native_window);
        }
#endif

        ++fps_frames;
        if (now_ms - fps_window_ms >= 500U) {
            panel_state.fps = static_cast<int>(static_cast<float>(fps_frames) * 1000.0F /
                                               static_cast<float>(now_ms - fps_window_ms));
            fps_frames = 0;
            fps_window_ms = now_ms;
        }
    }

    SDL_FreeCursor(cursor_arrow);
    SDL_FreeCursor(cursor_move);
    SDL_FreeCursor(cursor_hand);
    SDL_DestroyTexture(chrome_texture);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#endif  // EYES_SIM_HEADLESS_ONLY

}  // namespace

int main(int argc, char **argv)
{
    const Options options = parse_options(argc, argv);
    if (options.seconds < 0.0F) {
        return 0;  // --help
    }
    if (options.selftest) {
        return run_selftest();
    }
#ifdef EYES_SIM_HEADLESS_ONLY
    if (!options.headless) {
        std::fprintf(stderr, "built without SDL2; re-run with --headless\n");
        return 1;
    }
    return run_headless(options);
#else
    return options.headless ? run_headless(options) : run_window(options);
#endif
}
