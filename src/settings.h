#pragma once

#include "model.h"

// Device settings that survive a reboot, kept in NVS separately from the Wi-Fi
// credentials so clearing one does not disturb the other.

void settings_load(ReefState &state);
void settings_save_display_flipped(bool flipped);
