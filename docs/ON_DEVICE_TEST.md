# On-device bring-up and validation

Host and target compilation catch logic and API failures, but the following checks require the physical board.

## First flash

1. Disconnect any external hardware from the shared pins.
2. Flash with `idf.py -p PORT flash monitor`. A board carrying the pre-2026-10-01 16 MB table needs this full `flash` (not `app-flash`) once: it rewrites the bootloader, the 32 MB partition table and otadata. NVS sits at the same offset in both tables, so IMU calibration, settings and the device identity survive; `erase-flash` is not needed and would lose them.
3. Hold the board still in the pose that should count as neutral before reset.
4. Confirm the log prints QMI8658 initialization followed by a six-axis neutral/gyro calibration result. If the window fails its stillness or gravity check, keep holding still while it restarts.
5. Confirm the white capsule (pill) eye pair appears centered on black. Fresh settings boot the capsule face with inverted ink on the white `og` palette; the creature face is one tap away on the menu's FACE row.

Expected startup characteristics:

- the display initializes once without a reboot loop;
- the selection overlay disappears after 1.5 seconds;
- the face starts autonomous saccades and blinks;
- NVS preserves the selected look across reset.

## Display checks

| Symptom | Likely cause | Check |
|---|---|---|
| Red appears blue | Incorrect RGB565 byte/order handling | Keep canvas `LV_COLOR_FORMAT_RGB565`; the Waveshare adapter performs the flush swap |
| Image shifted horizontally | Controller offset bypassed | Use BSP 3.0.0 panel creation, not a raw panel replacement |
| Thin corrupted edge | Invalid CO5300 flush bounds | Keep dynamic dirty rectangles even-start/odd-end aligned and retain BSP rounding |
| Flicker during drag | Transfer starvation | Use 30 fps first; inspect dynamic dirty bounds and QSPI load |
| Black screen after startup | Zeroed LVGL adapter config | Ensure `ESP_LV_ADAPTER_DEFAULT_CONFIG()` is assigned before BSP start |

## Touch checks

Test all four corners. Pupils should move toward the finger without swapped or inverted axes. Then:

1. Drag right and release; the palette should move one or more wrapped cells depending on release speed.
2. Drag up and release; the shape should advance.
3. Drag diagonally; both values should change.
4. Tap without crossing 18 pixels; the face should poke/blink without changing selection.
5. Hold at least 1,000 ms and release; the eyes should stay awake and react like a normal release.
6. Cover the screen with a full palm; the authored closed-eye frame should appear without pupils and serial should report `Palm cover detected; sleeping` once. Remove the palm, then touch to wake.

If an axis is wrong, adjust only `touch_flags` in `initialize_display`; do not alter renderer coordinates and IMU orientation at the same time.

## IMU checks

After calibration in the intended neutral pose:

- slowly tilt left/right and confirm prompt, continuous horizontal pupil motion without stepping;
- slowly tilt forward/back and confirm prompt, continuous vertical pupil motion without stepping;
- hold one tilt for more than one second and confirm the pupils return to center and remain locked while the tilt is held;
- return to the calibrated neutral pose, tilt again, and confirm IMU gaze rearms;
- release a short tilt before its one-second limit and confirm the last target holds for 200 ms before lower-priority gaze resumes;
- shake once and confirm a short dizzy orbit;
- long-press BOOT while flat to repeat calibration.

### IMU quick verdict

Report four facts: whether left/right is smooth, whether forward/back is smooth, whether a held tilt recenters after one second, and whether a new tilt works after returning to neutral. Name the failed direction if one axis is wrong.

The current candidate inverts both accelerometer gaze axes in `EyeEngine::motion_sample`. Test the board with USB-C down before making another change. If one physical direction is still reversed, change only that axis sign. Keep the QMI units as m/s² and rad/s.

## Performance capture

For a release candidate, record at least 30 seconds of each mode:

- focus with autonomous motion;
- touch tracking;
- continuous diagonal grid drag;
- repeated fling and settle;
- tilt and shake reactions.

Watch for watchdog resets, failed QSPI transfers, I2C errors and PSRAM allocation failures. A 30 fps build should feel stable before trying 45 or 60 fps.

The bridge prints one `TE frames=60` line after each 60 completed logical frames. It does not print per frame or per strip. Record the average and maximum values for `bytes`, `strips`, `wait_us`, and `service_us` in each mode. `bytes` and `strips` cover the full logical frame. `wait_us` covers the first-strip TE wait. `service_us` covers the TE wait and all synchronous strip transfers.

Keep focus at 60 fps only if its maximum `service_us` stays below 16,000 us during the full capture and the display has no visible tear. Otherwise use 45 fps for the next test. Keep browse at 30 fps.

## Microphone checks

1. Start in quiet for at least 1 second. Confirm 50 floor-calibration windows and then at least 10 seconds with no sound attention.
2. Make one sharp sound near the device at least 5 times. Confirm that each sound causes the listening expression without moving the pupils left or right.
3. Confirm entry needs 3 loud windows. Confirm exit needs 3 quiet windows. Confirm attention lasts no more than 400 ms and cannot recur before 1,600 ms or before the gate closes.
4. While sound is loud, test sleep, browse, touch, and shake. Each source must take priority. Sound must not appear when that source releases.
5. Capture 60 seconds of idle serial output. Require 49 to 51 audio windows each second and `feature_max` at or below 2,000 us. Reject any frame-service gap above 25 ms, audio read error, display error, reset, memory failure, or blank screen.

These checks need direct board observation. A host test or successful firmware build does not prove microphone response, display timing, or visible output.

## Acceptance criteria

- 30 fps interaction has no visible multi-frame stalls.
- Drag release never leaves the field between cells.
- Selection changes once per settle and survives reset.
- Touch and IMU remain responsive after 10 minutes.
- No framebuffer corner is written outside the round mask.
- No I2C failure occurs while touch and IMU operate concurrently.

## Power key

- Hold PWR about 1.5 s: the log prints `PWR held: powering off` and every rail drops (screen, mics, the USB serial port). A short press of PWR powers it back on and the full boot log repeats. The PMIC's own 6 s hold still works as a fallback.
