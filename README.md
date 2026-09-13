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

## Secrets

Copy `secrets/secrets.example.h` to `secrets/secrets.h` and fill in your WiFi SSID/password and Apex username/password. `secrets/secrets.h` is gitignored and never committed.

## Wi-Fi Credentials

The compiled-in network from `secrets.h` is the fallback, so a freshly flashed board comes
up without any setup. Joining a network from the Setup tab saves it to NVS (namespace
`reefwifi`), and that saved network is used from then on. If it will not come up, the panel
falls back to the compiled-in one rather than stranding itself.

Joining blocks the radio for several seconds, so it runs as a small state machine that
lets the panel paint "Joining..." before everything stalls. Scanning is asynchronous and
does not block at all. A typed passphrase is wiped from RAM as soon as the attempt ends.

The Apex credentials stay in `secrets.h` — only Wi-Fi is configurable from the panel.

## Display Orientation

Setup's FLIP button rotates the panel 180° so the USB cable can leave the other side. The
choice is saved to NVS (namespace `reefcfg`) and reapplied at boot.

The rotation is done in software, in `display_flush()`. **The panel's own MADCTL rotation
is unreachable on this bus** — worth knowing before trying to "optimise" this back into a
register write:

- The NV3041A init sequence never writes MADCTL (`0x36`) at all, so the panel runs at its
  power-on scan direction.
- `Arduino_ESP32QSPI::write(uint8_t)` hardcodes the transaction address to `0x003C00`, the
  write-to-GRAM opcode. So `Arduino_NV3041A::setRotation()`, which does
  `writeCommand(0x36); write(r);`, never delivers `r` as a register parameter — it goes out
  as a stray pixel. Calling `setRotation()` on this bus flips nothing and dirties a pixel.

Turning a rectangular block 180° is exactly reversing its pixels in memory order, and the
block lands at the mirrored position — so the flush reverses the LVGL buffer in place and
draws it at `(W-1-x2, H-1-y2)`. That buffer is at most 480×24 px of internal RAM, which is
noise next to the full-frame blit to the panel that follows it.

The GT911 digitizer is glued to the glass and knows nothing about any of this, so
`touch_read()` mirrors both axes too. Forgetting that half is what makes a rotated display
feel haunted.

## Screens

The UI follows `design/V1/` (open `design/V1/Reef Controller.dc.html` in a browser for the
interactive mock, and see `LVGL-HANDOFF.md` for the spec). A persistent status bar and tab
bar frame four pages:

- **Home** — Temp / pH / Salinity cards with live sparklines and min-max, a warning banner
  when anything is out of auto, and the FEED button with its countdown.
- **Control** — one row per Apex outlet with an OFF / AUTO / ON selector, plus
  RETURN ALL TO AUTO.
- **Alerts** — live problems (outlets out of auto, a dead Apex link) above an
  acknowledgeable history.
- **Setup** — Wi-Fi and Apex link status, a FLIP button that turns the panel 180°, a
  scanned list of nearby networks, and RETRY / RESCAN. Tapping a network opens the
  on-screen keyboard to join it.

The power button in the status bar dims the panel to an idle screen; any touch wakes it.
There is no auto-sleep.

## Apex Integration

The dashboard talks to a Neptune Apex at `http://apex.local` over the local network (no
cloud, no Fusion account) using HTTP Basic auth, polling `/cgi-bin/status.json` every
10 seconds.

Read from `istat`:

- `inputs` — the `Tmp`, `pH` and `Salt` probes feed the three Home cards.
- `outputs` where `type == "outlet"` — the Control rows.
- `feed.active` — seconds left in the running feed cycle.
- `date` — used to timestamp alert history.

Outlet state is carried in `status[0]`: `AON` / `AOF` mean the outlet's own Apex program is
driving it on or off, while a bare `ON` / `OFF` is a manual override the Apex holds until
it is cleared. Anything else is treated as program-driven so an unfamiliar code cannot
raise a false out-of-auto alarm.

Writes are `PUT /rest/status/outputs/<did>` with
`{"did":"<did>","status":["<MODE>","","OK",""],"to":"<MODE>"}` where `<MODE>` is
`OFF`, `AUTO` or `ON`. The Apex answers `200` even when it rejects a request, so the
response's `errorCode` is what actually gets checked.

Feed cycles are `PUT /rest/status/feed/<index>`. The cycle index has to be in the URL
path — a body-only `name` field is accepted and then silently ignored.

## Controlling Real Equipment

This dashboard commands real outlets — pumps, heater, lights, dosers. That is deliberate,
and worth understanding before you flash it:

- Writes only ever go to the Apex on the local network. The panel switches nothing itself
  and has no relays of its own.
- The Apex remains in charge. An override changes what the Apex is doing; it does not
  bypass the Apex, and each outlet keeps its configured fallback (`Fallback ON` and
  friends) if the controller loses its link.
- An override never expires on its own. It raises the Home banner, a badge on the CONTROL
  tab and an Alerts row, and stays until someone clears it. RETURN ALL TO AUTO hands every
  outlet back to its program in one press.
- Feeding uses the Apex's own pre-programmed cycle rather than switching the return pump
  directly, so the Apex restores the pump even if this panel reboots mid-cycle.
- Taps are applied optimistically for responsiveness and reconciled on the next poll, so
  the displayed state can briefly lead the Apex by up to 10 seconds.

## Hardware Notes

- Board: Guition JC4827W543C, ESP32-S3, 4 MB QIO flash, 8 MB OPI PSRAM.
- Display: NV3041A, 480x272 QSPI. Pins: CS 45, SCK 47, D0 21, D1 48, D2 40, D3 39. Backlight: GPIO 1.
- Touch: GT911 capacitive I2C. SDA 8, SCL 4, reset GPIO 38, address `0x5D`.
- Display rendering uses Arduino_GFX `v1.4.6`; LVGL writes through a flush callback to the panel.
- Touch uses TouchLib with `TOUCH_MODULES_GT911`.
- Backlight is on LEDC channel 0; sleep dims it to 3% rather than 0, because a truly dark
  panel reads as broken rather than asleep.
- The board default partition table splits 4 MB into two 1.25 MB OTA slots, and the LVGL
  fonts alone push the image past 97% of one. `platformio.ini` selects `huge_app.csv`
  instead: a single 3 MB app partition, no OTA. Flashing is over USB.

## Source Layout

| File | Role |
|---|---|
| `src/main.cpp` | board bring-up, LVGL init, poll scheduling |
| `src/model.h` / `.cpp` | `ReefState` — everything the UI draws |
| `src/apex.h` / `.cpp` | Apex HTTP client: poll, outlet writes, feed cycles |
| `src/wifi_manager.h` / `.cpp` | credentials (NVS + secrets fallback), connect, scan, join |
| `src/settings.h` / `.cpp` | device settings kept in NVS (display orientation) |
| `src/theme.h` / `.cpp` | palette and shared LVGL styles |
| `src/ui.h` / `.cpp` | all screens; `ui_create()` builds, `ui_update()` repaints |
| `src/logo_mark.c` | generated from `design/V1/robonerd-mark.svg` |

`ui_update()` runs every loop, so every setter in it is a no-op when nothing changed —
this panel can only flush whole 480x272 frames, so a stray style write costs a full
repaint.

To regenerate the logo after the SVG changes: render it with
`inkscape --export-type=png -w N -h N`, then emit the PNG's alpha channel as an
`LV_IMG_CF_ALPHA_8BIT` array. Alpha-only images take their color from
`lv_obj_set_style_img_recolor()`, so one asset serves both the accent status-bar mark and
the dim sleep watermark.
