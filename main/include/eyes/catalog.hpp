#pragma once

#include <array>
#include <cstddef>

#include "eyes/types.hpp"

namespace eyes {

struct ShapePreset {
    const char *name;
    const char *state;
    float weight;
    float rarity_percent;

    // Legacy analytic fallback until the renderer consumes the authored rig paths.
    Geometry geometry;
    float radius_x;
    float radius_y;
    float gap;
    float pupil_scale_x;
    float pupil_scale_y;
    float left_shear;
    float right_shear;
    float left_y;
    float right_y;
    float base_open;
    float gaze_x;
    float gaze_y;
};

struct Palette {
    const char *name;
    Rgb outer;
    Rgb inner;
    Rgb accent;
    float rarity_percent;
};

constexpr std::size_t kShapeCount = 4;
constexpr std::size_t kPaletteCount = 100;
constexpr std::size_t kLookCount = kShapeCount * kPaletteCount;

const std::array<ShapePreset, kShapeCount> &shapes();
const std::array<Palette, kPaletteCount> &palettes();
const ShapePreset &shape_at(int index);
const Palette &palette_at(int index);
Selection wrap_selection(Selection selection);
Selection offset_selection(Selection selection, int delta_shape, int delta_palette);

}  // namespace eyes
