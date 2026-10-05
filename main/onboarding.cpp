#include "eyes/os/onboarding.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "eyes/geist_text.hpp"
#include "eyes/hey_strokes.hpp"
#include "eyes/os/motion.hpp"
#include "eyes/pixel.hpp"
#include "eyes/types.hpp"

namespace eyes {

namespace {

constexpr float kWordWidth = 0.80F;  // of the glass; the descender's tail is the widest point
constexpr float kStampSpacing = 0.75F;  // px of arc between brush dots
constexpr int kQrSpan = 250;            // px, leaves a quiet zone on the round glass
constexpr int kQrLift = 12;             // the code sits a touch above centre for the caption
constexpr float kPi = 3.14159265F;

void widen(PixelBounds &dirty, int x0, int y0, int x1, int y1)
{
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(kScreenWidth - 1, x1);
    y1 = std::min(kScreenHeight - 1, y1);
    if (x0 > x1 || y0 > y1) {
        return;
    }
    if (!dirty.valid()) {
        dirty = {x0, y0, x1, y1};
        return;
    }
    dirty.x0 = std::min(dirty.x0, x0);
    dirty.y0 = std::min(dirty.y0, y0);
    dirty.x1 = std::max(dirty.x1, x1);
    dirty.y1 = std::max(dirty.y1, y1);
}

void clear_glass(std::uint16_t *framebuffer, PixelBounds &dirty)
{
    std::fill(framebuffer, framebuffer + static_cast<std::size_t>(kScreenWidth) * kScreenHeight,
              static_cast<std::uint16_t>(0U));
    widen(dirty, 0, 0, kScreenWidth - 1, kScreenHeight - 1);
}

}  // namespace

void DeviceIdentity::set_name(const char *new_name)
{
    // Onboarding::begin re-derives the wake word by passing the stored name back
    // in; snprintf's buffers are restrict-qualified, so skip the self-copy.
    if (new_name != name) {
        std::snprintf(name, sizeof(name), "%s", new_name);
    }
    std::snprintf(wake_word, sizeof(wake_word), "hi %s", name[0] != '\0' ? name : "podbot");
}

// --- lifecycle ---------------------------------------------------------------

void Onboarding::begin(const DeviceIdentity &identity, OnboardingPaths paths,
                       std::uint32_t now_ms)
{
    identity_ = identity;
    paths_ = paths;
    save_pending_ = false;
    finished_pending_ = false;
    clear_qr();
    if (identity_.stage == OnboardingStage::done) {
        step_ = Step::off;
        return;
    }
    // Greeted before and still nobody to answer: the face, not the pen.
    if (!paths_.voice && identity_.stage != OnboardingStage::fresh) {
        step_ = Step::off;
        return;
    }
    if (identity_.stage == OnboardingStage::named) {
        // A name from before the wake word was derived from it: finish now.
        identity_.set_name(identity_.name);
        identity_.stage = OnboardingStage::done;
        save_pending_ = true;
        step_ = Step::off;
        return;
    }
    enter(Step::greet, now_ms);
}

void Onboarding::restart(std::uint32_t now_ms)
{
    identity_.stage = OnboardingStage::fresh;
    identity_.name[0] = '\0';
    clear_qr();
    enter(Step::greet, now_ms);
}

void Onboarding::enter(Step step, std::uint32_t now_ms)
{
    step_ = step;
    phase_ = Phase::in;
    step_started_ms_ = now_ms;
    phase_started_ms_ = now_ms;
    first_frame_ = true;
    after_out_ = Step::off;
}

void Onboarding::finish(bool completed, std::uint32_t now_ms)
{
    (void)now_ms;
    step_ = Step::off;
    finished_pending_ = true;
    finished_completed_ = completed;
}

bool Onboarding::consume_save(DeviceIdentity &out)
{
    if (!save_pending_) {
        return false;
    }
    save_pending_ = false;
    out = identity_;
    return true;
}

bool Onboarding::consume_finished(bool &completed)
{
    if (!finished_pending_) {
        return false;
    }
    finished_pending_ = false;
    completed = finished_completed_;
    return true;
}

// --- inputs ------------------------------------------------------------------

void Onboarding::copy_text(char *dst, std::size_t size, const char *src)
{
    std::size_t n = std::strlen(src);
    // Trim the recognizer's trailing period and spaces.
    while (n > 0 && (src[n - 1] == '.' || src[n - 1] == ' ')) {
        --n;
    }
    n = std::min(n, size - 1);
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

// The name is the whole of setup: it fixes the wake word ("hi <name>"),
// completes the identity, and leads to the confirmation card.
void Onboarding::name_and_finish(const char *name, std::uint32_t now_ms)
{
    identity_.set_name(name);
    identity_.stage = OnboardingStage::done;
    save_pending_ = true;
    clear_qr();
    phase_ = Phase::out;
    phase_started_ms_ = now_ms;
    after_out_ = Step::confirm;
}

void Onboarding::voice_text(const char *text, std::uint32_t now_ms)
{
    char trimmed[DeviceIdentity::kTextMax];
    copy_text(trimmed, sizeof(trimmed), text);
    if (trimmed[0] == '\0' || phase_ == Phase::out || step_ != Step::name) {
        return;
    }
    name_and_finish(trimmed, now_ms);
}

void Onboarding::app_claimed(const char *name, std::uint32_t now_ms)
{
    if (step_ != Step::greet && step_ != Step::name) {
        return;
    }
    char trimmed[DeviceIdentity::kTextMax];
    copy_text(trimmed, sizeof(trimmed), name);
    name_and_finish(trimmed[0] != '\0' ? trimmed
                    : (identity_.name[0] != '\0' ? identity_.name : "podbot"),
                    now_ms);
}

void Onboarding::show_qr(const char *rows, std::uint32_t now_ms)
{
    std::memset(qr_bits_, 0, sizeof(qr_bits_));
    qr_size_ = 0;
    // First row's length is the module count; every row must match it.
    int size = 0;
    while (rows[size] == '0' || rows[size] == '1') {
        ++size;
    }
    if (size == 0 || size > kQrMaxModules) {
        return;
    }
    const char *p = rows;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (p[x] != '0' && p[x] != '1') {
                qr_size_ = 0;
                return;
            }
            if (p[x] == '1') {
                const int bit = y * size + x;
                qr_bits_[bit >> 3] = static_cast<std::uint8_t>(qr_bits_[bit >> 3] | (1U << (bit & 7)));
            }
        }
        p += size;
        while (*p == '\n' || *p == '\r') {
            ++p;
        }
        if (y + 1 < size && *p == '\0') {
            qr_size_ = 0;
            return;
        }
    }
    qr_size_ = size;
    qr_shown_ms_ = now_ms;
    // Waiting on the word: the word dips out and the code launches in its place.
    if (step_ == Step::name && phase_ == Phase::hold) {
        phase_ = Phase::out;
        phase_started_ms_ = now_ms;
        after_out_ = Step::name;
    }
}

