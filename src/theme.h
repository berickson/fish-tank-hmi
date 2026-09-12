#pragma once

#include <lvgl.h>

// Palette straight out of LVGL-HANDOFF.md section 2.
constexpr uint32_t col_bg_screen = 0x0B0D10;
constexpr uint32_t col_panel = 0x12161C;
constexpr uint32_t col_bar = 0x11141A;
constexpr uint32_t col_border = 0x1E242C;
constexpr uint32_t col_border_hi = 0x262C34;
constexpr uint32_t col_text = 0xE6EBF0;
constexpr uint32_t col_text_strong = 0xDBE2E9;
constexpr uint32_t col_text_dim = 0x6F7A84;
constexpr uint32_t col_label_dim = 0x78838E;
constexpr uint32_t col_range_dim = 0x5F6A74;
constexpr uint32_t col_accent = 0x5FD3BF;
constexpr uint32_t col_warn = 0xE9A23B;
constexpr uint32_t col_danger = 0xE4655C;
constexpr uint32_t col_warn_banner = 0x231C10;
constexpr uint32_t col_on_bg = 0x3F6F63;
constexpr uint32_t col_on_ink = 0xEAFAF6;
constexpr uint32_t col_off_bg = 0x4A3A24;
constexpr uint32_t col_inactive_bg = 0x181D24;
constexpr uint32_t col_inactive_ink = 0x7C8691;
constexpr uint32_t col_feed_idle_bg = 0x141A20;
constexpr uint32_t col_feed_idle_border = 0x28303A;
constexpr uint32_t col_tab_active_bg = 0x151A21;
constexpr uint32_t col_outline_ink = 0x8D979F;
constexpr uint32_t col_key_bg = 0x1B212A;
constexpr uint32_t col_key_alt = 0x141A21;
constexpr uint32_t col_key_ink_dim = 0xAAB4BD;
constexpr uint32_t col_disabled_ink = 0x4D565F;

// The design asks for Archivo 9/10/11/12/15 px and IBM Plex Mono 10/14/18/27 px.
// LVGL only ships Montserrat in even sizes, so these are the nearest matches
// (LVGL-HANDOFF.md section 3 accepts the substitution). Letter-spacing is not a
// thing in LVGL, so the design's tracking on small caps labels is simply lost.
#define font_tiny (&lv_font_montserrat_10)   // design 9-10 px
#define font_small (&lv_font_montserrat_12)  // design 11-12 px
#define font_body (&lv_font_montserrat_14)
#define font_label (&lv_font_montserrat_16)    // design 15 px
#define font_num_mid (&lv_font_montserrat_20)  // design 18 px numerals
#define font_num_big (&lv_font_montserrat_28)  // design 27 px numerals

// Shared styles. Setting these per-widget (as the first dashboard did) allocates a
// local style block on every object; at ~150 objects that is what would exhaust
// the LVGL heap, so every screen reuses these instead.
extern lv_style_t st_screen;
extern lv_style_t st_bar;
extern lv_style_t st_page;
extern lv_style_t st_panel;
extern lv_style_t st_panel_warn;
extern lv_style_t st_plain;
extern lv_style_t st_mode_matrix;

void theme_init();

// Small helpers used all over the UI build; they keep the screen code readable.
lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);
lv_obj_t *make_dot(lv_obj_t *parent, lv_coord_t size, uint32_t color);
// These are safe to call every frame: each one returns without touching the
// object (and so without invalidating it) when the value has not changed.
void set_text_color(lv_obj_t *obj, uint32_t color);
void set_bg_color(lv_obj_t *obj, uint32_t color);
void set_border_color(lv_obj_t *obj, uint32_t color);
void set_bg_opa(lv_obj_t *obj, lv_opa_t opa);
void set_border_opa(lv_obj_t *obj, lv_opa_t opa);
