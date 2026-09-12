#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
// Every page's widgets live simultaneously (hidden, not rebuilt on tab change).
// The four tabs plus the keyboard sit at ~62% of a 64 KB pool before the Control
// rows are built on first visit, which is too thin a margin -- and the board has
// RAM to spare, since the framebuffer lives in PSRAM.
#define LV_MEM_SIZE (96U * 1024U)

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
