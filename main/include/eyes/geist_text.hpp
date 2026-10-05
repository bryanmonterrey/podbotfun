#pragma once

// Antialiased proportional text: Geist (SIL OFL) baked as alpha bitmaps by
// tools/bake_font.py. Glyph masks are tight; placement uses the bearings.
// Header-only and byte-order safe so the onboarding surface draws the same
// copy on the panel and in the simulator. Colors are encoded (framebuffer
// order): pass rgb565() on the device, a native literal on the host.

#include <cstdint>

#include "eyes/font_geist.hpp"
#include "eyes/pixel.hpp"
#include "eyes/raster.hpp"

namespace eyes {

inline int geist_width(const char *text, bool large)
{
    const gfont::Glyph *glyphs = large ? gfont::k_large_glyphs : gfont::k_small_glyphs;
    int width = 0;
    for (const char *c = text; *c != '\0'; ++c) {
        if (*c >= 32 && *c <= 126) {
            width += glyphs[*c - 32].advance;
        }
    }
    return width;
}

inline void draw_geist(std::uint16_t *dst, int pen_x, int top_y, const char *text,
                       std::uint16_t color, float alpha, bool large)
{
    const gfont::Glyph *glyphs = large ? gfont::k_large_glyphs : gfont::k_small_glyphs;
    const std::uint8_t *alpha_data = large ? gfont::k_large_alpha : gfont::k_small_alpha;
    for (const char *c = text; *c != '\0'; ++c) {
        if (*c < 32 || *c > 126) {
            continue;
        }
        const gfont::Glyph &glyph = glyphs[*c - 32];
        for (int row = 0; row < glyph.height; ++row) {
            const int py = top_y + glyph.bearing_y + row;
            if (py < 0 || py >= kScreenHeight) {
                continue;
            }
            for (int col = 0; col < glyph.width; ++col) {
                const std::uint8_t a8 =
                    alpha_data[glyph.data_offset + static_cast<std::uint32_t>(row) * glyph.width +
                               static_cast<std::uint32_t>(col)];
                if (a8 == 0) {
                    continue;
                }
                const int px = pen_x + glyph.bearing_x + col;
                if (px < 0 || px >= kScreenWidth) {
                    continue;
                }
                const std::size_t index =
                    static_cast<std::size_t>(py) * kScreenWidth + static_cast<std::size_t>(px);
                dst[index] = blend565(dst[index], color, alpha * static_cast<float>(a8) / 255.0F);
            }
        }
        pen_x += glyph.advance;
    }
}

// Centered on the cap-height midline (cy is the visual centre of a line).
inline void draw_geist_centered(std::uint16_t *dst, int cx, int cy, const char *text,
                                std::uint16_t color, float alpha, bool large)
{
    draw_geist(dst, cx - geist_width(text, large) / 2, cy - (large ? 19 : 12), text, color, alpha,
               large);
}

}  // namespace eyes
