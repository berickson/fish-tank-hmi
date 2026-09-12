#pragma once

#include "model.h"

// Wi-Fi credentials, connection and scanning.
//
// Credentials live in NVS once a network has been joined from the panel; until
// then the ones compiled in from secrets.h are used. That keeps a fresh board
// working out of the box while letting the panel be re-pointed at a new network
// without a reflash.

// Connect using whatever credentials we have. Falls back to the compiled-in
// network if the saved one will not come up.
void wifi_begin(ReefState &state);

// Kick off a non-blocking scan; results land in `state` via wifi_poll_scan().
void wifi_start_scan(ReefState &state);

// Collect scan results once the radio has them. Cheap to call every loop.
void wifi_poll_scan(ReefState &state);

// Join the network named in state.join_ssid / join_password, saving it to NVS on
// success. Blocks for up to the connect timeout.
void wifi_run_join(ReefState &state);

// SSID of the network currently joined, or "" when down.
const char *wifi_current_ssid();
