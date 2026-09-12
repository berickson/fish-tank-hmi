#include "theme.h"

lv_style_t st_screen;
lv_style_t st_bar;
lv_style_t st_page;
lv_style_t st_panel;
lv_style_t st_panel_warn;
lv_style_t st_plain;
lv_style_t st_mode_matrix;

namespace {

// LVGL's default object comes with a border, radius, padding and a scrollbar.
// Most of our containers want none of that, so strip it once here.
void style_reset(lv_style_t *style) {
  lv_style_init(style);
  lv_style_set_radius(style, 0);
  lv_style_set_border_width(style, 0);
  lv_style_set_pad_all(style, 0);
  lv_style_set_pad_row(style, 0);
  lv_style_set_pad_column(style, 0);
  lv_style_set_bg_opa(style, LV_OPA_COVER);
}

}  // namespace

void theme_init() {
  style_reset(&st_screen);
  lv_style_set_bg_color(&st_screen, lv_color_hex(col_bg_screen));

  style_reset(&st_bar);
  lv_style_set_bg_color(&st_bar, lv_color_hex(col_bar));

  style_reset(&st_page);
  lv_style_set_bg_color(&st_page, lv_color_hex(col_bg_screen));

  // Cards and list rows: panel fill, hairline border, 6 px radius.
  style_reset(&st_panel);
  lv_style_set_radius(&st_panel, 6);
  lv_style_set_bg_color(&st_panel, lv_color_hex(col_panel));
  lv_style_set_border_width(&st_panel, 1);
  lv_style_set_border_color(&st_panel, lv_color_hex(col_border));

  // Same, but flagged: a row whose outlet is not following its program.
  lv_style_init(&st_panel_warn);
  lv_style_set_border_color(&st_panel_warn, lv_color_hex(col_warn));
  lv_style_set_border_opa(&st_panel_warn, LV_OPA_40);

  // A container that contributes nothing visually -- used for groupings.
  style_reset(&st_plain);
  lv_style_set_bg_opa(&st_plain, LV_OPA_TRANSP);

  // The OFF/AUTO/ON selector. Per-button colors cannot come from a style (each
  // button needs a different checked color), so they are painted in the draw
  // hook in ui.cpp; this just sets the shared geometry.
  style_reset(&st_mode_matrix);
  lv_style_set_bg_opa(&st_mode_matrix, LV_OPA_TRANSP);
  lv_style_set_pad_all(&st_mode_matrix, 0);
  lv_style_set_pad_column(&st_mode_matrix, 3);
  lv_style_set_text_font(&st_mode_matrix, font_tiny);
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return label;
}

lv_obj_t *make_dot(lv_obj_t *parent, lv_coord_t size, uint32_t color) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, size, size);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
  return dot;
}

// ui_update() runs every loop, and every style write invalidates the object --
// which on this panel means re-flushing the whole 480x272 frame. So none of
// these touch the object unless the value actually changed.

void set_text_color(lv_obj_t *obj, uint32_t color) {
  const lv_color_t next = lv_color_hex(color);
  if (lv_obj_get_style_text_color(obj, LV_PART_MAIN).full == next.full) {
    return;
  }
  lv_obj_set_style_text_color(obj, next, 0);
}

void set_bg_color(lv_obj_t *obj, uint32_t color) {
  const lv_color_t next = lv_color_hex(color);
  if (lv_obj_get_style_bg_color(obj, LV_PART_MAIN).full == next.full) {
    return;
  }
  lv_obj_set_style_bg_color(obj, next, 0);
}

void set_border_color(lv_obj_t *obj, uint32_t color) {
  const lv_color_t next = lv_color_hex(color);
  if (lv_obj_get_style_border_color(obj, LV_PART_MAIN).full == next.full) {
    return;
  }
  lv_obj_set_style_border_color(obj, next, 0);
}

void set_bg_opa(lv_obj_t *obj, lv_opa_t opa) {
  if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == opa) {
    return;
  }
  lv_obj_set_style_bg_opa(obj, opa, 0);
}

void set_border_opa(lv_obj_t *obj, lv_opa_t opa) {
  if (lv_obj_get_style_border_opa(obj, LV_PART_MAIN) == opa) {
    return;
  }
  lv_obj_set_style_border_opa(obj, opa, 0);
}
