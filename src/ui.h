#pragma once

#include <lvgl.h>

#include "model.h"

// Things the UI needs the board to do for it.
struct UiHooks {
  void (*set_backlight_percent)(uint8_t percent);
  // Rotate the panel 180 degrees and remember the choice.
  void (*set_display_flipped)(bool flipped);
};

// Build every screen once. The UI keeps the pointer and reads through it.
void ui_create(ReefState *state, const UiHooks &hooks);

// Repaint from the current state. Cheap enough to call every loop.
void ui_update();

// Send at most one queued Apex write. Taps are queued rather than sent inline so
// the panel can repaint before it blocks on the network; call this once a loop,
// after lv_timer_handler().
void ui_service_actions();

// True while the panel is showing the idle screen; the caller should stop
// polling the Apex at full rate.
bool ui_is_asleep();
