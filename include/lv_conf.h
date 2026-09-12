#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
// Five pages of widgets live simultaneously (hidden, not rebuilt on tab change),
// so the 48 KB that suited the single-card dashboard is no longer enough.
#define LV_MEM_SIZE (64U * 1024U)

// The design calls for 9/10/11/12/15 px UI text and 10/14/18/27 px numerals.
// LVGL's built-in Montserrat (even sizes only) stands in for Archivo and
// IBM Plex Mono, per LVGL-HANDOFF.md section 3.
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_28 1

#define LV_USE_LOG 0

#endif
#endif
