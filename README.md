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

- **Home** — Temp / pH / Salinity cards with 24-hour sparklines and min-max, a warning
  banner when anything is out of auto, and the FEED button with its countdown.
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
- `date` — a true Unix epoch, used to timestamp alert history and to place readings in
  the 24-hour trends.
- `timezone` — hours east of UTC as a decimal. `date` is *not* pre-shifted into local
  time, so this is what turns it into a wall clock.

Outlet state is carried in `status[0]`: `AON` / `AOF` mean the outlet's own Apex program is
driving it on or off, while a bare `ON` / `OFF` is a manual override the Apex holds until
it is cleared. Anything else is treated as program-driven so an unfamiliar code cannot
raise a false out-of-auto alarm.

Writes are `PUT /rest/status/outputs/<did>` with
`{"did":"<did>","status":["<MODE>","","OK",""],"to":"<MODE>"}` where `<MODE>` is
`OFF`, `AUTO` or `ON`. The Apex answers `200` even when it rejects a request, so the
response's `errorCode` is what actually gets checked.

## The 24-Hour Sparklines

Each Home card shows a fixed 24 hours as a min/max band: one pixel-wide column per slice
of the day, spanning that slice's low to its high, with a dot on the newest column at the
current reading. There is no time axis and no "24H" label — the window never changes.

**Columns are anchored to absolute epoch multiples of the column width**, never to "now
minus k columns". A closed column is therefore never recomputed: as time passes the window
slides left by whole columns and is otherwise the same picture. Re-binning against a moving
origin is what makes these charts wobble, and it is the one thing to preserve if any of
this is ever rewritten.

The chart is 130 px wide — 480 screen, 10 px of page padding a side, two 8 px gaps between
cards, then each card's 1 px border and 8 px padding. 128 is the largest column count that
divides a day exactly (128 × 675 s = 86400), so a column is a whole number of seconds and
the geometry needs no rounding anywhere.

`lv_chart` cannot draw this: `LV_CHART_TYPE_BAR` grows from the baseline and there is no
band type, so `spark_draw()` in `ui.cpp` renders the columns itself from a
`LV_EVENT_DRAW_MAIN` handler. The columns that fit are drawn and any excess falls off the
left, rather than the chart refusing to draw at all if the geometry shifts by a pixel.

### Backfill from the Apex datalog

A freshly booted panel would otherwise take a day to draw a day, so it fills the window
from the Apex's own log: `GET /cgi-bin/datalog.json?sdate=YYMMDDHHMM`, which returns
everything from that timestamp to now. Beware the units — `sdate` is Apex *local* time
while the record timestamps inside are true Unix epochs. There is no `edate` or `hours`
parameter; `days` only caps the span. The request runs at boot, on every reconnect, and as
a half-hour top-up every 10 minutes to heal gaps.

Two things make this awkward. A full day is **~725 KB** — the Apex logs 21 channels every
10 minutes, repeats timestamps, and supports neither gzip nor any field filter (all of
`did`, `type`, `name`, `inputs` and `probes` are accepted and ignored). And it has to
arrive without the panel freezing.

So `history.cpp` scans the response as a byte stream rather than parsing it: it tracks the
latest `"date"`, the latest `"name"`, and applies each `"value"` that follows a probe we
care about. Everything else streams past unread, nothing is buffered, and the scan is
resumable at any byte. `history_pump()` drains the socket for 20 ms per `loop()` pass and
returns, so the panel keeps painting and responding to touch throughout. Measured on this
board: 725 KB in 5-12 s depending on how fast the Apex feels like reading its SD card,
against a worst loop pass of ~114 ms versus a ~65 ms idle baseline.

The 20 ms budget matters more than it looks. At 4 ms the socket was drained more slowly
than the Apex filled it, the TCP window closed, and the same transfer took 40 s.

Because the Apex logs only once per 10 minutes, a backfilled column has a single value and
draws one pixel tall. Live polling gives a column a real low and high, so the chart fills
out into proper bars over the first day of uptime. Merging is always safe: a top-up folded
into a column the panel already watched can only confirm a value already inside the band.

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
- Audio (unused so far): an **NS4168** I2S mono class-D amp, 2.5 W — identified from the
  chip markings under a microscope, not just the schematic. The vendor also ships an
  AX98357A datasheet; that part is not on this board. Driven by BCLK GPIO 42, LRCLK GPIO 2,
  DIN GPIO 41 (from the schematic, not yet exercised in code). Its CTRL pin is tied high
  through 1 MΩ, so the amp is always enabled — there is no GPIO mute, and silence has to be
  fed as samples.
