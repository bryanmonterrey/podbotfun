#pragma once

// On-device earcon synth: the instant, pre-TTS acknowledgments (confirm chime,
// "received", listening blip, error) rendered as PCM on the fly — no WAV files
// on flash. The whole point of synthesizing rather than shipping samples is
// that every play is *slightly different*: pitch, timing and harmonic mix jitter
// within the recipe (seed-driven), so the sounds never wear out the way a fixed
// file does. Ported from the procedural-sounds recipes (m1ckc3s, MIT) — the Web
// Audio oscillator+envelope idea, restated as integer-friendly host code.
//
// Placement in the architecture: this is the DEVICE AUDIO LAYER, not the render
// core. The jitter (its lone PRNG) lives here on purpose — the eye engine and
// rasterizer stay deterministic and seedless; only the speaker path is allowed
// to sound alive. Platform-free and host-testable like everything in eyes/os:
// fills a caller-owned int16 buffer, allocates nothing, throws nothing, so it
// drops onto the Xtensa/Snapdragon speaker DMA path unchanged.

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace eyes {
namespace earcon {

// The vocabulary. Each is a short, distinct gesture — nothing longer than the
// half-second an acknowledgment can occupy before it stops being instant.
enum class Sound : std::uint8_t {
    confirm,    // a bright two-note rise: "yes, doing it"
    received,   // a warm three-note major chime: money/message landed
    listening,  // a single soft blip: the mic just opened
    error,      // a low, faintly dissonant fall: "no / couldn't"
    tick,       // a tiny click: a discrete UI step
};

// xorshift32 — the per-play variation source. Deterministic given a seed (so a
// test can pin it), but the caller advances the seed each play so no two plays
// land on the same micro-detuning. Never wired into core/render code.
struct Rng {
    std::uint32_t state;

    explicit Rng(std::uint32_t seed) : state(seed ? seed : 0x9E3779B9U) {}

    std::uint32_t next()
    {
        std::uint32_t x = state;
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        state = x;
        return x;
    }
    // Uniform in [0,1).
    float unit() { return static_cast<float>(next() >> 8U) / 16777216.0F; }
    // Uniform in [-1,1].
    float bipolar() { return unit() * 2.0F - 1.0F; }
};

// One scheduled partial: a sine of `freq` Hz starting at `start_s` for `dur_s`,
// scaled by `amp`. A recipe fills a few of these; the renderer sums the active
// ones per sample. Kept a POD so the whole recipe lives on the stack.
struct Voice {
    float start_s;
    float dur_s;
    float freq;
    float amp;
};

constexpr std::size_t kMaxVoices = 6;

struct Recipe {
    Voice voices[kMaxVoices];
    std::size_t count;
    float total_s;  // full duration incl. the last voice's tail
};

// Build the recipe for `sound`, applying `rng` jitter. Semitone ratio 2^(1/12)
// ~= 1.059463; a few named intervals below keep the chords readable.
inline Recipe build(Sound sound, Rng &rng)
{
    constexpr float kMajorThird = 1.259921F;   // 4 semitones
    constexpr float kFifth = 1.498307F;         // 7 semitones
    Recipe r{};
    // A small overall detune so the whole earcon sits a touch high or low each
    // play (+/- ~1.5%), on top of per-voice jitter — this is most of the "alive".
    const float detune = 1.0F + rng.bipolar() * 0.015F;

    const auto add = [&r](float start_s, float dur_s, float freq, float amp) {
        if (r.count < kMaxVoices) {
            r.voices[r.count++] = Voice{start_s, dur_s, freq, amp};
            const float end = start_s + dur_s;
            if (end > r.total_s) {
                r.total_s = end;
            }
        }
    };

    switch (sound) {
        case Sound::confirm: {
            const float base = 660.0F * detune;
            const float gap = 0.055F + rng.unit() * 0.02F;  // rise timing varies
            add(0.0F, 0.14F, base, 0.9F);
            add(0.0F, 0.14F, base * 2.0F, 0.18F);  // an octave sparkle
            add(gap, 0.18F, base * kFifth, 0.9F);
            add(gap, 0.18F, base * kFifth * 2.0F, 0.16F);
            break;
        }
        case Sound::received: {
            const float base = 523.25F * detune;  // ~C5
            const float step = 0.09F + rng.unit() * 0.02F;
            add(0.0F, 0.26F, base, 0.8F);
            add(step, 0.26F, base * kMajorThird, 0.8F);         // E
            add(step * 2.0F, 0.34F, base * kFifth, 0.85F);      // G — lands warm
            add(step * 2.0F, 0.34F, base * kFifth * 2.0F, 0.12F);
            break;
        }
        case Sound::listening: {
            const float base = 880.0F * detune * (1.0F + rng.bipolar() * 0.02F);
            add(0.0F, 0.10F, base, 0.7F);
            add(0.0F, 0.10F, base * 1.5F, 0.14F);
            break;
        }
        case Sound::error: {
            const float base = 320.0F * detune;
            const float gap = 0.10F + rng.unit() * 0.02F;
            add(0.0F, 0.16F, base, 0.85F);
            add(0.0F, 0.16F, base * 1.05F, 0.35F);   // a beat against itself: unease
            add(gap, 0.22F, base * 0.75F, 0.85F);    // falls a fourth
            break;
        }
        case Sound::tick: {
            const float base = 1200.0F * detune;
            add(0.0F, 0.028F, base, 0.6F);
            break;
        }
    }
    return r;
}

// Raised-cosine attack into an exponential decay: a soft onset (no click) and a
// natural tail. t_s is seconds since the voice started; dur_s its length.
inline float envelope(float t_s, float dur_s)
{
    if (t_s < 0.0F || t_s > dur_s) {
        return 0.0F;
    }
    const float attack = dur_s * 0.18F;
    float amp;
    if (t_s < attack) {
        // 0..1 raised cosine.
        amp = 0.5F - 0.5F * std::cos(3.14159265F * (t_s / attack));
    } else {
        const float tau = dur_s * 0.35F;
        amp = std::exp(-(t_s - attack) / tau);
    }
    return amp;
}

// An UPPER BOUND on the samples any play of `sound` produces at `sample_rate` —
// size the buffer with this. Because the recipes jitter note *timing* per play,
// the exact length varies by seed; the +0.08 s pad covers the worst-case timing
// spread so a buffer this size never truncates a real play.
inline std::size_t sample_count(Sound sound, std::uint32_t sample_rate)
{
    Rng probe(1U);
    const Recipe r = build(sound, probe);
    return static_cast<std::size_t>((r.total_s + 0.08F) * static_cast<float>(sample_rate)) + 1U;
}

// Render `sound` into `out` (mono int16), jittered by `seed`. Writes at most
// `capacity` samples; returns how many were written. Allocation-free, no throw.
// Output is peak-normalized to ~0.9 full-scale so recipes are level-matched and
// never clip.
inline std::size_t render(Sound sound, std::uint32_t sample_rate, std::uint32_t seed,
                          std::int16_t *out, std::size_t capacity)
{
    if (out == nullptr || capacity == 0U || sample_rate == 0U) {
        return 0U;
    }
    Rng rng(seed);
    const Recipe recipe = build(sound, rng);
    const float sr = static_cast<float>(sample_rate);
    std::size_t n = static_cast<std::size_t>(recipe.total_s * sr) + 1U;
    if (n > capacity) {
        n = capacity;
    }

    constexpr float kTwoPi = 6.28318531F;
    // First pass could find the true peak; instead a fixed master keeps this
    // single-pass (the recipes are hand-leveled and the envelope caps at 1).
    constexpr float kMaster = 0.42F;

    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        float sample = 0.0F;
        for (std::size_t v = 0; v < recipe.count; ++v) {
            const Voice &voice = recipe.voices[v];
            const float local = t - voice.start_s;
            const float env = envelope(local, voice.dur_s);
            if (env <= 0.0F) {
                continue;
            }
            sample += voice.amp * env * std::sin(kTwoPi * voice.freq * local);
        }
        sample *= kMaster;
        // Hard-limit into [-1,1] as a safety net, then to int16.
        if (sample > 1.0F) {
            sample = 1.0F;
        } else if (sample < -1.0F) {
            sample = -1.0F;
        }
        out[i] = static_cast<std::int16_t>(sample * 32767.0F);
    }
    return n;
}

}  // namespace earcon
}  // namespace eyes
