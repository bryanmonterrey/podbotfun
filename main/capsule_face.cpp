#include "eyes/capsule_face.hpp"

#include <algorithm>
#include <cmath>

namespace eyes {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kSubstepS = 1.0F / 120.0F;  // fixed spring substep (RE doc)
constexpr float kSubstepMs = 1000.0F / 120.0F;

// Spring constants (RE doc section 3 channel table).
constexpr float kPoseZeta = 1.0F;
constexpr float kBlinkOmega = 26.0F;
constexpr float kBreathOmega = 10.0F;
constexpr float kBreathZeta = 0.8F;
constexpr float kGazeOmega = 13.0F;

constexpr float kScreenCenter = static_cast<float>(kScreenWidth) * 0.5F;
constexpr float kGazeReachPx = 18.0F;

struct PosePool {
    const std::uint8_t *poses;
    std::uint8_t count;
    std::uint16_t dwell_min_ms;
    std::uint16_t dwell_max_ms;
};

// Expression -> pose pools (indices into kCapsulePoses) with per-mood dwell.
constexpr std::uint8_t kPoolContent[] = {0, 10, 17};
constexpr std::uint8_t kPoolCurious[] = {9, 14, 18};
constexpr std::uint8_t kPoolPlayful[] = {17, 2, 9};
constexpr std::uint8_t kPoolHappy[] = {2, 20};
constexpr std::uint8_t kPoolSurprised[] = {3, 21};
constexpr std::uint8_t kPoolSleepy[] = {4, 13, 22};
constexpr std::uint8_t kPoolAnnoyed[] = {7, 16};
constexpr std::uint8_t kPoolShy[] = {24, 6};
constexpr std::uint8_t kPoolListening[] = {10, 12};
constexpr std::uint8_t kPoolThinking[] = {5, 23, 14};

PosePool pool_for(Expression expression)
{
    switch (expression) {
        case Expression::curious:
            return {kPoolCurious, 3, 3000U, 6000U};
        case Expression::playful:
            return {kPoolPlayful, 3, 2500U, 5000U};
        case Expression::happy:
            return {kPoolHappy, 2, 2500U, 5000U};
        case Expression::surprised:
            return {kPoolSurprised, 2, 2000U, 4000U};
        case Expression::sleepy:
            return {kPoolSleepy, 3, 6000U, 10000U};
        case Expression::annoyed:
            return {kPoolAnnoyed, 2, 4000U, 8000U};
        case Expression::shy:
            return {kPoolShy, 2, 4000U, 8000U};
        case Expression::listening:
            return {kPoolListening, 2, 4000U, 8000U};
        case Expression::thinking:
            return {kPoolThinking, 3, 3000U, 6000U};
        case Expression::content:
        default:
            return {kPoolContent, 3, 9000U, 16000U};  // idle dwell 9-16 s
    }
}

float wrap_angle_delta(float delta_deg)
{
    // Capsule at angle a == a + 180: shortest path within +-90 deg.
    return delta_deg - 180.0F * std::round(delta_deg / 180.0F);
}

bool content_mood(Expression expression)
{
    return expression == Expression::content || expression == Expression::playful ||
           expression == Expression::happy;
}

// ---- Qi state choreography ---------------------------------------------------
// Transcribed from docs/grokbot_data/grok_3d_moves.json qi_choreography.states.
// Each head channel is a generic 2-term sinusoid: base + a.amp*sin(a.freq*t +
// a.phase) + b.amp*sin(b.freq*t + b.phase), t = engine seconds. roll is in
// degrees, x/y in design units (kCapsuleScale converts to screen px), squash is
// the unitless breath target. The |sin| hop terms (happy/playful y) are
// transcribed as their first Fourier harmonic:
//   -A*|sin(w*t)| ~= -A*2/pi + A*4/(3*pi)*sin(2*w*t + pi/2).
// Expression -> Qi state mapping: content->idle, curious->curious,
// playful->playful, happy->happy, surprised->surprised (entry recoil kicks),
// sleepy->drowsy base, annoyed->angry, shy->shy, listening->listening,
// thinking->thinking; mode overrides: sleeping->sleeping, dizzy->scared.
struct QiTerm {
    float amp;
    float freq;
    float phase;
};

struct QiChannel {
    float base;
    QiTerm a;
    QiTerm b;
};

struct QiBurst {
    std::uint16_t min_ms;  // 0 => this state has no burst
    std::uint16_t max_ms;
    std::uint16_t duration_ms;
    float x_amp;     // sin(p*pi) window amplitudes (design units / degrees)
    float y_amp;
    float roll_amp;
    float kick_y_v;  // one-shot y spring velocity kick at burst start
    bool tremor;     // roll = sin(ms*.05)*roll_amp instead of the sine window
};

struct QiState {
    QiChannel roll;
    QiChannel x;
    QiChannel y;
    QiChannel squash;
    QiBurst burst;
    // One-shot spring velocity kicks at state entry (surprised recoil: the
    // authored x=-4(1-u)/y=-8(1-u) decays map onto critically damped springs
    // kicked to the same peak: v = peak * omega * e).
    float entry_kick_x_v;
    float entry_kick_y_v;
};

constexpr float kHalfPi = 1.57079632679F;
constexpr float kHopBase = -1.90986F;  // -3*2/pi
constexpr float kHopAmp = 1.27324F;    // 3*4/(3*pi)

// Indexed by Expression (content..thinking); see mapping comment above.
constexpr QiState kQiStates[10] = {
    // content -> idle
    {{0.0F, {1.5F, 0.5F, 0.0F}, {0.6F, 0.17F, 0.0F}},
     {0.0F, {1.0F, 0.27F, 0.0F}, {}},
     {0.0F, {1.2F, 0.85F, 0.0F}, {}},
     {1.0F, {0.007F, 0.85F, 0.0F}, {}},
     {}, 0.0F, 0.0F},
    // curious (tilt-bounce burst every 1.6-2.8 s: 440 ms, x+8, roll+5)
    {{10.0F, {6.0F, 0.7F, 0.0F}, {}},
     {0.0F, {5.0F, 0.6F, 0.0F}, {}},
     {-2.0F, {1.5F, 0.9F, 0.0F}, {}},
     {1.01F, {}, {}},
     {1600U, 2800U, 440U, 8.0F, 0.0F, 5.0F, 0.0F, false}, 0.0F, 0.0F},
    // playful (y = -|sin(2.2t)|*3 first harmonic)
    {{0.0F, {8.0F, 1.4F, 0.0F}, {}},
     {0.0F, {4.0F, 1.1F, 0.0F}, {}},
     {kHopBase, {kHopAmp, 4.4F, kHalfPi}, {}},
     {1.0F, {0.015F, 2.2F, 0.0F}, {}},
     {}, 0.0F, 0.0F},
    // happy (y = -|sin(2.4t)|*3 first harmonic)
    {{0.0F, {3.0F, 1.2F, 0.0F}, {}},
     {0.0F, {2.5F, 1.1F, 0.0F}, {}},
     {kHopBase, {kHopAmp, 4.8F, kHalfPi}, {}},
     {1.0F, {0.02F, 2.4F, 0.0F}, {}},
     {}, 0.0F, 0.0F},
    // surprised (recoil kicks: peaks x -4 / y -8 through omega 3.5/4 springs)
    {{0.0F, {}, {}}, {0.0F, {}, {}}, {0.0F, {}, {}}, {1.0F, {}, {}},
     {}, -38.0F, -87.0F},
    // sleepy -> drowsy base (nod-off drives the lid separately)
    {{0.0F, {2.5F, 0.32F, 0.0F}, {}},
     {0.0F, {1.5F, 0.2F, 0.0F}, {}},
     {6.0F, {2.2F, 0.36F, 0.0F}, {}},
     {1.0F, {0.022F, 0.36F, 0.0F}, {}},
     {}, 0.0F, 0.0F},
    // annoyed -> angry (shake burst every 1.8-3.2 s: y.v += 70 + 420 ms tremor)
    {{0.0F, {}, {}}, {0.0F, {}, {}}, {3.5F, {}, {}}, {0.975F, {}, {}},
     {1800U, 3200U, 420U, 0.0F, 0.0F, 4.5F, 70.0F, true}, 0.0F, 0.0F},
    // shy
    {{-8.0F, {3.0F, 0.5F, 0.0F}, {}},
     {-3.0F, {2.0F, 0.4F, 0.0F}, {}},
     {3.0F, {}, {}},
     {0.98F, {}, {}},
     {}, 0.0F, 0.0F},
    // listening (nod burst every 1.8-3.2 s: 380 ms, y+4.5, roll+2)
    {{8.0F, {1.5F, 0.5F, 0.0F}, {}},
     {2.0F, {}, {}},
     {-2.0F, {0.8F, 0.8F, 0.0F}, {}},
     {1.015F, {}, {}},
     {1800U, 3200U, 380U, 0.0F, 4.5F, 2.0F, 0.0F, false}, 0.0F, 0.0F},
    // thinking
    {{-9.0F, {5.0F, 0.35F, 0.0F}, {}},
     {0.0F, {5.0F, 0.3F, 0.0F}, {}},
     {0.0F, {2.5F, 0.6F, 0.0F}, {}},
     {1.0F, {}, {}},
     {}, 0.0F, 0.0F},
};

// Mode overrides. sleeping bases ramp in with u = min(ts/2, 1) (Grok entry
// ramp; ts = seconds since mode entry) — the asleep analogue of the boot snap.
constexpr QiState kQiSleeping = {
    {4.0F, {2.0F, 0.25F, 0.0F}, {}},
    {-2.0F, {}, {}},
    {8.0F, {3.0F, 0.55F, 0.0F}, {}},
    {1.0F, {0.016F, 0.55F, 0.0F}, {}},
    {}, 0.0F, 0.0F};

// dizzy -> scared tremble (40-50 rad/s; the omega-5/3.5 head springs attenuate
// it just like the reference's channels do).
constexpr QiState kQiScared = {
    {0.0F, {2.0F, 40.0F, 0.0F}, {}},
    {-2.0F, {1.5F, 50.0F, 0.0F}, {}},
    {2.0F, {1.0F, 1.5F, 0.0F}, {}},
    {0.97F, {}, {}},
    {}, 0.0F, 0.0F};

const QiState &qi_for(const FrameState &state)
{
    if (state.mode == InteractionMode::sleeping) {
        return kQiSleeping;
    }
    if (state.mode == InteractionMode::dizzy) {
        return kQiScared;
    }
    return kQiStates[static_cast<std::size_t>(state.expression)];
}

float qi_channel(const QiChannel &channel, float time_s)
{
    return channel.base +
           channel.a.amp * std::sin(channel.a.freq * time_s + channel.a.phase) +
           channel.b.amp * std::sin(channel.b.freq * time_s + channel.b.phase);
}

// ---- spinWild (grok_3d_moves.json flourishes.spinWild) ------------------------
// Belt angle eo(t) through backswing/accel/cruise/quartic-decel, landing on
// exactly 9*2pi at t=3.79 s; visual face rotation = eo/3 (3 full turns).
// Wobble envelope rises over the back 60% of the decel, decays (1-t/1.7)^1.6
// over the 1.7 s tail; drives roll/x/y wobble, gaze darts, lid and eye size.
struct SpinEval {
    float offset_rad{0.0F};
    float roll_deg{0.0F};
    float x{0.0F};
    float y{0.0F};
    float gaze_x{0.0F};
    float gaze_y{0.0F};
    float lid{1.0F};
    float size{1.0F};
};

SpinEval eval_spin_wild(float t, float dir)
{
    constexpr float kTurnsRad = 9.0F * 2.0F * kPi;
    constexpr float kCruiseV = (kTurnsRad + 0.5F) / 2.4625F;  // ~23.167 rad/s
    float eo;
    if (t < 0.24F) {
        eo = -0.5F * (1.0F - std::cos(t / 0.24F * kPi)) * 0.5F;
    } else if (t < 0.54F) {
        const float a = t - 0.24F;
        eo = -0.5F + kCruiseV * a * a / (2.0F * 0.3F);
    } else if (t < 2.54F) {
        eo = -0.5F + kCruiseV * (0.15F + (t - 0.54F));
    } else if (t < 3.79F) {
        const float inv = 1.0F - (t - 2.54F) / 1.25F;
        eo = -0.5F + kCruiseV * 2.15F +
             kCruiseV * 1.25F * (1.0F - inv * inv * inv * inv) * 0.25F;
    } else {
        eo = kTurnsRad;  // exact: offset lands on 3 full visual turns
    }
    float envelope = 0.0F;
    if (t >= 3.79F) {
        envelope = std::pow(std::max(1.0F - (t - 3.79F) / 1.7F, 0.0F), 1.6F);
    } else if (t >= 2.54F) {
        const float progress = (t - 2.54F) / 1.25F;
        if (progress > 0.4F) {
            const float rise = (progress - 0.4F) / 0.6F;
            envelope = rise * rise;
        }
    }
    const float d = std::max(t - 2.54F, 0.0F);
    SpinEval out;
    out.offset_rad = eo / 3.0F * dir;
    out.roll_deg = std::sin(d * 9.2F) * 11.0F * dir * envelope;
    out.x = (std::cos(d * 9.2F) - 1.0F) * 6.0F * dir * envelope;
    out.y = std::sin(d * 18.4F) * 2.6F * envelope;
    out.gaze_x = std::sin(d * 11.5F) * 13.0F * dir * envelope;
    out.gaze_y = (std::cos(d * 9.0F) - 1.0F) * 3.5F * envelope;
    out.lid = 1.14F - 0.44F * envelope + 0.1F * std::sin(d * 16.0F) * envelope;
    out.size = 1.12F - 0.09F * envelope;
    return out;
}

}  // namespace

