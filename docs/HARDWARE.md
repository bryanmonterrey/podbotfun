# Hardware target

Target: **Waveshare ESP32-S3-Touch-AMOLED-1.75C**.

## Confirmed resources

| Resource | Device / value | Firmware use |
|---|---|---|
| MCU | ESP32-S3R8, dual-core, up to 240 MHz | Behavior, rendering, LVGL |
| PSRAM | 8 MB octal | Procedural canvas and display buffers |
| Display | CO5300, 466 x 466, QSPI | RGB565 output |
| Touch | CST9217 at I2C address `0x5A` | Gaze, tap, long press and drag |
| IMU | QMI8658 at I2C address `0x6B` | Tilt gaze and shake reaction |
| Flash | 32 MB (the chip reports it; the schematic's `XM25QH128DHIQT` is 128 Mbit) | 32 MB build target: two 6 MB OTA slots + storage, NVS state |
| SD card | Not present; BSP capability is `BSP_CAPS_SDCARD 0` | No SD code or storage assumption |

`BSP_CAPS_IMU` is also zero, but that means the BSP lacks a convenience IMU wrapper. The QMI8658 is physically present and is accessed through the official standalone `waveshare/qmi8658` component and the BSP's shared I2C handle.

## Relevant pins

| Function | GPIO |
|---|---:|
| I2C SDA | 15 |
| I2C SCL | 14 |
| AMOLED CS | 12 |
| AMOLED clock | 38 |
| AMOLED data 0 | 4 |
| AMOLED data 1 | 5 |
| AMOLED data 2 | 6 |
| AMOLED data 3 | 7 |
| AMOLED reset | 1 |
| AMOLED TE | 13 |
| Touch reset | 2 |
| Touch interrupt | 11 |
| BOOT button | 0 |

Touch, IMU, audio codec, RTC and power-management devices share I2C. The firmware initializes display/touch first, then adds QMI8658 to the already-created BSP bus. It does not install a second I2C driver.

## Panel details that matter

- The logical canvas is 466 x 466; the panel driver applies the controller's six-pixel X offset.
- Touch defaults are `swap_xy=0`, `mirror_x=1`, `mirror_y=1` at rotation zero.
- QSPI flush areas must use an even start and odd end on both axes.
- There is no backlight GPIO. Brightness is controlled by CO5300 command `0x51` through the BSP.
- The glass is square with rounded corners. The circular renderer mask intentionally follows the round LilGuy composition rather than pretending the physical panel is round.

## Flash-size discrepancy

The schematic identifies a 128-Mbit (16 MB) flash part, but the board flashed on 2026-09-10 reports 32 MB and Waveshare's product material says the same. The project built for 16 MB until 2026-09-30; with the owner's approval `partitions.csv` now uses the 32 MB part (two 6 MB OTA slots, the rest storage). If a unit ever reports 16 MB, the 16 MB layout is in git history (two 3 MB slots at the same NVS offset).
