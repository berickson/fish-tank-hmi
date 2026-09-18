#pragma once

#include "model.h"

// Device settings that survive a reboot, kept in NVS separately from the Wi-Fi
// credentials so clearing one does not disturb the other.

void settings_load(ReefState &state);
void settings_save_display_flipped(bool flipped);

// Remember the address the Apex last answered on, so a boot with mDNS not yet
// working still reaches it. Written only when the address actually changes.
void settings_save_apex_ip(uint32_t address);
uint32_t settings_load_apex_ip();

// Remember when a feed cycle was last seen starting, so "fed 3h ago" survives a
// power cut. Written only on the edge into a cycle -- a few times a day at most,
// which is nothing to NVS.
void settings_save_last_feed(uint32_t epoch);
