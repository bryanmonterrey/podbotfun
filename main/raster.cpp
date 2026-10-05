#include "eyes/raster.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace eyes {

namespace {

[[maybe_unused]] inline std::uint16_t swap16(std::uint16_t value)
{
    return static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
}

// alpha is 0..256 where 256 means fully `source`. Both pixel operands are in
// framebuffer byte order; when kSwapBytes the channels are unswapped, blended
// in native RGB565, and the result swapped back.
std::uint16_t blend_rgb565(std::uint16_t destination, std::uint16_t source, unsigned alpha)
{
    if (alpha >= 256U) {
        return source;
    }
    if (alpha == 0U) {
        return destination;
    }
    if constexpr (kSwapBytes) {
        destination = swap16(destination);
        source = swap16(source);
    }
    const unsigned inverse = 256U - alpha;
    const unsigned red = (((destination >> 11U) & 0x1fU) * inverse +
                          ((source >> 11U) & 0x1fU) * alpha + 128U) >> 8U;
    const unsigned green = (((destination >> 5U) & 0x3fU) * inverse +
                            ((source >> 5U) & 0x3fU) * alpha + 128U) >> 8U;
    const unsigned blue = ((destination & 0x1fU) * inverse + (source & 0x1fU) * alpha + 128U) >> 8U;
    std::uint16_t result = static_cast<std::uint16_t>((red << 11U) | (green << 5U) | blue);
    if constexpr (kSwapBytes) {
        result = swap16(result);
    }
    return result;
}

}  // namespace

std::uint16_t rgb565(Rgb color)
{
    const std::uint16_t value =
        static_cast<std::uint16_t>(((static_cast<std::uint16_t>(color.r) & 0xf8U) << 8U) |
                                   ((static_cast<std::uint16_t>(color.g) & 0xfcU) << 3U) |
                                   (static_cast<std::uint16_t>(color.b) >> 3U));
    if constexpr (kSwapBytes) {
        return swap16(value);
    }
    return value;
}

Rgb scale_color(Rgb color, float brightness)
{
    brightness = std::clamp(brightness, 0.0F, 1.0F);
    return {
        static_cast<std::uint8_t>(static_cast<float>(color.r) * brightness),
        static_cast<std::uint8_t>(static_cast<float>(color.g) * brightness),
        static_cast<std::uint8_t>(static_cast<float>(color.b) * brightness),
    };
}

void Raster::clear(Rgb color)
{
    if (surface_.pixels == nullptr || surface_.width <= 0 || surface_.height <= 0) {
        return;
    }
    const std::uint16_t packed = rgb565(color);
    for (int y = 0; y < surface_.height; ++y) {
        std::fill_n(surface_.pixels + static_cast<std::ptrdiff_t>(y) * surface_.stride,
                    surface_.width, packed);
    }
}

void Raster::pixel(int x, int y, Rgb color)
{
    if (surface_.pixels == nullptr || x < 0 || y < 0 || x >= surface_.width || y >= surface_.height) {
        return;
    }
    surface_.pixels[static_cast<std::ptrdiff_t>(y) * surface_.stride + x] = rgb565(color);
    include_bounds(x, y, x, y);
}

void Raster::horizontal_line(int x0, int x1, int y, Rgb color)
{
    if (surface_.pixels == nullptr || y < 0 || y >= surface_.height || x0 > x1 || x1 < 0 ||
        x0 >= surface_.width) {
        return;
    }
    x0 = std::max(x0, 0);
    x1 = std::min(x1, surface_.width - 1);
    std::fill_n(surface_.pixels + static_cast<std::ptrdiff_t>(y) * surface_.stride + x0,
                x1 - x0 + 1, rgb565(color));
    include_bounds(x0, y, x1, y);
}

