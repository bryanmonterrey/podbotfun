#pragma once

// First-boot onboarding, the way the owner wants it: the device writes "hey"
// by hand, then you name it by voice, or the phone app claims it
// (docs/APP_SIGNIN.md §5: the device shows a pairing QR, the signed-in app
// scans it and names the device). The wake word is "hi <name>" (owner's
// call, 2026-09-30): the name is the only thing setup asks for. No tap rows,
// no typing on the glass. The surface owns the whole screen while it runs.
//
// Platform-free and allocation-free: the device runs this from app_main, the
// simulator from its window loop, the tests headless. Time is a parameter.
// The two things a platform supplies are the inputs (a transcript, an app
// claim, a QR bitmap) and a place to persist DeviceIdentity (NVS on the
// board, ~/.lilguy/device.json on the host).
//
// Stages persist so a boot with no way to answer (this board has no voice
// transport yet) greets once and then shows the face on every later boot,
// instead of writing "hey" to nobody forever. Once a path exists the flow
// resumes where it left off.

#include <cstddef>
#include <cstdint>

#include "eyes/raster.hpp"

namespace eyes {

enum class OnboardingStage : std::uint8_t { fresh, greeted, named, done };

struct DeviceIdentity {
    static constexpr std::size_t kTextMax = 32;
    static constexpr std::size_t kWakeMax = kTextMax + 4;  // "hi " + name
    char name[kTextMax]{};
    // Always "hi <name>"; stored so the platform's wake detector reads one
    // field, derived by set_name so it can never drift from the name.
    char wake_word[kWakeMax]{"hi podbot"};
    OnboardingStage stage{OnboardingStage::fresh};

    void set_name(const char *new_name);
};

// What can answer the greeting on this platform. voice = a transcript source
// is fitted (a mic plus STT). The app path needs no flag: it is live the
// moment a pairing QR is pushed with show_qr().
struct OnboardingPaths {
    bool voice{false};
};

class Onboarding {
  public:
    enum class Step : std::uint8_t { off, greet, name, confirm, finish };

    // The pen: a short still beat so the first frame is blank glass and the
    // stroke visibly begins, then the word.
    static constexpr std::uint32_t kHeyDelayMs = 350U;
    static constexpr std::uint32_t kHeyWriteMs = 2600U;
    // With no path to answer, the word holds this long after the pen lifts,
    // then dips out and the face comes up.
    static constexpr std::uint32_t kGreetHoldMs = 1400U;
    // The QR's two-beat launch, and how long "Nice to meet you" stays up.
    static constexpr std::uint32_t kQrLaunchMs = 340U;
    static constexpr std::uint32_t kConfirmHoldMs = 1800U;
    // Largest QR the surface renders: version 6 (41 modules), enough for the
    // pairing payload with medium error correction.
    static constexpr int kQrMaxModules = 41;

    // Decide whether to run at all, from the stored stage and the fitted
    // paths, and start the pen if so.
    void begin(const DeviceIdentity &identity, OnboardingPaths paths, std::uint32_t now_ms);
    // Run the whole flow again from the greeting (the simulator's O key).
    void restart(std::uint32_t now_ms);

    bool active() const { return step_ != Step::off; }
    Step step() const { return step_; }
    const DeviceIdentity &identity() const { return identity_; }

    // --- inputs ---
    // A spoken transcript: the name, while the word waits on the glass.
    void voice_text(const char *text, std::uint32_t now_ms);
    // The phone app claimed the pairing code and named the device.
    void app_claimed(const char *name, std::uint32_t now_ms);
    // The pairing QR, rows of '1'/'0' separated by '\n' (the agent link's
    // show_qr payload). During `name` the word dips out and the code launches.
    void show_qr(const char *rows, std::uint32_t now_ms);
    void clear_qr();
    bool qr_showing() const { return qr_size_ > 0; }

    // --- per frame ---
    // Draws into the platform's persistent framebuffer and returns the rows it
    // touched (invalid when nothing changed). The first frame of every step
    // clears the glass; the pen only stamps what it newly wrote, so the device
    // blits a band, not the panel.
    PixelBounds render(std::uint16_t *framebuffer, std::uint32_t now_ms);

    // One-shot: identity changed and should be persisted.
    bool consume_save(DeviceIdentity &out);
    // One-shot: the surface just dismissed. The platform blinks, shows READY
    // when setup completed, and repaints the face.
    bool consume_finished(bool &completed);

  private:
    enum class Phase : std::uint8_t { in, hold, out };

    struct Sample {
        float x;
        float y;
        float length;  // cumulative, screen px
        bool pen_up;   // first point of a stroke: no segment joins it
    };
    // 4 strokes x ~6 cubics x kSteps, plus stroke starts; the guard in
    // build_word() stops short rather than overrun.
    static constexpr int kSteps = 20;
    static constexpr int kMaxSamples = 640;

    void enter(Step step, std::uint32_t now_ms);
    void finish(bool completed, std::uint32_t now_ms);
    void build_word();
    void stamp(std::uint16_t *framebuffer, float cx, float cy, PixelBounds &dirty);
    void write_word(std::uint16_t *framebuffer, float progress, PixelBounds &dirty);
    void darken(std::uint16_t *framebuffer, float keep, PixelBounds &dirty) const;
    void draw_qr(std::uint16_t *framebuffer, float scale, float alpha, PixelBounds &dirty) const;
    void draw_confirm_copy(std::uint16_t *framebuffer, float alpha, PixelBounds &dirty) const;
    void name_and_finish(const char *name, std::uint32_t now_ms);
    static void copy_text(char *dst, std::size_t size, const char *src);

    DeviceIdentity identity_{};
    OnboardingPaths paths_{};
    Step step_{Step::off};
    Phase phase_{Phase::in};
    std::uint32_t step_started_ms_{0U};
    std::uint32_t phase_started_ms_{0U};
    bool first_frame_{true};
    bool save_pending_{false};
    bool finished_pending_{false};
    bool finished_completed_{false};
    Step after_out_{Step::off};  // where the dip-out leads

    // The pen.
    Sample samples_[kMaxSamples]{};
    int sample_count_{0};
    float total_length_{0.0F};
    float radius_{6.0F};
    int pen_index_{0};            // next sample to consider
    float last_stamp_length_{0.0F};
    float word_brightness_{1.0F};  // in-place darken tracks what is on the glass

    // The pairing QR as a bit matrix.
    std::uint8_t qr_bits_[(kQrMaxModules * kQrMaxModules + 7) / 8]{};
    int qr_size_{0};
    std::uint32_t qr_shown_ms_{0U};
};

}  // namespace eyes
