#pragma once

#include <HTTPClient.h>

#include "model.h"

// Talks to the Neptune Apex on the local network. No cloud, no Fusion account.

// One poll of /cgi-bin/status.json into `state`. Returns false on any failure,
// in which case the previous readings are left untouched so the UI can keep
// showing the last known values instead of blanking.
bool apex_poll(ReefState &state);

// PUT the outlet into OFF / AUTO / ON. `AUTO` hands it back to its Apex program.
bool apex_set_outlet(const char *did, OutletMode mode);

// Start or cancel a pre-programmed feed cycle.
bool apex_set_feed_cycle(uint8_t cycle_index, bool active);

// What the RETRY button does: forget the cached address and restart the mDNS
// responder, so nothing carried over from the failing state is trusted.
void apex_retry_now();

// Forget the cached apex.local address so the next call re-resolves it.
void apex_forget_address();

// Point `http` at a path on the Apex with credentials and timeouts already set,
// resolving apex.local if that has not happened yet. False if there is no way to
// reach it right now. Shared with the datalog fetch in history.cpp.
bool apex_begin_request(HTTPClient &http, const String &path);
