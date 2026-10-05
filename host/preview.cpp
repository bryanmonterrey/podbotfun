#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "eyes/eye_renderer.hpp"

namespace {

std::uint8_t expand5(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value * 255U + 15U) / 31U);
}

std::uint8_t expand6(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value * 255U + 31U) / 63U);
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string output = argc > 1 ? argv[1] : "eyes_preview.ppm";
    const std::string palette_output = argc > 2 ? argv[2] : "host/eyes_palette_grid.ppm";
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(eyes::kScreenWidth) *
                                      eyes::kScreenHeight);
    eyes::EyeRenderer renderer(
        {pixels.data(), eyes::kScreenWidth, eyes::kScreenHeight, eyes::kScreenWidth}, true);
    eyes::FrameState state{};
    state.selection = {1, 35};
    state.gaze = {};
    state.blink_open = 1.0F;
    renderer.render(state);

    std::ofstream stream(output, std::ios::binary);
    if (!stream) {
        std::cerr << "Could not open " << output << '\n';
        return 1;
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
    std::cout << "Wrote " << output << '\n';

    constexpr int columns = 10;
    constexpr int swatch_width = 30;
    constexpr int swatch_height = 20;
    std::ofstream palette_stream(palette_output, std::ios::binary);
    if (!palette_stream) {
        std::cerr << "Could not open " << palette_output << '\n';
        return 1;
    }
    palette_stream << "P6\n" << columns * swatch_width << ' '
                   << columns * swatch_height << "\n255\n";
    for (int y = 0; y < columns * swatch_height; ++y) {
        const std::size_t palette_index = static_cast<std::size_t>(y / swatch_height) * columns;
        const bool source = y % swatch_height < swatch_height / 2;
        for (int x = 0; x < columns * swatch_width; ++x) {
            const eyes::Palette &palette = eyes::palettes()[palette_index +
                                                           static_cast<std::size_t>(x / swatch_width)];
            const eyes::Rgb colors[3]{palette.outer, palette.inner, palette.accent};
            const eyes::Rgb color = colors[(x % swatch_width) / (swatch_width / 3)];
            const std::uint16_t packed = eyes::rgb565(color);
            const char rgb[3]{
                static_cast<char>(source ? color.r : expand5((packed >> 11U) & 0x1fU)),
                static_cast<char>(source ? color.g : expand6((packed >> 5U) & 0x3fU)),
                static_cast<char>(source ? color.b : expand5(packed & 0x1fU)),
            };
            palette_stream.write(rgb, 3);
        }
    }
    std::cout << "Wrote " << palette_output << '\n';
    return 0;
}
