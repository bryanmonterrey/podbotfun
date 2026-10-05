#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "eyes/types.hpp"

namespace eyes {

// When defined (device display lane), rgb565() emits byte-swapped pixels so the
// framebuffer can go to the panel without a swap pass. Host builds keep native
// order so golden images stay meaningful.
#ifdef EYES_RGB565_SWAPPED
inline constexpr bool kSwapBytes = true;
#else
inline constexpr bool kSwapBytes = false;
#endif

struct Surface {
    std::uint16_t *pixels{nullptr};
    int width{0};
    int height{0};
    int stride{0};
};

struct PixelBounds {
    int x0{0};
    int y0{0};
    int x1{-1};
    int y1{-1};

    bool valid() const { return x0 <= x1 && y0 <= y1; }
};

constexpr std::size_t kDirtyRegionRows = 7U;
constexpr std::size_t kDirtyRegionColumns = 4U;
constexpr std::size_t kDirtyRegionCount = kDirtyRegionRows * kDirtyRegionColumns;
using DirtyRegions = std::array<PixelBounds, kDirtyRegionCount>;

// Vertical AA lines the rasterizer actually uses for a requested subsample
// count. Only 8 and 2 exist: anything below 8 falls back to 2, so a request of
// 4 samples the same rows as 2. Horizontal coverage is analytic either way.
constexpr int aa_lines_for(int subsamples) { return subsamples >= 8 ? 8 : 2; }

std::uint16_t rgb565(Rgb color);
Rgb scale_color(Rgb color, float brightness);

class Raster {
  public:
    explicit Raster(Surface surface) : surface_(surface) {}

    void clear(Rgb color);
    void pixel(int x, int y, Rgb color);
    void horizontal_line(int x0, int x1, int y, Rgb color);
    // clip: optional pixel rect; rows and spans are clamped so only pixels
    // inside it are written, with coverage identical to the unclipped fill.
    void fill_circle(float cx, float cy, float radius, Rgb color,
                     const PixelBounds *clip = nullptr);
    void fill_ellipse(float cx, float cy, float rx, float ry, float shear, Rgb color);
    void fill_capsule(float cx, float cy, float rx, float ry, float radius, Rgb color);
    void fill_diamond(float cx, float cy, float rx, float ry, Rgb color);
    // steps_per_curve: bezier flatten steps (2..32); subsamples: vertical AA
    // lines per pixel row (8 or 2). Defaults keep the original quality; small
    // grid cells pass reduced values to cut per-path fixed cost.
    void fill_cubic_path(const Vec2 *points, std::size_t point_count, Rgb color,
                         int steps_per_curve = 32, int subsamples = 8);
    // opacity: global path opacity 0..256 multiplied into coverage before
    // blending (256 = opaque, identical to the unparameterized behavior).
    void fill_cubic_path_clipped(const Vec2 *points, std::size_t point_count, Rgb color,
                                 Rgb existing, int steps_per_curve = 32, int subsamples = 8,
                                 unsigned opacity = 256U);
    void quadratic_curve(Vec2 p0, Vec2 p1, Vec2 p2, float thickness, Rgb color);
    void apply_round_mask(float cx, float cy, float radius, Rgb outside, int y_first = 0,
                          int y_last = -1);

    // Frame-level covered-run tracking for the hero diff-clear. Between begin
    // and end, unclipped cubic fills accumulate their per-row covered runs
    // (2 runs/row; overflow merges). With erase_previous, the first fill that
    // touches a row first erases the PREVIOUS frame's runs on it, and
    // end_row_runs sweeps rows the frame never revisited — pixel-equivalent to
    // a full pre-clear of last frame's painted area, but the overlap is erased
    // and repainted while the row is cache-hot and untouched rows cost nothing.
    // All erases go through horizontal_line, so the dirty set covers them.
    // Only valid when every write in the frame comes from fill_cubic_path
    // (clipped fills stay inside the last unclipped path's runs).
    void begin_row_runs(bool erase_previous);
    void end_row_runs();