void Onboarding::clear_qr()
{
    qr_size_ = 0;
    qr_shown_ms_ = 0U;
}

// --- the pen -----------------------------------------------------------------

void Onboarding::build_word()
{
    sample_count_ = 0;
    const float word_w = hey::kMaxX - hey::kMinX;
    const float word_h = hey::kMaxY - hey::kMinY;
    const float scale = static_cast<float>(kScreenWidth) * kWordWidth / word_w;
    radius_ = hey::kStrokeWidth * 0.5F * scale;
    // Centre the ink box on the glass; SF Hello is y-up, the panel y-down.
    const float ox = static_cast<float>(kScreenWidth) * 0.5F - (hey::kMinX + word_w * 0.5F) * scale;
    const float oy = static_cast<float>(kScreenHeight) * 0.5F + (hey::kMinY + word_h * 0.5F) * scale;
    const auto to_screen = [&](float ux, float uy, float &sx, float &sy) {
        sx = ox + ux * scale;
        sy = oy - uy * scale;
    };
    const auto push = [&](float x, float y, float length, bool pen_up) {
        if (sample_count_ < kMaxSamples) {
            samples_[sample_count_++] = {x, y, length, pen_up};
        }
    };
    float length = 0.0F;
    for (const hey::Stroke &stroke : hey::kStrokes) {
        float px = 0.0F;
        float py = 0.0F;
        to_screen(stroke.data[0], stroke.data[1], px, py);
        push(px, py, length, true);
        float x0 = stroke.data[0];
        float y0 = stroke.data[1];
        for (int i = 2; i + 5 < stroke.count; i += 6) {
            const float x1 = stroke.data[i], y1 = stroke.data[i + 1];
            const float x2 = stroke.data[i + 2], y2 = stroke.data[i + 3];
            const float x3 = stroke.data[i + 4], y3 = stroke.data[i + 5];
            for (int step = 1; step <= kSteps; ++step) {
                const float t = static_cast<float>(step) / kSteps;
                const float u = 1.0F - t;
                const float ux = u * u * u * x0 + 3.0F * u * u * t * x1 + 3.0F * u * t * t * x2 +
                                 t * t * t * x3;
                const float uy = u * u * u * y0 + 3.0F * u * u * t * y1 + 3.0F * u * t * t * y2 +
                                 t * t * t * y3;
                float sx = 0.0F;
                float sy = 0.0F;
                to_screen(ux, uy, sx, sy);
                length += std::sqrt((sx - px) * (sx - px) + (sy - py) * (sy - py));
                push(sx, sy, length, false);
                px = sx;
                py = sy;
            }
            x0 = x3;
            y0 = y3;
        }
    }
    total_length_ = length;
    pen_index_ = 0;
    last_stamp_length_ = -1.0F;
}