- **Two identical JST 1.25 2-pin connectors sit on this board and are easy to confuse.**
  `P7` is the speaker output; `P6` is `BAT+`/`BAT−` on the lithium charging circuit. A
  speaker on the battery header, or a cell on the amp output, damages something. Identify
  them by their neighbours: the speaker header sits beside the two small output-filter
  inductors `L4`/`L5`; the battery header sits by the charge controller, its larger inductor
  `L2`, and the side button.
- The board default partition table splits 4 MB into two 1.25 MB OTA slots, and the LVGL
  fonts alone push the image past 97% of one. `platformio.ini` selects `huge_app.csv`
  instead: a single 3 MB app partition, no OTA. Flashing is over USB.

## Vendor Documentation

The hardware facts above come from Guition's SDK bundle for this board:

```
https://pan.jczn1688.com/directlink/1/HMI%20display/JC4827W543.zip
```

Unpack it to `docs/JC4827W543/`, which is gitignored — it comes to ~290 MB of datasheets,
Windows binaries and `.rar` archives, none of it ours to version. Nothing in the build
depends on it; it is reference material for when the hardware misbehaves.

What is actually worth opening:

- **`5-IO pin distribution/`** — misnamed, and the most valuable folder in the bundle. The
  two PNGs are not pin tables but the board's full **schematics**: power, USB-C, SD slot,
  lithium charging, the LCD and touch connectors, and the audio amp. The `.xlsx` beside them
  is worthless — a bare ESP32-S3-WROOM-1 pin-number map with no peripheral assignments.

  Read the schematics with one caveat: sheet 2's title block says `ESP32-4827A043 v0.2`, not
  our part, and it carries an XPT2046 resistive-touch block and a 16-bit parallel LCD
  interface we do not have. It is a shared multi-variant sheet with unpopulated sections. It
  is still right for this board where it counts — the capacitive-touch FPC block shows
  `IO4` SCL / `IO8` SDA / `IO38` reset, and backlight on `IO1`, all matching Hardware Notes.
  Treat anything on it we have not confirmed in silicon as likely-but-unverified.
- **`2-Specification/`** and **`6-User_Manual/`** — English PDFs, panel timings and
  mechanical dimensions.
- **`4-Driver_IC_Data_Sheet/`** — ESP32-S3 and WROOM-1 datasheets, plus `Nsiway-NS4168.pdf`
  for the audio amp actually fitted. Ignore `AX98357AETE T_2021-01-06.PDF`: it is a renamed
  Maxim MAX98357A datasheet for a part this board does not use.
- **`1-Demo/Demo_Arduino/`** — a vendor fork of Arduino_GFX 1.4.4 carrying the NV3041A
  driver. `3_3-2-TFT-LVGL-Benchmark/LvglBenchmark/LvglBenchmark.ino:69` constructs the panel
  the same way `src/main.cpp` does; the Wi-Fi and BLE sketches are stock ESP32 examples.
- **`8-Burn operation/`** — factory-image `.bin` files and Espressif's Windows flash tool.
  The bins are the only thing here you cannot regenerate: they restore the board to its
  shipped demo firmware.

`latest initialization_4031A-01配IPS.docx` (duplicated inside `4-Driver_IC_Data_Sheet/`) is
worth a note, because the filename translates badly. 配 means "paired with", so it reads
"4031A-01, for the IPS panel" — 4031A-01 being the NV3041A. Inside is no prose at all: 96
`Write_Comm`/`Write_Data` pairs, the panel maker's own power-on register sequence. It is the
authority for the MADCTL claim in Display Orientation above — the sequence never writes
`0x36`, and neither does the bundled Arduino_GFX, so the panel really does run at its
power-on scan direction by design.

Skip **`7-Character&Picture_Molding_Tool/`** entirely. The name is a machine translation of
字符和图片取模工具; 取模 is bitmap extraction, not "molding". Despite that, the folder holds no
extraction tool — just five Windows archives repackaged from Chinese freeware portals, some
with binaries dated 2006. On Linux the CH340 driver is in-kernel, `pio device monitor`
replaces their serial terminal, and LVGL's `lv_font_conv` / `LVGLImage.py` do the bitmap
conversion the folder is named after (see `src/logo_mark.c`).

## Source Layout

| File | Role |
|---|---|
| `src/main.cpp` | board bring-up, LVGL init, poll scheduling |
| `src/model.h` / `.cpp` | `ReefState` — everything the UI draws |
| `src/apex.h` / `.cpp` | Apex HTTP client: poll, outlet writes, feed cycles |
| `src/history.h` / `.cpp` | streaming datalog backfill for the 24-hour sparklines |
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