void Raster::include_bounds(int x0, int y0, int x1, int y1)
{
    if (!recent_bounds_.valid()) {
        recent_bounds_ = {x0, y0, x1, y1};
    } else {
        recent_bounds_.x0 = std::min(recent_bounds_.x0, x0);
        recent_bounds_.y0 = std::min(recent_bounds_.y0, y0);
        recent_bounds_.x1 = std::max(recent_bounds_.x1, x1);
        recent_bounds_.y1 = std::max(recent_bounds_.y1, y1);
    }
    if (!dirty_bounds_.valid()) {
        dirty_bounds_ = {x0, y0, x1, y1};
    } else {
        dirty_bounds_.x0 = std::min(dirty_bounds_.x0, x0);
        dirty_bounds_.y0 = std::min(dirty_bounds_.y0, y0);
        dirty_bounds_.x1 = std::max(dirty_bounds_.x1, x1);
        dirty_bounds_.y1 = std::max(dirty_bounds_.y1, y1);
    }

    const int region_width = std::max(2, (surface_.width /
                                                  static_cast<int>(kDirtyRegionColumns)) &
                                             ~1);
    const int region_height = std::max(2, (surface_.height /
                                                   static_cast<int>(kDirtyRegionRows)) &
                                              ~1);
    const int first_column = std::clamp(x0 / region_width, 0,
                                        static_cast<int>(kDirtyRegionColumns) - 1);
    const int last_column = std::clamp(x1 / region_width, 0,
                                       static_cast<int>(kDirtyRegionColumns) - 1);
    const int first_row = std::clamp(y0 / region_height, 0,
                                     static_cast<int>(kDirtyRegionRows) - 1);
    const int last_row = std::clamp(y1 / region_height, 0,
                                    static_cast<int>(kDirtyRegionRows) - 1);
    for (int row = first_row; row <= last_row; ++row) {
        const int region_y0 = row * region_height;
        const int region_y1 = row + 1 == static_cast<int>(kDirtyRegionRows)
                                  ? surface_.height - 1
                                  : region_y0 + region_height - 1;
        for (int column = first_column; column <= last_column; ++column) {
            const int region_x0 = column * region_width;
            const int region_x1 = column + 1 == static_cast<int>(kDirtyRegionColumns)
                                      ? surface_.width - 1
                                      : region_x0 + region_width - 1;
            PixelBounds &region = dirty_regions_[static_cast<std::size_t>(row) *
                                                     kDirtyRegionColumns +
                                                 static_cast<std::size_t>(column)];
            const PixelBounds clipped{
                std::max(x0, region_x0), std::max(y0, region_y0),
                std::min(x1, region_x1), std::min(y1, region_y1)};
            if (!region.valid()) {
                region = clipped;
            } else {
                region.x0 = std::min(region.x0, clipped.x0);
                region.y0 = std::min(region.y0, clipped.y0);
                region.x1 = std::max(region.x1, clipped.x1);
                region.y1 = std::max(region.y1, clipped.y1);
            }
        }
    }
}

void Raster::covered_span(float x0, float x1, int y, std::uint16_t color,
                          const std::uint16_t *clip_to, float row_coverage)
{
    if (surface_.pixels == nullptr || y < 0 || y >= surface_.height || x0 >= x1 ||
        row_coverage <= 0.0F) {
        return;
    }
    const int first = std::max(0, static_cast<int>(std::floor(x0 + 0.5F)));
    const int last = std::min(surface_.width - 1,
                              static_cast<int>(std::ceil(x1 + 0.5F)) - 1);
    if (first > last) {
        return;
    }
    std::uint16_t *row = surface_.pixels + static_cast<std::ptrdiff_t>(y) * surface_.stride;
    int written_first = surface_.width;
    int written_last = -1;
    auto write = [&](int x, float coverage) {
        if (coverage <= 0.0F || (clip_to != nullptr && row[x] != *clip_to)) {
            return;
        }
        const unsigned alpha =
            static_cast<unsigned>(std::clamp(coverage, 0.0F, 1.0F) * 256.0F + 0.5F);
        const std::uint16_t blended = blend_rgb565(row[x], color, alpha);
        if (blended != row[x]) {
            row[x] = blended;
            written_first = std::min(written_first, x);
            written_last = std::max(written_last, x);
        }
    };
    auto coverage_at = [&](int x) {
        return std::max(0.0F, std::min(x1, static_cast<float>(x) + 0.5F) -
                                  std::max(x0, static_cast<float>(x) - 0.5F)) *
               row_coverage;
    };

    if (first == last || row_coverage < 1.0F) {
        for (int x = first; x <= last; ++x) {
            write(x, coverage_at(x));
        }
    } else {
        write(first, coverage_at(first));
        if (clip_to == nullptr) {
            if (first + 1 <= last - 1) {
                std::fill(row + first + 1, row + last, color);
                written_first = std::min(written_first, first + 1);
                written_last = std::max(written_last, last - 1);
            }
        } else {
            for (int x = first + 1; x < last; ++x) {
                write(x, 1.0F);
            }
        }
        write(last, coverage_at(last));
    }
    if (written_last >= 0) {
        include_bounds(written_first, y, written_last, y);
    }
}

