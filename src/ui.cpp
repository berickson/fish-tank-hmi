#include "ui.h"

#include <WiFi.h>

#include "apex.h"
#include "theme.h"
#include "wifi_manager.h"

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
constexpr uint32_t join_result_linger_ms = 5000;

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
  lv_obj_t *spark;
  lv_obj_t *range;
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
lv_obj_t *setup_scan_note;
lv_obj_t *setup_display_state;

struct NetworkUi {
  lv_obj_t *row;
  lv_obj_t *ssid;
  lv_obj_t *rssi;
  lv_obj_t *tag;
};
NetworkUi network_rows[max_networks];

lv_obj_t *sleep_overlay;

// ------------------------------------------------------- keyboard overlay

lv_obj_t *kb_overlay;
lv_obj_t *kb_title;
lv_obj_t *kb_join_btn;
lv_obj_t *kb_join_label;
lv_obj_t *kb_field;
lv_obj_t *kb_reveal_label;
lv_obj_t *kb_matrix;

enum class KbLayer : uint8_t { lower, upper, symbols };
KbLayer kb_layer = KbLayer::lower;
// Swapping layers re-maps the matrix, which is unsafe to do from inside an event
// callback, so the key handler only asks for it and update_keyboard() does it.
bool kb_layout_dirty = false;
bool kb_pressing = false;
bool kb_reveal = false;
char kb_ssid[33] = {};
char kb_password[max_password_len + 1] = {};
uint8_t kb_length = 0;
// Masked display: one bullet per character, so it needs three bytes each.
char kb_display[max_password_len * 3 + 1];

// WPA2 personal will not accept anything shorter, so JOIN stays inert until
// there are at least this many characters.
constexpr uint8_t kb_min_password = 8;

// The maps are handed to LVGL by pointer and never copied, so they must outlive
// every call -- hence file scope rather than locals.
const char *kb_map_lower[] = {"q",           "w", "e", "r", "t", "y", "u", "i", "o",
                              "p",           "\n", " ", "a", "s", "d", "f", "g", "h",
                              "j",           "k", "l", " ", "\n", LV_SYMBOL_UP, "z", "x", "c",
                              "v",           "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
                              "123",         "space", "_", "clear", ""};

const char *kb_map_upper[] = {"Q",           "W", "E", "R", "T", "Y", "U", "I", "O",
                              "P",           "\n", " ", "A", "S", "D", "F", "G", "H",
                              "J",           "K", "L", " ", "\n", LV_SYMBOL_UP, "Z", "X", "C",
                              "V",           "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
                              "123",         "space", "_", "clear", ""};

const char *kb_map_symbols[] = {"1",   "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
                                "-",   "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
                                LV_SYMBOL_UP, ".", ",", "?", "!", "'", "*", "#", "%", "+", "=",
                                LV_SYMBOL_BACKSPACE, "\n",
                                "abc", "space", "_", "clear", ""};

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
  // The third argument places the track itself on the cross axis. Leaving it at
  // START pinned single-track rows to the top of their container even when the
  // items inside the track were centred, so it follows `cross` too.
  lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, cross, cross);
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

void kb_close();