// Verbatim 25-pose table from docs/GROKBOT_FACE_RE.md ("Eyes: 25 pose variants").
const std::array<CapsulePose, kCapsulePoseCount> kCapsulePoses{{
    {{22.4F, -47.5F, 63.0F, 22.6F, 10.6F}, {71.4F, -57.1F, 63.0F, 22.9F, 7.4F}},    // 0 neutral up-right
    {{-23.3F, 39.5F, 78.0F, 29.4F, 12.6F}, {36.4F, 27.9F, 80.0F, 29.5F, 12.6F}},    // 1 down-center
    {{-16.8F, 33.4F, 104.0F, 44.4F, 19.6F}, {50.9F, 44.8F, 107.0F, 44.6F, 18.1F}},  // 2 huge happy
    {{-68.4F, 16.9F, 76.0F, 28.2F, 21.8F}, {0.5F, -2.4F, 19.0F, 28.2F, 28.2F}},     // 3 wide surprised
    {{-1.5F, -4.1F, 11.0F, 28.2F, 6.9F}, {60.2F, 8.8F, 13.0F, 23.8F, 6.9F}},        // 4 flat squint
    {{-28.6F, 46.0F, 75.0F, 30.5F, 13.6F}, {34.2F, 34.6F, 162.0F, 26.5F, 6.3F}},    // 5 uneven suspicious
    {{4.6F, 10.2F, 101.0F, 19.3F, 11.5F}, {57.4F, 18.9F, 102.0F, 19.3F, 10.0F}},    // 6 small
    {{4.7F, -15.5F, 50.0F, 31.0F, 11.5F}, {58.7F, -21.9F, 105.0F, 28.9F, 11.0F}},   // 7 angry slant
    {{-61.7F, -7.4F, 125.0F, 26.9F, 9.7F}, {-6.5F, 6.4F, 74.0F, 28.2F, 10.3F}},     // 8 look left
    {{-5.5F, 43.5F, 8.0F, 23.8F, 21.9F}, {60.4F, 26.7F, 114.0F, 12.5F, 10.2F}},     // 9 big+small
    {{-14.4F, -5.6F, 103.0F, 31.0F, 12.6F}, {44.3F, 8.0F, 103.0F, 31.0F, 12.1F}},   // 10 attentive
    {{-61.1F, 23.0F, 73.0F, 47.0F, 17.6F}, {3.9F, 4.9F, 73.0F, 47.0F, 19.4F}},      // 11 huge left
    {{-11.7F, 41.4F, 16.0F, 28.3F, 26.0F}, {59.5F, 51.7F, 131.0F, 28.3F, 19.9F}},   // 12 round low
    {{-46.3F, 26.4F, 173.0F, 25.3F, 7.1F}, {17.3F, 14.4F, 167.0F, 28.1F, 7.0F}},    // 13 closed (sleep)
    {{-36.7F, -16.6F, 102.0F, 32.5F, 13.4F}, {25.9F, -4.5F, 13.0F, 26.5F, 6.4F}},   // 14 confused uneven
    {{56.3F, -9.5F, 80.0F, 19.4F, 10.1F}, {95.5F, -17.1F, 80.0F, 19.3F, 6.3F}},     // 15 look hard right
    {{-69.8F, 20.6F, 75.0F, 31.0F, 9.6F}, {-17.3F, 36.1F, 132.0F, 29.2F, 11.6F}},   // 16 angry left
    {{-4.8F, 0.8F, 106.0F, 28.2F, 10.3F}, {52.2F, -11.9F, 54.0F, 27.7F, 9.9F}},     // 17 playful
    {{-23.3F, -14.4F, 122.0F, 23.8F, 23.1F}, {43.8F, 0.4F, 90.0F, 12.6F, 11.6F}},   // 18 big+tiny
    {{-51.1F, 27.9F, 76.0F, 30.7F, 11.6F}, {7.1F, 17.4F, 77.0F, 30.6F, 12.6F}},     // 19 down-left
    {{-8.1F, 47.2F, 107.0F, 42.4F, 19.6F}, {58.1F, 54.7F, 110.0F, 43.4F, 17.0F}},   // 20 huge low
    {{-59.2F, 25.1F, 67.0F, 28.2F, 23.0F}, {12.4F, 9.1F, 127.0F, 28.3F, 27.9F}},    // 21 round wide
    {{-22.8F, -25.4F, 8.0F, 27.5F, 7.0F}, {41.4F, -12.2F, 15.0F, 26.6F, 7.0F}},     // 22 squint up
    {{-4.9F, 33.0F, 80.0F, 31.3F, 13.8F}, {56.3F, 20.1F, 161.0F, 24.3F, 6.4F}},     // 23 skeptical
    {{-15.8F, 25.9F, 97.0F, 18.7F, 11.5F}, {39.4F, 31.6F, 101.0F, 18.8F, 11.0F}},   // 24 small shy
}};

