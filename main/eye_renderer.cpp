#include "eyes/eye_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "eyes/creature_animation_data.hpp"
#include "eyes/creature_rig_data.hpp"

namespace eyes {
namespace {

constexpr Rgb kBlack{0, 0, 0};
constexpr float kSourceCenterX = 200.0F;
constexpr float kSourceCenterY = 189.25F;
constexpr float kHeroScale = 1.315F;
constexpr float kHeroYOffset = -5.0F;
constexpr float kLookLimit = 0.75F;
constexpr float kLookStep = 0.1875F;
constexpr std::uint8_t kNoRotClip = 0xFFU;

using RigPoints = std::array<Vec2, creature::kPathCount * creature::kPointsPerPath>;

float clamp01(float value) { return std::clamp(value, 0.0F, 1.0F); }

[[maybe_unused]] float smoothstep01(float value)  // grid-only when world view is off
{
    value = clamp01(value);
    return value * value * (3.0F - 2.0F * value);
}

// Skip pupil fills that would land below ~3 px (sub-pixel work on tiny cells;
// hero renders at scale 1.0 always pass).
constexpr float kMinPupilScale = 0.03F;
// Reduced raster quality for small cells: fewer bezier flatten steps below
// kFastStepsScale (chord error stays sub-pixel), and 2 instead of 8 vertical
// AA subsamples below kFastAaScale. Hero-sized renders keep full quality.
constexpr float kFastStepsScale = 0.7F;
constexpr float kFastAaScale = 0.35F;

#if CONFIG_LILGUY_WORLD_VIEW
// --- Hex browse grid (reference client's infinite canvas) -------------------
// Radial fisheye: cells live on the hex lattice in "world" pixels; a world
// point at radius d lands on screen at radius fisheye_radius(d), whose
// derivative fisheye_scale(d) is the local magnification. Because the radius
// mapping is the exact integral of the scale falloff, radially adjacent cells
// whose glyphs fill kHexCellFill of their pitch cannot overlap (trapezoid
// sum of half-widths equals the mapped spacing) — except inside the center
// zoom bubble (kCenterBoost), whose overlaps the relaxation pass resolves.
constexpr float kFishEyeRadius = 0.5F * static_cast<float>(
    kScreenWidth < kScreenHeight ? kScreenWidth : kScreenHeight);
constexpr float kFishEyeCenter = 1.85F;  // reference ls(0)
constexpr float kFishEyeEdge = 0.35F;    // reference ls(1)
constexpr float kCenterBoost = 1.90F;    // reference zoom bubble peak
constexpr float kCenterBoostRadius = kFishEyeRadius * 0.40F;
constexpr float kHexCellFill = 0.92F;    // glyph width as a fraction of the local pitch
constexpr float kGlyphSpanPx = 360.0F;   // eye-pair width at render_single scale 1
// Reference collision relaxation (eyes_grid.min.js): <=8 iterations of
// pairwise circle separation over the cells, positions only (scales stay),
// pad added to the summed radii, asymmetric push q = clamp01(0.5 + 4(sB-sA))
// so the smaller cell yields. Needed because the 1.9 zoom bubble overlaps its
// ring (the plain fisheye only guarantees no overlap without the boost).
constexpr int kRelaxIterations = 8;
constexpr float kRelaxPadPx = 2.0F;
// The reference's 4x gain is in blob-scale units; cell.scale is
// render_scale = blob_scale * kHexCellFill * kHexCellPitch / kGlyphSpanPx.
constexpr float kRelaxScaleGain = 4.0F * kGlyphSpanPx / (kHexCellFill * kHexCellPitch);
constexpr float kHexWorldReach = 215.0F; // world radius that can still touch the screen
// Only the innermost cells animate (idle/pupil/blink); everything further out
// renders a fixed neutral pose so pan-idle frames can skip repainting it.
constexpr float kAnimatedRadius = 2.2F * kHexRowPitch;

float fisheye_scale(float world_distance)
{
    return kFishEyeCenter -
           (kFishEyeCenter - kFishEyeEdge) * smoothstep01(world_distance / kFishEyeRadius);
}

float fisheye_radius(float world_distance)
{
    const float t = std::min(world_distance / kFishEyeRadius, 1.0F);
    const float integral = t * t * t * (1.0F - 0.5F * t);  // ∫ smoothstep = R(t³ - t⁴/2)
    float mapped = kFishEyeCenter * std::min(world_distance, kFishEyeRadius) -
                   (kFishEyeCenter - kFishEyeEdge) * kFishEyeRadius * integral;
    if (world_distance > kFishEyeRadius) {
        mapped += kFishEyeEdge * (world_distance - kFishEyeRadius);
    }
    return mapped;
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

std::uint32_t cell_idle_phase(int torus_index) { return hex_cell_hash(torus_index) % 3000U; }

// Boot ramp: the reference desyncs each instance with a random pre-tick and
// ramps every clip's weight in from zero, so playback starts at a zero delta.
// Firmware bakes full-weight steady cycles instead, so ease both the per-cell
// phase offset and the pupil-clip deltas in over the first 300 ms of engine
// time; at t=0 the pose is exactly neutral (idle frame 0 is a zero delta).
constexpr std::uint32_t kBootRampMs = 300U;

float boot_ramp(std::uint32_t time_ms)
{
    return static_cast<float>(std::min(time_ms, kBootRampMs)) /
           static_cast<float>(kBootRampMs);
}

std::uint32_t cell_time(std::uint32_t time_ms, int torus_index)
{
    return time_ms + static_cast<std::uint32_t>(
                         boot_ramp(time_ms) * static_cast<float>(cell_idle_phase(torus_index)));
}

#if CONFIG_LILGUY_WORLD_VIEW
bool bounds_intersect(PixelBounds a, PixelBounds b, int margin)
{
    if (!a.valid() || !b.valid()) {
        return false;
    }
    return a.x0 - margin <= b.x1 && b.x0 - margin <= a.x1 &&
           a.y0 - margin <= b.y1 && b.y0 - margin <= a.y1;
}

// Erase a rect to black through the raster so the frame dirty set (and thus
// the transfer band) covers it — unlike clear_bounds, which is untracked.
void erase_tracked(Raster &raster, PixelBounds bounds)
{
    for (int y = bounds.y0; y <= bounds.y1; ++y) {
        raster.horizontal_line(bounds.x0, bounds.x1, y, kBlack);
    }
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

#ifdef ESP_PLATFORM
extern "C" std::int64_t esp_timer_get_time(void);
std::int64_t phase_clock() { return esp_timer_get_time(); }
#else
std::int64_t phase_clock() { return 0; }
#endif

float mix(float a, float b, float amount) { return a + (b - a) * amount; }

PixelBounds union_bounds(PixelBounds left, PixelBounds right)
{
    if (!left.valid()) {
        return right;
    }
    if (!right.valid()) {
        return left;
    }
    return {std::min(left.x0, right.x0), std::min(left.y0, right.y0),
            std::max(left.x1, right.x1), std::max(left.y1, right.y1)};
}

void clear_bounds(Surface surface, PixelBounds bounds)
{
    if (surface.pixels == nullptr || !bounds.valid()) {
        return;
    }
    bounds.x0 = std::clamp(bounds.x0, 0, surface.width - 1);
    bounds.y0 = std::clamp(bounds.y0, 0, surface.height - 1);
    bounds.x1 = std::clamp(bounds.x1, 0, surface.width - 1);
    bounds.y1 = std::clamp(bounds.y1, 0, surface.height - 1);
    for (int y = bounds.y0; y <= bounds.y1; ++y) {
        std::fill_n(surface.pixels + static_cast<std::ptrdiff_t>(y) * surface.stride + bounds.x0,
                    bounds.x1 - bounds.x0 + 1, rgb565(kBlack));
    }
}

std::uint16_t closed_blink_elapsed(int state)
{
    const auto &clip = creature::kBlinkClips[0];
    for (std::size_t frame = 0; frame < clip.frame_count; ++frame) {
        const std::size_t index = static_cast<std::size_t>(state) * creature::kBlinkTotalFrameCount +
                                  clip.frame_offset + frame;
        if ((creature::kBlinkPathMasks[index] & 0x0aU) == 0U) {
            return creature::kBlinkFrameTimesMs[clip.frame_offset + frame];
        }
    }
    return 0U;
}

const creature::RigFrame &rig_frame(int state, int x, int y)
{
    const std::size_t index = static_cast<std::size_t>(state) * creature::kLookGridSize *
                                  creature::kLookGridSize +
                              static_cast<std::size_t>(y) * creature::kLookGridSize +
                              static_cast<std::size_t>(x);
    return creature::kLookFrames[index];
}

// The 4 look-grid corner frames plus bilinear weights for one gaze value.
struct GazeSample {
    const creature::RigFrame *corners[4];
    float tx;
    float ty;
};

GazeSample gaze_sample(int state, Vec2 gaze)
{
    const float grid_x = (std::clamp(gaze.x, -kLookLimit, kLookLimit) + kLookLimit) / kLookStep;
    // The client rig uses positive Y for an upward look. Touch coordinates use positive Y down.
    const float grid_y = (std::clamp(-gaze.y, -kLookLimit, kLookLimit) + kLookLimit) / kLookStep;
    const int x0 = std::clamp(static_cast<int>(std::floor(grid_x)), 0,
                              static_cast<int>(creature::kLookGridSize) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(grid_y)), 0,
                              static_cast<int>(creature::kLookGridSize) - 1);
    const int x1 = std::min(x0 + 1, static_cast<int>(creature::kLookGridSize) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(creature::kLookGridSize) - 1);
    return {
        {&rig_frame(state, x0, y0), &rig_frame(state, x1, y0),
         &rig_frame(state, x0, y1), &rig_frame(state, x1, y1)},
        grid_x - static_cast<float>(x0),
        grid_y - static_cast<float>(y0),
    };
}

// Outer eye paths (0, 2) follow eye_gaze; pupil paths (1, 3) follow pupil_gaze
// (the reference smooths the eye outline harder than the pupils, so pupils
// lead). One bilinear per point either way; only the weights differ per path.
RigPoints sample_rig(int state, Vec2 eye_gaze, Vec2 pupil_gaze)
{
    state = std::clamp(state, 0, static_cast<int>(creature::kStateCount) - 1);
    const GazeSample eye_sample = gaze_sample(state, eye_gaze);
    const GazeSample pupil_sample = gaze_sample(state, pupil_gaze);

    RigPoints points{};
    for (std::size_t point = 0; point < points.size(); ++point) {
        const GazeSample &sample =
            (point / creature::kPointsPerPath) % 2U == 1U ? pupil_sample : eye_sample;
        const float tx = sample.tx;
        const float ty = sample.ty;
        const auto &top_left = *sample.corners[0];
        const auto &top_right = *sample.corners[1];
        const auto &bottom_left = *sample.corners[2];
        const auto &bottom_right = *sample.corners[3];
        const std::size_t coordinate = point * 2U;
        const float top_x = mix(static_cast<float>(top_left[coordinate]),
                                static_cast<float>(top_right[coordinate]), tx);
        const float bottom_x = mix(static_cast<float>(bottom_left[coordinate]),
                                   static_cast<float>(bottom_right[coordinate]), tx);
        const float top_y = mix(static_cast<float>(top_left[coordinate + 1U]),
                                static_cast<float>(top_right[coordinate + 1U]), tx);
        const float bottom_y = mix(static_cast<float>(bottom_left[coordinate + 1U]),
                                   static_cast<float>(bottom_right[coordinate + 1U]), tx);
        points[point] = {
            mix(top_x, bottom_x, ty) * creature::kCoordinateScale,
            mix(top_y, bottom_y, ty) * creature::kCoordinateScale,
        };
    }
    return points;
}

void apply_idle(RigPoints &points, int state, std::uint32_t time_ms)
{
    state = std::clamp(state, 0, static_cast<int>(creature::kStateCount) - 1);
    const std::uint16_t elapsed = static_cast<std::uint16_t>(time_ms % creature::kIdleDurationMs);
    std::size_t frame = 0;
    while (frame + 1U < creature::kIdleFrameCount &&
           creature::kIdleFrameTimesMs[frame + 1U] <= elapsed) {
        ++frame;
    }
    const std::size_t next = std::min(frame + 1U, creature::kIdleFrameCount - 1U);
    const std::uint16_t start_ms = creature::kIdleFrameTimesMs[frame];
    const std::uint16_t end_ms = creature::kIdleFrameTimesMs[next];
    const float amount = end_ms == start_ms
                             ? 0.0F
                             : static_cast<float>(elapsed - start_ms) /
                                   static_cast<float>(end_ms - start_ms);
    const std::size_t offset = static_cast<std::size_t>(state) * creature::kIdleFrameCount;
    const auto &from = creature::kIdleDeltas[offset + frame];
    const auto &to = creature::kIdleDeltas[offset + next];
    for (std::size_t point = 0; point < points.size(); ++point) {
        const std::size_t coordinate = point * 2U;
        points[point].x += mix(static_cast<float>(from[coordinate]),
                               static_cast<float>(to[coordinate]), amount) *
                           creature::kCoordinateScale;
        points[point].y += mix(static_cast<float>(from[coordinate + 1U]),
                               static_cast<float>(to[coordinate + 1U]), amount) *
                           creature::kCoordinateScale;
    }
}

// Bracketing samples for a clip-local time. Looping clips have no duplicate
// end sample; the seam interpolates from the last sample back to frame 0
// across [times[last], duration_ms].
struct ClipSpan {
    std::size_t frame;
    std::size_t next;
    float amount;
};

ClipSpan clip_span(const creature::FlourishClip &clip, const std::uint16_t *times,
                   std::uint16_t elapsed_ms, bool loop)
{
    std::size_t frame = 0;
    while (frame + 1U < clip.frame_count &&
           times[clip.time_offset + frame + 1U] <= elapsed_ms) {
        ++frame;
    }
    std::size_t next = frame + 1U;
    const std::uint16_t start_ms = times[clip.time_offset + frame];
    std::uint16_t end_ms;
    if (next < clip.frame_count) {
        end_ms = times[clip.time_offset + next];
    } else {
        next = loop ? 0U : frame;
        end_ms = clip.duration_ms;
    }
    const float amount = end_ms <= start_ms
                             ? 0.0F
                             : static_cast<float>(elapsed_ms - start_ms) /
                                   static_cast<float>(end_ms - start_ms);
    return {frame, next, amount};
}

// Additive playback shared by the pupil and rotation flourish clips: lerp the
// bracketing samples and add the deltas (same scheme as apply_idle). weight
// scales the contribution (boot ramp; 1 = authored).
template <typename DeltaFrames>
void add_flourish(RigPoints &points, const creature::FlourishClip &clip, const ClipSpan &span,
                  const DeltaFrames &deltas, std::size_t total_frames, int state, float weight)
{
    const std::size_t offset = static_cast<std::size_t>(state) * total_frames + clip.delta_offset;
    const auto &from = deltas[offset + span.frame];
    const auto &to = deltas[offset + span.next];
    const float unit = weight * creature::kCoordinateScale;
    for (std::size_t point = 0; point < points.size(); ++point) {
        const std::size_t coordinate = point * 2U;
        points[point].x += mix(static_cast<float>(from[coordinate]),
                               static_cast<float>(to[coordinate]), span.amount) *
                           unit;
        points[point].y += mix(static_cast<float>(from[coordinate + 1U]),
                               static_cast<float>(to[coordinate + 1U]), span.amount) *
                           unit;
    }
}

// Gaze x flourish cross-terms: the clip deltas are baked at center gaze, and
// int8 corrections were sampled at the 4 look-grid corner anchors. Runtime
// blends bilinearly over a 3x3 grid whose corners are stored, edges are
// adjacent-corner averages, and center is zero — collapsed here into one
// weight per stored corner (rig-Y-major (-,-), (+,-), (-,+), (+,+)).
struct CornerWeights {
    float w[4];
    bool any;
};

CornerWeights corner_weights(Vec2 gaze)
{
    const float grid_x = (std::clamp(gaze.x, -kLookLimit, kLookLimit) + kLookLimit) / kLookLimit;
    // Rig Y is up; touch Y is down (same orientation as sample_rig).
    const float grid_y = (std::clamp(-gaze.y, -kLookLimit, kLookLimit) + kLookLimit) / kLookLimit;
    const int anchor_x = std::min(static_cast<int>(grid_x), 1);
    const int anchor_y = std::min(static_cast<int>(grid_y), 1);
    const float tx = grid_x - static_cast<float>(anchor_x);
    const float ty = grid_y - static_cast<float>(anchor_y);
    CornerWeights result{{0.0F, 0.0F, 0.0F, 0.0F}, false};
    const auto add_node = [&result](int node_x, int node_y, float weight) {
        if (weight <= 0.0F || (node_x == 1 && node_y == 1)) {
            return;  // center node: zero correction by construction
        }
        if (node_x != 1 && node_y != 1) {
            result.w[(node_y / 2) * 2 + node_x / 2] += weight;
        } else if (node_x == 1) {  // horizontal edge: average of its two corners
            result.w[(node_y / 2) * 2 + 0] += 0.5F * weight;
            result.w[(node_y / 2) * 2 + 1] += 0.5F * weight;
        } else {  // vertical edge
            result.w[0 * 2 + node_x / 2] += 0.5F * weight;
            result.w[1 * 2 + node_x / 2] += 0.5F * weight;
        }
    };
    add_node(anchor_x, anchor_y, (1.0F - tx) * (1.0F - ty));
    add_node(anchor_x + 1, anchor_y, tx * (1.0F - ty));
    add_node(anchor_x, anchor_y + 1, (1.0F - tx) * ty);
    add_node(anchor_x + 1, anchor_y + 1, tx * ty);
    result.any = result.w[0] + result.w[1] + result.w[2] + result.w[3] > 0.001F;
    return result;
}

void add_corner_corrections(RigPoints &points, const creature::IdleFrame *table, float scale,
                            std::size_t total_frames, int state,
                            const creature::FlourishClip &clip, const ClipSpan &span,
                            const CornerWeights &corners, float weight)
{
    const creature::IdleFrame *from[4];
    const creature::IdleFrame *to[4];
    for (int corner = 0; corner < 4; ++corner) {
        const std::size_t base =
            (static_cast<std::size_t>(state) * creature::kFlourishCornerCount +
             static_cast<std::size_t>(corner)) *
                total_frames +
            clip.time_offset;
        from[corner] = &table[base + span.frame];
        to[corner] = &table[base + span.next];
    }
    const float unit = weight * scale * creature::kCoordinateScale;
    for (std::size_t point = 0; point < points.size(); ++point) {
        const std::size_t coordinate = point * 2U;
        float from_x = 0.0F;
        float from_y = 0.0F;
        float to_x = 0.0F;
        float to_y = 0.0F;
        for (int corner = 0; corner < 4; ++corner) {
            const float corner_weight = corners.w[corner];
            if (corner_weight <= 0.0F) {
                continue;
            }
            from_x += corner_weight * static_cast<float>((*from[corner])[coordinate]);
            from_y += corner_weight * static_cast<float>((*from[corner])[coordinate + 1U]);
            to_x += corner_weight * static_cast<float>((*to[corner])[coordinate]);
            to_y += corner_weight * static_cast<float>((*to[corner])[coordinate + 1U]);
        }
        points[point].x += mix(from_x, to_x, span.amount) * unit;
        points[point].y += mix(from_y, to_y, span.amount) * unit;
    }
}

// Persistent pupil clips (pup_mov_1, pup_mov_2, pup_scale) loop like idle
// does. ramp fades their steady-cycle deltas in over the boot window (the
// reference ramps clip weight in from zero; pup_mov_2's steady cycle starts
// 3 px off neutral and would otherwise pop on the first frame).
void apply_pupils(RigPoints &points, int state, std::uint32_t time_ms, Vec2 gaze, float ramp)
{
    if (ramp <= 0.0F) {
        return;
    }
    state = std::clamp(state, 0, static_cast<int>(creature::kStateCount) - 1);
    const CornerWeights corners = corner_weights(gaze);
    for (const creature::FlourishClip &clip : creature::kPupilClips) {
        const std::uint16_t elapsed = static_cast<std::uint16_t>(time_ms % clip.duration_ms);
        const ClipSpan span = clip_span(clip, creature::kPupilFrameTimesMs.data(), elapsed, true);
        add_flourish(points, clip, span, creature::kPupilDeltasNarrow,
                     creature::kPupilNarrowFrameCount, state, ramp);
        if (corners.any) {
            add_corner_corrections(points, creature::kPupilGazeCorrections.data(),
                                   creature::kPupilGazeCorrectionScale,
                                   creature::kPupilTotalFrameCount, state, clip, span, corners,
                                   ramp);
        }
    }
}

// One-shot rotation flourish; clip_index >= kRotClipCount means none.
void apply_rot(RigPoints &points, int state, std::uint8_t clip_index, std::uint16_t elapsed_ms,
               Vec2 gaze)
{
    if (clip_index >= creature::kRotClipCount) {
        return;
    }
    state = std::clamp(state, 0, static_cast<int>(creature::kStateCount) - 1);
    const creature::FlourishClip &clip = creature::kRotClips[clip_index];
    elapsed_ms = std::min(elapsed_ms, clip.duration_ms);
    const ClipSpan span = clip_span(clip, creature::kRotFrameTimesMs.data(), elapsed_ms, false);
    if (clip.wide) {
        add_flourish(points, clip, span, creature::kRotDeltasWide,
                     creature::kRotWideFrameCount, state, 1.0F);
    } else {
        add_flourish(points, clip, span, creature::kRotDeltasNarrow,
                     creature::kRotNarrowFrameCount, state, 1.0F);
    }
    const CornerWeights corners = corner_weights(gaze);
    if (corners.any) {
        add_corner_corrections(points, creature::kRotGazeCorrections.data(),
                               creature::kRotGazeCorrectionScale,
                               creature::kRotTotalFrameCount, state, clip, span, corners, 1.0F);
    }
}

// wink: 0 = both eyes blink; 1/2 = left/right eye winks, so the OTHER eye's
// blink deltas are suppressed (its paths keep the neutral open shape).
std::uint8_t apply_blink(RigPoints &points, int state, std::uint8_t clip_index,
                         std::uint16_t elapsed_ms, Vec2 gaze, std::uint8_t wink)
{
    const std::uint8_t suppress =
        wink == 1U ? 0x0cU : wink == 2U ? 0x03U : 0x00U;  // paths: eye_l pup_l eye_r pup_r
    state = std::clamp(state, 0, static_cast<int>(creature::kStateCount) - 1);
    const auto &clip = creature::kBlinkClips[std::min<std::size_t>(
        clip_index, creature::kBlinkClipCount - 1U)];
    std::size_t frame = 0;
    while (frame + 1U < clip.frame_count &&
           creature::kBlinkFrameTimesMs[clip.frame_offset + frame + 1U] <= elapsed_ms) {
        ++frame;
    }
    const std::size_t next =
        std::min(frame + 1U, static_cast<std::size_t>(clip.frame_count - 1U));
    const std::uint16_t start_ms = creature::kBlinkFrameTimesMs[clip.frame_offset + frame];
    const std::uint16_t end_ms = creature::kBlinkFrameTimesMs[clip.frame_offset + next];
    const float amount = end_ms == start_ms
                             ? 0.0F
                             : static_cast<float>(elapsed_ms - start_ms) /
                                   static_cast<float>(end_ms - start_ms);
    const std::size_t state_offset =
        static_cast<std::size_t>(state) * creature::kBlinkTotalFrameCount;
    const std::size_t from_index = state_offset + clip.frame_offset + frame;
    const std::size_t to_index = state_offset + clip.frame_offset + next;
    const auto &from = creature::kBlinkFrames[from_index];
    const auto &to = creature::kBlinkFrames[to_index];
    const auto &neutral = rig_frame(state, 4, 4);
    const std::uint8_t from_mask = creature::kBlinkPathMasks[from_index];
    const std::uint8_t to_mask = creature::kBlinkPathMasks[to_index];

    // Gaze x blink cross-terms: bilinear-blend the int8 corrections from the 4
    // gaze anchors surrounding the current gaze (same orientation as sample_rig;
    // the center anchor stores zeros, so gaze (0,0) matches the base data).
    const float grid_x = (std::clamp(gaze.x, -kLookLimit, kLookLimit) + kLookLimit) / kLookLimit;
    const float grid_y = (std::clamp(-gaze.y, -kLookLimit, kLookLimit) + kLookLimit) / kLookLimit;
    const int anchor_x = std::min(static_cast<int>(grid_x), 1);
    const int anchor_y = std::min(static_cast<int>(grid_y), 1);
    const float tx = grid_x - static_cast<float>(anchor_x);
    const float ty = grid_y - static_cast<float>(anchor_y);
    const float weights[4] = {(1.0F - tx) * (1.0F - ty), tx * (1.0F - ty),
                              (1.0F - tx) * ty, tx * ty};
    const creature::IdleFrame *corr_from[4];
    const creature::IdleFrame *corr_to[4];
    for (int a = 0; a < 4; ++a) {
        const std::size_t anchor = static_cast<std::size_t>(
            (anchor_y + a / 2) * 3 + anchor_x + a % 2);
        const std::size_t base = (static_cast<std::size_t>(state) * creature::kBlinkAnchorCount +
                                  anchor) * creature::kBlinkTotalFrameCount + clip.frame_offset;
        corr_from[a] = &creature::kBlinkGazeCorrections[base + frame];
        corr_to[a] = &creature::kBlinkGazeCorrections[base + next];
    }
    const auto correction = [&](std::size_t coordinate) {
        float from_value = 0.0F;
        float to_value = 0.0F;
        for (int a = 0; a < 4; ++a) {
            from_value += weights[a] * static_cast<float>((*corr_from[a])[coordinate]);
            to_value += weights[a] * static_cast<float>((*corr_to[a])[coordinate]);
        }
        return mix(from_value, to_value, amount) * creature::kBlinkGazeCorrectionScale;
    };
    // Pupils (bits 1 and 3) only render when both endpoint frames want them:
    // mid-morph pupil geometry can poke above the folding lid otherwise.
    const std::uint8_t outer_mask = amount < 0.5F ? from_mask : to_mask;
    const std::uint8_t path_mask = static_cast<std::uint8_t>(
        ((outer_mask & 0x05U) | (from_mask & to_mask & 0x0aU)) | suppress);
    for (std::size_t path = 0; path < creature::kPathCount; ++path) {
        if ((suppress & (1U << path)) != 0U) {
            continue;  // winking: the other eye keeps its un-blinked shape
        }
        const bool from_active = (from_mask & (1U << path)) != 0U;
        const bool to_active = (to_mask & (1U << path)) != 0U;
        if (!from_active && !to_active) {
            continue;
        }
        const std::size_t first = path * creature::kPointsPerPath;
        const std::size_t end = first + creature::kPointsPerPath;
        for (std::size_t point = first; point < end; ++point) {
            const std::size_t coordinate = point * 2U;
            const float from_x = from_active ? static_cast<float>(from[coordinate])
                                             : static_cast<float>(neutral[coordinate]);
            const float to_x = to_active ? static_cast<float>(to[coordinate])
                                         : static_cast<float>(neutral[coordinate]);
            const float from_y = from_active ? static_cast<float>(from[coordinate + 1U])
                                             : static_cast<float>(neutral[coordinate + 1U]);
            const float to_y = to_active ? static_cast<float>(to[coordinate + 1U])
                                         : static_cast<float>(neutral[coordinate + 1U]);
            points[point].x +=
                (mix(from_x, to_x, amount) - static_cast<float>(neutral[coordinate]) +
                 correction(coordinate)) *
                creature::kCoordinateScale;
            points[point].y +=
                (mix(from_y, to_y, amount) - static_cast<float>(neutral[coordinate + 1U]) +
                 correction(coordinate + 1U)) *
                creature::kCoordinateScale;
        }
    }
    return path_mask;
}

void apply_expression(RigPoints &points, const ExpressionPose &pose)
{
    constexpr std::size_t points_per_path = creature::kPointsPerPath;
    for (std::size_t eye = 0; eye < 2U; ++eye) {
        const std::size_t outer = eye * 2U * points_per_path;
        float min_x = points[outer].x;
        float max_x = min_x;
        float min_y = points[outer].y;
        float max_y = min_y;
        for (std::size_t point = outer + 1U; point < outer + points_per_path; ++point) {
            min_x = std::min(min_x, points[point].x);
            max_x = std::max(max_x, points[point].x);
            min_y = std::min(min_y, points[point].y);
            max_y = std::max(max_y, points[point].y);
        }
        const float center_x = (min_x + max_x) * 0.5F;
        const float center_y = (min_y + max_y) * 0.5F;
        // Floor keeps expressions from flattening the eyes into slits; raise or
        // lower to taste (1.0 = no squash allowed, 0.48 = old sleepy squint).
        const float open = std::max(pose.openness, 0.68F);
        const float tilt = eye == 0U ? pose.tilt : -pose.tilt;
        const float cosine = std::cos(tilt);
        const float sine = std::sin(tilt);
        for (std::size_t path = eye * 2U; path < eye * 2U + 2U; ++path) {
            const float pupil_scale = path % 2U == 0U ? 1.0F : pose.pupil_scale;
            const std::size_t first = path * points_per_path;
            for (std::size_t point = first; point < first + points_per_path; ++point) {
                const float x = (points[point].x - center_x) * pupil_scale;
                const float y = (points[point].y - center_y) * open * pupil_scale;
                points[point] = {
                    center_x + x * cosine - y * sine,
                    center_y + x * sine + y * cosine,
                };
            }
        }
    }
}

// breath: uniform squash about the face center (1 + sin(.85t) * .007).
void transform_rig(RigPoints &points, float center_x, float center_y, float scale, float openness,
                   float rotation, float poke, float breath)
{
    const float open = clamp01(openness);
    const float vertical_scale = 0.055F + open * 0.945F;
    const float closure_drop = (1.0F - open) * 49.0F;
    const float live_scale = scale * kHeroScale * (1.0F + poke * 0.018F) * breath;
    const float cosine = std::cos(rotation);
    const float sine = std::sin(rotation);
    for (Vec2 &point : points) {
        point.y = kSourceCenterY + (point.y - kSourceCenterY) * vertical_scale + closure_drop;
        const float local_x = (point.x - kSourceCenterX) * live_scale;
        const float local_y = (point.y - kSourceCenterY) * live_scale;
        point = {
            center_x + local_x * cosine - local_y * sine,
            center_y + kHeroYOffset * scale + local_x * sine + local_y * cosine,
        };
    }
}

}  // namespace

// Pupil opacity (0..256) during an authored blink. The reference wasm ramps
// pupil alpha linearly 1 -> 0 -> 1 through per-clip keyframe windows (channel
// 0x0B); firmware previously hard-masked pupils, which popped. Outside the
// window the pupils are fully opaque.
unsigned blink_pupil_alpha(std::uint8_t clip_index, std::uint16_t elapsed_ms)
{
    const auto &clip = creature::kBlinkClips[std::min<std::size_t>(
        clip_index, creature::kBlinkClipCount - 1U)];
    const float t = static_cast<float>(elapsed_ms);
    float alpha = 1.0F;
    if (t > static_cast<float>(clip.fade_out_start_ms) &&
        t < static_cast<float>(clip.fade_in_end_ms)) {
        if (t < static_cast<float>(clip.fade_out_end_ms)) {
            alpha = (static_cast<float>(clip.fade_out_end_ms) - t) /
                    static_cast<float>(clip.fade_out_end_ms - clip.fade_out_start_ms);
        } else if (t > static_cast<float>(clip.fade_in_start_ms)) {
            alpha = (t - static_cast<float>(clip.fade_in_start_ms)) /
                    static_cast<float>(clip.fade_in_end_ms - clip.fade_in_start_ms);
        } else {
            alpha = 0.0F;
        }
    }
    return static_cast<unsigned>(alpha * 256.0F + 0.5F);
}

void EyeRenderer::set_face_mode(FaceMode mode)
{
    if (face_mode_ == mode) {
        return;
    }
    face_mode_ = mode;
    capsule_repaint_ = true;   // capsule entry: full disc repaint
    hero_row_runs_valid_ = false;  // creature re-entry: rect pre-clear path
}

void EyeRenderer::render(const FrameState &state)
{
    if (face_mode_ == FaceMode::capsule && state.grid_visibility <= 0.015F) {
        render_capsule(state);
        return;
    }
    const PixelBounds previous_bounds = painted_bounds_;
    const DirtyRegions previous_regions = painted_regions_;
    const std::int64_t t0 = phase_clock();
    // Motion proxy for adaptive AA: blinks, flourishes, pokes, grid activity,
    // fast gaze travel, or an expression pose still morphing toward its target.
    const float gaze_step = std::fabs(state.gaze.x - last_gaze_.x) +
                            std::fabs(state.gaze.y - last_gaze_.y) +
                            std::fabs(state.gaze_pupils.x - last_pupil_gaze_.x) +
                            std::fabs(state.gaze_pupils.y - last_pupil_gaze_.y);
    const float pose_step = std::fabs(state.expression_pose.openness - last_pose_.openness) +
                            std::fabs(state.expression_pose.tilt - last_pose_.tilt) +
                            std::fabs(state.expression_pose.pupil_scale - last_pose_.pupil_scale);
    motion_fast_ = state.blink_active || state.rot_active || state.morph_active ||
                   state.poke > 0.01F ||
                   state.grid_visibility > 0.015F || gaze_step > 0.004F || pose_step > 0.002F;
    last_gaze_ = state.gaze;
    last_pupil_gaze_ = state.gaze_pupils;
    last_pose_ = state.expression_pose;
#if CONFIG_LILGUY_WORLD_VIEW
    const bool grid_mode = state.grid_visibility > 0.015F;
    // Pan-idle: the grid field geometry is unchanged since last frame, so the
    // static cells' pixels are already correct — only the animated cells are
    // erased (tracked) and repainted; nothing else is cleared.
    const bool incremental = grid_mode && grid_cache_valid_ && grid_cache_matches(state);
#else
    // World view compiled out: the engine pins grid_visibility to zero.
    constexpr bool grid_mode = false;
    constexpr bool incremental = false;
#endif
    // Hero diff-clear: last frame's covered runs are known per row, so skip the
    // rect pre-clear and let the path fills erase each row lazily right before
    // repainting it (Raster::begin_row_runs) — pixel-equivalent to the full
    // clear, but the erase+repaint of the overlap shares one cache-hot pass
    // and rows outside both frames cost nothing.
    const bool diff_clear =
        !grid_mode && hero_row_runs_valid_ && initialized_ && !disable_diff_clear;
    diff_clear_frames += diff_clear ? 1U : 0U;
    if (!initialized_) {
        raster_.clear(kBlack);
        initialized_ = true;
    } else if (!incremental && !diff_clear) {
#if CONFIG_LILGUY_WORLD_VIEW
        if (grid_mode && grid_cache_valid_) {
            // Previous frame was the grid: every non-black pixel lies inside a
            // cached cell's bounds, so clearing those (typically ~40% of the
            // screen) beats clearing the coarse dirty buckets. The transfer
            // still covers the erased area via painted_regions_ union below.
            for (std::size_t index = 0; index < grid_cell_count_; ++index) {
                clear_bounds(raster_.surface(), grid_cells_[index].bounds);
            }
        } else
#endif
        {
            for (const PixelBounds bounds : previous_regions) {
                clear_bounds(raster_.surface(), bounds);
            }
        }
    }
    raster_.reset_dirty_bounds();
    const std::int64_t t1 = phase_clock();

    if (grid_mode) {
#if CONFIG_LILGUY_WORLD_VIEW
        hero_row_runs_valid_ = false;
        render_grid(state, incremental);
#endif
    } else {
#if CONFIG_LILGUY_WORLD_VIEW
        grid_cache_valid_ = false;
#endif
        // Accumulate this frame's covered runs even when diff_clear is off so
        // the NEXT hero frame can diff-clear against them.
        raster_.begin_row_runs(diff_clear);
        const bool authored_close = state.mode == InteractionMode::sleeping;
        const float openness = state.blink_active || authored_close ? -1.0F : state.blink_open;
        render_single(state.selection, static_cast<float>(kScreenWidth) * 0.5F,
                      static_cast<float>(kScreenHeight) * 0.5F, 1.0F, openness, state.gaze,
                      state.gaze_pupils, 1.0F,
                      state.face_rotation, state.poke, authored_close ? 0U : state.blink_clip,
                      authored_close ? closed_blink_elapsed(state.selection.shape)
                                     : state.blink_elapsed_ms,
                      // Reference client keeps idle running through blinks. The
                      // per-cell phase offset matches the grid's anchor cell so
                      // hero<->grid transitions keep the idle pose continuous.
                      !authored_close,
                      cell_time(state.time_ms, state.selection.palette),
                      state.expression_pose,
                      state.rot_active ? state.rot_clip : kNoRotClip, state.rot_elapsed_ms,
                      boot_ramp(state.time_ms), &state, state.wink);
        raster_.end_row_runs();  // sweep rows painted last frame, untouched now
        hero_row_runs_valid_ = true;
    }
    capsule_repaint_ = true;  // creature pixels invalidate the capsule paint cache

    const std::int64_t t2 = phase_clock();
    if (round_viewport_) {
        // Cleared rows are already black (mask color); only repainted rows need
        // masking, and only when the painted rect can reach outside the circle
        // (everything outside it is black by invariant, so the mask is a no-op
        // for interior-only frames — hero and pan-idle skip it entirely).
        const PixelBounds painted = raster_.dirty_bounds();
        if (painted.valid()) {
            const float center_x = static_cast<float>(kScreenWidth) * 0.5F;
            const float center_y = static_cast<float>(kScreenHeight) * 0.5F;
            const float radius = static_cast<float>(kScreenWidth) * 0.5F - 1.0F;
            const float dx = std::max(std::fabs(static_cast<float>(painted.x0) - center_x),
                                      std::fabs(static_cast<float>(painted.x1) - center_x));
            const float dy = std::max(std::fabs(static_cast<float>(painted.y0) - center_y),
                                      std::fabs(static_cast<float>(painted.y1) - center_y));
            if (dx * dx + dy * dy >= radius * radius) {
                raster_.apply_round_mask(center_x, center_y, radius, kBlack, painted.y0,
                                         painted.y1);
            }
        }
    }
    const std::int64_t t3 = phase_clock();
    phase_us[0] = static_cast<std::uint32_t>(t1 - t0);
    phase_us[1] = static_cast<std::uint32_t>(t2 - t1);
    phase_us[2] = static_cast<std::uint32_t>(t3 - t2);
    const PixelBounds frame_bounds = raster_.dirty_bounds();
    const DirtyRegions &frame_regions = raster_.dirty_regions();
    if (incremental) {
        // Erases went through the raster, so the frame dirty set alone covers
        // everything painted or erased — that is the transfer set. painted_*
        // keeps accumulating the occupied area so a later full repaint clears
        // the untouched static cells too.
        rendered_bounds_ = frame_bounds;
        rendered_regions_ = frame_regions;
        painted_bounds_ = union_bounds(previous_bounds, frame_bounds);
        for (std::size_t index = 0; index < painted_regions_.size(); ++index) {
            painted_regions_[index] =
                union_bounds(previous_regions[index], frame_regions[index]);
        }
    } else if (diff_clear) {
        // Every erase went through the raster (lazy row erases + end sweep),
        // so the frame dirty set alone covers all changed pixels — no union
        // with last frame's regions needed, which shrinks the transfer too.
        rendered_bounds_ = frame_bounds;
        rendered_regions_ = frame_regions;
        painted_bounds_ = frame_bounds;
        painted_regions_ = frame_regions;
    } else {
        painted_bounds_ = frame_bounds;
        painted_regions_ = frame_regions;
        rendered_bounds_ = union_bounds(previous_bounds, frame_bounds);
        for (std::size_t index = 0; index < rendered_regions_.size(); ++index) {
            rendered_regions_[index] =
                union_bounds(previous_regions[index], frame_regions[index]);
        }
    }
}

// Capsule face: palette-colored disc + two black stadium eyes (two-color
// model). The disc (~150 k px) is painted only on mode entry / palette change /
// first frame; steady frames erase last frame's eye regions by repainting the
// disc color under them (clipped to the disc circle) and then draw the new
// eyes, so transfers stay eye-sized.
void EyeRenderer::render_capsule(const FrameState &state)
{
    hero_row_runs_valid_ = false;  // creature re-entry must not diff-clear stale runs
#if CONFIG_LILGUY_WORLD_VIEW
    grid_cache_valid_ = false;
#endif
    const std::int64_t t0 = phase_clock();
    capsule_.update(state);
    const bool full = capsule_repaint_ || !initialized_ ||
                      capsule_palette_ != state.selection.palette;
    if (!full && !capsule_.needs_frame()) {
        // Quantized geometry unchanged: the rendered pixels would be identical.
        // Publish an empty transfer set so the app skips the blit entirely.
        rendered_bounds_ = {};
        rendered_regions_ = {};
        phase_us = {0U, 0U, 0U};
        return;
    }

    const PixelBounds previous_bounds = painted_bounds_;
    const DirtyRegions previous_regions = painted_regions_;
    const Palette &palette = palette_at(state.selection.palette);
    constexpr float center = static_cast<float>(kScreenWidth) * 0.5F;
    if (!initialized_) {
        raster_.clear(kBlack);
        initialized_ = true;
    }
    raster_.reset_dirty_bounds();
    const std::int64_t t1 = phase_clock();
    if (full) {
        // Erase whatever the previous mode/palette left behind, then repaint
        // the disc (normal style only; inverted style is a black field). The
        // clear_bounds writes are untracked, so the transfer below unions the
        // previous regions in.
        for (const PixelBounds bounds : previous_regions) {
            clear_bounds(raster_.surface(), bounds);
        }
        if (!capsule_invert_) {
            raster_.fill_circle(center, center, kCapsuleDiscRadius, palette.outer);
        }
        capsule_repaint_ = false;
        capsule_palette_ = state.selection.palette;
        capsule_eye_bounds_[0] = {};
        capsule_eye_bounds_[1] = {};
    } else {
        // Erase last frame's eye regions pixel-identically to a fresh repaint:
        // restore the black surround, then (normal style) re-blend the disc
        // through fill_circle's own AA math clipped to the bounds, so the rim's
        // antialiased ring survives eyes crossing it (an integer chord left
        // notches / hard full-color pixels there). Both writes are tracked,
        // so the frame dirty set covers the erases. Inverted style needs only
        // the black wipe.
        for (const PixelBounds bounds : capsule_eye_bounds_) {
            if (!bounds.valid()) {
                continue;
            }
            for (int y = bounds.y0; y <= bounds.y1; ++y) {
                raster_.horizontal_line(bounds.x0, bounds.x1, y, kBlack);
            }
            if (!capsule_invert_) {
                raster_.fill_circle(center, center, kCapsuleDiscRadius, palette.outer, &bounds);
            }
        }
    }
    raster_.take_recent_bounds();  // flush erase/disc writes off the eye marker
    for (int eye = 0; eye < 2; ++eye) {
        const CapsuleFace::EyeOut geometry = capsule_.eye(eye);
        std::array<Vec2, 12> path = capsule_path(
            geometry.cx, geometry.cy, geometry.angle_rad, geometry.half_len, geometry.half_w);
        // Blink: the lid comes DOWN — squash screen-vertically about the eye's
        // bottom edge so the lower rim stays planted while the top descends
        // (center-pivot closing reads as a mechanical iris, not a blink).
        float bottom = path[0].y;
        for (const Vec2 &point : path) {
            bottom = std::max(bottom, point.y);
        }
        for (Vec2 &point : path) {
            point.y = bottom - (bottom - point.y) * geometry.squash_y;
        }
        // The capsule eyes took the default 8 subsamples regardless; honour the
        // override here too so the control means the same thing in both faces.
        const int capsule_subsamples = aa_override_ > 0 ? aa_override_ : 8;
        last_aa_subsamples_ = capsule_subsamples;
        raster_.fill_cubic_path(path.data(), path.size(),
                                capsule_invert_ ? palette.outer : kBlack, 32,
                                capsule_subsamples);
        capsule_eye_bounds_[eye] = raster_.take_recent_bounds();
    }
    capsule_.mark_rendered();
    const std::int64_t t2 = phase_clock();

    const PixelBounds frame_bounds = raster_.dirty_bounds();
    const DirtyRegions &frame_regions = raster_.dirty_regions();
    if (full) {
        rendered_bounds_ = union_bounds(previous_bounds, frame_bounds);
        for (std::size_t index = 0; index < rendered_regions_.size(); ++index) {
            rendered_regions_[index] =
                union_bounds(previous_regions[index], frame_regions[index]);
        }
        painted_bounds_ = frame_bounds;
        painted_regions_ = frame_regions;
    } else {
        // Erases were tracked, so the frame dirty set alone is the transfer.
        // painted_* keeps accumulating the disc so a later full clear (mode
        // switch, palette change) erases everything on screen.
        rendered_bounds_ = frame_bounds;
        rendered_regions_ = frame_regions;
        painted_bounds_ = union_bounds(previous_bounds, frame_bounds);
        for (std::size_t index = 0; index < painted_regions_.size(); ++index) {
            painted_regions_[index] =
                union_bounds(previous_regions[index], frame_regions[index]);
        }
    }
    phase_us[0] = static_cast<std::uint32_t>(t1 - t0);
    phase_us[1] = static_cast<std::uint32_t>(t2 - t1);
    phase_us[2] = 0U;
}

#if CONFIG_LILGUY_WORLD_VIEW
bool EyeRenderer::grid_cache_matches(const FrameState &state) const
{
    return grid_prev_offset_.x == state.grid_offset.x &&
           grid_prev_offset_.y == state.grid_offset.y &&
           grid_prev_visibility_ == state.grid_visibility &&
           grid_prev_palette_ == state.selection.palette;
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

void EyeRenderer::render_single(const Selection &selection, float center_x, float center_y,
                                float scale, float openness, Vec2 gaze, Vec2 pupil_gaze,
                                float brightness,
                                float rotation, float poke, std::uint8_t blink_clip,
                                std::uint16_t blink_elapsed_ms, bool idle_active,
                                std::uint32_t time_ms, const ExpressionPose &expression_pose,
                                std::uint8_t rot_clip, std::uint16_t rot_elapsed_ms,
                                float pupil_ramp, const FrameState *morph, std::uint8_t wink)
{
    const Palette &palette = palette_at(selection.palette);
    const Rgb outer = scale_color(palette.outer, brightness);
    const Rgb left_pupil = scale_color(palette.inner, brightness);
    const Rgb right_pupil = scale_color(palette.accent, brightness);
    // Full pose composition for one shape (identical gaze/anim inputs across
    // shapes, so a morph blend of composed point sets stays coherent).
    const auto compose = [&](int shape, RigPoints &out) -> std::uint8_t {
        out = sample_rig(shape, gaze, pupil_gaze);
        if (idle_active) {
            // The reference client keeps idle plus the pupil clips looping on
            // every instance; all the flourish layers are additive deltas.
            apply_idle(out, shape, time_ms);
            apply_pupils(out, shape, time_ms, gaze, pupil_ramp);
        }
        apply_rot(out, shape, rot_clip, rot_elapsed_ms, gaze);
        return openness < 0.0F
                   ? apply_blink(out, shape, blink_clip, blink_elapsed_ms, gaze, wink)
                   : static_cast<std::uint8_t>(0x0fU);
    };
    RigPoints points{};
    std::uint8_t path_mask = 0x0fU;
    if (morph != nullptr && morph->morph_active) {
        // Shape-switch morph (reference: 800 ms point-wise lerp of the final
        // composed coordinates, scalar easeInOutCubic). Blend every shape the
        // weight vector still references; pupils render only where every
        // contributor keeps them (mask AND), matching the blink-morph rule.
        const float eased = ease_in_out_cubic(static_cast<float>(morph->morph_elapsed_ms) /
                                              static_cast<float>(kShapeMorphMs));
        std::array<float, kShapeCount> weights{};
        for (std::size_t shape = 0; shape < kShapeCount; ++shape) {
            weights[shape] = morph->morph_from_weights[shape] * (1.0F - eased);
        }
        weights[static_cast<std::size_t>(selection.shape)] += eased;
        RigPoints part;
        for (int shape = 0; shape < static_cast<int>(kShapeCount); ++shape) {
            const float weight = weights[static_cast<std::size_t>(shape)];
            if (weight <= 0.002F) {
                continue;
            }
            path_mask &= compose(shape, part);
            for (std::size_t point = 0; point < points.size(); ++point) {
                points[point].x += part[point].x * weight;
                points[point].y += part[point].y * weight;
            }
        }
    } else {
        path_mask = compose(selection.shape, points);
    }
    apply_expression(points, expression_pose);
    // Life-signal layers (Grok section 5): breath squash folded into the rig
    // transform scale, then per-eye micro-drift (<=1.2 px at hero scale, two
    // incommensurate sinusoids with per-eye phase). Both are time-driven and
    // gated on idle_active so static grid cells stay time-independent; the
    // boot ramp keeps t=0 exactly neutral (and the mirror test symmetric).
    const float ramp = idle_active ? boot_ramp(time_ms) : 0.0F;
    const float seconds = static_cast<float>(time_ms) * 0.001F;
    const float breath = 1.0F + (idle_active ? std::sin(0.85F * seconds) * 0.007F * ramp : 0.0F);
    transform_rig(points, center_x, center_y, scale, openness < 0.0F ? 1.0F : openness,
                  rotation, poke, breath);
    if (ramp > 0.0F) {
        constexpr std::size_t half = creature::kPathCount * creature::kPointsPerPath / 2U;
        for (std::size_t eye = 0; eye < 2U; ++eye) {
            const float phase = static_cast<float>(eye);
            const float unit = ramp * scale;
            const float drift_x = (std::sin(0.42F * seconds + phase) * 0.8F +
                                   std::sin(1.0F * seconds + 2.0F * phase) * 0.4F) *
                                  unit;
            const float drift_y = std::sin(0.58F * seconds + phase) * 0.9F * unit;
            for (std::size_t point = eye * half; point < (eye + 1U) * half; ++point) {
                points[point].x += drift_x;
                points[point].y += drift_y;
            }
        }
    }

    constexpr std::size_t points_per_path = creature::kPointsPerPath;
    const bool blinking = openness < 0.0F;
    // Reference behavior: pupil alpha ramps 1 -> 0 -> 1 through every blink, so
    // pupils ghost out under the closing lid instead of popping. The path mask
    // still AND-gates the fully-culled apex frames (alpha is 0 there anyway).
    // A wink keeps the open eye's pupil fully opaque.
    const unsigned pupil_alpha = blinking ? blink_pupil_alpha(blink_clip, blink_elapsed_ms) : 256U;
    const unsigned left_alpha = blinking && wink == 2U ? 256U : pupil_alpha;
    const unsigned right_alpha = blinking && wink == 1U ? 256U : pupil_alpha;
    // Sub-pixel pupils on tiny grid cells are invisible; skip the fill cost.
    const bool pupils_visible = scale >= kMinPupilScale && (blinking || openness > 0.14F);
    // Small cells trade flatten steps and vertical AA for per-path fixed cost;
    // hero-sized renders (scale >= 0.7) keep full quality when near-still, and
    // drop to half AA while content moves fast (motion masks the difference).
    const int steps = scale < kFastStepsScale ? 8 : (motion_fast_ ? 16 : 32);
    const int adaptive = scale < kFastAaScale ? 2 : (motion_fast_ ? 4 : 8);
    const int subsamples = aa_override_ > 0 ? aa_override_ : adaptive;
    last_aa_subsamples_ = subsamples;
    raster_.fill_cubic_path(points.data(), points_per_path, outer, steps, subsamples);
    if (pupils_visible && left_alpha > 0U && (path_mask & 0x02U) != 0U) {
        raster_.fill_cubic_path_clipped(points.data() + points_per_path, points_per_path,
                                        left_pupil, outer, steps, subsamples, left_alpha);
    }
    raster_.fill_cubic_path(points.data() + points_per_path * 2U, points_per_path, outer, steps,
                            subsamples);
    if (pupils_visible && right_alpha > 0U && (path_mask & 0x08U) != 0U) {
        raster_.fill_cubic_path_clipped(points.data() + points_per_path * 3U, points_per_path,
                                        right_pupil, outer, steps, subsamples, right_alpha);
    }
}

#if CONFIG_LILGUY_WORLD_VIEW
void EyeRenderer::render_grid(const FrameState &state, bool incremental)
{
    if (incremental) {
        // Pan-idle: field geometry unchanged, static cells' pixels already
        // correct. Erase (tracked) and repaint the animated cells, plus any
        // static cell whose recorded bounds touch the erased area — repainting
        // needs a clean black background for exact antialiasing.
        std::array<bool, 48> repaint{};
        for (std::size_t index = 0; index < grid_cell_count_; ++index) {
            repaint[index] = grid_cells_[index].animated;
        }
        // ponytail: O(n^2) fixpoint over <=48 rects; in practice zero static
        // cells join because glyphs are spaced by construction.
        bool grew = true;
        while (grew) {
            grew = false;
            for (std::size_t index = 0; index < grid_cell_count_; ++index) {
                if (repaint[index]) {
                    continue;
                }
                for (std::size_t other = 0; other < grid_cell_count_; ++other) {
                    // 3 px margin absorbs one frame of animation growth.
                    if (repaint[other] &&
                        bounds_intersect(grid_cells_[other].bounds,
                                         grid_cells_[index].bounds, 3)) {
                        repaint[index] = true;
                        grew = true;
                        break;
                    }
                }
            }
        }
        for (std::size_t index = 0; index < grid_cell_count_; ++index) {
            if (repaint[index] && grid_cells_[index].bounds.valid()) {
                erase_tracked(raster_, grid_cells_[index].bounds);
            }
        }
        raster_.take_recent_bounds();  // flush the erases off the per-cell marker
        // grid_cells_ is stored in paint order (small to large), so repainting
        // a subset reproduces the full-frame layering exactly.
        for (std::size_t index = 0; index < grid_cell_count_; ++index) {
            if (repaint[index]) {
                paint_grid_cell(state, grid_cells_[index]);
                grid_cells_[index].bounds = raster_.take_recent_bounds();
            }
        }
        return;
    }

    const float screen_cx = static_cast<float>(kScreenWidth) * 0.5F;
    const float screen_cy = static_cast<float>(kScreenHeight) * 0.5F;
    const int anchor_row = state.selection.palette / kHexTorusSize;
    const int anchor_col = state.selection.palette % kHexTorusSize;

    // ~28 cells fit inside kHexWorldReach; 48 leaves headroom without growing
    // the device task stack much.
    std::array<GridCellPaint, 48> &cells = grid_cells_;
    std::size_t cell_count = 0;
    bool anchor_seen = false;

    const auto make_cell = [&](int row, int col, bool cull) -> bool {
        const float shift = hex_row_shift(anchor_row + row) - hex_row_shift(anchor_row);
        const float world_x =
            state.grid_offset.x + static_cast<float>(col) * kHexCellPitch + shift;
        const float world_y = state.grid_offset.y + static_cast<float>(row) * kHexRowPitch;
        const float world_distance = std::sqrt(world_x * world_x + world_y * world_y);
        if (cull && world_distance > kHexWorldReach) {
            return false;
        }
        const float screen_distance = fisheye_radius(world_distance);
        const float boost =
            1.0F + (kCenterBoost - 1.0F) *
                       (1.0F - smoothstep01(screen_distance / kCenterBoostRadius));
        const float local_scale = fisheye_scale(world_distance) * boost;
        const float render_scale = local_scale * kHexCellFill * kHexCellPitch / kGlyphSpanPx;
        const float glyph_half = render_scale * kGlyphSpanPx * 0.5F;
        if (cull && screen_distance - glyph_half > kFishEyeRadius) {
            return false;  // fully outside the round viewport
        }
        // F(d)/d -> F'(0) = kFishEyeCenter as d -> 0.
        const float stretch =
            world_distance > 1.0F ? screen_distance / world_distance : kFishEyeCenter;
        GridCellPaint cell;
        cell.x = screen_cx + world_x * stretch;
        cell.y = screen_cy + world_y * stretch;
        cell.scale = render_scale;
        cell.brightness = 1.0F - 0.55F * smoothstep01(screen_distance / kFishEyeRadius);
        cell.palette =
            static_cast<std::uint8_t>(hex_torus_palette(anchor_row + row, anchor_col + col));
        cell.anchor = row == 0 && col == 0;
        cell.animated = cell.anchor || screen_distance < kAnimatedRadius;
        anchor_seen = anchor_seen || cell.anchor;
        cells[cell_count++] = cell;
        return true;
    };

    const int row_first = static_cast<int>(
        std::floor((-state.grid_offset.y - kHexWorldReach) / kHexRowPitch));
    const int row_last = static_cast<int>(
        std::ceil((-state.grid_offset.y + kHexWorldReach) / kHexRowPitch));
    for (int row = row_first; row <= row_last && cell_count + 1U < cells.size(); ++row) {
        const float shift = hex_row_shift(anchor_row + row) - hex_row_shift(anchor_row);
        const int col_first = static_cast<int>(std::floor(
            (-state.grid_offset.x - shift - kHexWorldReach) / kHexCellPitch));
        const int col_last = static_cast<int>(std::ceil(
            (-state.grid_offset.x - shift + kHexWorldReach) / kHexCellPitch));
        for (int col = col_first; col <= col_last && cell_count + 1U < cells.size(); ++col) {
            make_cell(row, col, true);
        }
    }
    if (!anchor_seen) {
        // The hero-blend below needs the anchor even when it was flung off screen.
        make_cell(0, 0, false);
    }

    // Collision relaxation (positions only, matching the reference; see the
    // kRelaxIterations comment). Deterministic — fixed enumeration order and
    // pure float math — so pan-idle frames can reuse the cached positions.
    // ponytail: O(cells^2 * 8) ~ 6k distance checks per full repaint; fine.
    for (int iteration = 0; iteration < kRelaxIterations; ++iteration) {
        bool any_overlap = false;
        for (std::size_t a = 0; a + 1U < cell_count; ++a) {
            GridCellPaint &first = cells[a];
            const float first_radius = first.scale * kGlyphSpanPx * 0.5F;
            for (std::size_t b = a + 1U; b < cell_count; ++b) {
                GridCellPaint &second = cells[b];
                const float limit =
                    first_radius + second.scale * kGlyphSpanPx * 0.5F + kRelaxPadPx;
                const float dx = first.x - second.x;
                const float dy = first.y - second.y;
                const float distance_sq = dx * dx + dy * dy;
                if (distance_sq >= limit * limit) {
                    continue;
                }
                const float distance = std::sqrt(distance_sq);
                const float nx = distance > 1e-6F ? dx / distance : 1.0F;
                const float ny = distance > 1e-6F ? dy / distance : 0.0F;
                const float push = limit - distance;
                const float q =
                    clamp01(0.5F + kRelaxScaleGain * (second.scale - first.scale));
                first.x += nx * push * q;
                first.y += ny * push * q;
                second.x -= nx * push * (1.0F - q);
                second.y -= ny * push * (1.0F - q);
                any_overlap = true;
            }
        }
        if (!any_overlap) {
            break;
        }
    }

    // Paint small (far) cells first so magnified center cells win any overlap.
    std::sort(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(cell_count),
              [](const GridCellPaint &left, const GridCellPaint &right) {
                  return left.scale < right.scale;
              });

    raster_.take_recent_bounds();  // flush the per-cell marker
    for (std::size_t index = 0; index < cell_count; ++index) {
        paint_grid_cell(state, cells[index]);
        cells[index].bounds = raster_.take_recent_bounds();
    }

    grid_cell_count_ = cell_count;
    grid_prev_offset_ = state.grid_offset;
    grid_prev_visibility_ = state.grid_visibility;
    grid_prev_palette_ = state.selection.palette;
    grid_cache_valid_ = true;
}

void EyeRenderer::paint_grid_cell(const FrameState &state, const GridCellPaint &cell)
{
    const float visibility = clamp01(state.grid_visibility);
    if (cell.anchor) {
        // The anchor is the hero pair: blend its hero pose (position, scale,
        // gaze, expression) toward its grid cell as visibility rises, so the
        // hero<->grid handoff at both ends of the fade never jumps.
        const float screen_cx = static_cast<float>(kScreenWidth) * 0.5F;
        const float screen_cy = static_cast<float>(kScreenHeight) * 0.5F;
        const ExpressionPose pose{
            mix(state.expression_pose.openness, 1.0F, visibility),
            mix(state.expression_pose.tilt, 0.0F, visibility),
            mix(state.expression_pose.pupil_scale, 1.0F, visibility),
        };
        render_single(state.selection, mix(screen_cx, cell.x, visibility),
                      mix(screen_cy, cell.y, visibility), mix(1.0F, cell.scale, visibility),
                      state.blink_active ? -1.0F : state.blink_open,
                      {state.gaze.x * (1.0F - visibility),
                       state.gaze.y * (1.0F - visibility)},
                      {state.gaze_pupils.x * (1.0F - visibility),
                       state.gaze_pupils.y * (1.0F - visibility)},
                      mix(1.0F, cell.brightness, visibility),
                      state.face_rotation * (1.0F - visibility),
                      state.poke * (1.0F - visibility), state.blink_clip,
                      state.blink_elapsed_ms, true,
                      cell_time(state.time_ms, cell.palette), pose,
                      state.rot_active ? state.rot_clip : kNoRotClip, state.rot_elapsed_ms,
                      boot_ramp(state.time_ms), &state, state.wink);
        return;
    }
    const float brightness = cell.brightness * visibility;
    if (brightness < 0.04F) {
        return;  // fades in from black, so skipping below threshold is invisible
    }
    // Every torus cell owns a deterministic shape (variety within a screenful).
    const Selection selection{hex_cell_shape(cell.palette), cell.palette};
    if (!cell.animated) {
        // Outer cells render a fixed neutral pose: time-independent pixels, so
        // pan-idle frames can skip them entirely.
        render_single(selection, cell.x, cell.y, cell.scale, 1.0F, Vec2{}, Vec2{}, brightness,
                      0.0F, 0.0F, 0U, 0U, false, 0U, ExpressionPose{}, kNoRotClip, 0U, 1.0F,
                      nullptr, 0U);
        return;
    }
    const std::uint32_t hash = hex_cell_hash(cell.palette);
    // Stateless independent blink: each cell blinks for one clip length out
    // of a hash-chosen 3000-6000 ms period, offset by the hash. Grid cells
    // keep the site's 3-clip pool; the drowsy blink4/blink5 (2 s of closed
    // lids) are hero-only flavor.
    const std::uint8_t blink_clip =
        static_cast<std::uint8_t>((hash >> 4U) % creature::kSiteBlinkClipCount);
    const std::uint32_t blink_period = 3000U + (hash >> 8U) % 3001U;
    const std::uint32_t blink_pos = (state.time_ms + (hash >> 12U)) % blink_period;
    const bool blinking = blink_pos < creature::kBlinkClips[blink_clip].duration_ms;
    render_single(selection, cell.x, cell.y, cell.scale, blinking ? -1.0F : 1.0F, Vec2{}, Vec2{},
                  brightness, 0.0F, 0.0F, blink_clip,
                  static_cast<std::uint16_t>(blinking ? blink_pos : 0U), true,
                  cell_time(state.time_ms, cell.palette),
                  ExpressionPose{}, kNoRotClip, 0U, boot_ramp(state.time_ms), nullptr, 0U);
}
#endif  // CONFIG_LILGUY_WORLD_VIEW

}  // namespace eyes
