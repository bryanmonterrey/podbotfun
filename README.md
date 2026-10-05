# LilGuy Eyes for Waveshare AMOLED 1.75C

A real ESP-IDF firmware project for the **Waveshare ESP32-S3-Touch-AMOLED-1.75C**. It starts on the public Creature **circle-siren** look and supports swipe browsing, live gaze, organic blinks, calm tilt tracking, and shake reactions.

The ESP32 rasterizer is original code. A host exporter runs the captured public client renderer and produces compact cubic-path samples for the firmware. The browser WASM and raw animation binary stay host-only.

## What is implemented

- Native 466 x 466 RGB565 rendering for the CO5300 QSPI AMOLED
- CST9217 capacitive touch through the official Waveshare BSP
- QMI8658 accelerometer and gyroscope on the board's shared I2C bus
- Client-rendered cubic geometry with the exact reference colors and gaze deformation
- 4 authored pupil shapes × 100 exact palettes, shown through a swipeable local grid
- Continuous calm idle motion, measured blink timing, touch gaze, poke, sleep and wake
- TE-synchronized display updates and an internal DMA staging buffer to reduce tearing
- Strongly smoothed tilt-driven pupils and shake/spin-driven dizzy reaction
- First-boot six-axis neutral-pose calibration and NVS persistence
- Circular composition mask for the round-display visual language
- Host-side deterministic behavior and raster tests
- A host preview renderer and an interactive desktop simulator for work without the board

## Controls

| Input | Behavior |
|---|---|
| Touch and hold | Pupils follow the touch point |
| Swipe left or right | Browse color palettes (world-view build; see Configuration) |
| Quick tap | Poke reaction and blink |
| Cover the screen with a full palm | Sleep from the CST9217 palm signal; the next touch wakes it |
| PWR double press | Sleep or wake |
| PWR single press | Listen: opens the voice stream, the same as the wake word |
| BOOT double press | Stats page (diagnostics); a single press is reserved |
| Tilt the board | Pupils follow gravity with smoothing |
| Shake or rotate quickly | Dizzy orbit reaction for about 1.6 seconds |
| BOOT press for at least 850 ms | Recalibrate the IMU neutral position |

## Build and flash

Use ESP-IDF 5.5.x. This project was target-built with ESP-IDF **5.5.1**, LVGL **9.5.0**, Waveshare BSP **3.0.0**, and Waveshare QMI8658 **2.0.0**.

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

On macOS the port is usually `/dev/cu.usbmodem*`. On Windows, use a port such as `COM5`.

Prebuilt firmware is published only as a verified GitHub release asset. The repository does not
track local or stale binary images.

For roughly the first 1.5 seconds after boot, leave the board still in the pose that should count as neutral. Calibration is accepted only when all accelerometer and gyroscope axes are stable and measured gravity is plausible. It can always be repeated with a long BOOT press.

## Host tests and preview

These do not require ESP-IDF:

```bash
node tools/export_creature_runtime.cjs
make host-test
make host-preview
```

The preview is written to `host/eyes_preview.png` when ImageMagick is available and always to `host/eyes_preview.ppm`.

## Desktop simulator

`make host-sim` runs the real `EyeEngine`, `EyeRenderer` and `DeviceUi` inside a rendering of the
physical product. On macOS the window is transparent and borderless, so the puck simply sits on the
desktop: powder-coated shell, cover glass, recessed side keys and mic ports, casting its own
shadow. Only the hardware layer is faked, so the onboarding, the press gestures and every
reaction are the firmware's own code paths rather than a lookalike.

The look is fixed: white capsule eyes on a black field, the same on the board and in the window.
There is no touch menu; touching the glass is petting. Setup is by voice (say a name when the
device writes "hey") or from the phone app claiming the pairing QR. The wake word is "hi" plus
that name; "new chat" starts a fresh conversation.

The shell is built from the Waveshare dimension drawing, keyed to the display: the 43.76 mm active
circle **is** the 466 px framebuffer, so the 48.96 mm cover glass and the 55.00 mm shell follow at
the same 10.65 px/mm. Clicks only register inside the active circle, exactly like the real panel.
The finish is a procedural powder coat: a wrapped-diffuse, low-specular response with an
orange-peel value-noise grain perturbing the surface normal, rather than the mirror reflections of
bare metal. The two keys sit in pockets machined into the rim, so the shell always overlaps their
edges and they read as part of the body.

The shell and both key states are baked once at startup at 3x supersampling, so the silhouette,
the glass seam, the pocket walls and the mic ports are properly averaged rather than taking one
coverage sample per pixel, and the grain resolves smoothly. It costs about 0.1 s at launch and
makes the per-frame cost of the keys a blit.

Needs SDL2 (`brew install sdl2`, or `apt install libsdl2-dev`). Without it the target still builds,
headless-only. The transparent window uses a little Cocoa (`host/mac_window.mm`); other platforms
get an ordinary decorated window, and `--framed` opts into one on macOS too. Transparency needs
more than a clear `NSWindow`: SDL renders into a `CAMetalLayer` that is created opaque, so every
layer in the view tree is marked non-opaque too, otherwise alpha still composites onto black.

The window sizes itself to the display and shrinks the device if the stage would not fit, since a
clipped borderless window has no title bar to drag it back by.

