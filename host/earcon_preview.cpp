// Audible proof of the device earcon synth: renders each recipe to a mono WAV
// (four plays back-to-back, advancing the seed, so you can HEAR the per-play
// variation a fixed sample file could never have). No core linkage — the synth
// is a standalone header. Build+run: `make earcon-preview`, then afplay the WAVs.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "eyes/os/earcon.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 22050U;

void put_u32(std::FILE *f, std::uint32_t v)
{
    unsigned char b[4] = {
        static_cast<unsigned char>(v & 0xFFU), static_cast<unsigned char>((v >> 8U) & 0xFFU),
        static_cast<unsigned char>((v >> 16U) & 0xFFU),
        static_cast<unsigned char>((v >> 24U) & 0xFFU)};
    std::fwrite(b, 1, 4, f);
}

void put_u16(std::FILE *f, std::uint16_t v)
{
    unsigned char b[2] = {static_cast<unsigned char>(v & 0xFFU),
                          static_cast<unsigned char>((v >> 8U) & 0xFFU)};
    std::fwrite(b, 1, 2, f);
}

// Minimal 16-bit mono PCM WAV.
bool write_wav(const char *path, const std::int16_t *samples, std::size_t count)
{
    std::FILE *f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(count) * 2U;
    std::fwrite("RIFF", 1, 4, f);
    put_u32(f, 36U + data_bytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    put_u32(f, 16U);                     // PCM fmt chunk size
    put_u16(f, 1U);                      // PCM
    put_u16(f, 1U);                      // mono
    put_u32(f, kSampleRate);             // sample rate
    put_u32(f, kSampleRate * 2U);        // byte rate (mono, 2 bytes/sample)
    put_u16(f, 2U);                      // block align
    put_u16(f, 16U);                     // bits per sample
    std::fwrite("data", 1, 4, f);
    put_u32(f, data_bytes);
    std::fwrite(samples, 2, count, f);
    std::fclose(f);
    return true;
}

}  // namespace

int main()
{
    struct Entry {
        eyes::earcon::Sound sound;
        const char *name;
    };
    const Entry entries[] = {
        {eyes::earcon::Sound::confirm, "confirm"},   {eyes::earcon::Sound::received, "received"},
        {eyes::earcon::Sound::listening, "listening"}, {eyes::earcon::Sound::error, "error"},
        {eyes::earcon::Sound::tick, "tick"}};

    static std::int16_t track[kSampleRate * 4U];  // up to 4 s per file (4 plays + gaps)
    constexpr std::size_t kGap = kSampleRate / 4U;  // 250 ms of silence between plays

    for (const Entry &entry : entries) {
        std::size_t written = 0;
        std::uint32_t seed = 0x100U;
        for (int play = 0; play < 4; ++play) {
            seed = seed * 1664525U + 1013904223U;  // advance -> each play varies
            std::int16_t *cursor = track + written;
            const std::size_t room = (sizeof(track) / sizeof(track[0])) - written;
            const std::size_t n = eyes::earcon::render(entry.sound, kSampleRate, seed, cursor, room);
            written += n;
            for (std::size_t g = 0; g < kGap && written < sizeof(track) / sizeof(track[0]); ++g) {
                track[written++] = 0;
            }
        }
        char path[64];
        std::snprintf(path, sizeof(path), "host/earcon_%s.wav", entry.name);
        if (write_wav(path, track, written)) {
            std::printf("wrote %s (%zu samples, %.2fs)\n", path, written,
                        static_cast<double>(written) / static_cast<double>(kSampleRate));
        } else {
            std::printf("FAILED to write %s\n", path);
        }
    }
    std::printf("done — afplay host/earcon_received.wav to hear the variation\n");
    return 0;
}
