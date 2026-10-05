# Behavior specification

This specification records the public Creature client behavior and its exported vector output, then maps it to the device.

## Observed interaction model

The designer has two visual states rather than a conventional page-and-menu UI.

### Focus state

- One selected, large circular face dominates the center.
- Nearby faces remain spatially implied behind a radial focus treatment.
- Eye and swatch labels identify the two independent selection dimensions.
- Pointer movement continuously steers the pupils; gaze is not limited to clicks.
- Individual eyes blink asynchronously, including occasional sleep-like closures.

### Browse state

- A drag reveals a two-dimensional field of looks instead of a list.
- Horizontal and vertical motion address independent selection dimensions.
- The focused face shrinks into the field while neighboring faces become legible.
- Releasing retains velocity, then settles elastically onto a cell.
- The selected URL/state changes only after the settle resolves.
- The selected cell remains live. Neighbor previews remain static to reduce browse work.

### Round product presentation

The LilGuy product presentation establishes the intended physical character: a round, wearable face that reacts to motion and environment. The public page specifically describes accelerometer-driven reactions such as becoming dizzy or angry after shaking. This board implementation maps that idea to its onboard QMI8658.

## Firmware mapping

| Observed behavior | Firmware implementation |
|---|---|
| Continuous pointer gaze | CST9217 touch point normalized around the 466 x 466 center |
| Focused central face | Exported four-path cubic rig centered at (233, 233) |
| Two-dimensional look field | 5 x 5 local toroidal neighborhood; horizontal palette, vertical shape |
| Drag reveal | 18-pixel activation threshold and immediate grid visibility |
| Inertial release | 140 ms velocity projection before cell quantization |
| Elastic snap | Critically damped spring integration at fixed substeps |
| Static neighbor previews | Only the selected browse cell uses live gaze and blink state |
| Authored idle motion | One looping 15 Hz track, stored as interpolated signed 8-bit deltas from the neutral rig; focus and the live browse cell only |
| Organic blink | Authored clip selected after a seeded 3 to 7 second interval |
| Ambient gaze | Seeded hold/center, adjacent drift, and 100 ms micro-saccade choices |
| Round-display composition | Circular framebuffer mask, even though this panel is square |
| Motion-aware face | Calibrated directional 9 x 9 gaze plus one bounded shake move |
| Sound attention | ES7210 MIC1/MIC2 integer energy gate; physical axis remains neutral |
| Persistent selection | User shape, palette and versioned six-axis calibration saved in NVS; emotions do not overwrite the user selection |

## Input precedence

1. Sleep suppresses every other source. A fresh touch wakes the engine.
2. Browse suppresses live behavior until 300 ms after settle.
3. Touch controls gaze while down and for 250 ms after release.
4. A committed shake controls one bounded gaze move for 400 ms.
5. A gated sound owns attention for at most 400 ms.
6. Calibrated IMU tilt controls gaze through an exact 200 ms release hold.
7. Seeded idle gaze and emotion decisions run when no higher source owns attention.

Shake has a 2,500 ms cooldown. IMU tilt changes gaze coordinates only. It never rotates the eye, face, or canvas.

Sound capture uses signed 16-bit stereo at 16 kHz. One unpinned priority-1 task blocks on each 320-frame, 20 ms read. It sums MIC1 and MIC2 energy into one `uint64` value and sends it through one overwrite mailbox. The engine calibrates its floor for 50 windows. While closed, the floor follows an integer EWMA with divisor 64. The gate enters above 4 times floor for 3 windows and exits below 9/4 times floor for 3 windows. Both microphones are on the left edge, so firmware does not infer left or right sound direction.

Sound packets older than 80 ms are ignored. Sleep, browse, touch, and shake clear any pending sound entry. Sound does not resume after a higher source releases. A committed sound has a 400 ms hold and 1,600 ms cooldown, and cannot trigger again until its gate closes.

## Deterministic policy

- One 32-bit xorshift state drives all choices. Seed zero maps to `0x4C494C47`.
- Random values are consumed only when a blink schedule, blink clip, shake, idle gaze, or emotion decision commits.
- IMU sensitivity is clamped to 0.5 through 2.0. Its default is 1.0.
- Calibrated tilt uses a 0.12 dead zone and maps each axis to the existing 9 x 9 gaze grid.
- Idle starts after 1,000 ms. Choices are 50% hold or center, 35% adjacent slow drift, and 15% 100 ms micro-saccade. Dwell is 1,200 through 3,000 ms.
- Emotion temporarily selects the four current states with weights 55%, 25%, 15%, and 5%. The first and later dwells are each seeded choices from 6,000 through 12,000 ms. Browse and user selection restore the saved base shape and palette.
- Blink intervals are 3,000 through 7,000 ms. At most one blink remains pending while sleep, browse, shake, or an emotion transition blocks it.
- A pending blink starts only after 250 ms of eligible stable behavior.

## Selection model

The catalog is a torus: moving beyond either edge wraps to the opposite edge. The visible browser needs only the local 5 x 5 neighborhood, so the firmware never allocates a 400-cell scene. A look is the product of:

- one of 4 authored pupil states: dot, circle, cat or acorn;
- one of 100 client palettes in the renderer's runtime order.

The result is 400 deterministic vector combinations without bitmap sprites.

Cubic eye paths and circular browse backgrounds blend only their one-pixel boundary coverage into the current RGB565 framebuffer. Solid interiors remain exact. Clipped pupil pixels can replace only exact outer-eye pixels, so softened outer edges cannot bleed.

`host/eyes_palette_grid.ppm` shows every source palette above its direct RGB565 result. The renderer does not apply panel color correction. Each of the 100 RGB565 palette tuples remains distinct.

## Timing constants

| Behavior | Value |
|---|---:|
| Default frame period | 16.7 ms |
| Authored idle sampling | 15 Hz, linearly interpolated |
| Physics substep | 8 ms maximum |
| Drag threshold | 18 px |
| Swipe commit threshold | 20% of screen width |
| Velocity projection | 140 ms |
| Grid pitch | 272 px |
| Long touch sleep | Disabled; only the CST9217 palm-cover event sleeps the eyes |
| Browse behavior hold after settle | 300 ms |
| Touch gaze hold after release | 250 ms |
| IMU gaze hold after release | 200 ms |
| Shake reaction | 400 ms |
| Shake cooldown | 2,500 ms |
| Sound window | 20 ms / 320 stereo frames |
| Sound calibration | 50 windows |
| Sound attention / cooldown | 400 ms / 1,600 ms |
| Sound packet stale limit | 80 ms |
| Idle start delay | 1,000 ms |
| Idle dwell | 1,200-3,000 ms |
| Emotion dwell | 6,000-12,000 ms |
| Blink interval | 3,000-7,000 ms |
| Blink stable eligibility | 250 ms |
| Selection overlay | 1,500 ms |