// One round brush dot, max-merged against the ink already on the glass so
// overlapping dots never double-brighten the antialiased edge.
void Onboarding::stamp(std::uint16_t *framebuffer, float cx, float cy, PixelBounds &dirty)
{
    const int reach = static_cast<int>(std::ceil(radius_ + 1.0F));
    const int x0 = std::max(0, static_cast<int>(cx) - reach);
    const int x1 = std::min(kScreenWidth - 1, static_cast<int>(cx) + reach);
    const int y0 = std::max(0, static_cast<int>(cy) - reach);
    const int y1 = std::min(kScreenHeight - 1, static_cast<int>(cy) + reach);
    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5F - cy;
        std::uint16_t *row = framebuffer + static_cast<std::size_t>(y) * kScreenWidth;
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float c = std::min(1.0F, std::max(0.0F, radius_ + 0.5F - std::sqrt(dx * dx + dy * dy)));
            if (c <= 0.0F) {
                continue;
            }
            const int level = static_cast<int>(c * 31.0F + 0.5F);
            if (level > pixel_red5(row[x])) {
                row[x] = white_at(c);
            }
        }
    }
    widen(dirty, x0, y0, x1, y1);
}

// Stamps only what the pen newly covered since the last frame; the tip is
// re-stamped every frame (harmless under max-merge) so the stroke ends
// exactly at the pen.
void Onboarding::write_word(std::uint16_t *framebuffer, float progress, PixelBounds &dirty)
{
    const float target = std::min(1.0F, progress) * total_length_;
    if (target <= 0.0F || sample_count_ == 0) {
        return;
    }
    while (pen_index_ < sample_count_ && samples_[pen_index_].length <= target) {
        const Sample &s = samples_[pen_index_];
        if (s.pen_up || last_stamp_length_ < 0.0F || s.length - last_stamp_length_ >= kStampSpacing) {
            stamp(framebuffer, s.x, s.y, dirty);
            last_stamp_length_ = s.length;
        }
        ++pen_index_;
    }
    if (pen_index_ > 0 && pen_index_ < sample_count_ && !samples_[pen_index_].pen_up) {
        const Sample &a = samples_[pen_index_ - 1];
        const Sample &b = samples_[pen_index_];
        const float span = b.length - a.length;
        const float t = span > 0.0F ? (target - a.length) / span : 0.0F;
        stamp(framebuffer, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, dirty);
    }
}

// In-place dip toward black: multiplies what is on the glass, so a static
// surface (the written word, the code) fades without being redrawn.
void Onboarding::darken(std::uint16_t *framebuffer, float keep, PixelBounds &dirty) const
{
    const float ratio = word_brightness_ > 0.0F ? keep / word_brightness_ : 0.0F;
    const float amount = 1.0F - std::min(1.0F, std::max(0.0F, ratio));
    if (amount <= 0.0F) {
        return;
    }
    const std::uint16_t black = 0U;
    const std::size_t count = static_cast<std::size_t>(kScreenWidth) * kScreenHeight;
    for (std::size_t i = 0; i < count; ++i) {
        if (framebuffer[i] != 0U) {
            framebuffer[i] = blend565(framebuffer[i], black, amount);
        }
    }
    widen(dirty, 0, 0, kScreenWidth - 1, kScreenHeight - 1);
}

