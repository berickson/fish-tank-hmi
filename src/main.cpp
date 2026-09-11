#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <TouchLib.h>
#include <Wire.h>

namespace {
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

void feed_button_event(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_label_set_text(feed_status, "Feeding requested");
    Serial.println("Simulated feed requested");
  }
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
  temperature_value = lv_label_create(temperature_card);
  lv_obj_set_style_text_color(temperature_value, lv_color_hex(0x1928), 0);
  lv_obj_set_style_text_font(temperature_value, &lv_font_montserrat_18, 0);
  set_label_text(temperature_value, "76.4 F");

  lv_obj_t *heater_card = create_card(screen, "Heater", 248, 56);
  lv_obj_t *heater_switch = lv_switch_create(heater_card);
  lv_obj_align(heater_switch, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_state(heater_switch, LV_STATE_CHECKED);

  lv_obj_t *lights_card = create_card(screen, "Lights", 14, 142);
  lv_obj_t *lights_switch = lv_switch_create(lights_card);
  lv_obj_align(lights_switch, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_state(lights_switch, LV_STATE_CHECKED);

  lv_obj_t *filter_card = create_card(screen, "Filter", 248, 142);
  lv_obj_t *filter_switch = lv_switch_create(filter_card);
  lv_obj_align(filter_switch, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_state(filter_switch, LV_STATE_CHECKED);

  lv_obj_t *feed_button = lv_btn_create(screen);
  lv_obj_set_size(feed_button, 218, 40);
  lv_obj_set_pos(feed_button, 14, 230);
  lv_obj_set_style_radius(feed_button, 6, 0);
  lv_obj_set_style_bg_color(feed_button, lv_color_hex(0xE88F4D), 0);
  lv_obj_add_event_cb(feed_button, feed_button_event, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *feed_label = lv_label_create(feed_button);
  lv_label_set_text(feed_label, "Feed fish");
  lv_obj_set_style_text_color(feed_label, lv_color_hex(0x1928), 0);
  lv_obj_set_style_text_font(feed_label, &lv_font_montserrat_18, 0);
  lv_obj_center(feed_label);

  feed_status = lv_label_create(screen);
  lv_label_set_text(feed_status, "Simulation mode");
  lv_obj_set_style_text_color(feed_status, lv_color_hex(0x40665B), 0);
  lv_obj_set_style_text_font(feed_status, &lv_font_montserrat_14, 0);
  lv_obj_align(feed_status, LV_ALIGN_BOTTOM_RIGHT, -20, -14);
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
}

void loop() {
  static uint32_t last_tick = 0;
  static uint32_t last_report = 0;

  const uint32_t now = millis();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();

  if (now - last_report >= 1000) {
    last_report = now;
    Serial.println("Fish tank HMI alive");
  }

  delay(5);
}
