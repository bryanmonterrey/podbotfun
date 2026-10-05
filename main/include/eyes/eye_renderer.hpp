#pragma once

#include "eyes/capsule_face.hpp"
#include "eyes/eye_engine.hpp"
#include "eyes/raster.hpp"

namespace eyes {

// Pupil opacity (0..256) at a point in an authored blink clip: the reference
// ramps pupil alpha linearly 1 -> 0 -> 1 through per-clip keyframe windows.
unsigned blink_pupil_alpha(std::uint8_t clip_index, std::uint16_t elapsed_ms);

// Second personality: the Grok Bot capsule face (disc + two stadium eyes).
enum class FaceMode : std::uint8_t { creature, capsule };

class EyeRenderer {
  public:
    explicit EyeRenderer(Surface surface, bool round_viewport = true)
        : raster_(surface), round_viewport_(round_viewport)
    {
    }

    void render(const FrameState &state);
    PixelBounds rendered_bounds() const { return rendered_bounds_; }
    const DirtyRegions &rendered_regions() const { return rendered_regions_; }

    // Switching forces a full repaint on the next render.
    void set_face_mode(FaceMode mode);
    FaceMode face_mode() const { return face_mode_; }
    // Capsule ink style: false = palette disc with black eyes (Grok default),
    // true = inverted, black field with palette-colored eyes.
    void set_capsule_invert(bool invert)
    {
        if (capsule_invert_ != invert) {
            capsule_invert_ = invert;
            capsule_repaint_ = true;
        }
    }
    bool capsule_invert() const { return capsule_invert_; }
    CapsuleFace &capsule_face() { return capsule_; }
    // Text overlays paint over the capsule disc; the app calls this so the
    // next capsule frame repaints the disc fully (no-op in creature mode).
    void invalidate() { capsule_repaint_ = true; }

    // Raster antialiasing. The renderer normally picks the vertical subsample
    // count per path: 2 on tiny grid cells, 4 while content moves fast, 8 when
    // near-still. Setting a non-zero override pins it for tuning and A/B
    // comparison; 0 restores the adaptive choice the device ships with.
    void set_aa_override(int subsamples) { aa_override_ = subsamples; }
    int aa_override() const { return aa_override_; }
    // What the last rendered path actually asked for, and the lines per row
    // the rasterizer gave it (see aa_lines_for: a request of 4 samples 2).
    int last_aa_subsamples() const { return last_aa_subsamples_; }
    int last_aa_lines() const { return aa_lines_for(last_aa_subsamples_); }

    // Render phase timings from the last frame: [0]=clear [1]=paths [2]=mask (us).
    std::array<std::uint32_t, 3> phase_us{};
    // Test hooks: force the old full-rect pre-clear on hero frames (reference
    // behavior for the diff-clear exactness test) / count frames that took the
    // diff-clear path.
    bool disable_diff_clear{false};
    std::uint32_t diff_clear_frames{0};

  private:
#if CONFIG_LILGUY_WORLD_VIEW
    // One browse-grid cell as painted last frame: fisheye geometry plus the
    // pixel bounds it wrote, so pan-idle frames can erase and repaint only the
    // animated cells and leave static ones untouched.
    struct GridCellPaint {
        float x{0.0F};
        float y{0.0F};
        float scale{0.0F};
        float brightness{0.0F};
        PixelBounds bounds{};
        std::uint8_t palette{0};
        bool animated{false};
        bool anchor{false};
    };
#endif

    // pupil_ramp fades the pupil-clip deltas in at boot; morph (nullable)
    // carries the shape-switch morph state and only applies to the selection
    // (hero / grid anchor) — grid neighbor cells own fixed shapes.
    void render_single(const Selection &selection, float center_x, float center_y, float scale,
                       float openness, Vec2 gaze, Vec2 pupil_gaze, float brightness,
                       float rotation, float poke,
                       std::uint8_t blink_clip, std::uint16_t blink_elapsed_ms, bool idle_active,
                       std::uint32_t time_ms, const ExpressionPose &expression_pose,
                       std::uint8_t rot_clip, std::uint16_t rot_elapsed_ms,
                       float pupil_ramp, const FrameState *morph, std::uint8_t wink);
    void render_capsule(const FrameState &state);
#if CONFIG_LILGUY_WORLD_VIEW
    void render_grid(const FrameState &state, bool incremental);
    void paint_grid_cell(const FrameState &state, const GridCellPaint &cell);
    bool grid_cache_matches(const FrameState &state) const;
#endif

    Raster raster_;
    int aa_override_{0};
    int last_aa_subsamples_{8};
    bool round_viewport_{true};
    bool initialized_{false};
    bool motion_fast_{false};
    FaceMode face_mode_{FaceMode::creature};
    CapsuleFace capsule_{};
    // Capsule mode paint state: full repaint pending (mode entry / palette
    // change / first frame) and last frame's per-eye painted bounds (eye-only
    // frames erase these by repainting the disc color under them).
    bool capsule_repaint_{true};
    bool capsule_invert_{false};
    int capsule_palette_{-1};
    PixelBounds capsule_eye_bounds_[2]{};
    // Last frame was a pure hero frame whose covered runs were accumulated, so
    // this frame may diff-clear (lazy row erases) instead of rect pre-clears.
    bool hero_row_runs_valid_{false};
    Vec2 last_gaze_{};
    Vec2 last_pupil_gaze_{};
    ExpressionPose last_pose_{};
    PixelBounds painted_bounds_{};
    PixelBounds rendered_bounds_{};
    DirtyRegions painted_regions_{};
    DirtyRegions rendered_regions_{};
#if CONFIG_LILGUY_WORLD_VIEW
    // Grid paint cache (valid while the field geometry is unchanged frame to
    // frame: same offset, visibility, and anchor palette).
    std::array<GridCellPaint, 48> grid_cells_{};
    std::size_t grid_cell_count_{0};
    Vec2 grid_prev_offset_{};
    float grid_prev_visibility_{-1.0F};
    int grid_prev_palette_{-1};
    bool grid_cache_valid_{false};
#endif
};

}  // namespace eyes