void Raster::fill_circle(float cx, float cy, float radius, Rgb color, const PixelBounds *clip)
{
    if (radius <= 0.0F) {
        return;
    }
    int y0 = static_cast<int>(std::floor(cy - radius - 0.5F));
    int y1 = static_cast<int>(std::ceil(cy + radius + 0.5F)) - 1;
    if (clip != nullptr) {
        y0 = std::max(y0, clip->y0);
        y1 = std::min(y1, clip->y1);
    }
    const float radius_squared = radius * radius;
    const std::uint16_t packed = rgb565(color);
    for (int y = y0; y <= y1; ++y) {
        const float dy = std::fabs(static_cast<float>(y) - cy);
        const float row_coverage = std::clamp(radius + 0.5F - dy, 0.0F, 1.0F);
        if (row_coverage <= 0.0F) {
            continue;
        }
        const float sample_dy = row_coverage < 1.0F ? std::max(0.0F, dy - 0.5F) : dy;
        const float remainder = radius_squared - sample_dy * sample_dy;
        const float half = std::sqrt(remainder);
        float x0 = cx - half;
        float x1 = cx + half;
        if (clip != nullptr) {
            // Clamping to the clip's half-open pixel edges keeps every
            // in-clip pixel's coverage identical to the unclipped fill.
            x0 = std::max(x0, static_cast<float>(clip->x0) - 0.5F);
            x1 = std::min(x1, static_cast<float>(clip->x1) + 0.5F);
        }
        covered_span(x0, x1, y, packed, nullptr, row_coverage);
    }
}

void Raster::fill_ellipse(float cx, float cy, float rx, float ry, float shear, Rgb color)
{
    if (rx <= 0.0F || ry <= 0.0F) {
        return;
    }
    const int y0 = static_cast<int>(std::floor(cy - ry));
    const int y1 = static_cast<int>(std::ceil(cy + ry));
    const float inverse_ry = 1.0F / ry;
    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) - cy;
        const float normalized = dy * inverse_ry;
        const float remainder = 1.0F - normalized * normalized;
        if (remainder < 0.0F) {
            continue;
        }
        const float half = rx * std::sqrt(remainder);
        const float row_center = cx + shear * dy;
        horizontal_line(static_cast<int>(std::ceil(row_center - half)),
                        static_cast<int>(std::floor(row_center + half)), y, color);
    }
}

void Raster::fill_capsule(float cx, float cy, float rx, float ry, float radius, Rgb color)
{
    if (rx <= 0.0F || ry <= 0.0F) {
        return;
    }
    radius = std::clamp(radius, 1.0F, std::min(rx, ry));
    const int y0 = static_cast<int>(std::floor(cy - ry));
    const int y1 = static_cast<int>(std::ceil(cy + ry));
    const float flat_y = ry - radius;
    for (int y = y0; y <= y1; ++y) {
        const float dy = std::fabs(static_cast<float>(y) - cy);
        if (dy > ry) {
            continue;
        }
        float half = rx;
        if (dy > flat_y) {
            const float corner_y = dy - flat_y;
            const float remainder = radius * radius - corner_y * corner_y;
            if (remainder < 0.0F) {
                continue;
            }
            half = (rx - radius) + std::sqrt(remainder);
        }
        horizontal_line(static_cast<int>(std::ceil(cx - half)),
                        static_cast<int>(std::floor(cx + half)), y, color);
    }
}