| Where | What |
|---|---|
| The glass | Click and drag it: that is a finger on the panel, and the creature reacts to it |
| The two side keys | Press and hold the actual keys. Duration decides the action, so a 900 ms BOOT hold really does recalibrate and a 3 s hold toggles the debug line. Hovering names the key |
| The grey shell | The drag handle: drag it to move the device around your desktop, chip and panel included. The glass and the keys are excluded, and the cursor changes over it |
| Right-drag the glass | Tilt the board; the gravity vector holds where you leave it, like a propped-up device |
| The `DEBUG` chip | A small chip floating clear of the shell, anchored beside it. Click it to spawn the debug panel; click again to dismiss |
| The debug panel | Drag it by its title bar, `Tab` also toggles it |

Everywhere the window is see-through, clicks pass to whatever is behind it, so the invisible parts
of the window do not block the desktop.

The panel groups the rest: the same `BOOT`/`PWR` as buttons, the touch gestures (`TAP`,
`HOLD 800MS`, `PALM`, four swipes), a tilt pad with `FLAT`, `SHAKE` and a held `SOUND`, face
rotation in 15-degree steps, a raster antialiasing override, and the two settings the device
still has (tilt sensitivity, rotation reset) labelled from the firmware's own strings. Swipes are scripted pointer streams, so they carry real velocity into the fling logic. It
also reports live mode, expression, attention source, gaze, tilt and settings-save count.

**Raster antialiasing.** `EyeRenderer` normally picks the vertical subsample count per path:
2 on tiny grid cells, 4 while content is moving fast, 8 when near-still (`eye_renderer.cpp`,
"Small cells trade flatten steps and vertical AA"). The panel's `AUTO / 2X / 4X / 8X` row pins it
through `EyeRenderer::set_aa_override`, for A/B comparison against the shipped adaptive choice;
`AUTO` restores it. Two things worth knowing while reading the numbers, both shown in the readout:
the rasterizer only implements 8 or 2 lines per row, so **a request of 4 samples the same rows as
2** (`aa_lines_for` in `raster.hpp`), and this is vertical AA only — horizontal coverage is
computed analytically and is unaffected. The readout line reports what was asked for and what the
raster actually gave it.

Two consequences worth knowing before A/B-ing it: on a **still** hero frame the adaptive choice is
already 8, so `AUTO` and `8X` are pixel-identical and pinning 8X changes nothing — the adaptive
path only drops below 8 while content moves (mid-blink it picks 4, i.e. 2 lines). And across the
whole frame, 2X versus 8X moves about **0.5% of pixels** (the edge fringe), so it is a subtle
difference at best.

**Magnify.** Separately from raster AA, the panel has a `MAGNIFY: SMOOTH / PANEL PIXELS` toggle.
The device is ~270 PPI, well past what the eye resolves, but this window blows the 466 px panel up
on a much coarser monitor, so point sampling shows panel pixels the hardware never would — that
stair-stepping is a magnification artifact, not renderer aliasing, and no AA setting touches it.
`SMOOTH` (bilinear, the default) is the closer likeness to the real thing; `PANEL PIXELS` is the
honest 1:1 view for inspecting the framebuffer. `--panel-pixels` starts in that mode.

Keyboard: `B`/`N` hold BOOT/PWR, `S` `P` `X` sleep-wake / palm / shake, space holds a loud sound, `1` `2` `3`
set tilt sensitivity, `Q` `E` `0` rotate the face, `W` toggles a webcam viewfinder on the panel
(macOS; the simulator's stand-in for the future CameraService hardware), `G` captures a PPM,
`Esc` quits.

Flags: `--scale N` sizes the device (default 1.15, a float; the whole shell scales with it),
`--shape N`, `--palette N`, `--capsule`, `--framed` for a conventional window, `--bob` for a slow
idle float (off by default: it shifts the composited shell in whole-pixel steps, which reads as the
face jittering the case), `--panel-pixels` to start unsmoothed.

Off-device stand-ins: settings persist in memory rather than NVS, the battery gauge reports a fixed
pack, and a calibration request completes after 1.5 s. The keys are drawn at the positions in the
dimension drawing; which of the pair is BOOT and which is PWR is an assumption, so hovering names
each one.

For CI or a machine with no display, the same binary runs without a window:

```bash
./host/eyes_simulator --selftest                        # drives every panel control, checks each lands
./host/eyes_simulator --headless --seconds 6 --out /tmp/sim
./host/eyes_simulator --headless --composite --seconds 1 --out /tmp/shot   # device view, on a checkerboard
```

## Configuration

Run `idf.py menuconfig`, then open **LilGuy Eyes**:

- Target timer rate: 20 to 60 Hz. The current renderer separately caps focus near 30 fps and browse near 20 fps.
- Circular viewport mask: enabled by default
- First-boot IMU auto-calibration: enabled by default
- World-view browse carousel: **off** by default. With it off, drags are pure gaze-follow and the
  swipe-to-browse rows above do not apply. Host builds compile it in, where it additionally starts
  behind `EyeEngine`'s selection lock.

## Board-specific facts

This target has **no SD-card slot or SD interface in the BSP**. Selection and calibration state are stored in NVS on internal flash. The board has 8 MB octal PSRAM, which holds the 434,312-byte framebuffer. The flash is 32 MB (the chip reports it; the schematic's part number says 16 MB), laid out as two 6 MB OTA app slots plus storage; see `docs/HARDWARE.md`.

See `docs/OBSERVATION_LOG.md`, `docs/BEHAVIOR_SPEC.md`, `docs/ARCHITECTURE.md`, `docs/HARDWARE.md`, and `docs/ON_DEVICE_TEST.md` for the black-box trace, detailed design, and bring-up notes.

## References

- https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C
- https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C
