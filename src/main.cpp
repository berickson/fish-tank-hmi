#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <TouchLib.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

namespace {
constexpr char apex_mdns_name[] = "apex";
constexpr uint32_t apex_poll_interval_ms = 10000;
constexpr uint32_t wifi_connect_timeout_ms = 15000;
// Apex feed cycle index for "Feed Cycle A" (per the controller's /rest/status/feed/<index> API).
constexpr uint8_t feed_cycle_a_index = 1;
constexpr uint8_t backlight_pin = 1;
constexpr uint8_t touch_sda = 8;
constexpr uint8_t touch_scl = 4;
constexpr uint8_t touch_reset = 38;
constexpr int16_t screen_width = 480;
constexpr int16_t screen_height = 272;
constexpr uint16_t lvgl_buffer_rows = 24;

constexpr uint16_t background_color = 0xE7BE;
constexpr uint16_t header_color = 0xDFFB;
constexpr uint16_t text_color = 0x1928;
constexpr uint16_t accent_color = 0x2C55;
constexpr uint16_t button_color = 0xF468;

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
lv_obj_t *temperature_value;
lv_obj_t *feed_status;
lv_obj_t *feed_button;
lv_obj_t *cancel_button;
lv_obj_t *connection_status;
IPAddress apex_ip;
bool feed_active = false;

bool ensure_apex_ip() {
  if (apex_ip != INADDR_NONE) {
    return true;
  }

  const IPAddress resolved = MDNS.queryHost(apex_mdns_name);
  if (resolved == INADDR_NONE) {
    Serial.println("mDNS lookup for apex.local failed");
    return false;
  }

  apex_ip = resolved;
  Serial.printf("Resolved apex.local to %s\n", apex_ip.toString().c_str());
  return true;
}

void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(wifi_ssid, wifi_password);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < wifi_connect_timeout_ms) {
    delay(500);
    Serial.printf("WiFi status: %d\n", WiFi.status());
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin("fish-tank-hmi");
  } else {
    Serial.printf("WiFi connection failed, status: %d\n", WiFi.status());
  }
}

bool fetch_apex_status(float &temperature_f, int &feed_remaining_s) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!ensure_apex_ip()) {
    return false;
  }

  HTTPClient http;
  const String url = String("http://") + apex_ip.toString() + "/cgi-bin/status.json";
  http.begin(url);
  http.setAuthorization(apex_username, apex_password);

  const int http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    Serial.printf("Apex request failed, code: %d\n", http_code);
    http.end();
    apex_ip = INADDR_NONE;
    return false;
  }

  JsonDocument status_doc;
  const DeserializationError parse_error = deserializeJson(status_doc, http.getStream());
  http.end();

  if (parse_error) {
    Serial.printf("Apex JSON parse failed: %s\n", parse_error.c_str());
    return false;
  }

  feed_remaining_s = status_doc["istat"]["feed"]["active"].as<int>();

  for (JsonObject probe : status_doc["istat"]["inputs"].as<JsonArray>()) {
    const char *name = probe["name"];
    if (name != nullptr && strcmp(name, "Tmp") == 0) {
      temperature_f = probe["value"].as<float>();
      return true;
    }
  }

  Serial.println("Apex response had no Tmp probe");
  return false;
}

bool set_apex_feed_cycle(uint8_t name, bool active) {
  if (WiFi.status() != WL_CONNECTED || !ensure_apex_ip()) {
    return false;
  }

  HTTPClient http;
  // The cycle index must be in the URL path (matches the Apex app's own request);
  // a body-only "name" field is silently accepted but never actually starts the cycle.
  const String url = String("http://") + apex_ip.toString() + "/rest/status/feed/" + name;
  http.begin(url);
  http.setAuthorization(apex_username, apex_password);
  http.addHeader("Content-Type", "application/json");

  char body[64];
  snprintf(body, sizeof(body), "{\"name\":%u,\"active\":%u}", name, active ? 1 : 0);

  const int http_code = http.PUT(reinterpret_cast<uint8_t *>(body), strlen(body));
  http.end();

  if (http_code != HTTP_CODE_OK) {
    Serial.printf("Apex feed request failed, code: %d\n", http_code);
    return false;
  }

  return true;
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

void set_label_text(lv_obj_t *label, const char *text) {
  lv_label_set_text(label, text);
  lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -14, 11);
}

void feed_start_event(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }

  if (set_apex_feed_cycle(feed_cycle_a_index, true)) {
    feed_active = true;
    lv_obj_add_state(feed_button, LV_STATE_DISABLED);
    lv_obj_clear_state(cancel_button, LV_STATE_DISABLED);
    Serial.println("Feed cycle A started");
  } else {
    lv_label_set_text(feed_status, "Feed request failed");
  }
}

void feed_cancel_event(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }

  set_apex_feed_cycle(0, false);
  feed_active = false;
  lv_obj_clear_state(feed_button, LV_STATE_DISABLED);
  lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
  lv_label_set_text(feed_status, "Feed cancelled");
  Serial.println("Feed cycle cancelled");
}

lv_obj_t *create_card(lv_obj_t *parent, const char *title, int16_t x, int16_t y) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 218, 74);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_style_radius(card, 6, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF7FBF7), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xB5C9BB), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_pad_all(card, 10, 0);

  lv_obj_t *label = lv_label_create(card);
  lv_label_set_text(label, title);
  lv_obj_set_style_text_color(label, lv_color_hex(0x1928), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
  return card;
}