void Raster::fill_diamond(float cx, float cy, float rx, float ry, Rgb color)
{
    if (rx <= 0.0F || ry <= 0.0F) {
        return;
    }
    const int y0 = static_cast<int>(std::floor(cy - ry));
    const int y1 = static_cast<int>(std::ceil(cy + ry));
    for (int y = y0; y <= y1; ++y) {
        const float dy = std::fabs(static_cast<float>(y) - cy);
        const float half = rx * std::max(0.0F, 1.0F - dy / ry);
        horizontal_line(static_cast<int>(std::ceil(cx - half)),
                        static_cast<int>(std::floor(cx + half)), y, color);
    }
}

void Raster::begin_row_runs(bool erase_previous)
{
    row_runs_active_ = true;
    row_runs_erase_ = erase_previous;
    row_visited_ = {};
    RowRuns &runs = row_runs_[row_runs_current_];
    runs.lo1.fill(1);
    runs.hi1.fill(0);
    runs.lo2.fill(1);
    runs.hi2.fill(0);
}

void Raster::note_frame_run(int y, int lo, int hi)
{
    RowRuns &runs = row_runs_[row_runs_current_];
    const std::size_t row = static_cast<std::size_t>(y);
    if (runs.hi1[row] < runs.lo1[row]) {
        runs.lo1[row] = static_cast<std::int16_t>(lo);
        runs.hi1[row] = static_cast<std::int16_t>(hi);
    } else if (runs.hi2[row] < runs.lo2[row]) {
        runs.lo2[row] = static_cast<std::int16_t>(lo);
        runs.hi2[row] = static_cast<std::int16_t>(hi);
    } else {
        runs.lo2[row] = std::min(runs.lo2[row], static_cast<std::int16_t>(lo));
        runs.hi2[row] = std::max(runs.hi2[row], static_cast<std::int16_t>(hi));
    }
}

// First touch of a row this frame: erase last frame's covered runs on it so
// fills blend against black exactly as they would after a full pre-clear.
void Raster::pre_erase_row(int y)
{
    const std::size_t row = static_cast<std::size_t>(y);
    if (row_visited_[row]) {
        return;
    }
    row_visited_[row] = true;
    if (!row_runs_erase_) {
        return;
    }
    const RowRuns &previous = row_runs_[row_runs_current_ ^ 1];
    constexpr Rgb black{0, 0, 0};
    if (previous.hi1[row] >= previous.lo1[row]) {
        horizontal_line(previous.lo1[row], previous.hi1[row], y, black);
    }
    if (previous.hi2[row] >= previous.lo2[row]) {
        horizontal_line(previous.lo2[row], previous.hi2[row], y, black);
    }
}

void Raster::end_row_runs()
{
    if (row_runs_erase_) {
        // Rows painted last frame but never touched this frame keep stale
        // pixels; sweep them (tracked, so the transfer covers the erase).
        for (int y = 0; y < surface_.height; ++y) {
            pre_erase_row(y);
        }
    }
    row_runs_current_ ^= 1;
    row_runs_active_ = false;
    row_runs_erase_ = false;
}

void Raster::fill_cubic_path(const Vec2 *points, std::size_t point_count, Rgb color,
                             int steps_per_curve, int subsamples)
{
    fill_cubic_path_impl(points, point_count, color, nullptr, steps_per_curve, subsamples, 256U);
}

void Raster::fill_cubic_path_clipped(const Vec2 *points, std::size_t point_count, Rgb color,
                                     Rgb existing, int steps_per_curve, int subsamples,
                                     unsigned opacity)
{
    if (opacity == 0U) {
        return;
    }
    const std::uint16_t clip_to = rgb565(existing);
    fill_cubic_path_impl(points, point_count, color, &clip_to, steps_per_curve, subsamples,
                         opacity);
}