// --- surfaces ----------------------------------------------------------------

void Onboarding::draw_qr(std::uint16_t *framebuffer, float scale, float alpha,
                         PixelBounds &dirty) const
{
    if (qr_size_ <= 0 || scale <= 0.0F) {
        return;
    }
    const int module = kQrSpan / qr_size_;
    const int total = module * qr_size_;
    const float size = static_cast<float>(total) * scale;
    const float cx = static_cast<float>(kScreenWidth) * 0.5F;
    const float cy = static_cast<float>(kScreenHeight) * 0.5F - static_cast<float>(kQrLift);
    const float left = cx - size * 0.5F;
    const float top = cy - size * 0.5F;
    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
    const int y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int x1 = std::min(kScreenWidth - 1, static_cast<int>(std::ceil(left + size)));
    const int y1 = std::min(kScreenHeight - 1, static_cast<int>(std::ceil(top + size)));
    const std::uint16_t white = white_at(1.0F);
    const float per_module = size / static_cast<float>(qr_size_);
    // A white card with black modules: the phone's scanner wants contrast,
    // and black modules on the black glass would vanish.
    for (int y = y0; y <= y1; ++y) {
        const int my = static_cast<int>((static_cast<float>(y) + 0.5F - top) / per_module);
        if (my < 0 || my >= qr_size_) {
            continue;
        }
        std::uint16_t *row = framebuffer + static_cast<std::size_t>(y) * kScreenWidth;
        for (int x = x0; x <= x1; ++x) {
            const int mx = static_cast<int>((static_cast<float>(x) + 0.5F - left) / per_module);
            if (mx < 0 || mx >= qr_size_) {
                continue;
            }
            const int bit = my * qr_size_ + mx;
            const bool dark = (qr_bits_[bit >> 3] >> (bit & 7)) & 1U;
            row[x] = dark ? 0U : (alpha >= 1.0F ? white : blend565(row[x], white, alpha));
        }
    }
    widen(dirty, x0, y0, x1, y1);
    const int caption_y = static_cast<int>(cy + static_cast<float>(total) * 0.5F) + 30;
    draw_geist_centered(framebuffer, kScreenWidth / 2, caption_y,
                        paths_.voice ? "say a name, or scan with the app" : "scan with the app",
                        white, 0.6F * alpha, false);
    widen(dirty, 0, caption_y - 14, kScreenWidth - 1, caption_y + 14);
}

void Onboarding::draw_confirm_copy(std::uint16_t *framebuffer, float alpha,
                                   PixelBounds &dirty) const
{
    char hello[DeviceIdentity::kTextMax + 24];
    std::snprintf(hello, sizeof(hello), "Nice to meet you, %s.", identity_.name);
    char wake[DeviceIdentity::kWakeMax + 24];
    std::snprintf(wake, sizeof(wake), "Say '%s' to talk.", identity_.wake_word);
    const std::uint16_t white = white_at(1.0F);
    draw_geist_centered(framebuffer, kScreenWidth / 2, 196, hello, white, alpha, true);
    draw_geist_centered(framebuffer, kScreenWidth / 2, 252, wake, white, 0.62F * alpha, false);
    widen(dirty, 0, 166, kScreenWidth - 1, 280);
}

// --- per frame ---------------------------------------------------------------