std::array<Vec2, 12> capsule_path(float cx, float cy, float angle_rad, float half_len,
                                  float half_w)
{
    const float w = std::min(std::max(half_w, 0.5F), std::max(half_len, 0.5F));
    const float body = std::max(half_len, 0.5F) - w;  // straight half-length
    // Single-cubic semicircle: control distance (4/3)w hits the apex exactly
    // but bulges 1.8% diagonally; 0.9875 equioscillates the error to +-1.3%.
    const float k = 1.3333333F * 0.9875F * w;
    const float cosine = std::cos(angle_rad);
    const float sine = std::sin(angle_rad);
    const auto point = [&](float ax, float ay) {
        return Vec2{cx + ax * cosine - ay * sine, cy + ax * sine + ay * cosine};
    };
    const float third = body * (2.0F / 3.0F);  // straight-edge cubic controls
    return {{
        point(-body, w),                // start of top edge
        point(-body + third, w),        // edge control 1
        point(body - third, w),         // edge control 2
        point(body, w),                 // right cap start
        point(body + k, w),             // cap control 1
        point(body + k, -w),            // cap control 2
        point(body, -w),                // bottom edge start
        point(body - third, -w),        // edge control 1
        point(-body + third, -w),       // edge control 2
        point(-body, -w),               // left cap start
        point(-body - k, -w),           // cap control 1
        point(-body - k, w),            // cap control 2 (closes back to start)
    }};
}