void create_dashboard() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0xE7F3EE), 0);

  lv_obj_t *header = lv_obj_create(screen);
  lv_obj_set_size(header, screen_width, 42);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(0xD5EEE1), 0);
  lv_obj_set_style_border_width(header, 0, 0);

  lv_obj_t *title = lv_label_create(header);
  lv_label_set_text(title, "Fish Tank");
  lv_obj_set_style_text_color(title, lv_color_hex(0x1928), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 14, 0);

  lv_obj_t *temperature_card = create_card(screen, "Water temperature", 14, 56);
  lv_obj_set_size(temperature_card, screen_width - 28, 74);
  temperature_value = lv_label_create(temperature_card);
  lv_obj_set_style_text_color(temperature_value, lv_color_hex(0x1928), 0);
  lv_obj_set_style_text_font(temperature_value, &lv_font_montserrat_18, 0);
  set_label_text(temperature_value, "-- F");

  lv_obj_t *feed_button_obj = lv_btn_create(screen);
  feed_button = feed_button_obj;
  lv_obj_set_size(feed_button_obj, 150, 50);
  lv_obj_set_pos(feed_button_obj, 14, 230);
  lv_obj_set_style_radius(feed_button_obj, 6, 0);
  lv_obj_set_style_bg_color(feed_button_obj, lv_color_hex(0xE88F4D), 0);
  lv_obj_add_event_cb(feed_button_obj, feed_start_event, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *feed_label = lv_label_create(feed_button_obj);
  lv_label_set_text(feed_label, "Feed Cycle A");
  lv_obj_set_style_text_color(feed_label, lv_color_hex(0x1928), 0);
  lv_obj_set_style_text_font(feed_label, &lv_font_montserrat_14, 0);
  lv_obj_center(feed_label);

  lv_obj_t *cancel_button_obj = lv_btn_create(screen);
  cancel_button = cancel_button_obj;
  lv_obj_set_size(cancel_button_obj, 100, 50);
  lv_obj_set_pos(cancel_button_obj, 174, 230);
  lv_obj_set_style_radius(cancel_button_obj, 6, 0);
  lv_obj_set_style_bg_color(cancel_button_obj, lv_color_hex(0xB5C9BB), 0);
  lv_obj_add_state(cancel_button_obj, LV_STATE_DISABLED);
  lv_obj_add_event_cb(cancel_button_obj, feed_cancel_event, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *cancel_label = lv_label_create(cancel_button_obj);
  lv_label_set_text(cancel_label, "Cancel");
  lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x1928), 0);
  lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_14, 0);
  lv_obj_center(cancel_label);

  feed_status = lv_label_create(screen);
  lv_label_set_text(feed_status, "Idle");
  lv_obj_set_style_text_color(feed_status, lv_color_hex(0x40665B), 0);
  lv_obj_set_style_text_font(feed_status, &lv_font_montserrat_14, 0);
  lv_obj_align(feed_status, LV_ALIGN_BOTTOM_RIGHT, -20, -14);

  connection_status = lv_label_create(screen);
  lv_label_set_text(connection_status, "Apex: connecting");
  lv_obj_set_style_text_color(connection_status, lv_color_hex(0x40665B), 0);
  lv_obj_set_style_text_font(connection_status, &lv_font_montserrat_14, 0);
  lv_obj_align(connection_status, LV_ALIGN_BOTTOM_LEFT, 20, -14);
}
}

void setup() {
  Serial.begin(115200);
  pinMode(backlight_pin, OUTPUT);
  digitalWrite(backlight_pin, HIGH);

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

  create_dashboard();
  Serial.println("Fish tank HMI started in simulation mode");

  connect_wifi();
}

void loop() {
  static uint32_t last_tick = 0;
  static uint32_t last_report = 0;
  static uint32_t last_apex_poll = 0;

  const uint32_t now = millis();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();

  // Poll more often while a feed cycle is active so the countdown stays current.
  const uint32_t poll_interval_ms = feed_active ? 2000 : apex_poll_interval_ms;
  if (now - last_apex_poll >= poll_interval_ms) {
    last_apex_poll = now;

    float temperature_f;
    int feed_remaining_s = 0;
    if (fetch_apex_status(temperature_f, feed_remaining_s)) {
      char text[16];
      snprintf(text, sizeof(text), "%.1f F", temperature_f);
      set_label_text(temperature_value, text);
      lv_label_set_text(connection_status, "Apex: connected");

      if (feed_remaining_s > 0) {
        feed_active = true;
        lv_obj_add_state(feed_button, LV_STATE_DISABLED);
        lv_obj_clear_state(cancel_button, LV_STATE_DISABLED);
        char feed_text[32];
        snprintf(feed_text, sizeof(feed_text), "Feeding: %d:%02d remaining", feed_remaining_s / 60,
                  feed_remaining_s % 60);
        lv_label_set_text(feed_status, feed_text);
      } else if (feed_active) {
        feed_active = false;
        lv_obj_clear_state(feed_button, LV_STATE_DISABLED);
        lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
        lv_label_set_text(feed_status, "Idle");
      }
    } else {
      set_label_text(temperature_value, "-- F");
      lv_label_set_text(connection_status, "Apex: unreachable");
    }
  }

  if (now - last_report >= 1000) {
    last_report = now;
    Serial.println("Fish tank HMI alive");
  }

  delay(5);
}