PixelBounds Onboarding::render(std::uint16_t *framebuffer, std::uint32_t now_ms)
{
    PixelBounds dirty{};
    if (step_ == Step::off) {
        return dirty;
    }
    const std::uint32_t phase_elapsed = now_ms - phase_started_ms_;

    // Every step's dip-out is the same in-place darken; where it leads is
    // decided by whoever started it.
    if (phase_ == Phase::out) {
        const float keep = motion::dip_out(static_cast<float>(phase_elapsed));
        darken(framebuffer, keep, dirty);
        word_brightness_ = keep;
        if (phase_elapsed >= static_cast<std::uint32_t>(motion::kDipOutMs)) {
            clear_glass(framebuffer, dirty);
            word_brightness_ = 1.0F;
            if (after_out_ == Step::off || after_out_ == Step::finish) {
                finish(identity_.stage == OnboardingStage::done, now_ms);
            } else {
                enter(after_out_, now_ms);
            }
        }
        return dirty;
    }

    switch (step_) {
        case Step::greet: {
            if (first_frame_) {
                first_frame_ = false;
                clear_glass(framebuffer, dirty);
                build_word();
                word_brightness_ = 1.0F;
            }
            const std::uint32_t elapsed = now_ms - step_started_ms_;
            if (phase_ == Phase::in) {
                float progress = 0.0F;
                if (elapsed > kHeyDelayMs) {
                    const float t = std::min(1.0F, static_cast<float>(elapsed - kHeyDelayMs) /
                                                       static_cast<float>(kHeyWriteMs));
                    // Sine ease: the pen settles in and lifts off, near-linear
                    // through the word so the hand looks steady.
                    progress = 0.5F - 0.5F * std::cos(t * kPi);
                }
                write_word(framebuffer, progress, dirty);
                if (elapsed >= kHeyDelayMs + kHeyWriteMs) {
                    if (identity_.stage == OnboardingStage::fresh) {
                        identity_.stage = OnboardingStage::greeted;
                        save_pending_ = true;
                    }
                    if (paths_.voice || qr_size_ > 0) {
                        // Somebody can answer: the word stays and we wait.
                        step_ = Step::name;
                        step_started_ms_ = now_ms;
                        phase_ = Phase::hold;
                        phase_started_ms_ = now_ms;
                        first_frame_ = false;
                        if (qr_size_ > 0) {
                            phase_ = Phase::out;
                            after_out_ = Step::name;
                        }
                    } else {
                        phase_ = Phase::hold;
                        phase_started_ms_ = now_ms;
                    }
                }
            } else if (phase_elapsed >= kGreetHoldMs) {
                phase_ = Phase::out;
                phase_started_ms_ = now_ms;
                after_out_ = Step::off;
            }
            break;
        }
        case Step::name: {
            // Two looks: the written word waiting for a voice, or the pairing
            // code once the app path is live. Entering from the word's dip-out
            // (after show_qr) is the only time first_frame_ is set here.
            if (first_frame_) {
                first_frame_ = false;
                clear_glass(framebuffer, dirty);
                if (qr_size_ > 0) {
                    phase_ = Phase::in;  // launch
                    phase_started_ms_ = now_ms;
                } else {
                    phase_ = Phase::hold;
                }
            }
            if (qr_size_ > 0 && phase_ == Phase::in) {
                const float scale = motion::two_beat_launch(static_cast<float>(phase_elapsed),
                                                            static_cast<float>(kQrLaunchMs));
                const float alpha = motion::dip_in(static_cast<float>(phase_elapsed));
                // The launch overshoots; clear the glass each frame so the
                // larger silhouette of the previous frame never lingers.
                clear_glass(framebuffer, dirty);
                draw_qr(framebuffer, scale, alpha, dirty);
                if (phase_elapsed >= kQrLaunchMs + 120U) {
                    clear_glass(framebuffer, dirty);
                    draw_qr(framebuffer, 1.0F, 1.0F, dirty);
                    phase_ = Phase::hold;
                    phase_started_ms_ = now_ms;
                }
            }
            break;
        }
        case Step::confirm: {
            // "Nice to meet you" dips in, holds, dips out to the face.
            if (first_frame_) {
                first_frame_ = false;
                clear_glass(framebuffer, dirty);
            }
            if (phase_ == Phase::in) {
                const float alpha = motion::dip_in(static_cast<float>(phase_elapsed));
                clear_glass(framebuffer, dirty);
                draw_confirm_copy(framebuffer, alpha, dirty);
                if (phase_elapsed >= static_cast<std::uint32_t>(motion::kDipInMs)) {
                    clear_glass(framebuffer, dirty);
                    draw_confirm_copy(framebuffer, 1.0F, dirty);
                    phase_ = Phase::hold;
                    phase_started_ms_ = now_ms;
                }
            } else if (phase_ == Phase::hold && phase_elapsed >= kConfirmHoldMs) {
                phase_ = Phase::out;
                phase_started_ms_ = now_ms;
                after_out_ = Step::finish;
            }
            break;
        }
        case Step::finish:
        case Step::off:
            break;
    }
    return dirty;
}

}  // namespace eyes