CapsuleFace::CapsuleFace(std::uint32_t seed) { reset(seed); }

void CapsuleFace::reset(std::uint32_t seed)
{
    rng_state_ = seed == 0U ? 0x43415053U : seed;
    for (auto &eye_channels : eyes_) {
        for (Spring &spring : eye_channels) {
            spring = {};
        }
    }
    blink_ = {1.0F, 0.0F};
    breath_ = {1.0F, 0.0F};
    gaze_[0] = {};
    gaze_[1] = {};
    face_roll_ = {};
    face_x_ = {};
    face_y_ = {};
    wave_ = {};
    quantized_ = {};
    last_rendered_ = {std::int16_t(-32768)};  // force a first frame
    last_ms_ = 0U;
    substep_carry_ms_ = 0.0F;
    last_poke_ = 0.0F;
    last_expression_ = Expression::content;
    last_mode_ = InteractionMode::idle;
    pose_index_ = 0;
    pose_omega_ = 6.0F;
    initialized_ = false;
    next_pose_ms_ = 0U;
    next_blink_ms_ = 0U;
    next_wink_ms_ = 0U;
    wink_start_ms_ = 0U;
    wink_eye_ = 0;
    wink_active_ = false;
    blink_key_count_ = 0U;
    spin_active_ = false;
    spin_dir_ = 1.0F;
    spin_offset_rad_ = 0.0F;
    spin_start_ms_ = 0U;
    last_poke_count_ = 0U;
    last_poke_event_ms_ = 0U;
    burst_active_ = false;
    burst_start_ms_ = 0U;
    next_burst_ms_ = 0U;
    state_entry_ms_ = 0U;
    nod_active_ = false;
    nod_start_ms_ = 0U;
    next_nod_ms_ = 0U;
}

std::uint32_t CapsuleFace::random_u32()
{
    std::uint32_t value = rng_state_;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    rng_state_ = value;
    return value;
}

std::uint32_t CapsuleFace::random_range(std::uint32_t minimum, std::uint32_t maximum)
{
    return minimum + random_u32() % (maximum - minimum + 1U);
}

void CapsuleFace::step_spring(Spring &spring, float target, float omega, float zeta, float h)
{
    spring.v += (-2.0F * zeta * omega * spring.v - omega * omega * (spring.x - target)) * h;
    spring.x += spring.v * h;
}

void CapsuleFace::retarget_pose(int pose, bool entry)
{
    pose_index_ = pose;
    pose_omega_ = entry ? 8.0F : 6.0F;  // Grok: entry morph omega 8-10, cycle 6
}