// Integer edge-table scanline rasterizer. The path is flattened once into a
// polyline; every non-horizontal segment becomes a Q12 fixed-point edge that is
// advanced incrementally per subsample line, replacing the old
// all-vertices-per-line float crossing scan. Sampling matches the previous
// renderer exactly: two subsample lines per pixel row (y - 0.25, y + 0.25) for
// plain fills and a single centered line for clipped fills, 256 total coverage.
void Raster::fill_cubic_path_impl(const Vec2 *points, std::size_t point_count, Rgb color,
                                  const std::uint16_t *clip_to, int steps_per_curve,
                                  int subsamples, unsigned opacity)
{
    const std::size_t steps =
        static_cast<std::size_t>(std::clamp(steps_per_curve, 2,
                                            static_cast<int>(kPathStepsPerCurve)));
    if (surface_.pixels == nullptr || points == nullptr || point_count < 3 ||
        point_count > kMaxPathPoints || point_count % 3 != 0 ||
        !std::isfinite(points[0].x) || !std::isfinite(points[0].y)) {
        return;
    }

    std::array<Vec2, kMaxPathVertices> vertices{};
    std::size_t vertex_count = 1;
    vertices[0] = points[0];
    Vec2 start = points[0];

    for (std::size_t index = 1; index + 1 < point_count; index += 3) {
        const Vec2 control1 = points[index];
        const Vec2 control2 = points[index + 1];
        const Vec2 end = points[(index + 2) % point_count];
        if (!std::isfinite(control1.x) || !std::isfinite(control1.y) ||
            !std::isfinite(control2.x) || !std::isfinite(control2.y) ||
            !std::isfinite(end.x) || !std::isfinite(end.y)) {
            continue;
        }
        // The closing curve stops one step short; the wrap-around edge closes
        // the polygon without a duplicate vertex.
        const std::size_t last_step = index + 2 == point_count ? steps - 1 : steps;
        for (std::size_t step = 1; step <= last_step; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const float u = 1.0F - t;
            const float uu = u * u;
            const float tt = t * t;
            vertices[vertex_count++] = {
                uu * u * start.x + 3.0F * uu * t * control1.x +
                    3.0F * u * tt * control2.x + tt * t * end.x,
                uu * u * start.y + 3.0F * uu * t * control1.y +
                    3.0F * u * tt * control2.y + tt * t * end.y,
            };
        }
        start = end;
    }

    if (vertex_count < 3) {
        return;
    }

    float min_y = vertices[0].y;
    float max_y = vertices[0].y;
    for (std::size_t index = 1; index < vertex_count; ++index) {
        min_y = std::min(min_y, vertices[index].y);
        max_y = std::max(max_y, vertices[index].y);
    }
    const int first_y = std::max(0, static_cast<int>(std::ceil(min_y - 0.5F)));
    const int last_y =
        std::min(surface_.height - 1, static_cast<int>(std::ceil(max_y + 0.5F)) - 1);
    if (first_y > last_y) {
        return;
    }
    if (clip_to == nullptr) {
        // Reset the published clip runs to empty; covered rows fill them below.
        clip_span_y0_ = first_y;
        clip_span_y1_ = last_y;
        for (int y = first_y; y <= last_y; ++y) {
            clip_run1_lo_[static_cast<std::size_t>(y)] = 1;
            clip_run1_hi_[static_cast<std::size_t>(y)] = 0;
            clip_run2_lo_[static_cast<std::size_t>(y)] = 1;
            clip_run2_hi_[static_cast<std::size_t>(y)] = 0;
        }
    }

    // Subsample line k samples y = (k + 0.5) / lines_per_row - 0.5. An edge
    // covers lines with top.y <= sample_y < bottom.y (the old half-open
    // crossing rule).
    // 8 vertical subsamples by default (clipped paths too): eyelid folds cross
    // dark areas and coarse sampling makes their edge step in visible
    // increments. Small grid cells request 2 (coarser vertical AA, invisible at
    // their size and much cheaper).
    const int lines_per_row = aa_lines_for(subsamples);
    const float lines_per_row_f = static_cast<float>(lines_per_row);
    const std::uint16_t full_coverage = static_cast<std::uint16_t>(256 / lines_per_row);
    // Q12 width -> coverage: 4096 >> 7 = 32 = 256/8; 4096 >> 5 = 128 = 256/2.
    const int coverage_shift = lines_per_row == 8 ? 7 : 5;
    const std::int32_t coverage_round = 1 << (coverage_shift - 1);
    const int first_line = first_y * lines_per_row;
    const int end_line = (last_y + 1) * lines_per_row;

    std::size_t edge_count = 0;
    for (std::size_t index = 0; index < vertex_count; ++index) {
        Vec2 a = vertices[index];
        Vec2 b = vertices[(index + 1) % vertex_count];
        if (a.y == b.y) {
            continue;
        }
        std::int32_t winding = 1;
        if (a.y > b.y) {
            std::swap(a, b);
            winding = -1;
        }
        const int top = std::max(
            static_cast<int>(std::ceil((a.y + 0.5F) * lines_per_row_f - 0.5F)), first_line);
        const int bottom = std::min(
            static_cast<int>(std::ceil((b.y + 0.5F) * lines_per_row_f - 0.5F)), end_line);
        if (top >= bottom) {
            continue;
        }
        // ponytail: clamps only guard int32 Q12 overflow on garbage geometry;
        // rig coordinates stay orders of magnitude below them.
        const float inverse_slope =
            std::clamp((b.x - a.x) / (b.y - a.y), -1000.0F, 1000.0F);
        const float line_y = (static_cast<float>(top) + 0.5F) / lines_per_row_f - 0.5F;
        const float start_x =
            std::clamp(a.x + (line_y - a.y) * inverse_slope, -32000.0F, 32000.0F);
        path_edges_[edge_count++] = {
            static_cast<std::int32_t>(std::lround(start_x * 4096.0F)),
            static_cast<std::int32_t>(std::lround(inverse_slope * 4096.0F / lines_per_row_f)),
            top,
            bottom,
            winding,
        };
    }
    if (edge_count == 0) {
        return;
    }
    std::sort(path_edges_.begin(),
              path_edges_.begin() + static_cast<std::ptrdiff_t>(edge_count),
              [](const PathEdge &left, const PathEdge &right) {
                  return left.first_line < right.first_line;
              });

    const std::uint16_t packed = rgb565(color);
    std::array<std::uint8_t, kMaxPathVertices> active{};
    std::size_t next_edge = 0;
    std::size_t active_count = 0;

    for (int y = first_y; y <= last_y; ++y) {
        if (row_runs_active_) {
            pre_erase_row(y);  // diff-clear: old pixels go black before any write
        }
        int first_covered = surface_.width;
        int last_covered = -1;

        const int row_line = y * lines_per_row;
        for (int line = row_line; line < row_line + lines_per_row; ++line) {
            while (next_edge < edge_count && path_edges_[next_edge].first_line <= line) {
                active[active_count++] = static_cast<std::uint8_t>(next_edge);
                ++next_edge;
            }
            std::size_t kept = 0;
            for (std::size_t index = 0; index < active_count; ++index) {
                if (path_edges_[active[index]].last_line > line) {
                    active[kept++] = active[index];
                }
            }
            active_count = kept;
            for (std::size_t index = 1; index < active_count; ++index) {  // sort by x
                const std::uint8_t edge = active[index];
                std::size_t slot = index;
                while (slot > 0 && path_edges_[active[slot - 1]].x > path_edges_[edge].x) {
                    active[slot] = active[slot - 1];
                    --slot;
                }
                active[slot] = edge;
            }

            // Nonzero winding: folding eyelids self-overlap mid-blink; even-odd
            // would punch a hole where the fold crosses itself.
            std::int32_t winding = 0;
            for (std::size_t index = 0; index + 1 < active_count; ++index) {
                winding += path_edges_[active[index]].winding;
                if (winding == 0) {
                    continue;
                }
                const std::int32_t left = path_edges_[active[index]].x;
                const std::int32_t right = path_edges_[active[index + 1]].x;
                if (left >= right) {
                    continue;
                }
                const int first = std::max(0, static_cast<int>((left + 2048) >> 12));
                const int last = std::min(surface_.width - 1,
                                          static_cast<int>(((right + 2048 + 4095) >> 12) - 1));
                if (first > last) {
                    continue;
                }
                first_covered = std::min(first_covered, first);
                last_covered = std::max(last_covered, last);
                // coverage_ is a DIFFERENCE array (unsigned wraparound): spans cost
                // two boundary writes instead of one write per interior pixel; the
                // blend loop reconstructs coverage with a running prefix sum.
                const auto add_partial = [&](int x) {
                    const std::int32_t lo =
                        std::max(left, static_cast<std::int32_t>(x << 12) - 2048);
                    const std::int32_t hi =
                        std::min(right, static_cast<std::int32_t>(x << 12) + 2048);
                    if (hi > lo) {
                        const std::uint16_t amount = static_cast<std::uint16_t>(
                            (hi - lo + coverage_round) >> coverage_shift);
                        coverage_[static_cast<std::size_t>(x)] =
                            static_cast<std::uint16_t>(coverage_[static_cast<std::size_t>(x)] +
                                                       amount);
                        coverage_[static_cast<std::size_t>(x) + 1U] = static_cast<std::uint16_t>(
                            coverage_[static_cast<std::size_t>(x) + 1U] - amount);
                    }
                };
                add_partial(first);
                if (first != last) {
                    coverage_[static_cast<std::size_t>(first) + 1U] = static_cast<std::uint16_t>(
                        coverage_[static_cast<std::size_t>(first) + 1U] + full_coverage);
                    coverage_[static_cast<std::size_t>(last)] = static_cast<std::uint16_t>(
                        coverage_[static_cast<std::size_t>(last)] - full_coverage);
                    add_partial(last);
                }
            }

            for (std::size_t index = 0; index < active_count; ++index) {
                path_edges_[active[index]].x += path_edges_[active[index]].step;
            }
        }

        if (last_covered < first_covered) {
            continue;
        }
        // Geometric clip to the last unclipped path (reference-client behavior):
        // the clipped path may blend over the outer path's antialiased fringe.
        // Unclipped fills use the full row and publish their covered runs below.
        int clip_lo1 = 0;
        int clip_hi1 = surface_.width - 1;
        int clip_lo2 = 0;
        int clip_hi2 = -1;
        if (clip_to != nullptr) {
            if (y < clip_span_y0_ || y > clip_span_y1_) {
                for (int x = first_covered; x <= last_covered; ++x) {
                    coverage_[static_cast<std::size_t>(x)] = 0U;
                }
                // The difference array also carries a residual one past the last
                // covered pixel; leaving it would corrupt a later fill's row.
                coverage_[static_cast<std::size_t>(last_covered) + 1U] = 0U;
                continue;
            }
            clip_lo1 = clip_run1_lo_[static_cast<std::size_t>(y)];
            clip_hi1 = clip_run1_hi_[static_cast<std::size_t>(y)];
            clip_lo2 = clip_run2_lo_[static_cast<std::size_t>(y)];
            clip_hi2 = clip_run2_hi_[static_cast<std::size_t>(y)];
        }
        // Track this row's covered runs (coverage > 0) so clipped fills can
        // respect a mid-blink lid fold that crosses the row as two horns.
        int run_start = -1;
        const auto publish_run = [&](int lo, int hi) {
            if (row_runs_active_) {
                note_frame_run(y, lo, hi);
            }
            const std::size_t row_index = static_cast<std::size_t>(y);
            if (clip_run1_hi_[row_index] < clip_run1_lo_[row_index]) {
                clip_run1_lo_[row_index] = static_cast<std::int16_t>(lo);
                clip_run1_hi_[row_index] = static_cast<std::int16_t>(hi);
            } else if (clip_run2_hi_[row_index] < clip_run2_lo_[row_index]) {
                clip_run2_lo_[row_index] = static_cast<std::int16_t>(lo);
                clip_run2_hi_[row_index] = static_cast<std::int16_t>(hi);
            } else {
                clip_run2_hi_[row_index] = static_cast<std::int16_t>(hi);
            }
        };
        std::uint16_t *row =
            surface_.pixels + static_cast<std::ptrdiff_t>(y) * surface_.stride;
        int written_first = 0;
        int written_last = -1;
        unsigned running = 0U;  // prefix sum over the difference array
        for (int x = first_covered; x <= last_covered; ++x) {
            const std::uint16_t delta = coverage_[static_cast<std::size_t>(x)];
            if (delta != 0U) {
                running = (running + delta) & 0xFFFFU;
                coverage_[static_cast<std::size_t>(x)] = 0U;  // keep the buffer all-zero
            }
            if (running == 0U) {
                if (clip_to == nullptr && run_start >= 0) {
                    publish_run(run_start, x - 1);
                    run_start = -1;
                }
                continue;
            }
            if (clip_to == nullptr && run_start < 0) {
                run_start = x;
            }
            // Solid interior fast path: while no deltas arrive, coverage is constant,
            // so a full-coverage run becomes one fill; this is ~90% of an eye.
            if (running >= 256U && opacity >= 256U) {
                const int allowed_hi = x >= clip_lo1 && x <= clip_hi1   ? clip_hi1
                                       : x >= clip_lo2 && x <= clip_hi2 ? clip_hi2
                                                                        : -1;
                if (allowed_hi >= 0) {
                    int run_end = x + 1;
                    while (run_end <= last_covered && run_end <= allowed_hi &&
                           coverage_[static_cast<std::size_t>(run_end)] == 0U) {
                        ++run_end;
                    }
                    std::fill(row + x, row + run_end, packed);
                    if (written_last < 0) {
                        written_first = x;
                    }
                    written_last = run_end - 1;
                    x = run_end - 1;
                    continue;
                }
            }
            if ((x < clip_lo1 || x > clip_hi1) && (x < clip_lo2 || x > clip_hi2)) {
                continue;
            }
            const std::uint16_t blended = blend_rgb565(
                row[x], packed, ((running > 256U ? 256U : running) * opacity) >> 8U);
            if (blended != row[x]) {
                row[x] = blended;
                if (written_last < 0) {
                    written_first = x;
                }
                written_last = x;
            }
        }
        if (clip_to == nullptr && run_start >= 0) {
            publish_run(run_start, last_covered);
        }
        coverage_[static_cast<std::size_t>(last_covered) + 1U] = 0U;  // trailing residual
        if (written_last >= 0) {
            include_bounds(written_first, y, written_last, y);
        }
    }
}

