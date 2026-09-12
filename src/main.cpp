#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TouchLib.h>
#include <Wire.h>
#include <lvgl.h>

#include "apex.h"
#include "model.h"
#include "ui.h"
#include "wifi_manager.h"

namespace {
constexpr uint32_t apex_poll_interval_ms = 10000;
// Asleep the panel shows nothing, so back the polling right off.
constexpr uint32_t apex_sleep_poll_interval_ms = 60000;
constexpr uint32_t health_report_interval_ms = 30000;
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

void display_flush(lv_disp_drv_t *display_driver, const lv_area_t *area, lv_color_t *color_data) {
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;

  display->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(color_data), width, height);
  if (lv_disp_flush_is_last(display_driver)) {
    display->flush();
  }
  lv_disp_flush_ready(display_driver);
}

void touch_read(lv_indev_drv_t *input_driver, lv_indev_data_t *data) {
  if (touch.read()) {
    const TP_Point point = touch.getPoint(0);
    data->point.x = constrain(point.x, 0, screen_width - 1);
    data->point.y = constrain(point.y, 0, screen_height - 1);
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

  const UiHooks hooks = {set_backlight_percent};
  ui_create(&reef, hooks);

  // Paint the shell before the blocking Wi-Fi connect, so the panel comes up
  // looking alive instead of black for the first 15 seconds.
  ui_update();
  lv_timer_handler();

  Serial.println("Reef controller starting");
  wifi_begin(reef);
}

void loop() {
  static uint32_t last_tick = 0;
  static uint32_t last_apex_poll = 0;
  static uint32_t last_health_report = 0;
  static bool first_poll_done = false;

  const uint32_t now = millis();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();

  wifi_poll_scan(reef);
  ui_service_actions();

  const uint32_t interval = ui_is_asleep() ? apex_sleep_poll_interval_ms : apex_poll_interval_ms;
  if (!first_poll_done || now - last_apex_poll >= interval) {
    last_apex_poll = now;
    first_poll_done = true;

    const bool ok = apex_poll(reef);
    if (!ok) {
      reef.apex_up = false;
    }
    record_link_transition(ok);
  }

  ui_update();

  if (now - last_health_report >= health_report_interval_ms) {
    last_health_report = now;
    report_health();
  }

  delay(5);
}