void CapsuleFace::select_pose(const FrameState &state, bool entry)
{
    int pose;
    if (state.mode == InteractionMode::sleeping) {
        pose = 13;  // closed
    } else if (state.mode == InteractionMode::dizzy) {
        pose = 3;  // wide surprised
    } else {
        const PosePool pool = pool_for(state.expression);
        std::uint32_t index = random_range(0U, pool.count - 1U);
        if (pool.count > 1U && pool.poses[index] == pose_index_) {
            index = (index + 1U) % pool.count;  // random non-repeating member
        }
        pose = pool.poses[index];
        next_pose_ms_ = state.time_ms + random_range(pool.dwell_min_ms, pool.dwell_max_ms);
    }
    retarget_pose(pose, entry);
    // Blink on STATE entry only (expression/mode change), never on intra-mood
    // pose swaps — those fire every few seconds in lively moods and blinking
    // on each one reads as a tic. A 2.5s refractory guard covers rapid state
    // churn (e.g. touch bursts).
    if (entry && state.mode != InteractionMode::sleeping &&
        state.expression != Expression::sleepy &&
        state.time_ms - last_blink_start_ms_ > 2500U) {
        start_blink(state.time_ms);
    }
}

void CapsuleFace::start_blink(std::uint32_t now_ms)
{
    last_blink_start_ms_ = now_ms;
    // Grok pr(t): .05 @0, hold @70 ms, 1.08 overshoot @150 ms, then settle to
    // 1.0; 14% double blink appends .05 @370 ms, 1.0 @480 ms. The settle key
    // sits at 340 ms (doc says 300) so the omega=26 spring actually expresses
    // the 1.08 overshoot (peak ~1.036) instead of stalling at ~1.004.
    const float t0 = static_cast<float>(now_ms);
    blink_key_ms_ = {t0, t0 + 70.0F, t0 + 150.0F, t0 + 340.0F, 0.0F, 0.0F};
    blink_key_value_ = {0.05F, 0.05F, 1.08F, 1.0F, 0.0F, 0.0F};
    blink_key_count_ = 4U;
    if (random_range(0U, 99U) < 14U) {
        blink_key_ms_[4] = t0 + 370.0F;
        blink_key_value_[4] = 0.05F;
        blink_key_ms_[5] = t0 + 480.0F;
        blink_key_value_[5] = 1.0F;
        blink_key_count_ = 6U;
    }
    const BlinkRange range = blink_range_for(last_expression_);
    next_blink_ms_ = now_ms + random_range(range.min_ms, range.max_ms);
}

float CapsuleFace::blink_target(float time_ms, const FrameState &state) const
{
    if (blink_key_count_ > 0U && time_ms <= blink_key_ms_[blink_key_count_ - 1U] + 400.0F) {
        float value = 1.0F;
        for (std::size_t key = 0; key < blink_key_count_; ++key) {
            if (time_ms >= blink_key_ms_[key]) {
                value = blink_key_value_[key];
            }
        }
        return value;
    }
    if (state.mode == InteractionMode::sleeping) {
        return 1.0F;  // pose 13 is authored closed; lids stay "open" on the slits
    }
    if (spin_active_) {
        // spinWild lid coupling: wide 1.14 through the spin, dipping with the
        // wobble envelope on the tail.
        return eval_spin_wild((time_ms - static_cast<float>(spin_start_ms_)) * 0.001F,
                              spin_dir_)
            .lid;
    }
    if (state.expression == Expression::sleepy) {
        // Drowsy nod-off, raw pre-floor curve (grok_3d_moves.json
        // qi_choreography.drowsy_nod_off — the reference's >=.8 lid floor is
        // deliberately skipped because our lids are real, not shape-baked):
        //   A 1.7 s droop: lid = .34 - .30*p^2 (-> .04)
        //   B 0.3 s bottom bounce: lid = .04 + sin(p*pi)*.42 (peek .46)
        //   C 1.5 s recover: lid = .46 - .12*(1-(1-p)^2.2), micro-sleep .05
        //     dip at p in (.32,.46)
        // Between nods the base lid breathes .34 +- .07 at 0.8 rad/s.
        if (nod_active_) {
            const float phase_ms = time_ms - static_cast<float>(nod_start_ms_);
            if (phase_ms < 1700.0F) {
                const float p = phase_ms / 1700.0F;
                return 0.34F - 0.30F * p * p;
            }
            if (phase_ms < 2000.0F) {
                const float p = (phase_ms - 1700.0F) / 300.0F;
                return 0.04F + std::sin(p * kPi) * 0.42F;
            }
            const float p = std::min((phase_ms - 2000.0F) / 1500.0F, 1.0F);
            if (p > 0.32F && p < 0.46F) {
                return 0.05F;  // micro-sleep re-droop dip
            }
            const float ease = 1.0F - std::pow(1.0F - p, 2.2F);
            return 0.46F - 0.12F * ease;
        }
        return 0.34F + std::sin(0.8F * time_ms * 0.001F) * 0.07F;
    }
    return 1.0F;
}

void CapsuleFace::celebrate(std::uint32_t now_ms)
{
    if (spin_active_) {
        return;
    }
    spin_active_ = true;
    spin_start_ms_ = now_ms;
    spin_dir_ = (random_u32() & 1U) != 0U ? 1.0F : -1.0F;
}

