#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TouchLib.h>
#include <Wire.h>
#include <lvgl.h>

#include "apex.h"
#include "history.h"
#include "model.h"
#include "settings.h"
#include "ui.h"
#include "wifi_manager.h"

namespace {
constexpr uint32_t apex_poll_interval_ms = 10000;
// Asleep the panel shows nothing, so back the polling right off.
constexpr uint32_t apex_sleep_poll_interval_ms = 60000;
// Every attempt at an absent Apex costs a blocking mDNS lookup and an HTTP
// connect timeout on the UI thread, so stretch the interval while it is away
// instead of stuttering every ten seconds. Polling never stops -- only the gap
// between attempts grows, and it snaps back to normal on the first good reply.
constexpr uint32_t apex_retry_max_interval_ms = 60000;
constexpr uint32_t health_report_interval_ms = 30000;
// The Apex logs every 10 minutes, so asking more often than that only re-reads
// what we already have. It is a few KB and exists to heal gaps, not to refresh:
// live polling fills the same columns with a real min and max.
constexpr uint32_t history_topup_interval_ms = 10UL * 60 * 1000;
constexpr uint8_t backlight_pin = 1;
constexpr uint8_t backlight_channel = 0;
constexpr uint32_t backlight_freq_hz = 5000;
constexpr uint8_t backlight_resolution_bits = 8;
constexpr uint8_t touch_sda = 8;
constexpr uint8_t touch_scl = 4;
constexpr uint8_t touch_reset = 38;
constexpr int16_t screen_width = 480;
constexpr int16_t screen_height = 272;
constexpr uint16_t lvgl_buffer_rows = 24;

Arduino_DataBus *display_bus = new Arduino_ESP32QSPI(
    45, 47, 21, 48, 40, 39);
Arduino_GFX *display_panel = new Arduino_NV3041A(
    display_bus, GFX_NOT_DEFINED, 0, true);
// Panel writes only work reliably for full-frame transfers, so draw into an
// off-screen canvas and flush the whole frame to the panel at once.
Arduino_GFX *display = new Arduino_Canvas(screen_width, screen_height, display_panel);
TouchLib touch(Wire, touch_sda, touch_scl, GT911_SLAVE_ADDRESS1);

lv_disp_draw_buf_t draw_buffer;
lv_color_t display_buffer[screen_width * lvgl_buffer_rows];

ReefState reef;

void set_backlight_percent(uint8_t percent) {
  const uint32_t duty = (static_cast<uint32_t>(percent) * 255) / 100;
  ledcWrite(backlight_channel, duty);
}

// The panel's own MADCTL rotation is unreachable on this bus: the NV3041A init
// sequence never writes 0x36, and Arduino_ESP32QSPI::write() hardcodes the
// GRAM-write address, so the driver's setRotation() sends the MADCTL parameter
// as a stray pixel instead of a register value. So the flip is done in the
// flush below, where it costs a reverse of one small LVGL buffer.
void apply_display_flip(bool flipped) { reef.display_flipped = flipped; }

void set_display_flipped(bool flipped) {
  apply_display_flip(flipped);
  settings_save_display_flipped(flipped);
  Serial.printf("Display orientation: %s (saved)\n", flipped ? "flipped 180" : "normal");
  // The canvas holds the old frame in the old orientation; redraw all of it.
  lv_obj_invalidate(lv_scr_act());
}

void display_flush(lv_disp_drv_t *display_driver, const lv_area_t *area, lv_color_t *color_data) {
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;
  uint16_t *pixels = reinterpret_cast<uint16_t *>(color_data);
  int16_t x = area->x1;
  int16_t y = area->y1;

  if (reef.display_flipped) {
    // Turning a rectangular block 180 degrees is exactly reversing its pixels in
    // memory order -- and the block itself lands at the mirrored position. LVGL
    // is finished with this buffer once we return, so reversing it in place is
    // safe, and it is at most a few thousand pixels of internal RAM.
    for (int32_t head = 0, tail = width * height - 1; head < tail; ++head, --tail) {
      const uint16_t swap = pixels[head];
      pixels[head] = pixels[tail];
      pixels[tail] = swap;
    }
    x = screen_width - 1 - area->x2;
    y = screen_height - 1 - area->y2;
  }

  display->draw16bitRGBBitmap(x, y, pixels, width, height);
  if (lv_disp_flush_is_last(display_driver)) {
    display->flush();
  }
  lv_disp_flush_ready(display_driver);
}

void touch_read(lv_indev_drv_t *input_driver, lv_indev_data_t *data) {
  if (touch.read()) {
    const TP_Point point = touch.getPoint(0);
    int16_t x = constrain(point.x, 0, screen_width - 1);
    int16_t y = constrain(point.y, 0, screen_height - 1);
    // The digitizer is glued to the glass and knows nothing about MADCTL, so a
    // flipped panel needs its coordinates mirrored to match what is on screen.
    if (reef.display_flipped) {
      x = screen_width - 1 - x;
      y = screen_height - 1 - y;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PR;
    return;
  }

  data->state = LV_INDEV_STATE_REL;
}

// Periodic health line. LVGL's heap is a fixed pool sized in lv_conf.h, so its
// usage is the number worth watching as screens grow.
void report_health() {
  lv_mem_monitor_t lvgl_mem;
  lv_mem_monitor(&lvgl_mem);

  Serial.printf(
      "health: lvgl %u%% used (frag %u%%, free %u B) | heap %u B free | apex %s | "
      "%.1fF pH %.2f %.1fppt | %u outlets, %u out of auto\n",
      lvgl_mem.used_pct, lvgl_mem.frag_pct, static_cast<unsigned>(lvgl_mem.free_size),
      static_cast<unsigned>(ESP.getFreeHeap()), reef.apex_up ? "up" : "down",
      reef.temperature.value, reef.ph.value, reef.salinity.value, reef.outlet_count,
      reef.override_count());
}

// Watch for a feed cycle starting. A cycle runs for minutes against a poll every
// few seconds, so this catches one whoever started it -- this panel, the Apex
// display or the phone app.
void record_feed_start() {
  static bool was_feeding = false;

  const bool feeding = reef.feed_remaining_s > 0;
  if (feeding && !was_feeding) {
    const uint32_t epoch = reef.now_epoch();
    if (epoch != 0) {
      reef.last_feed_epoch = epoch;
      // Seen with our own eyes this boot, so there is no doubt about it.
      reef.feed_confirmed = true;
      settings_save_last_feed(epoch);
      Serial.println("Feed cycle started");
    }
  }
  was_feeding = feeding;
}

// Turn poll results into the acknowledgeable history the Alerts tab shows.
void record_link_transition(bool now_up) {
  static bool was_up = false;
  static bool first = true;

  if (first) {
    first = false;
    was_up = now_up;
    return;
  }

  if (now_up != was_up) {
    if (now_up) {
      reef.add_event(EventKind::info, "Apex link restored", "Controller replying again");
      // However long the link was down for, the Apex logged through it.
      history_request_backfill();
    } else {
      reef.add_event(EventKind::danger, "Apex link lost", "No reply from controller");
    }
    was_up = now_up;
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);

  ledcSetup(backlight_channel, backlight_freq_hz, backlight_resolution_bits);
  ledcAttachPin(backlight_pin, backlight_channel);
  set_backlight_percent(100);

  pinMode(touch_reset, OUTPUT);
  digitalWrite(touch_reset, LOW);
  delay(200);
  digitalWrite(touch_reset, HIGH);
  delay(200);
  Wire.begin(touch_sda, touch_scl);
  touch.init();

  if (!display->begin()) {
    Serial.println("Display initialization failed");
    return;
  }

  settings_load(reef);
  apply_display_flip(reef.display_flipped);

  lv_init();
  lv_disp_draw_buf_init(&draw_buffer, display_buffer, nullptr, screen_width * lvgl_buffer_rows);

  static lv_disp_drv_t display_driver;
  lv_disp_drv_init(&display_driver);
  display_driver.hor_res = screen_width;
  display_driver.ver_res = screen_height;
  display_driver.flush_cb = display_flush;
  display_driver.draw_buf = &draw_buffer;
  lv_disp_drv_register(&display_driver);

  static lv_indev_drv_t input_driver;
  lv_indev_drv_init(&input_driver);
  input_driver.type = LV_INDEV_TYPE_POINTER;
  input_driver.read_cb = touch_read;
  lv_indev_drv_register(&input_driver);

  const UiHooks hooks = {set_backlight_percent, set_display_flipped};
  ui_create(&reef, hooks);

  // Paint the shell before the blocking Wi-Fi connect, so the panel comes up
  // looking alive instead of black for the first 15 seconds.
  ui_update();
  lv_timer_handler();

  Serial.printf("Reef controller starting, display %s\n",
                reef.display_flipped ? "flipped 180" : "normal");
  wifi_begin(reef);
}

void loop() {
  static uint32_t last_tick = 0;
  static uint32_t last_apex_poll = 0;
  static uint32_t last_health_report = 0;
  static uint32_t last_history_topup = 0;
  static uint32_t apex_retry_interval_ms = apex_poll_interval_ms;
  static bool first_poll_done = false;
  static bool history_seeded = false;

  const uint32_t now = millis();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();

  // Before the poll, so a link that just came back is known about -- and a link
  // that just went away is not spent waiting on HTTP timeouts.
  wifi_service(reef);
  wifi_poll_scan(reef);
  ui_service_actions();

  uint32_t interval = ui_is_asleep() ? apex_sleep_poll_interval_ms : apex_poll_interval_ms;
  if (!reef.apex_up && apex_retry_interval_ms > interval) {
    interval = apex_retry_interval_ms;
  }

  const bool retry_now = reef.apex_retry_requested;
  if (retry_now) {
    reef.apex_retry_requested = false;
    apex_retry_interval_ms = apex_poll_interval_ms;
  }

  if (!first_poll_done || retry_now || now - last_apex_poll >= interval) {
    last_apex_poll = now;
    first_poll_done = true;

    const bool ok = apex_poll(reef);
    if (!ok) {
      reef.apex_up = false;
    }
    apex_retry_interval_ms =
        ok ? apex_poll_interval_ms
           : min(apex_retry_interval_ms * 2, apex_retry_max_interval_ms);
    record_link_transition(ok);
    if (ok) {
      record_feed_start();
    }

    // The first poll is what tells us the time, so it is also the earliest the
    // datalog can be asked for anything.
    if (ok && !history_seeded) {
      history_seeded = true;
      last_history_topup = now;
      history_request_backfill();
    }
  }

  if (history_seeded && !history_busy() && now - last_history_topup >= history_topup_interval_ms) {
    last_history_topup = now;
    history_request_topup();
  }

  // Reads one slice of any datalog fetch in flight, bounded so the panel keeps
  // painting and responding to touch while a day of history streams in.
  history_pump(reef);

  ui_update();

  if (now - last_health_report >= health_report_interval_ms) {
    last_health_report = now;
    report_health();
  }

  // The idle delay is there to stop the loop spinning for nothing. While a day
  // of history is streaming in there is something to do, and the pump has its
  // own budget, so skip it.
  if (!history_busy()) {
    delay(5);
  }
}