    void reset_dirty_bounds()
    {
        dirty_bounds_ = {};
        dirty_regions_ = {};
    }
    PixelBounds dirty_bounds() const { return dirty_bounds_; }
    const DirtyRegions &dirty_regions() const { return dirty_regions_; }
    // Secondary bounds accumulator: everything written since the last take.
    // Lets the renderer record per-cell painted bounds without disturbing the
    // frame-level dirty tracking above.
    PixelBounds take_recent_bounds()
    {
        const PixelBounds bounds = recent_bounds_;
        recent_bounds_ = {};
        return bounds;
    }

    const Surface &surface() const { return surface_; }

  private:
    void fill_cubic_path_impl(const Vec2 *points, std::size_t point_count, Rgb color,
                              const std::uint16_t *clip_to, int steps_per_curve,
                              int subsamples, unsigned opacity);
    void covered_span(float x0, float x1, int y, std::uint16_t color,
                      const std::uint16_t *clip_to = nullptr, float row_coverage = 1.0F);
    void include_bounds(int x0, int y0, int x1, int y1);
    void note_frame_run(int y, int lo, int hi);
    void pre_erase_row(int y);

    // Fixed-point (12 fractional bits) polygon edge for the scanline rasterizer.
    struct PathEdge {
        std::int32_t x;           // x at the current subsample line, Q12
        std::int32_t step;        // x increment per subsample line, Q12
        std::int32_t first_line;  // first subsample line index
        std::int32_t last_line;   // one past the last subsample line index
        std::int32_t winding;     // +1 edge points down, -1 up (nonzero fill rule)
    };
    static constexpr std::size_t kMaxPathPoints = 12U;
    static constexpr std::size_t kPathStepsPerCurve = 32U;
    static constexpr std::size_t kMaxPathVertices =
        (kMaxPathPoints / 3U) * kPathStepsPerCurve;

    Surface surface_{};
    PixelBounds dirty_bounds_{};
    PixelBounds recent_bounds_{};
    DirtyRegions dirty_regions_{};
    // Per-row covered runs (up to two) of the last unclipped cubic path; clipped
    // paths clip to them geometrically (like the reference client's canvas clip).
    // Two runs matter mid-blink: the folding lid crosses a row as two horns and
    // the pupil must not paint the black gap between them.
    // ponytail: >2 runs per row are merged into run 2; no rig shape produces them.
    std::array<std::int16_t, kScreenHeight> clip_run1_lo_{};
    std::array<std::int16_t, kScreenHeight> clip_run1_hi_{};
    std::array<std::int16_t, kScreenHeight> clip_run2_lo_{};
    std::array<std::int16_t, kScreenHeight> clip_run2_hi_{};
    int clip_span_y0_{0};
    int clip_span_y1_{-1};
    std::array<std::uint16_t, kScreenWidth> coverage_{};
    // Double-buffered per-row covered runs of a whole frame's unclipped fills
    // (current accumulates, the other holds last frame's for the diff-clear).
    // ponytail: capacity 2 runs/row; a third run (blink fold horns + the other
    // eye) merges into run 2 — the union re-erases black gap pixels next
    // frame, which is harmless extra traffic on fold rows only.
    struct RowRuns {
        std::array<std::int16_t, kScreenHeight> lo1;
        std::array<std::int16_t, kScreenHeight> hi1;
        std::array<std::int16_t, kScreenHeight> lo2;
        std::array<std::int16_t, kScreenHeight> hi2;
    };
    RowRuns row_runs_[2]{};
    int row_runs_current_{0};
    bool row_runs_active_{false};
    bool row_runs_erase_{false};
    std::array<bool, kScreenHeight> row_visited_{};
    // A member (not a local) so the device task stack stays small.
    std::array<PathEdge, kMaxPathVertices> path_edges_{};
};

}  // namespace eyes