void CapsuleFace::update(const FrameState &state)
{
    const bool expression_changed = state.expression != last_expression_;
    const bool mode_changed = state.mode != last_mode_;
    if (!initialized_) {
        initialized_ = true;
        last_ms_ = state.time_ms;
        last_expression_ = state.expression;
        last_mode_ = state.mode;
        last_poke_ = state.poke;
        select_pose(state, true);
        // Snap springs onto the initial pose so boot shows a settled face.
        const CapsulePose &pose = kCapsulePoses[static_cast<std::size_t>(pose_index_)];
        for (int eye = 0; eye < 2; ++eye) {
            const CapsuleEye &source = eye == 0 ? pose.left : pose.right;
            eyes_[eye][0].x = source.cx;
            eyes_[eye][1].x = source.cy;
            eyes_[eye][2].x = source.angle_deg;
            eyes_[eye][3].x = source.half_len;
            eyes_[eye][4].x = source.half_w;
        }
        blink_key_count_ = 0U;
        blink_ = {1.0F, 0.0F};
        const BlinkRange range = blink_range_for(state.expression);
        next_blink_ms_ = state.time_ms + random_range(range.min_ms, range.max_ms);
        next_wink_ms_ = state.time_ms + random_range(25000U, 60000U);
        state_entry_ms_ = state.time_ms;
        last_poke_count_ = state.poke_count;
        // Boot snap: face springs start exactly on the Qi targets so the first
        // frame shows a settled face (no jump; sinusoid phases start at t).
        const QiState &boot_qi = qi_for(state);
        const float boot_s = static_cast<float>(state.time_ms) * 0.001F;
        face_roll_.x = qi_channel(boot_qi.roll, boot_s);
        face_x_.x = qi_channel(boot_qi.x, boot_s);
        face_y_.x = qi_channel(boot_qi.y, boot_s);
        breath_.x = qi_channel(boot_qi.squash, boot_s);
        if (boot_qi.burst.min_ms != 0U) {
            next_burst_ms_ =
                state.time_ms + random_range(boot_qi.burst.min_ms, boot_qi.burst.max_ms);
        }
        if (state.expression == Expression::sleepy) {
            next_nod_ms_ = state.time_ms + random_range(1200U, 2200U);
        }
        refresh_outputs(state);
        return;
    }

    if (expression_changed || mode_changed) {
        last_expression_ = state.expression;
        last_mode_ = state.mode;
        state_entry_ms_ = state.time_ms;
        const QiState &entry_qi = qi_for(state);
        face_x_.v += entry_qi.entry_kick_x_v;  // surprised recoil
        face_y_.v += entry_qi.entry_kick_y_v;
        burst_active_ = false;
        if (entry_qi.burst.min_ms != 0U) {
            next_burst_ms_ =
                state.time_ms + random_range(entry_qi.burst.min_ms, entry_qi.burst.max_ms);
        }
        if (expression_changed && state.expression == Expression::sleepy) {
            nod_active_ = false;
            next_nod_ms_ = state.time_ms + random_range(1200U, 2200U);
        }
        // Rare celebrate on entering the party moods (Grok flourish flavor).
        if (expression_changed && state.mode != InteractionMode::sleeping &&
            (state.expression == Expression::happy ||
             state.expression == Expression::playful) &&
            random_range(0U, 99U) < 10U) {
            celebrate(state.time_ms);
        }
        select_pose(state, true);
    }
    const bool asleep = state.mode == InteractionMode::sleeping;
    // Poke double-tap (two pokes within 600 ms) -> spinWild celebrate.
    if (state.poke_count != last_poke_count_) {
        if (!asleep && state.time_ms - last_poke_event_ms_ <= 600U &&
            last_poke_event_ms_ != 0U) {
            celebrate(state.time_ms);
        }
        last_poke_count_ = state.poke_count;
        last_poke_event_ms_ = state.time_ms;
    }
    if (spin_active_ && state.time_ms - spin_start_ms_ >= kSpinWildDurationMs) {
        spin_active_ = false;
    }
    const auto reached = [&](std::uint32_t deadline) {
        return static_cast<std::int32_t>(state.time_ms - deadline) >= 0;
    };
    if (!asleep && state.mode != InteractionMode::dizzy && reached(next_pose_ms_)) {
        select_pose(state, false);
    }
    if (!asleep && state.expression != Expression::sleepy && reached(next_blink_ms_)) {
        start_blink(state.time_ms);
    }
    if (reached(next_wink_ms_)) {
        if (!asleep && content_mood(state.expression) && blink_key_count_ == 0U) {
            wink_active_ = true;
            wink_start_ms_ = state.time_ms;
            wink_eye_ = static_cast<int>(random_u32() & 1U);
        }
        next_wink_ms_ = state.time_ms + random_range(25000U, 60000U);
    }
    if (wink_active_ && state.time_ms - wink_start_ms_ >= 320U) {
        wink_active_ = false;
    }
    // Drowsy nod-off scheduling: first nod entry+1.2-2.2 s, then 1.5-3.5 s
    // after each recovery (droop 1.7 s + bounce 0.3 s + recover 1.5 s).
    if (state.expression == Expression::sleepy && !asleep) {
        if (nod_active_ && state.time_ms - nod_start_ms_ >= 3500U) {
            nod_active_ = false;
            next_nod_ms_ = state.time_ms + random_range(1500U, 3500U);
        }
        if (!nod_active_ && reached(next_nod_ms_)) {
            nod_active_ = true;
            nod_start_ms_ = state.time_ms;
        }
    } else {
        nod_active_ = false;
    }

    // Poke: spring velocity kick on halfLen/halfW (bounce).
    if (state.poke > 0.02F && last_poke_ <= 0.02F) {
        for (auto &eye_channels : eyes_) {
            eye_channels[3].v += 20.0F;
            eye_channels[4].v += 30.0F;
        }
    }
    last_poke_ = state.poke;

    // Spring targets for this frame.
    const CapsulePose &pose = kCapsulePoses[static_cast<std::size_t>(pose_index_)];
    float targets[2][5];
    for (int eye = 0; eye < 2; ++eye) {
        const CapsuleEye &source = eye == 0 ? pose.left : pose.right;
        targets[eye][0] = source.cx;
        targets[eye][1] = source.cy;
        targets[eye][2] = eyes_[eye][2].x + wrap_angle_delta(source.angle_deg - eyes_[eye][2].x);
        targets[eye][3] = source.half_len;
        targets[eye][4] = source.half_w;
    }
    // Qi head targets (roll deg, x/y design units, squash) from the compiled
    // per-state tables, plus timed bursts and velocity kicks.
    const float time_s = static_cast<float>(state.time_ms) * 0.001F;
    const QiState &qi = qi_for(state);
    float qi_roll = qi_channel(qi.roll, time_s);
    float qi_x = qi_channel(qi.x, time_s);
    float qi_y = qi_channel(qi.y, time_s);
    const float breath_target = qi_channel(qi.squash, time_s);
    if (asleep) {
        // Sleeping entry ramp: bases fade in with u = min(ts/2, 1).
        const float u =
            std::min(static_cast<float>(state.time_ms - state_entry_ms_) * 0.0005F, 1.0F);
        qi_roll += (u - 1.0F) * qi.roll.base;
        qi_x += (u - 1.0F) * qi.x.base;
        qi_y += (u - 1.0F) * qi.y.base;
    }
    if (qi.burst.min_ms != 0U && !asleep) {
        if (!burst_active_ && reached(next_burst_ms_)) {
            burst_active_ = true;
            burst_start_ms_ = state.time_ms;
            face_y_.v += qi.burst.kick_y_v;  // angry shake velocity kick
            next_burst_ms_ = state.time_ms + random_range(qi.burst.min_ms, qi.burst.max_ms);
        }
        if (burst_active_) {
            const std::uint32_t elapsed = state.time_ms - burst_start_ms_;
            if (elapsed >= qi.burst.duration_ms) {
                burst_active_ = false;
            } else {
                const float window = std::sin(static_cast<float>(elapsed) /
                                              static_cast<float>(qi.burst.duration_ms) * kPi);
                qi_x += qi.burst.x_amp * window;
                qi_y += qi.burst.y_amp * window;
                qi_roll += qi.burst.tremor
                               ? std::sin(static_cast<float>(state.time_ms) * 0.05F) *
                                     qi.burst.roll_amp
                               : qi.burst.roll_amp * window;
            }
        }
    } else {
        burst_active_ = false;
    }
    const float wave_target =
        (state.attention == AttentionSource::sound && !asleep) ? 1.0F : 0.0F;
    float gaze_target[2] = {0.0F, 0.0F};
    const float gaze_length = std::sqrt(state.gaze_pupils.x * state.gaze_pupils.x +
                                        state.gaze_pupils.y * state.gaze_pupils.y);
    if (gaze_length > 1e-4F && !asleep) {
        const float reach = kGazeReachPx * std::sqrt(std::min(gaze_length, 1.0F));
        gaze_target[0] = state.gaze_pupils.x / gaze_length * reach;
        gaze_target[1] = state.gaze_pupils.y / gaze_length * reach;
    }

    // Fixed 1/120 s substeps, frame dt clamped to 100 ms (RE doc physics core).
    const std::uint32_t delta_ms = state.time_ms - last_ms_;
    last_ms_ = state.time_ms;
    substep_carry_ms_ += static_cast<float>(std::min<std::uint32_t>(delta_ms, 100U));
    float sub_time_ms = static_cast<float>(state.time_ms) - substep_carry_ms_;
    while (substep_carry_ms_ >= kSubstepMs) {
        substep_carry_ms_ -= kSubstepMs;
        sub_time_ms += kSubstepMs;
        for (int eye = 0; eye < 2; ++eye) {
            for (int channel = 0; channel < 5; ++channel) {
                step_spring(eyes_[eye][channel], targets[eye][channel], pose_omega_, kPoseZeta,
                            kSubstepS);
            }
        }
        step_spring(blink_, blink_target(sub_time_ms, state), kBlinkOmega, 1.0F, kSubstepS);
        step_spring(breath_, breath_target, kBreathOmega, kBreathZeta, kSubstepS);
        step_spring(gaze_[0], gaze_target[0], kGazeOmega, 1.0F, kSubstepS);
        step_spring(gaze_[1], gaze_target[1], kGazeOmega, 1.0F, kSubstepS);
        // Grok head springs: roll omega 5 zeta .9, x omega 3.5, y omega 4.
        step_spring(face_roll_, qi_roll, 5.0F, 0.9F, kSubstepS);
        step_spring(face_x_, qi_x, 3.5F, 1.0F, kSubstepS);
        step_spring(face_y_, qi_y, 4.0F, 1.0F, kSubstepS);
        step_spring(wave_, wave_target, 10.0F, 1.0F, kSubstepS);
    }
    if (blink_key_count_ > 0U &&
        static_cast<float>(state.time_ms) > blink_key_ms_[blink_key_count_ - 1U] + 400.0F) {
        blink_key_count_ = 0U;  // envelope finished; target falls back to droop
    }

    refresh_outputs(state);
}