void go_to_tab(uint8_t tab) {
  // The status bar stays live above the keyboard, so its shortcuts -- and the
  // power button, via wake() -- can land here with the overlay still up.
  // Leaving it would strand it over whichever page we switch to.
  kb_close();
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

void flip_clicked(lv_event_t *) {
  if (state != nullptr && hooks.set_display_flipped != nullptr) {
    hooks.set_display_flipped(!state->display_flipped);
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

// ------------------------------------------------------- keyboard events

void kb_render_display() {
  if (kb_length == 0) {
    snprintf(kb_display, sizeof(kb_display), "Password");
    return;
  }
  if (kb_reveal) {
    snprintf(kb_display, sizeof(kb_display), "%s", kb_password);
    return;
  }
  char *out = kb_display;
  for (uint8_t i = 0; i < kb_length; ++i) {
    memcpy(out, GLYPH_BULLET, 3);
    out += 3;
  }
  *out = '\0';
}

// set_map only clears the control bits when the button count changes (so
// lower <-> upper keeps the old ones), which is why widths and flags are
// re-applied unconditionally here.
void apply_kb_layout() {
  const bool symbols = kb_layer == KbLayer::symbols;
  const char **map = symbols                    ? kb_map_symbols
                     : kb_layer == KbLayer::upper ? kb_map_upper
                                                  : kb_map_lower;
  lv_btnmatrix_set_map(kb_matrix, map);

  auto width = [&](uint16_t from, uint16_t to, uint8_t units) {
    for (uint16_t i = from; i <= to; ++i) {
      lv_btnmatrix_set_btn_width(kb_matrix, i, units);
    }
  };

  // A button matrix re-fires VALUE_CHANGED every 100 ms once a press passes the
  // long-press threshold. On a keyboard that turns a slightly long tap into a
  // burst of repeats, so it is off everywhere except backspace, where holding to
  // delete is the one place it helps.
  auto set_repeat_policy = [&](uint16_t backspace_id) {
    lv_btnmatrix_set_btn_ctrl_all(kb_matrix, LV_BTNMATRIX_CTRL_NO_REPEAT);
    lv_btnmatrix_clear_btn_ctrl(kb_matrix, backspace_id, LV_BTNMATRIX_CTRL_NO_REPEAT);
  };

  if (symbols) {
    width(0, 19, 5);
    lv_btnmatrix_set_btn_width(kb_matrix, 20, 8);  // shift, returns to caps
    width(21, 30, 5);
    lv_btnmatrix_set_btn_width(kb_matrix, 31, 8);  // backspace
    lv_btnmatrix_set_btn_width(kb_matrix, 32, 4);  // abc
    lv_btnmatrix_set_btn_width(kb_matrix, 33, 12);
    lv_btnmatrix_set_btn_width(kb_matrix, 34, 2);  // _
    lv_btnmatrix_set_btn_width(kb_matrix, 35, 4);  // clear
    set_repeat_policy(31);
    return;
  }

  width(0, 9, 5);
  // Half-key inset either side of the home row, as in the design. The spacers
  // are real buttons that are simply never drawn or pressed.
  for (const uint16_t spacer : {10, 20}) {
    lv_btnmatrix_set_btn_width(kb_matrix, spacer, 3);
    lv_btnmatrix_set_btn_ctrl(kb_matrix, spacer,
                              LV_BTNMATRIX_CTRL_HIDDEN | LV_BTNMATRIX_CTRL_DISABLED);
  }
  width(11, 19, 5);
  lv_btnmatrix_set_btn_width(kb_matrix, 21, 8);  // shift
  width(22, 28, 5);
  lv_btnmatrix_set_btn_width(kb_matrix, 29, 8);  // backspace
  lv_btnmatrix_set_btn_width(kb_matrix, 30, 4);  // 123
  lv_btnmatrix_set_btn_width(kb_matrix, 31, 12);
  lv_btnmatrix_set_btn_width(kb_matrix, 32, 2);  // _
  lv_btnmatrix_set_btn_width(kb_matrix, 33, 4);  // clear
  set_repeat_policy(29);
}

void kb_close() {
  show(kb_overlay, false);
  // The passphrase should not outlive the dialog that collected it.
  memset(kb_password, 0, sizeof(kb_password));
  memset(kb_display, 0, sizeof(kb_display));
  kb_length = 0;
  kb_reveal = false;
  kb_layer = KbLayer::lower;
  kb_layout_dirty = false;
  kb_pressing = false;
}

void kb_open(const char *ssid) {
  snprintf(kb_ssid, sizeof(kb_ssid), "%s", ssid);
  memset(kb_password, 0, sizeof(kb_password));
  kb_length = 0;
  kb_reveal = false;
  kb_layer = KbLayer::lower;
  kb_layout_dirty = false;
  kb_pressing = false;
  apply_kb_layout();
  kb_render_display();

  char title[48];
  snprintf(title, sizeof(title), "Join %s", kb_ssid);
  set_label(kb_title, title);
  show(kb_overlay, true);
}

void kb_append(const char *text) {
  const size_t len = strlen(text);
  if (kb_length + len > max_password_len) {
    return;
  }
  memcpy(kb_password + kb_length, text, len);
  kb_length += len;
  kb_password[kb_length] = '\0';

  // Shift is one-shot: it applies to a single character and then releases.
  if (kb_layer == KbLayer::upper) {
    kb_layer = KbLayer::lower;
    kb_layout_dirty = true;
  }
}

void kb_key_event(lv_event_t *event) {
  lv_obj_t *matrix = lv_event_get_target(event);
  const uint16_t id = lv_btnmatrix_get_selected_btn(matrix);
  if (id == LV_BTNMATRIX_BTN_NONE) {
    return;
  }
  const char *text = lv_btnmatrix_get_btn_text(matrix, id);
  if (text == nullptr) {
    return;
  }

  if (strcmp(text, LV_SYMBOL_UP) == 0) {
    // From the symbol layer this comes back to the letters with caps latched,
    // which is the only useful thing shift can mean there.
    kb_layer = kb_layer == KbLayer::upper ? KbLayer::lower : KbLayer::upper;
    kb_layout_dirty = true;
  } else if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
    if (kb_length > 0) {
      kb_password[--kb_length] = '\0';
    }
  } else if (strcmp(text, "123") == 0 || strcmp(text, "abc") == 0) {
    kb_layer = kb_layer == KbLayer::symbols ? KbLayer::lower : KbLayer::symbols;
    kb_layout_dirty = true;
  } else if (strcmp(text, "clear") == 0) {
    memset(kb_password, 0, sizeof(kb_password));
    kb_length = 0;
  } else if (strcmp(text, "space") == 0) {
    kb_append(" ");
  } else {
    kb_append(text);
  }

  kb_render_display();
}

// A layer swap frees and reallocates the matrix's button array. If that happened
// while a finger were still down, LVGL would go on to index the freed array with
// the old button number -- which the shorter layer may not even have. So track
// the press and let update_keyboard() swap once the finger is up.
void kb_press_event(lv_event_t *event) {
  kb_pressing = lv_event_get_code(event) == LV_EVENT_PRESSED;
}

void kb_cancel_clicked(lv_event_t *) { kb_close(); }

void kb_reveal_clicked(lv_event_t *) {
  kb_reveal = !kb_reveal;
  kb_render_display();
}

void kb_join_clicked(lv_event_t *) {
  if (state == nullptr || kb_length < kb_min_password) {
    return;
  }
  snprintf(state->join_ssid, sizeof(state->join_ssid), "%s", kb_ssid);
  snprintf(state->join_password, sizeof(state->join_password), "%s", kb_password);
  state->join_state = JoinState::requested;
  kb_close();
}

void network_clicked(lv_event_t *event) {
  const uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (state == nullptr || index >= state->network_count) {
    return;
  }

  const Network &network = state->networks[index];
  if (network.open) {
    // Nothing to type, so skip the keyboard entirely.
    snprintf(state->join_ssid, sizeof(state->join_ssid), "%s", network.ssid);
    state->join_password[0] = '\0';
    state->join_state = JoinState::requested;
    return;
  }

  kb_open(network.ssid);
}

void rescan_clicked(lv_event_t *) {
  if (state != nullptr) {
    state->join_state = JoinState::none;
    wifi_start_scan(*state);
  }
}

// Keys differ from each other only in color and text size, which a single style
// cannot express, so they are painted per button here.
void kb_matrix_draw(lv_event_t *event) {
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(event);
  if (dsc->part != LV_PART_ITEMS || dsc->rect_dsc == nullptr) {
    return;
  }

  lv_obj_t *matrix = lv_event_get_target(event);
  const char *text = lv_btnmatrix_get_btn_text(matrix, static_cast<uint16_t>(dsc->id));
  if (text == nullptr) {
    return;
  }

  uint32_t bg = col_key_bg;
  uint32_t ink = col_text;
  const lv_font_t *font = font_body;

  if (strcmp(text, LV_SYMBOL_UP) == 0) {
    const bool latched = kb_layer == KbLayer::upper;
    bg = latched ? col_accent : col_key_alt;
    ink = latched ? col_bg_screen : col_key_ink_dim;
  } else if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
    bg = col_key_alt;
    ink = col_key_ink_dim;
  } else if (strcmp(text, "123") == 0 || strcmp(text, "abc") == 0 || strcmp(text, "clear") == 0) {
    bg = col_key_alt;
    ink = col_key_ink_dim;
    font = font_tiny;
  } else if (strcmp(text, "space") == 0) {
    ink = col_outline_ink;
    font = font_tiny;
  }

  dsc->rect_dsc->bg_color = lv_color_hex(bg);
  dsc->rect_dsc->bg_opa = LV_OPA_COVER;
  dsc->rect_dsc->radius = 4;
  dsc->rect_dsc->border_width = 0;
  if (dsc->label_dsc != nullptr) {
    dsc->label_dsc->color = lv_color_hex(ink);
    dsc->label_dsc->font = font;
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

// The sparklines are drawn by hand rather than with lv_chart, which has no way
// to draw a floating min/max bar: LV_CHART_TYPE_BAR grows from the baseline and
// there is no band type. Each column of the Trend becomes one pixel-wide bar
// spanning that slice of the day's low to its high, and the live reading gets a
// dot on the newest column.
constexpr lv_coord_t spark_dot_size = 3;

void spark_draw(lv_event_t *event) {
  lv_obj_t *obj = lv_event_get_target(event);
  const Reading *reading = static_cast<const Reading *>(lv_obj_get_user_data(obj));
  if (reading == nullptr) {
    return;
  }

  float low = 0.0f;
  float high = 0.0f;
  if (!reading->trend.range(low, high)) {
    return;  // nothing recorded yet
  }
  // A dead-flat day would divide by zero and, worse, draw a line along one edge.
  if (high - low < 0.01f) {
    const float mid = (high + low) / 2.0f;
    low = mid - 0.05f;
    high = mid + 0.05f;
  }

  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  const lv_coord_t width = lv_area_get_width(&area);
  const lv_coord_t height = lv_area_get_height(&area);
  if (width <= 0 || height <= 0) {
    return;
  }

  // Right-aligned, so the newest column sits against the right edge and any
  // slack between the chart width and the column count falls off the left.
  const lv_coord_t right = area.x2;
  const float span = high - low;
  const auto y_for = [&](float value) {
    const float fraction = (value - low) / span;
    const lv_coord_t offset = static_cast<lv_coord_t>(fraction * (height - 1) + 0.5f);
    return static_cast<lv_coord_t>(area.y2 - offset);
  };

  lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);

  lv_draw_rect_dsc_t bar_dsc;
  lv_draw_rect_dsc_init(&bar_dsc);
  bar_dsc.bg_color = lv_color_hex(col_accent);
  bar_dsc.bg_opa = LV_OPA_COVER;

  // If the chart is narrower than the window, the oldest columns fall off the
  // left rather than the whole thing refusing to draw.
  const uint8_t first = width < trend_columns ? trend_columns - static_cast<uint8_t>(width) : 0;

  const Trend &trend = reading->trend;
  for (uint8_t i = first; i < trend_columns; ++i) {
    if (trend.lo[i] == trend_empty) {
      continue;
    }
    const lv_coord_t x = right - (trend_columns - 1 - i);

    lv_area_t bar;
    bar.x1 = x;
    bar.x2 = x;
    bar.y1 = y_for(trend.hi[i] / 100.0f);
    bar.y2 = y_for(trend.lo[i] / 100.0f);
    lv_draw_rect(draw_ctx, &bar_dsc, &bar);
  }

  // A single logged value per column has no spread to draw, so most of a fresh
  // backfill is one pixel tall. Live columns fill out as the panel watches them.
  if (!reading->valid) {
    return;
  }

  lv_draw_rect_dsc_t dot_dsc;
  lv_draw_rect_dsc_init(&dot_dsc);
  dot_dsc.bg_color = lv_color_hex(col_text);
  dot_dsc.bg_opa = LV_OPA_COVER;
  dot_dsc.radius = LV_RADIUS_CIRCLE;

  const lv_coord_t centre = y_for(constrain(reading->value, low, high));
  lv_area_t dot;
  dot.x2 = right;
  dot.x1 = right - (spark_dot_size - 1);
  dot.y1 = centre - spark_dot_size / 2;
  dot.y2 = dot.y1 + spark_dot_size - 1;
  // Keep the whole dot inside the chart so it never clips to a half-moon.
  if (dot.y1 < area.y1) {
    dot.y2 += area.y1 - dot.y1;
    dot.y1 = area.y1;
  }
  if (dot.y2 > area.y2) {
    dot.y1 -= dot.y2 - area.y2;
    dot.y2 = area.y2;
  }
  lv_draw_rect(draw_ctx, &dot_dsc, &dot);
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
  const Reading *card_readings[3] = {&state->temperature, &state->ph, &state->salinity};
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

    lv_obj_t *spark = make_box(card, &st_plain);
    lv_obj_set_size(spark, LV_PCT(100), 16);
    lv_obj_set_style_bg_opa(spark, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(spark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(spark, const_cast<Reading *>(card_readings[i]));
    lv_obj_add_event_cb(spark, spark_draw, LV_EVENT_DRAW_MAIN, nullptr);
    cards[i].spark = spark;

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

  lv_obj_t *display_card = make_box(page, &st_panel);
  lv_obj_set_size(display_card, LV_PCT(100), LV_SIZE_CONTENT);
  set_flex(display_card, LV_FLEX_FLOW_ROW, 9, LV_FLEX_ALIGN_CENTER);
  set_pad(display_card, 8, 9, 8);

  lv_obj_t *display_info = make_box(display_card, &st_plain);
  lv_obj_set_height(display_info, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(display_info, 1);
  set_flex(display_info, LV_FLEX_FLOW_COLUMN, 0, LV_FLEX_ALIGN_START);
  make_label(display_info, "DISPLAY", font_tiny, col_label_dim);
  setup_display_state = make_label(display_info, "--", font_small, col_text_strong);
  make_label(display_info, "Flip to run the USB cable out the other side", font_tiny,
             col_text_dim);

  lv_obj_t *flip = make_box(display_card, &st_plain);
  lv_obj_set_size(flip, 74, 32);
  lv_obj_set_style_radius(flip, 4, 0);
  lv_obj_set_style_bg_opa(flip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(flip, 1, 0);
  set_border_color(flip, 0x2B323B);
  lv_obj_add_flag(flip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(flip, flip_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_center(make_label(flip, "FLIP", font_tiny, 0xAAB4BD));

  make_label(page, "NETWORKS", font_tiny, col_label_dim);

  setup_scan_note = make_label(page, "Scanning...", font_tiny, col_range_dim);

  for (uint8_t i = 0; i < max_networks; ++i) {
    NetworkUi &ui = network_rows[i];
    ui.row = make_box(page, &st_panel);
    lv_obj_set_size(ui.row, LV_PCT(100), 34);
    lv_obj_set_style_radius(ui.row, 5, 0);
    set_flex(ui.row, LV_FLEX_FLOW_ROW, 8, LV_FLEX_ALIGN_CENTER);
    set_pad(ui.row, 0, 9, 0);
    lv_obj_add_flag(ui.row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.row, network_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(i)));

    ui.ssid = make_label(ui.row, "", font_small, col_text_strong);
    lv_obj_set_flex_grow(ui.ssid, 1);
    ui.rssi = make_label(ui.row, "", font_tiny, col_text_dim);
    ui.tag = make_label(ui.row, "", font_tiny, col_text_dim);

    show(ui.row, false);
  }

  lv_obj_t *rescan = make_box(page, &st_plain);
  lv_obj_set_size(rescan, LV_PCT(100), 32);
  lv_obj_set_style_radius(rescan, 5, 0);
  lv_obj_set_style_bg_opa(rescan, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rescan, 1, 0);
  set_border_color(rescan, col_border_hi);
  lv_obj_add_flag(rescan, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(rescan, rescan_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *rescan_label = make_label(rescan, "RESCAN", font_tiny, col_outline_ink);
  lv_obj_center(rescan_label);
}

void build_keyboard(lv_obj_t *screen) {
  // Covers the content and the tab bar but never the status bar, so the Wi-Fi
  // and Apex indicators stay readable while typing.
  kb_overlay = make_box(screen, &st_page);
  lv_obj_set_size(kb_overlay, 480, 272 - status_bar_h);
  lv_obj_set_pos(kb_overlay, 0, status_bar_h);
  set_flex(kb_overlay, LV_FLEX_FLOW_COLUMN, 0, LV_FLEX_ALIGN_START);

  lv_obj_t *header = make_box(kb_overlay, &st_bar);
  lv_obj_set_size(header, LV_PCT(100), 32);
  set_flex(header, LV_FLEX_FLOW_ROW, 8, LV_FLEX_ALIGN_CENTER);
  set_pad(header, 0, 10, 0);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(header, 1, 0);
  set_border_color(header, 0x1D222A);

  lv_obj_t *cancel = make_box(header, &st_plain);
  lv_obj_set_size(cancel, 54, 26);
  lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(cancel, kb_cancel_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_center(make_label(cancel, "CANCEL", font_tiny, col_outline_ink));

  kb_title = make_label(header, "", font_small, col_text_strong);
  lv_obj_set_flex_grow(kb_title, 1);
  lv_obj_set_style_text_align(kb_title, LV_TEXT_ALIGN_CENTER, 0);

  kb_join_btn = make_box(header, &st_plain);
  lv_obj_set_size(kb_join_btn, 54, 26);
  lv_obj_set_style_radius(kb_join_btn, 4, 0);
  lv_obj_set_style_bg_opa(kb_join_btn, LV_OPA_COVER, 0);
  lv_obj_add_flag(kb_join_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(kb_join_btn, kb_join_clicked, LV_EVENT_CLICKED, nullptr);
  kb_join_label = make_label(kb_join_btn, "JOIN", font_tiny, col_disabled_ink);
  lv_obj_center(kb_join_label);

  lv_obj_t *field_row = make_box(kb_overlay, &st_plain);
  lv_obj_set_size(field_row, LV_PCT(100), 40);
  set_flex(field_row, LV_FLEX_FLOW_ROW, 6, LV_FLEX_ALIGN_CENTER);
  set_pad(field_row, 5, 10, 5);

  lv_obj_t *field_box = make_box(field_row, &st_panel);
  lv_obj_set_height(field_box, 30);
  lv_obj_set_flex_grow(field_box, 1);
  lv_obj_set_style_radius(field_box, 4, 0);
  set_border_color(field_box, col_border_hi);
  set_pad(field_box, 0, 9, 0);
  set_flex(field_box, LV_FLEX_FLOW_ROW, 0, LV_FLEX_ALIGN_CENTER);
  kb_field = make_label(field_box, "Password", font_body, col_text_dim);

  lv_obj_t *reveal = make_box(field_row, &st_plain);
  lv_obj_set_size(reveal, 52, 30);
  lv_obj_set_style_radius(reveal, 4, 0);
  lv_obj_set_style_bg_opa(reveal, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(reveal, 1, 0);
  set_border_color(reveal, col_border_hi);
  lv_obj_add_flag(reveal, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(reveal, kb_reveal_clicked, LV_EVENT_CLICKED, nullptr);
  kb_reveal_label = make_label(reveal, "SHOW", font_tiny, col_outline_ink);
  lv_obj_center(kb_reveal_label);

  kb_matrix = lv_btnmatrix_create(kb_overlay);
  lv_obj_remove_style_all(kb_matrix);
  lv_obj_set_width(kb_matrix, LV_PCT(100));
  lv_obj_set_flex_grow(kb_matrix, 1);
  lv_obj_set_style_pad_all(kb_matrix, 0, 0);
  lv_obj_set_style_pad_left(kb_matrix, 5, 0);
  lv_obj_set_style_pad_right(kb_matrix, 5, 0);
  lv_obj_set_style_pad_bottom(kb_matrix, 6, 0);
  lv_obj_set_style_pad_row(kb_matrix, 4, 0);
  lv_obj_set_style_pad_column(kb_matrix, 4, 0);
  lv_obj_set_style_text_font(kb_matrix, font_body, 0);
  lv_obj_add_event_cb(kb_matrix, kb_key_event, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(kb_matrix, kb_matrix_draw, LV_EVENT_DRAW_PART_BEGIN, nullptr);
  lv_obj_add_event_cb(kb_matrix, kb_press_event, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(kb_matrix, kb_press_event, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(kb_matrix, kb_press_event, LV_EVENT_PRESS_LOST, nullptr);
  apply_kb_layout();

  show(kb_overlay, false);
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
  // Joining takes the radio down and blocks, so this is the last frame drawn
  // before everything stalls -- the dot has to say so rather than sitting green
  // on a connection that is already gone.
  const bool joining =
      state->join_state == JoinState::requested || state->join_state == JoinState::running;
  set_bg_color(wifi_dot, joining ? col_warn : (state->wifi_up ? col_accent : col_danger));
  set_label(wifi_label, joining ? "JOINING" : (state->wifi_up ? "WI-FI" : "NO WI-FI"));
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

  float low = 0.0f;
  float high = 0.0f;
  if (!reading.trend.range(low, high)) {
    show(card.spark, false);
    set_label(card.range, "");
    return;
  }

  show(card.spark, true);

  char range_text[24];
  snprintf(range_text, sizeof(range_text), "%.*f - %.*f", decimals, low, decimals, high);
  set_label(card.range, range_text);

  // The sparkline reads the Trend directly when it draws, so all this has to do
  // is say when the picture changed. Redrawing it costs a full-frame flush, so
  // it is gated on the revision rather than done every loop.
  if (redraw_trend) {
    lv_obj_invalidate(card.spark);
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

void update_networks(uint32_t now);

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

  set_label(setup_display_state, state->display_flipped ? "Flipped 180" GLYPH_DEGREE : "Normal");

  update_networks(now);
}

void update_networks(uint32_t now) {
  // A result is worth reading for a few seconds, then the note goes back to
  // describing the list.
  static JoinState shown_join = JoinState::none;
  static uint32_t join_shown_ms = 0;
  if (state->join_state != shown_join) {
    shown_join = state->join_state;
    join_shown_ms = now;
  }
  const bool join_settled =
      state->join_state == JoinState::succeeded || state->join_state == JoinState::failed;
  if (join_settled && now - join_shown_ms > join_result_linger_ms) {
    state->join_state = JoinState::none;
  }

  // First visit to the tab kicks off a scan; after that it takes a RESCAN.
  if (state->scan_state == ScanState::idle) {
    wifi_start_scan(*state);
  }

  char note[64];
  switch (state->join_state) {
    case JoinState::requested:
    case JoinState::running:
      snprintf(note, sizeof(note), "Joining %s...", state->join_ssid);
      break;
    case JoinState::succeeded:
      snprintf(note, sizeof(note), "Joined %s", state->join_ssid);
      break;
    case JoinState::failed:
      snprintf(note, sizeof(note), "Could not join %s", state->join_ssid);
      break;
    default:
      if (state->scan_state == ScanState::running) {
        snprintf(note, sizeof(note), "Scanning...");
      } else if (state->scan_state == ScanState::failed) {
        snprintf(note, sizeof(note), "Scan failed - tap RESCAN");
      } else if (state->network_count == 0) {
        snprintf(note, sizeof(note), "No networks found");
      } else {
        snprintf(note, sizeof(note), "Tap a network to join");
      }
      break;
  }
  set_label(setup_scan_note, note);
  set_text_color(setup_scan_note, state->join_state == JoinState::failed ? col_danger
                                                                        : col_range_dim);

  for (uint8_t i = 0; i < max_networks; ++i) {
    NetworkUi &ui = network_rows[i];
    if (i >= state->network_count) {
      show(ui.row, false);
      continue;
    }
    show(ui.row, true);

    const Network &network = state->networks[i];
    set_label(ui.ssid, network.ssid);

    char rssi_text[12];
    snprintf(rssi_text, sizeof(rssi_text), "%d dBm", network.rssi);
    set_label(ui.rssi, rssi_text);

    set_label(ui.tag, network.saved ? "SAVED" : (network.open ? "OPEN" : "JOIN"));
    set_text_color(ui.tag, network.saved ? col_accent : col_text_dim);
    set_bg_color(ui.row, network.saved ? 0x141D1B : col_panel);
    set_border_color(ui.row, network.saved ? col_accent : col_border);
    set_border_opa(ui.row, network.saved ? LV_OPA_40 : LV_OPA_COVER);
  }
}

void update_keyboard() {
  // Runs from loop(), so LVGL is no longer dispatching on the matrix; waiting
  // for the finger to lift makes the re-map safe.
  if (kb_layout_dirty && !kb_pressing) {
    apply_kb_layout();
    kb_layout_dirty = false;
  }

  set_label(kb_field, kb_display);
  set_text_color(kb_field, kb_length == 0 ? col_text_dim : col_text);
  set_label(kb_reveal_label, kb_reveal ? "HIDE" : "SHOW");

  const bool can_join = kb_length >= kb_min_password;
  set_bg_color(kb_join_btn, can_join ? col_accent : col_inactive_bg);
  set_text_color(kb_join_label, can_join ? col_bg_screen : col_disabled_ink);
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
  build_keyboard(screen);
  build_sleep(screen);

  go_to_tab(tab_home);
}

void ui_update() {
  if (state == nullptr) {
    return;
  }
  update_status_bar();
  update_tabs();

  if (!lv_obj_has_flag(kb_overlay, LV_OBJ_FLAG_HIDDEN)) {
    // The keyboard covers the pages, so nothing behind it needs refreshing.
    update_keyboard();
    return;
  }

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

  // Joining takes the radio down for several seconds. Burn one iteration first
  // so lv_timer_handler() paints "Joining..." before everything stalls.
  if (state->join_state == JoinState::requested) {
    state->join_state = JoinState::running;
    return;
  }
  if (state->join_state == JoinState::running) {
    wifi_run_join(*state);
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