void Raster::quadratic_curve(Vec2 p0, Vec2 p1, Vec2 p2, float thickness, Rgb color)
{
    constexpr int segments = 72;
    const float radius = std::max(0.7F, thickness * 0.5F);
    for (int index = 0; index <= segments; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(segments);
        const float u = 1.0F - t;
        const float x = u * u * p0.x + 2.0F * u * t * p1.x + t * t * p2.x;
        const float y = u * u * p0.y + 2.0F * u * t * p1.y + t * t * p2.y;
        fill_circle(x, y, radius, color);
    }
}

void Raster::apply_round_mask(float cx, float cy, float radius, Rgb outside, int y_first,
                              int y_last)
{
    if (surface_.pixels == nullptr || radius <= 0.0F) {
        return;
    }
    y_first = std::max(y_first, 0);
    y_last = y_last < 0 ? surface_.height - 1 : std::min(y_last, surface_.height - 1);
    const std::uint16_t packed = rgb565(outside);
    const float radius_squared = radius * radius;
    for (int y = y_first; y <= y_last; ++y) {
        const float dy = static_cast<float>(y) - cy;
        const float remainder = radius_squared - dy * dy;
        int inside_start = surface_.width;
        int inside_end = -1;
        if (remainder >= 0.0F) {
            const float half = std::sqrt(remainder);
            inside_start = std::max(0, static_cast<int>(std::ceil(cx - half)));
            inside_end = std::min(surface_.width - 1, static_cast<int>(std::floor(cx + half)));
        }
        std::uint16_t *row = surface_.pixels + static_cast<std::ptrdiff_t>(y) * surface_.stride;
        if (inside_start > 0) {
            std::fill_n(row, inside_start, packed);
        }
        if (inside_end + 1 < surface_.width) {
            std::fill(row + inside_end + 1, row + surface_.width, packed);
        }
    }
}

}  // namespace eyes