void CapsuleFace::refresh_outputs(const FrameState &state)
{
    const float time_s = static_cast<float>(state.time_ms) * 0.001F;
    const bool asleep = state.mode == InteractionMode::sleeping;
    SpinEval spin{};
    if (spin_active_) {
        spin = eval_spin_wild(static_cast<float>(state.time_ms - spin_start_ms_) * 0.001F,
                              spin_dir_);
    }
    spin_offset_rad_ = spin_active_ ? spin.offset_rad : 0.0F;
    // Face rotation = persisted user orientation + Qi head roll (spring) +
    // spinWild visual turns + spinWild wobble roll — the transient parts add
    // on top and return to zero, never touching state.face_rotation itself.
    const float rotation = state.face_rotation + spin_offset_rad_ +
                           (face_roll_.x + spin.roll_deg) * (kPi / 180.0F);
    const float cosine = std::cos(rotation);
    const float sine = std::sin(rotation);
    const float breath = breath_.x;
    // Engine gaze is in the anchored face frame; rotate back to screen space.
    // spinWild gaze darts (design units) add in screen space.
    const float gaze_screen_x =
        gaze_[0].x * cosine - gaze_[1].x * sine + spin.gaze_x * kCapsuleScale;
    const float gaze_screen_y =
        gaze_[0].x * sine + gaze_[1].x * cosine + spin.gaze_y * kCapsuleScale;
    // Whole-face sway: the Qi head x/y springs (design units -> px), plus the
    // spinWild wobble. Sleeping uses the authored Qi sleeping table (with its
    // entry u-ramp) instead of the old hand-scaled sway.
    const float sway_x = (face_x_.x + spin.x) * kCapsuleScale;
    const float sway_y = (face_y_.x + spin.y) * kCapsuleScale;
    // Wave-bar "talking" pulse (grok_overlays.json wave): synthesized voice
    // level Li(t) with per-eye detune, gated by the sound-attention spring.
    const float wave_gain = std::clamp(wave_.x, 0.0F, 1.0F);
    float wave_level[2] = {0.0F, 0.0F};
    if (wave_gain > 0.001F) {
        const float t_ms = static_cast<float>(state.time_ms);
        const float li = 0.42F + 0.29F * std::sin(0.0021F * t_ms) * std::sin(0.0034F * t_ms) +
                         0.29F * std::sin(0.0013F * t_ms + 1.7F);
        for (int eye = 0; eye < 2; ++eye) {
            const float detune_phase = eye == 0 ? 1.05F : 2.10F;  // |off|*1.05, off 1/2
            wave_level[eye] =
                li * (0.55F + 0.45F * std::sin(0.012F * t_ms - detune_phase)) * wave_gain;
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        // Per-eye micro-drift (RE doc section 5, design units * kCapsuleScale):
        // x = sin(.42t+i)*1.4 + sin(1.0t+2i)*.5, y = sin(.58t+i)*.9, i = eye.
        const float phase = static_cast<float>(eye);
        const float drift_scale = asleep ? 0.0F : kCapsuleScale;  // sleep: sway only
        const float drift_x = sway_x +
                              drift_scale * (std::sin(0.42F * time_s + phase) * 1.4F +
                                             std::sin(1.0F * time_s + 2.0F * phase) * 0.5F);
        const float drift_y = sway_y + drift_scale * std::sin(0.58F * time_s + phase) * 0.9F;

        float openness = blink_.x;
        if (wink_active_ && eye == wink_eye_) {
            // 320 ms triangle: close over 42%, reopen over 58% (content moods).
            const float progress =
                static_cast<float>(state.time_ms - wink_start_ms_) / 320.0F;
            const float triangle = progress < 0.42F
                                       ? 1.0F - (progress / 0.42F) * 0.92F
                                       : 0.08F + ((progress - 0.42F) / 0.58F) * 0.92F;
            openness *= std::clamp(triangle, 0.08F, 1.0F);
        }
        openness = std::clamp(openness, 0.04F, 1.25F);

        const float design_x = eyes_[eye][0].x * breath;
        const float design_y = eyes_[eye][1].x * breath;
        const float wave_px = 9.0F * wave_level[eye] * kCapsuleScale;
        EyeOut &out = out_[static_cast<std::size_t>(eye)];
        // Orientation rotates the whole face: centers and angles (disc is
        // rotation-invariant).
        out.cx = kScreenCenter + (design_x * cosine - design_y * sine) * kCapsuleScale +
                 gaze_screen_x + drift_x;
        out.cy = kScreenCenter + (design_x * sine + design_y * cosine) * kCapsuleScale +
                 gaze_screen_y + drift_y - 6.0F * wave_level[eye] * kCapsuleScale;
        out.angle_rad = eyes_[eye][2].x * (kPi / 180.0F) + rotation;
        out.half_len = std::max(
            std::max(eyes_[eye][3].x, 1.0F) * kCapsuleScale * breath * spin.size + wave_px,
            2.0F);
        out.half_w = std::min(
            std::max(std::max(eyes_[eye][4].x, 0.6F) * kCapsuleScale * breath * spin.size +
                         wave_px,
                     1.0F),
            out.half_len);
        // Blink = screen-vertical lid squash, not a width scale: shrinking
        // half_w on a mostly-vertical capsule closed it sideways (thin upright
        // sliver) instead of shutting like a lid.
        out.squash_y = openness;
    }
    // Quantize to 1/4 px (angle ~0.11 deg): equal quantized params => the
    // rasterized pixels are identical, so the frame can be skipped. Every
    // animated channel (Qi roll/x/y, spinWild offset/wobble/size, nod-off lid,
    // wave pulse) flows through these 12 EyeOut values, so the power gate can
    // never freeze mid-move.
    for (int eye = 0; eye < 2; ++eye) {
        const EyeOut &out = out_[static_cast<std::size_t>(eye)];
        const std::size_t base = static_cast<std::size_t>(eye) * 6U;
        quantized_[base + 0] = static_cast<std::int16_t>(std::lround(out.cx * 4.0F));
        quantized_[base + 1] = static_cast<std::int16_t>(std::lround(out.cy * 4.0F));
        quantized_[base + 2] = static_cast<std::int16_t>(
            std::lround(std::remainder(out.angle_rad, kPi) * 512.0F));
        quantized_[base + 3] = static_cast<std::int16_t>(std::lround(out.half_len * 4.0F));
        quantized_[base + 4] = static_cast<std::int16_t>(std::lround(out.half_w * 4.0F));
        quantized_[base + 5] = static_cast<std::int16_t>(std::lround(out.squash_y * 256.0F));
    }
}

bool CapsuleFace::needs_frame() const { return quantized_ != last_rendered_; }

}  // namespace eyes
