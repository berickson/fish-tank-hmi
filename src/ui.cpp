#include "ui.h"

#include <WiFi.h>

#include "apex.h"
#include "theme.h"

// The built-in Montserrat fonts cover ASCII plus the degree sign and the bullet
// (and nothing else), so those two are the only non-ASCII glyphs used here --
// the design's middle dot and en-dash become a bullet and a hyphen.
#define GLYPH_DEGREE "\xC2\xB0"
#define GLYPH_BULLET "\xE2\x80\xA2"

extern const lv_img_dsc_t logo_mark_15;
extern const lv_img_dsc_t logo_mark_56;

namespace {

constexpr lv_coord_t status_bar_h = 30;
constexpr lv_coord_t tab_bar_h = 38;
constexpr lv_coord_t content_h = 272 - status_bar_h - tab_bar_h;  // 204
constexpr uint8_t tab_count = 4;
constexpr uint8_t alert_row_count = 10;
constexpr uint8_t feed_cycle_a_index = 1;
constexpr uint8_t backlight_awake_percent = 100;
// Sleep dims the panel rather than killing it; a true 0 makes some panels look
// broken rather than asleep.
constexpr uint8_t backlight_asleep_percent = 3;
constexpr uint32_t idle_refresh_period_ms = 100;

enum Tab : uint8_t { tab_home, tab_control, tab_alerts, tab_setup };

ReefState *state = nullptr;
UiHooks hooks = {};
uint8_t active_tab = tab_home;
bool asleep = false;

// User intent is queued rather than acted on inside the event callback: the
// Apex write blocks for up to 2 s, and doing it inline would stall LVGL before
// it ever painted the button the user just pressed.
struct PendingOutlet {
  char did[8];
  OutletMode mode;
};
PendingOutlet pending_outlets[max_outlets];
uint8_t pending_outlet_count = 0;
int8_t pending_feed = -1;  // -1 none, 0 cancel, 1 start

lv_obj_t *status_bar;
lv_obj_t *wifi_dot;
lv_obj_t *wifi_label;
lv_obj_t *apex_dot;
lv_obj_t *apex_label;

lv_obj_t *content;
lv_obj_t *pages[tab_count];
lv_obj_t *tab_cells[tab_count];
lv_obj_t *tab_labels[tab_count];
lv_obj_t *tab_badge;
lv_obj_t *tab_badge_label;

lv_obj_t *banner;
lv_obj_t *banner_label;

struct CardUi {
  lv_obj_t *value;
  lv_obj_t *chart;
  lv_obj_t *range;
  lv_chart_series_t *series;
};
CardUi cards[3];

lv_obj_t *feed_btn;
lv_obj_t *feed_label;
lv_obj_t *feed_clock;
lv_obj_t *feed_hint;

struct RowUi {
  lv_obj_t *row;
  lv_obj_t *name;
  lv_obj_t *state_label;
  lv_obj_t *matrix;
};
RowUi outlet_rows[max_outlets];
uint8_t outlet_row_count = 0;
lv_obj_t *control_page_footer;

struct AlertUi {
  lv_obj_t *row;
  lv_obj_t *title;
  lv_obj_t *detail;
  lv_obj_t *time;
};
AlertUi alert_rows[alert_row_count];

lv_obj_t *setup_wifi_ssid;
lv_obj_t *setup_wifi_meta;
lv_obj_t *setup_apex_state;
lv_obj_t *setup_apex_meta;

lv_obj_t *sleep_overlay;

const char *mode_matrix_map[] = {"OFF", "AUTO", "ON", ""};

// ---------------------------------------------------------------- helpers

lv_obj_t *make_box(lv_obj_t *parent, lv_style_t *style) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_add_style(box, style, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

void set_flex(lv_obj_t *obj, lv_flex_flow_t flow, lv_coord_t gap, lv_flex_align_t cross) {
  lv_obj_set_flex_flow(obj, flow);
  lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, cross, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(obj, gap, 0);
  lv_obj_set_style_pad_column(obj, gap, 0);
}

void set_pad(lv_obj_t *obj, lv_coord_t top, lv_coord_t side, lv_coord_t bottom) {
  lv_obj_set_style_pad_top(obj, top, 0);
  lv_obj_set_style_pad_bottom(obj, bottom, 0);
  lv_obj_set_style_pad_left(obj, side, 0);
  lv_obj_set_style_pad_right(obj, side, 0);
}

void show(lv_obj_t *obj, bool visible) {
  const bool visible_now = !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
  if (visible_now == visible) {
    return;  // already as asked; toggling the flag would invalidate for nothing
  }
  if (visible) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

void set_label(lv_obj_t *label, const char *text) {
  if (strcmp(lv_label_get_text(label), text) != 0) {
    lv_label_set_text(label, text);
  }
}

void anim_set_opa(void *obj, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(value), 0);
}

// The design's 3.4 s breathing dot on the idle screen, and the 1.6 s pulse on
// the warning banner.
void pulse_opacity(lv_obj_t *obj, uint32_t period_ms, lv_opa_t low, lv_opa_t high) {
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, obj);
  lv_anim_set_exec_cb(&anim, anim_set_opa);
  lv_anim_set_values(&anim, low, high);
  lv_anim_set_time(&anim, period_ms / 2);
  lv_anim_set_playback_time(&anim, period_ms / 2);
  lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
  lv_anim_start(&anim);
}

void go_to_tab(uint8_t tab) {
  active_tab = tab;
  for (uint8_t i = 0; i < tab_count; ++i) {
    show(pages[i], i == tab);
  }
}

// The panel can only be written a whole frame at a time, so anything that
// animates costs a full 480x272 flush per refresh. The idle screen's breathing
// dot would otherwise keep that running forever; at 100 ms a 3.4 s breath still
// looks smooth.
void set_refresh_period(uint32_t period_ms) {
  lv_disp_t *disp = lv_disp_get_default();
  if (disp != nullptr && disp->refr_timer != nullptr) {
    lv_timer_set_period(disp->refr_timer, period_ms);
  }
}

void wake() {
  if (!asleep) {
    return;
  }
  asleep = false;
  show(sleep_overlay, false);
  set_refresh_period(LV_DISP_DEF_REFR_PERIOD);
  if (hooks.set_backlight_percent != nullptr) {
    hooks.set_backlight_percent(backlight_awake_percent);
  }
  go_to_tab(tab_home);
}

void sleep_now() {
  if (asleep) {
    return;
  }
  asleep = true;
  show(sleep_overlay, true);
  set_refresh_period(idle_refresh_period_ms);
  if (hooks.set_backlight_percent != nullptr) {
    hooks.set_backlight_percent(backlight_asleep_percent);
  }
}

void queue_outlet(const char *did, OutletMode mode) {
  // Collapse repeats so hammering a row cannot overflow the queue.
  for (uint8_t i = 0; i < pending_outlet_count; ++i) {
    if (strcmp(pending_outlets[i].did, did) == 0) {
      pending_outlets[i].mode = mode;
      return;
    }
  }
  if (pending_outlet_count >= max_outlets) {
    return;
  }
  PendingOutlet &pending = pending_outlets[pending_outlet_count++];
  snprintf(pending.did, sizeof(pending.did), "%s", did);
  pending.mode = mode;
}

const char *mode_word(OutletMode mode) {
  switch (mode) {
    case OutletMode::off:
      return "OFF";
    case OutletMode::on:
      return "ON";
    default:
      return "AUTO";
  }
}

// ---------------------------------------------------------------- events

void tab_clicked(lv_event_t *event) {
  go_to_tab(static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void setup_shortcut_clicked(lv_event_t *) { go_to_tab(tab_setup); }

void banner_clicked(lv_event_t *) { go_to_tab(tab_alerts); }

void power_clicked(lv_event_t *) { sleep_now(); }

void sleep_overlay_clicked(lv_event_t *) { wake(); }

void mode_selected(lv_event_t *event) {
  lv_obj_t *matrix = lv_event_get_target(event);
  const uint8_t row = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (state == nullptr || row >= state->outlet_count) {
    return;
  }

  const uint16_t selected = lv_btnmatrix_get_selected_btn(matrix);
  if (selected == LV_BTNMATRIX_BTN_NONE) {
    return;
  }

  const OutletMode mode = selected == 0   ? OutletMode::off
                          : selected == 1 ? OutletMode::automatic
                                          : OutletMode::on;

  Outlet &outlet = state->outlets[row];
  if (outlet.mode == mode) {
    return;
  }

  // Show the new state immediately; the next poll is the source of truth.
  outlet.mode = mode;
  if (mode != OutletMode::automatic) {
    outlet.live = mode == OutletMode::on;
    char title[32];
    snprintf(title, sizeof(title), "%s left %s", outlet.name, mode_word(mode));
    state->add_event(EventKind::warn, title, "Manual override from the panel");
  }
  queue_outlet(outlet.did, mode);
}

void return_all_clicked(lv_event_t *) {
  if (state == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < state->outlet_count; ++i) {
    if (state->outlets[i].mode != OutletMode::automatic) {
      state->outlets[i].mode = OutletMode::automatic;
      queue_outlet(state->outlets[i].did, OutletMode::automatic);
    }
  }
  if (state->feeding()) {
    state->feed_remaining_s = 0;
    pending_feed = 0;
  }
  state->add_event(EventKind::info, "Returned all to auto", "Every outlet back on program");
}

void feed_clicked(lv_event_t *) {
  if (state == nullptr) {
    return;
  }
  if (state->feeding()) {
    state->feed_remaining_s = 0;
    pending_feed = 0;
    state->add_event(EventKind::info, "Feed cancelled", "Return pump resuming");
  } else {
    // Optimistic 15:00; the next poll replaces it with the Apex's own countdown.
    state->feed_remaining_s = 15 * 60;
    state->feed_sampled_ms = millis();
    pending_feed = 1;
    state->add_event(EventKind::info, "Feed cycle started", "Return pump off for 15 min");
  }
}

void ack_clicked(lv_event_t *) {
  if (state != nullptr) {
    state->clear_events();
  }
}

void retry_clicked(lv_event_t *) {
  apex_forget_address();
  if (state != nullptr) {
    state->apex_up = false;
  }
}

// Each button in the OFF/AUTO/ON selector needs its own checked color, which a
// single style cannot express, so they are painted here instead.
void mode_matrix_draw(lv_event_t *event) {
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(event);
  if (dsc->part != LV_PART_ITEMS || dsc->rect_dsc == nullptr) {
    return;
  }

  lv_obj_t *matrix = lv_event_get_target(event);
  const bool checked =
      lv_btnmatrix_has_btn_ctrl(matrix, static_cast<uint16_t>(dsc->id), LV_BTNMATRIX_CTRL_CHECKED);

  uint32_t bg = col_inactive_bg;
  uint32_t ink = col_inactive_ink;
  if (checked) {
    if (dsc->id == 0) {
      bg = col_off_bg;
      ink = col_warn;
    } else if (dsc->id == 1) {
      bg = col_accent;
      ink = col_bg_screen;
    } else {
      bg = col_on_bg;
      ink = col_on_ink;
    }
  }

  dsc->rect_dsc->bg_color = lv_color_hex(bg);
  dsc->rect_dsc->bg_opa = LV_OPA_COVER;
  dsc->rect_dsc->radius = 4;
  dsc->rect_dsc->border_width = 0;
  if (dsc->label_dsc != nullptr) {
    dsc->label_dsc->color = lv_color_hex(ink);
  }
}

// ---------------------------------------------------------------- build

void build_status_bar(lv_obj_t *screen) {
  status_bar = make_box(screen, &st_bar);
  lv_obj_set_size(status_bar, 480, status_bar_h);
  lv_obj_set_pos(status_bar, 0, 0);
  lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(status_bar, 1, 0);
  set_border_color(status_bar, 0x1D222A);
  set_flex(status_bar, LV_FLEX_FLOW_ROW, 10, LV_FLEX_ALIGN_CENTER);
  set_pad(status_bar, 0, 12, 0);

  lv_obj_t *logo = lv_img_create(status_bar);
  lv_img_set_src(logo, &logo_mark_15);
  lv_obj_set_style_img_recolor(logo, lv_color_hex(col_accent), 0);
  lv_obj_set_style_img_recolor_opa(logo, LV_OPA_COVER, 0);

  auto divider = [&]() {
    lv_obj_t *line = make_box(status_bar, &st_plain);
    lv_obj_set_size(line, 1, 12);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    set_bg_color(line, col_border_hi);
  };

  auto pill = [&](lv_obj_t **dot, lv_obj_t **label, const char *text) {
    lv_obj_t *group = make_box(status_bar, &st_plain);
    lv_obj_set_size(group, LV_SIZE_CONTENT, status_bar_h);
    set_flex(group, LV_FLEX_FLOW_ROW, 6, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(group, setup_shortcut_clicked, LV_EVENT_CLICKED, nullptr);
    *dot = make_dot(group, 6, col_accent);
    *label = make_label(group, text, font_tiny, 0x7D8791);
  };

  divider();
  pill(&wifi_dot, &wifi_label, "WI-FI");
  divider();
  pill(&apex_dot, &apex_label, "APEX");

  lv_obj_t *spacer = make_box(status_bar, &st_plain);
  lv_obj_set_height(spacer, 1);
  lv_obj_set_flex_grow(spacer, 1);

  lv_obj_t *power = make_box(status_bar, &st_plain);
  lv_obj_set_size(power, 44, status_bar_h);
  lv_obj_set_style_border_side(power, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_width(power, 1, 0);
  set_border_color(power, 0x1D222A);
  lv_obj_add_flag(power, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(power, power_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *glyph = make_label(power, LV_SYMBOL_POWER, font_body, col_outline_ink);
  lv_obj_center(glyph);
}

void build_tab_bar(lv_obj_t *screen) {
  lv_obj_t *bar = make_box(screen, &st_bar);
  lv_obj_set_size(bar, 480, tab_bar_h);
  lv_obj_set_pos(bar, 0, 272 - tab_bar_h);
  set_flex(bar, LV_FLEX_FLOW_ROW, 0, LV_FLEX_ALIGN_CENTER);

  static const char *labels[tab_count] = {"HOME", "CONTROL", "ALERTS", "SETUP"};
  for (uint8_t i = 0; i < tab_count; ++i) {
    lv_obj_t *cell = make_box(bar, &st_plain);
    lv_obj_set_size(cell, 120, tab_bar_h);
    set_flex(cell, LV_FLEX_FLOW_ROW, 5, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(cell, 2, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cell, tab_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(i)));

    tab_cells[i] = cell;
    tab_labels[i] = make_label(cell, labels[i], font_tiny, col_text_dim);

    if (i == tab_control) {
      tab_badge = make_box(cell, &st_plain);
      lv_obj_set_size(tab_badge, 16, 14);
      lv_obj_set_style_radius(tab_badge, 7, 0);
      lv_obj_set_style_bg_opa(tab_badge, LV_OPA_COVER, 0);
      set_bg_color(tab_badge, col_warn);
      tab_badge_label = make_label(tab_badge, "0", font_tiny, col_bg_screen);
      lv_obj_center(tab_badge_label);
      show(tab_badge, false);
    }
  }
}

lv_obj_t *build_page() {
  lv_obj_t *page = make_box(content, &st_page);
  lv_obj_set_size(page, 480, content_h);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_set_style_width(page, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(page, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(page, lv_color_hex(0x3A4048), LV_PART_SCROLLBAR);
  return page;
}

void build_home() {
  lv_obj_t *page = build_page();
  pages[tab_home] = page;
  set_flex(page, LV_FLEX_FLOW_COLUMN, 8, LV_FLEX_ALIGN_START);
  set_pad(page, 8, 10, 10);

  banner = make_box(page, &st_plain);
  lv_obj_set_size(banner, LV_PCT(100), 25);
  lv_obj_set_style_radius(banner, 5, 0);
  lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
  set_bg_color(banner, col_warn_banner);
  lv_obj_set_style_border_side(banner, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_width(banner, 3, 0);
  set_border_color(banner, col_warn);
  set_flex(banner, LV_FLEX_FLOW_ROW, 8, LV_FLEX_ALIGN_CENTER);
  set_pad(banner, 0, 9, 0);
  lv_obj_add_flag(banner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(banner, banner_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *banner_dot = make_dot(banner, 7, col_warn);
  pulse_opacity(banner_dot, 1600, LV_OPA_50, LV_OPA_COVER);
  banner_label = make_label(banner, "", font_small, col_warn);
  lv_obj_t *banner_spacer = make_box(banner, &st_plain);
  lv_obj_set_height(banner_spacer, 1);
  lv_obj_set_flex_grow(banner_spacer, 1);
  lv_obj_t *view = make_label(banner, "VIEW", font_tiny, col_warn);
  lv_obj_set_style_opa(view, LV_OPA_70, 0);

  lv_obj_t *card_row = make_box(page, &st_plain);
  lv_obj_set_size(card_row, LV_PCT(100), 86);
  set_flex(card_row, LV_FLEX_FLOW_ROW, 8, LV_FLEX_ALIGN_START);

  static const char *card_labels[3] = {"TEMP", "PH", "SALINITY"};
  static const char *card_units[3] = {GLYPH_DEGREE "F", "", "PPT"};
  for (uint8_t i = 0; i < 3; ++i) {
    lv_obj_t *card = make_box(card_row, &st_panel);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    set_flex(card, LV_FLEX_FLOW_COLUMN, 0, LV_FLEX_ALIGN_START);
    set_pad(card, 4, 8, 4);

    make_label(card, card_labels[i], font_tiny, col_label_dim);

    lv_obj_t *value_row = make_box(card, &st_plain);
    lv_obj_set_size(value_row, LV_PCT(100), LV_SIZE_CONTENT);
    set_flex(value_row, LV_FLEX_FLOW_ROW, 3, LV_FLEX_ALIGN_END);
    cards[i].value = make_label(value_row, "--", font_num_big, col_text);
    make_label(value_row, card_units[i], font_tiny, 0x6B757F);

    lv_obj_t *chart = lv_chart_create(card);
    lv_obj_set_size(chart, LV_PCT(100), 16);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, spark_points);
    lv_chart_set_div_line_count(chart, 0, 0);
    cards[i].chart = chart;
    cards[i].series = lv_chart_add_series(chart, lv_color_hex(col_accent), LV_CHART_AXIS_PRIMARY_Y);

    cards[i].range = make_label(card, "", font_tiny, col_range_dim);
  }

  feed_btn = make_box(page, &st_plain);
  lv_obj_set_size(feed_btn, LV_PCT(100), 52);
  lv_obj_set_style_radius(feed_btn, 6, 0);
  lv_obj_set_style_bg_opa(feed_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(feed_btn, 1, 0);
  lv_obj_set_flex_flow(feed_btn, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(feed_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(feed_btn, 2, 0);
  lv_obj_add_flag(feed_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(feed_btn, feed_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *feed_top = make_box(feed_btn, &st_plain);
  lv_obj_set_size(feed_top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  set_flex(feed_top, LV_FLEX_FLOW_ROW, 10, LV_FLEX_ALIGN_END);
  feed_label = make_label(feed_top, "FEED", font_label, col_accent);
  feed_clock = make_label(feed_top, "00:00", font_num_mid, col_warn);
  feed_hint = make_label(feed_btn, "PRE-PROGRAMMED 15 MIN CYCLE", font_tiny, 0x58626C);
}

void build_control() {
  lv_obj_t *page = build_page();
  pages[tab_control] = page;
  set_flex(page, LV_FLEX_FLOW_COLUMN, 5, LV_FLEX_ALIGN_START);
  set_pad(page, 6, 10, 10);

  control_page_footer = make_box(page, &st_plain);
  lv_obj_set_size(control_page_footer, LV_PCT(100), 34);
  lv_obj_set_style_radius(control_page_footer, 5, 0);
  lv_obj_set_style_bg_opa(control_page_footer, LV_OPA_COVER, 0);
  set_bg_color(control_page_footer, col_accent);
  lv_obj_add_flag(control_page_footer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(control_page_footer, return_all_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *label = make_label(control_page_footer, "RETURN ALL TO AUTO", font_small, col_bg_screen);
  lv_obj_center(label);
}

// Rows are built on demand the first time the Apex tells us how many outlets it
// has, then reused; they are never destroyed.
void ensure_outlet_rows(uint8_t needed) {
  lv_obj_t *page = pages[tab_control];
  while (outlet_row_count < needed && outlet_row_count < max_outlets) {
    const uint8_t index = outlet_row_count;
    RowUi &ui = outlet_rows[index];

    ui.row = make_box(page, &st_panel);
    lv_obj_set_size(ui.row, LV_PCT(100), 44);
    set_flex(ui.row, LV_FLEX_FLOW_ROW, 8, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(ui.row, 5, 0);
    lv_obj_set_style_pad_bottom(ui.row, 5, 0);
    lv_obj_set_style_pad_left(ui.row, 9, 0);
    lv_obj_set_style_pad_right(ui.row, 6, 0);

    lv_obj_t *info = make_box(ui.row, &st_plain);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    set_flex(info, LV_FLEX_FLOW_COLUMN, 0, LV_FLEX_ALIGN_START);
    ui.name = make_label(info, "", font_small, col_text_strong);
    ui.state_label = make_label(info, "", font_tiny, col_text_dim);

    ui.matrix = lv_btnmatrix_create(ui.row);
    lv_obj_remove_style_all(ui.matrix);
    lv_obj_add_style(ui.matrix, &st_mode_matrix, 0);
    lv_obj_set_size(ui.matrix, 170, 32);
    lv_btnmatrix_set_map(ui.matrix, mode_matrix_map);
    lv_btnmatrix_set_btn_width(ui.matrix, 0, 52);
    lv_btnmatrix_set_btn_width(ui.matrix, 1, 58);
    lv_btnmatrix_set_btn_width(ui.matrix, 2, 52);
    lv_btnmatrix_set_btn_ctrl_all(ui.matrix, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(ui.matrix, true);
    lv_obj_add_event_cb(ui.matrix, mode_selected, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
    lv_obj_add_event_cb(ui.matrix, mode_matrix_draw, LV_EVENT_DRAW_PART_BEGIN, nullptr);

    ++outlet_row_count;
    // Keep RETURN ALL TO AUTO below the rows, however many there turn out to be.
    lv_obj_move_foreground(control_page_footer);
  }
}

void build_alerts() {
  lv_obj_t *page = build_page();
  pages[tab_alerts] = page;
  set_flex(page, LV_FLEX_FLOW_COLUMN, 5, LV_FLEX_ALIGN_START);
  set_pad(page, 6, 10, 10);

  for (uint8_t i = 0; i < alert_row_count; ++i) {
    AlertUi &ui = alert_rows[i];
    ui.row = make_box(page, &st_panel);
    lv_obj_set_size(ui.row, LV_PCT(100), 42);
    // The design's 1 px outline plus a 3 px colored left edge cannot both live on
    // one object, and the colored edge is the part that carries meaning.
    lv_obj_set_style_border_side(ui.row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(ui.row, 3, 0);
    set_flex(ui.row, LV_FLEX_FLOW_ROW, 9, LV_FLEX_ALIGN_CENTER);
    set_pad(ui.row, 0, 9, 0);

    lv_obj_t *info = make_box(ui.row, &st_plain);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    set_flex(info, LV_FLEX_FLOW_COLUMN, 0, LV_FLEX_ALIGN_START);
    ui.title = make_label(info, "", font_small, col_text_strong);
    ui.detail = make_label(info, "", font_tiny, col_text_dim);
    ui.time = make_label(ui.row, "", font_tiny, col_range_dim);

    show(ui.row, false);
  }

  lv_obj_t *ack = make_box(page, &st_plain);
  lv_obj_set_size(ack, LV_PCT(100), 32);
  lv_obj_set_style_radius(ack, 5, 0);
  lv_obj_set_style_bg_opa(ack, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ack, 1, 0);
  set_border_color(ack, col_border_hi);
  lv_obj_add_flag(ack, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ack, ack_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *label = make_label(ack, "ACKNOWLEDGE ALL", font_tiny, col_outline_ink);
  lv_obj_center(label);
}

void build_setup() {
  lv_obj_t *page = build_page();
  pages[tab_setup] = page;
  set_flex(page, LV_FLEX_FLOW_COLUMN, 6, LV_FLEX_ALIGN_START);
  set_pad(page, 6, 10, 10);

  lv_obj_t *wifi_card = make_box(page, &st_panel);
  lv_obj_set_size(wifi_card, LV_PCT(100), LV_SIZE_CONTENT);
  set_flex(wifi_card, LV_FLEX_FLOW_COLUMN, 0, LV_FLEX_ALIGN_START);
  set_pad(wifi_card, 8, 9, 8);
  make_label(wifi_card, "WI-FI", font_tiny, col_label_dim);
  setup_wifi_ssid = make_label(wifi_card, "--", font_small, col_text_strong);
  setup_wifi_meta = make_label(wifi_card, "--", font_tiny, col_text_dim);

  lv_obj_t *apex_card = make_box(page, &st_panel);
  lv_obj_set_size(apex_card, LV_PCT(100), LV_SIZE_CONTENT);
  set_flex(apex_card, LV_FLEX_FLOW_ROW, 9, LV_FLEX_ALIGN_CENTER);
  set_pad(apex_card, 8, 9, 8);

  lv_obj_t *apex_info = make_box(apex_card, &st_plain);
  lv_obj_set_height(apex_info, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(apex_info, 1);
  set_flex(apex_info, LV_FLEX_FLOW_COLUMN, 0, LV_FLEX_ALIGN_START);
  make_label(apex_info, "APEX LINK", font_tiny, col_label_dim);
  setup_apex_state = make_label(apex_info, "--", font_small, col_accent);
  setup_apex_meta = make_label(apex_info, "apex.local", font_tiny, col_text_dim);

  lv_obj_t *retry = make_box(apex_card, &st_plain);
  lv_obj_set_size(retry, 74, 32);
  lv_obj_set_style_radius(retry, 4, 0);
  lv_obj_set_style_bg_opa(retry, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(retry, 1, 0);
  set_border_color(retry, 0x2B323B);
  lv_obj_add_flag(retry, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(retry, retry_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *retry_label = make_label(retry, "RETRY", font_tiny, 0xAAB4BD);
  lv_obj_center(retry_label);

  make_label(page, "NETWORK SELECTION COMES IN THE NEXT RELEASE", font_tiny, col_range_dim);
}

void build_sleep(lv_obj_t *screen) {
  sleep_overlay = make_box(screen, &st_plain);
  lv_obj_set_size(sleep_overlay, 480, 272);
  lv_obj_set_pos(sleep_overlay, 0, 0);
  lv_obj_set_style_bg_opa(sleep_overlay, LV_OPA_COVER, 0);
  set_bg_color(sleep_overlay, 0x000000);
  lv_obj_set_flex_flow(sleep_overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sleep_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(sleep_overlay, 16, 0);
  lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sleep_overlay, sleep_overlay_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *watermark = lv_img_create(sleep_overlay);
  lv_img_set_src(watermark, &logo_mark_56);
  lv_obj_set_style_img_recolor(watermark, lv_color_hex(col_accent), 0);
  lv_obj_set_style_img_recolor_opa(watermark, LV_OPA_COVER, 0);
  lv_obj_set_style_img_opa(watermark, LV_OPA_10, 0);

  lv_obj_t *dot = make_dot(sleep_overlay, 10, col_accent);
  pulse_opacity(dot, 3400, LV_OPA_20, LV_OPA_60);

  lv_obj_t *touch = make_label(sleep_overlay, "TOUCH", font_tiny, 0x1D2227);
  lv_obj_align(touch, LV_ALIGN_BOTTOM_MID, 0, -14);

  show(sleep_overlay, false);
}

// ---------------------------------------------------------------- update

void update_status_bar() {
  set_bg_color(wifi_dot, state->wifi_up ? col_accent : col_danger);
  set_label(wifi_label, state->wifi_up ? "WI-FI" : "NO WI-FI");
  set_bg_color(apex_dot, state->apex_up ? col_accent : col_danger);
  set_label(apex_label, state->apex_up ? "APEX" : "APEX X");
}

void update_tabs() {
  for (uint8_t i = 0; i < tab_count; ++i) {
    const bool active = i == active_tab;
    set_border_opa(tab_cells[i], active ? LV_OPA_COVER : LV_OPA_TRANSP);
    set_border_color(tab_cells[i], col_accent);
    set_bg_opa(tab_cells[i], active ? LV_OPA_COVER : LV_OPA_TRANSP);
    set_bg_color(tab_cells[i], col_tab_active_bg);
    set_text_color(tab_labels[i], active ? col_text : col_text_dim);
  }

  const uint8_t warnings = state->warn_count();
  show(tab_badge, warnings > 0);
  if (warnings > 0) {
    char text[4];
    snprintf(text, sizeof(text), "%u", warnings);
    set_label(tab_badge_label, text);
  }
}

void update_card(CardUi &card, const Reading &reading, uint8_t decimals, bool redraw_trend) {
  char text[12];
  if (reading.valid) {
    snprintf(text, sizeof(text), "%.*f", decimals, reading.value);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  set_label(card.value, text);
  // A stale reading is dimmed rather than blanked, so the last known value stays
  // readable while the Apex is unreachable.
  set_text_color(card.value, state->apex_up ? col_text : col_text_dim);

  const Trend &trend = reading.trend;
  if (trend.count < 2) {
    show(card.chart, false);
    set_label(card.range, "");
    return;
  }

  show(card.chart, true);
  float low = 0.0f;
  float high = 0.0f;
  trend.range(low, high);

  char range_text[24];
  snprintf(range_text, sizeof(range_text), "%.*f - %.*f", decimals, low, decimals, high);
  set_label(card.range, range_text);

  // Writing chart points invalidates the chart, and there is nothing new to draw
  // between polls, so only do it when the sample ring actually moved.
  if (!redraw_trend) {
    return;
  }

  // lv_coord_t is 16-bit here, so readings are carried as hundredths.
  lv_coord_t low_scaled = static_cast<lv_coord_t>(low * 100.0f);
  lv_coord_t high_scaled = static_cast<lv_coord_t>(high * 100.0f);
  if (high_scaled <= low_scaled) {
    low_scaled -= 1;
    high_scaled += 1;
  }
  lv_chart_set_range(card.chart, LV_CHART_AXIS_PRIMARY_Y, low_scaled, high_scaled);

  // Right-align the samples so a part-full trend grows in from the right.
  const uint8_t blank = spark_points - trend.count;
  for (uint8_t i = 0; i < spark_points; ++i) {
    const lv_coord_t value =
        i < blank ? LV_CHART_POINT_NONE
                  : static_cast<lv_coord_t>(trend.points[i - blank] * 100.0f);
    lv_chart_set_value_by_id(card.chart, card.series, i, value);
  }
}

void update_home() {
  const uint8_t warnings = state->warn_count();
  show(banner, warnings > 0);
  if (warnings > 0) {
    char text[48];
    const uint8_t overrides = state->override_count();
    if (overrides == 1 && state->apex_up) {
      for (uint8_t i = 0; i < state->outlet_count; ++i) {
        if (state->outlets[i].mode != OutletMode::automatic) {
          snprintf(text, sizeof(text), "%s is %s - not in auto", state->outlets[i].name,
                   mode_word(state->outlets[i].mode));
          break;
        }
      }
    } else if (overrides == 0) {
      snprintf(text, sizeof(text), "Apex link lost");
    } else {
      snprintf(text, sizeof(text), "%u outlets out of auto", warnings);
    }
    set_label(banner_label, text);
  }

  static uint32_t drawn_revision = 0;
  const bool redraw_trend = drawn_revision != state->revision;
  drawn_revision = state->revision;
  update_card(cards[0], state->temperature, 1, redraw_trend);
  update_card(cards[1], state->ph, 2, redraw_trend);
  update_card(cards[2], state->salinity, 1, redraw_trend);

  const bool feeding = state->feeding();
  set_bg_color(feed_btn, feeding ? col_warn_banner : col_feed_idle_bg);
  set_border_color(feed_btn, feeding ? col_warn : col_feed_idle_border);
  set_text_color(feed_label, feeding ? col_warn : col_accent);
  set_label(feed_label, feeding ? "FEEDING" : "FEED");
  set_label(feed_hint, feeding ? "TAP TO CANCEL " GLYPH_BULLET " RETURN RESUMES"
                               : "PRE-PROGRAMMED 15 MIN CYCLE");

  show(feed_clock, feeding);
  if (feeding) {
    // The countdown comes from the Apex, so clamp it rather than trusting it to
    // fit MM:SS.
    constexpr int longest_clock_s = 99 * 60 + 59;
    const int reported = state->live_feed_seconds();
    const int remaining = reported < 0 ? 0 : (reported > longest_clock_s ? longest_clock_s : reported);
    char clock[12];
    snprintf(clock, sizeof(clock), "%02d:%02d", remaining / 60, remaining % 60);
    set_label(feed_clock, clock);
  }
}

void update_control() {
  ensure_outlet_rows(state->outlet_count);

  for (uint8_t i = 0; i < outlet_row_count; ++i) {
    RowUi &ui = outlet_rows[i];
    if (i >= state->outlet_count) {
      show(ui.row, false);
      continue;
    }
    show(ui.row, true);

    const Outlet &outlet = state->outlets[i];
    set_label(ui.name, outlet.name);

    char text[24];
    if (outlet.mode == OutletMode::automatic) {
      snprintf(text, sizeof(text), "AUTO " GLYPH_BULLET " %s", outlet.live ? "ON" : "OFF");
    } else {
      snprintf(text, sizeof(text), "MANUAL %s", mode_word(outlet.mode));
    }
    set_label(ui.state_label, text);
    set_text_color(ui.state_label, outlet.mode == OutletMode::automatic ? col_text_dim : col_warn);

    const bool in_auto = outlet.mode == OutletMode::automatic;
    set_border_color(ui.row, in_auto ? col_border : col_warn);
    set_border_opa(ui.row, in_auto ? LV_OPA_COVER : LV_OPA_40);

    const uint16_t checked = outlet.mode == OutletMode::off  ? 0
                             : outlet.mode == OutletMode::on ? 2
                                                             : 1;
    for (uint16_t button = 0; button < 3; ++button) {
      const bool want = button == checked;
      if (lv_btnmatrix_has_btn_ctrl(ui.matrix, button, LV_BTNMATRIX_CTRL_CHECKED) == want) {
        continue;
      }
      if (want) {
        lv_btnmatrix_set_btn_ctrl(ui.matrix, button, LV_BTNMATRIX_CTRL_CHECKED);
      } else {
        lv_btnmatrix_clear_btn_ctrl(ui.matrix, button, LV_BTNMATRIX_CTRL_CHECKED);
      }
    }
  }
}

void update_alerts() {
  uint8_t row = 0;

  auto emit = [&](EventKind kind, const char *title, const char *detail, const char *time) {
    if (row >= alert_row_count) {
      return;
    }
    AlertUi &ui = alert_rows[row++];
    show(ui.row, true);
    set_label(ui.title, title);
    set_label(ui.detail, detail);
    set_label(ui.time, time);
    const uint32_t ink = kind == EventKind::danger ? col_danger
                         : kind == EventKind::warn ? col_warn
                                                   : col_accent;
    set_border_color(ui.row, ink);
  };

  // Live problems come first and cannot be acknowledged away -- they clear only
  // when the thing causing them is actually fixed.
  for (uint8_t i = 0; i < state->outlet_count; ++i) {
    const Outlet &outlet = state->outlets[i];
    if (outlet.mode == OutletMode::automatic) {
      continue;
    }
    char title[40];
    snprintf(title, sizeof(title), "%s left %s", outlet.name, mode_word(outlet.mode));
    emit(EventKind::warn, title, "Manual override - not following program", "now");
  }

  if (!state->apex_up) {
    emit(EventKind::danger, "Apex link lost", "No reply from controller", "now");
  }

  for (uint8_t i = 0; i < state->event_count; ++i) {
    const Event &event = state->events[i];
    emit(event.kind, event.title, event.detail, event.time);
  }

  if (row == 0) {
    emit(EventKind::info, "All clear", "Everything on program", "-");
  }

  for (uint8_t i = row; i < alert_row_count; ++i) {
    show(alert_rows[i].row, false);
  }
}

void update_setup() {
  // WiFi.SSID() builds a String, so this is paced rather than run every frame.
  static uint32_t last_run = 0;
  const uint32_t now = millis();
  if (last_run != 0 && now - last_run < 500) {
    return;
  }
  last_run = now;

  char text[64];

  set_label(setup_wifi_ssid, state->wifi_up ? WiFi.SSID().c_str() : "Not connected");
  if (state->wifi_up) {
    snprintf(text, sizeof(text), "%s " GLYPH_BULLET " %d dBm", WiFi.localIP().toString().c_str(),
             WiFi.RSSI());
  } else {
    snprintf(text, sizeof(text), "No network");
  }
  set_label(setup_wifi_meta, text);

  set_label(setup_apex_state, state->apex_up ? "Connected" : "Disconnected");
  set_text_color(setup_apex_state, state->apex_up ? col_accent : col_danger);

  if (state->ever_connected) {
    const uint32_t age_s = (millis() - state->last_reply_ms) / 1000;
    snprintf(text, sizeof(text), "apex.local " GLYPH_BULLET " last reply %us ago",
             static_cast<unsigned>(age_s));
  } else {
    snprintf(text, sizeof(text), "apex.local " GLYPH_BULLET " no reply yet");
  }
  set_label(setup_apex_meta, text);
}

}  // namespace

void ui_create(ReefState *reef_state, const UiHooks &ui_hooks) {
  state = reef_state;
  hooks = ui_hooks;

  theme_init();

  lv_obj_t *screen = lv_scr_act();
  lv_obj_remove_style_all(screen);
  lv_obj_add_style(screen, &st_screen, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  build_status_bar(screen);

  content = make_box(screen, &st_page);
  lv_obj_set_size(content, 480, content_h);
  lv_obj_set_pos(content, 0, status_bar_h);

  build_home();
  build_control();
  build_alerts();
  build_setup();
  build_tab_bar(screen);
  build_sleep(screen);

  go_to_tab(tab_home);
}

void ui_update() {
  if (state == nullptr) {
    return;
  }
  update_status_bar();
  update_tabs();

  // Only the visible page is worth refreshing; the other three are hidden and
  // rebuilding them would just burn cycles.
  switch (active_tab) {
    case tab_home:
      update_home();
      break;
    case tab_control:
      update_control();
      break;
    case tab_alerts:
      update_alerts();
      break;
    case tab_setup:
      update_setup();
      break;
    default:
      break;
  }
}

void ui_service_actions() {
  if (state == nullptr) {
    return;
  }

  // One blocking Apex write per call, so the panel keeps redrawing in between.
  if (pending_feed >= 0) {
    const bool start = pending_feed == 1;
    // Cancelling addresses cycle 0, which is how the Apex expects a stop.
    apex_set_feed_cycle(start ? feed_cycle_a_index : 0, start);
    pending_feed = -1;
    return;
  }

  if (pending_outlet_count == 0) {
    return;
  }

  const PendingOutlet pending = pending_outlets[0];
  --pending_outlet_count;
  memmove(pending_outlets, pending_outlets + 1, sizeof(PendingOutlet) * pending_outlet_count);

  if (!apex_set_outlet(pending.did, pending.mode)) {
    // The optimistic repaint was wrong; say so rather than showing a state the
    // Apex never accepted. The next poll restores the truth.
    Outlet *outlet = state->find_outlet(pending.did);
    char title[32];
    snprintf(title, sizeof(title), "%s not changed", outlet != nullptr ? outlet->name : pending.did);
    state->add_event(EventKind::danger, title, "Apex refused the command");
  }
}

bool ui_is_asleep() { return asleep; }
