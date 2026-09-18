#pragma once

#include "model.h"

// Wi-Fi credentials, connection, scanning and keeping the link up.
//
// Credentials live in NVS once a network has been joined from the panel; until
// then the ones compiled in from secrets.h are used. That keeps a fresh board
// working out of the box while letting the panel be re-pointed at a new network
// without a reflash.

// Connect using whatever credentials we have. Falls back to the compiled-in
// network if the saved one will not come up.
void wifi_begin(ReefState &state);

// Keep the link up: notice drops, retry with a backoff, and bring mDNS and the
// cached Apex address back with the connection. Non-blocking -- an attempt is
// started and then watched across later calls -- so call it every loop.
//
// This is what stops a panel that is hanging on a wall from sitting at "NO
// WI-FI" until somebody replugs it.
void wifi_service(ReefState &state);

// Kick off a non-blocking scan; results land in `state` via wifi_poll_scan().
void wifi_start_scan(ReefState &state);

// Collect scan results once the radio has them. Cheap to call every loop.
void wifi_poll_scan(ReefState &state);

// Join the network named in state.join_ssid / join_password, saving it to NVS on
// success. Blocks for up to the connect timeout.
void wifi_run_join(ReefState &state);

// Tear the mDNS responder down and bring it back up. The responder is bound to
// the network interface, so it has to be rebuilt whenever that changes -- and it
// is also the escalation apex.cpp reaches for when apex.local has stopped
// resolving. The panel's own mDNS hostname lives here so there is one copy of it.
void wifi_restart_mdns();

// SSID of the network currently joined, or "" when down.
const char *wifi_current_ssid();
