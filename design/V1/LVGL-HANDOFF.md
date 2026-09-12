# Reef Controller — LVGL implementation spec

Target: ESP32 + 4.3" capacitive panel, **480 × 272**, LVGL 8/9.
Reference design: `Reef Controller.dc.html` (open in a browser — it is an interactive mock of every screen and state).
Logo asset: `robonerd-mark.svg` — convert to a 1-bit/alpha `lv_img` (see Assets).

---

## 1. Layout grid (exact px)

| Region | Geometry |
|---|---|
| Status bar | y 0–30, full width, bg `#11141A`, 1px bottom border `#1D222A` |
| Content | y 30–234 (204 px tall), scrollable vertically only |
| Tab bar | y 234–272 (38 px), 4 equal cells of 120 px |
| Keyboard overlay | y 30–272 (covers content + tab bar, **never the status bar**) |

Content pages use 10 px side padding, 5–8 px gaps.

## 2. Palette

```
bg_screen    #0B0D10   panel       #12161C   bar         #11141A
border       #1E242C   border_hi   #262C34   key_bg      #1B212A
key_alt      #141A21
text         #E6EBF0   text_dim    #6F7A84   label_dim   #78838E
accent       #5FD3BF   (auto state, ok status, logo, primary button)
warn         #E9A23B   (out-of-auto, feed running, warn banner/badge)
danger       #E4655C   (apex link lost)
on_bg        #3F6F63 / on_ink #EAFAF6      manual-ON pill
off_bg       #4A3A24 / off_ink = warn      manual-OFF pill
warn_banner_bg #231C10
```

## 3. Type

- UI: Archivo 400/600/700 → embed as `lv_font` at **9, 10, 11, 12, 15 px**. Montserrat (LVGL built-in) is an acceptable substitute.
- Numerals: IBM Plex Mono 500 → **10, 14, 18, 27 px**.
- All small caps labels are uppercase with ~0.1–0.18em tracking; LVGL has no letter-spacing, so either bake tracking into the font or accept tighter labels.

## 4. Screens

### Status bar (always visible, all screens)
`[logo 15×17 accent] | [● wifi dot + "WI-FI"] | [● apex dot + "APEX"] ...spacer... [power button 44×30]`
- Both status pills are touch targets → jump to **Setup**.
- Dots: accent when ok, `#E4655C` when down. Apex label becomes `APEX ✕` when disconnected.
- Power button: circle glyph 13 px + stem, ink `#8D979F` → puts the panel to sleep immediately.

### Idle / sleep
Full black. Logo watermark 56×64 at 7 % opacity, 16 px above a 10 px accent dot pulsing 3.4 s (`opacity .22 → .6`, `scale 1 → 1.35`, ease-in-out). Word `TOUCH` 9 px `#1D2227` at the bottom.
Any touch wakes → Home. **No auto-sleep** (timeout configurable, default off). Backlight: drop PWM to ~2–4 %, not 0.

### Home
1. Warn banner (only if something is out of auto or Apex is down): 6×9 px padding, bg `#231C10`, 3 px left border warn, pulsing 7 px dot, text `"<Outlet> is OFF — not in auto"` or `"N outlets out of auto"`, `VIEW` on the right → Alerts.
2. Three reading cards in a 3-col grid, 8 px gap: label 9 px dim / value 27 px mono / **24 h sparkline 16 px tall** (`lv_chart`, line mode, no grid, accent, 1.6 px) / min–max 9 px mono. Readings: Temp °F, pH, Salinity ppt.
3. FEED button, 52 px tall, full width: label 15 px bold + 9 px subtitle. Idle = bg `#141A20`, border `#28303A`, accent ink. Running = bg `#231C10`, warn border/ink, `MM:SS` countdown next to the label.

### Control
One row per outlet: name 11 px + state line 9 px (`Auto · on` / `Manual OFF`), then three buttons `OFF 52×32`, `AUTO 58×32`, `ON 52×32`, 3 px gaps. Active AUTO = accent fill, dark ink; active ON = `#3F6F63`; active OFF = `#4A3A24` + warn ink; inactive = `#181D24` / `#7C8691`. Rows with mode ≠ auto get a warn-tinted border. Footer button: **RETURN ALL TO AUTO** (accent fill, 34 px).

### Alerts
List rows with 3 px colored left border, title 11 px, detail 10 px dim, timestamp 10 px mono. Sources: every outlet not in auto, apex link lost, historical sensor alarms. `ACKNOWLEDGE ALL` outlined button at the bottom.

### Setup
Wi-Fi card (SSID, IP · RSSI · security), Apex link card (state + `apex.local` + last reply, `RETRY` button 74×32), `NETWORKS` list (SSID, RSSI, `SAVED`/`JOIN` tag, 34 px rows) → opens keyboard, `RESCAN` button.

### Keyboard overlay
- Header 32 px: `CANCEL` | `Join <SSID>` | `JOIN` (accent fill, disabled until ≥ 8 chars).
- Field row: masked value (`•`) 14 px mono, `SHOW/HIDE` button 52×30.
- 4 rows filling the rest, 4 px gaps, keys flex-sized:
  - `q w e r t y u i o p`
  - (half-key inset) `a s d f g h j k l`
  - `⇧` (1.6×, accent when latched) `z x c v b n m` `⌫` (1.6×)
  - `123/abc` (1.8×) `space` (6×) `_` (1.2×) `clear` (1.8×)
- Symbol layer: `1234567890` / `- / : ; ( ) $ & @ "` / `. , ? ! ' * # % + =`
- Shift is one-shot (clears after a key). Max 63 chars.

## 5. State machine / behavior

- **Outlet mode** ∈ {off, auto, on}. Anything ≠ auto is an *override*: raises the Home banner, a warn badge with the count on the Control tab, and an Alerts entry. It never clears itself.
- **Feed**: sets the return pump to off for **15:00** (pre-programmed, not editable), counts down in the button, then restores it to auto automatically. Tapping while running cancels and restores immediately. The feed-induced override is *excluded* from the out-of-auto warning.
- **Return all to auto** clears every override and cancels feed.
- **Apex link**: poll; on failure show danger dot, `APEX ✕`, an alert, and a red state on the Setup card. Cache the last good values and keep showing them (dimmed is fine) rather than blanking.
- Wi-Fi has no off switch by design.

## 6. LVGL notes

- Build as 5 screens (`lv_obj_t*`) or one screen with swapped content containers; the status bar and tab bar are persistent parents so they don't re-create on tab change.
- Mode selectors: `lv_btnmatrix` (one row, 3 buttons, `LV_BTNMATRIX_CTRL_CHECKABLE` + one-of-many) is the cheapest match for OFF/AUTO/ON.
- Keyboard: a custom `lv_btnmatrix` map per layer, not `lv_keyboard`, to match the layout above; `lv_textarea` with `lv_textarea_set_password_mode`.
- Sparklines: `lv_chart` with `LV_CHART_TYPE_LINE`, 16 point series, `lv_chart_set_div_line_count(0,0)`, no points (`LV_PART_INDICATOR` size 0).
- Touch targets are ≥ 30 px; keep that floor if you re-flow anything.
- Use styles, not per-widget setters: one style per token above (`st_panel`, `st_key`, `st_accent_btn`, …).

## 7. Assets

- `robonerd-mark.svg` — the brand mark. Render to PNG at 15×17 and 56×64 (plus 2× if you keep a higher-DPI panel), export as `lv_img` with `LV_IMG_CF_ALPHA_8` and recolor with `lv_obj_set_style_img_recolor(accent)` so it follows the theme.
