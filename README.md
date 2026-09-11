# Fish Tank HMI

An LVGL-based fish-tank dashboard for the Guition JC4827W543C 4.3-inch capacitive display.

## Commands

PlatformIO is installed at `~/.platformio/penv/bin/pio`.

```sh
PIO=~/.platformio/penv/bin/pio
$PIO device list
$PIO run --environment display_4_3_capacitive
$PIO run --target upload --environment display_4_3_capacitive --upload-port /dev/ttyACM1
$PIO device monitor --environment display_4_3_capacitive --port /dev/ttyACM1
```

The USB device can re-enumerate as a different `/dev/ttyACM*` path. Use `$PIO device list` before upload or monitoring.

## Hardware Notes

- Board: Guition JC4827W543C, ESP32-S3, 4 MB QIO flash, 8 MB OPI PSRAM.
- Display: NV3041A, 480x272 QSPI. Pins: CS 45, SCK 47, D0 21, D1 48, D2 40, D3 39. Backlight: GPIO 1.
- Touch: GT911 capacitive I2C. SDA 8, SCL 4, reset GPIO 38, address `0x5D`.
- Display rendering uses Arduino_GFX `v1.4.6`; LVGL writes through a flush callback to the panel.
- Touch uses TouchLib with `TOUCH_MODULES_GT911`.

## Safety

The initial dashboard is simulated only. It must not control heaters, lights, pumps, feeders, or relays. Add real outputs only behind explicit safety limits, independent sensor validation, and fail-safe defaults.
