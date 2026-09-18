#include "wifi_manager.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "apex.h"
#include "secrets.h"

namespace {

// How long a blocking attempt -- boot, or a join typed on the panel -- waits.
constexpr uint32_t wifi_connect_timeout_ms = 15000;
// A background retry gets a shorter leash: nothing is waiting on it, and giving
// up early just means trying the other network sooner.
constexpr uint32_t retry_timeout_ms = 10000;
// First retry is quick, in case it was one lost beacon; then back off so a
// genuinely absent AP is not hammered.
constexpr uint32_t retry_backoff_min_ms = 3000;
constexpr uint32_t retry_backoff_max_ms = 60000;
// Retries never stop, but after this long of them getting nowhere the radio
// itself may be wedged -- a state where WiFi.begin() returns happily and still
// cannot associate until the radio is cycled. So every five minutes one retry is
// escalated into a full stop and restart, which is the software equivalent of
// replugging the panel.
constexpr uint32_t radio_restart_after_ms = 5UL * 60 * 1000;

constexpr char mdns_hostname[] = "fish-tank-hmi";
constexpr char prefs_namespace[] = "reefwifi";
constexpr char prefs_key_ssid[] = "ssid";
constexpr char prefs_key_password[] = "pass";

char joined_ssid[33] = {};

// The networks to try, best first: the saved one, then the compiled-in one when
// that is a different network. Retrying alternates between them, because "the
// saved network is gone for good" and "the AP is rebooting" look identical from
// here and only one of them is fixed by waiting.
//
// Holding the passphrases in RAM is what lets a retry happen without another NVS
// read; esp_wifi keeps its own copy of the active one regardless, so this adds
// no exposure that was not already there.
struct Candidate {
  char ssid[33];
  char password[max_password_len + 1];
};

Candidate candidates[2];
uint8_t candidate_count = 0;
uint8_t next_candidate = 0;

// Supervisor bookkeeping. `link_up` is the last state we acted on, so that
// transitions are reported once rather than every loop.
bool link_up = false;
bool retry_in_flight = false;
uint32_t retry_started_ms = 0;
uint32_t next_retry_ms = 0;
uint32_t retry_backoff_ms = retry_backoff_min_ms;
uint32_t down_since_ms = 0;
uint32_t last_restart_ms = 0;

// Returns false when nothing has been saved yet.
bool load_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len) {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, true)) {
    return false;
  }
  const String stored_ssid = prefs.getString(prefs_key_ssid, "");
  const String stored_password = prefs.getString(prefs_key_password, "");
  prefs.end();

  if (stored_ssid.isEmpty()) {
    return false;
  }
  snprintf(ssid, ssid_len, "%s", stored_ssid.c_str());
  snprintf(password, password_len, "%s", stored_password.c_str());
  return true;
}

void save_credentials(const char *ssid, const char *password) {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, false)) {
    Serial.println("Could not open NVS to save WiFi credentials");
    return;
  }
  prefs.putString(prefs_key_ssid, ssid);
  prefs.putString(prefs_key_password, password);
  prefs.end();
  Serial.printf("Saved WiFi credentials for %s\n", ssid);
}

void set_candidate(Candidate &candidate, const char *ssid, const char *password) {
  snprintf(candidate.ssid, sizeof(candidate.ssid), "%s", ssid);
  snprintf(candidate.password, sizeof(candidate.password), "%s", password);
}

// Rebuild the try-list from NVS. Called at boot and after a join changes what is
// saved, so a retry never reaches for credentials that have been replaced.
void load_candidates() {
  candidate_count = 0;
  next_candidate = 0;

  Candidate saved;
  if (load_credentials(saved.ssid, sizeof(saved.ssid), saved.password, sizeof(saved.password))) {
    candidates[candidate_count++] = saved;
  }

  // Only worth a slot if it is actually a different network from the saved one.
  if (candidate_count == 0 || strcmp(candidates[0].ssid, wifi_ssid) != 0) {
    set_candidate(candidates[candidate_count++], wifi_ssid, wifi_password);
  }
}

void mark_saved_networks(ReefState &state) {
  for (uint8_t i = 0; i < state.network_count; ++i) {
    state.networks[i].saved = strcmp(state.networks[i].ssid, joined_ssid) == 0;
  }
}

// Everything that has to happen when the link comes up, however it came up --
// a boot connect, a join, or a background retry.
void on_link_up(const char *ssid, ReefState &state) {
  snprintf(joined_ssid, sizeof(joined_ssid), "%s", ssid);
  state.wifi_up = true;
  link_up = true;
  retry_in_flight = false;
  retry_backoff_ms = retry_backoff_min_ms;
  down_since_ms = 0;
  Serial.printf("WiFi connected to %s, IP: %s\n", ssid, WiFi.localIP().toString().c_str());

  // mDNS is bound to the interface, so it has to come back up with the new
  // connection, and any cached Apex address belongs to the old network. This is
  // why a reconnect has to be noticed rather than left to the core: a link that
  // silently came back with a dead responder still cannot resolve apex.local.
  wifi_restart_mdns();
  apex_forget_address();
}

void on_link_down(ReefState &state) {
  state.wifi_up = false;
  link_up = false;
  joined_ssid[0] = '\0';
  // Whatever address we had was learned on a connection that no longer exists.
  apex_forget_address();
}

bool connect_to(const char *ssid, const char *password, ReefState &state) {
  Serial.printf("WiFi connecting to %s\n", ssid);
  WiFi.mode(WIFI_STA);
  // Let the core retry on its own between our checks; we supervise on top of it
  // rather than instead of it. Credentials live in NVS under our own key, so
  // there is no reason to let the stack write its copy on every connect.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, password);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < wifi_connect_timeout_ms) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi connection to %s failed, status: %d\n", ssid, WiFi.status());
    on_link_down(state);
    if (down_since_ms == 0) {
      down_since_ms = millis();
    }
    return false;
  }

  on_link_up(ssid, state);
  return true;
}

// Fire off one non-blocking attempt at the next candidate. Unlike connect_to()
// this returns immediately; wifi_service() watches for the result.
void start_retry() {
  if (candidate_count == 0) {
    load_candidates();
  }

  const Candidate &candidate = candidates[next_candidate];
  next_candidate = (next_candidate + 1) % candidate_count;

  Serial.printf("WiFi retrying %s\n", candidate.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(candidate.ssid, candidate.password);

  retry_in_flight = true;
  retry_started_ms = millis();
}

// Stop the radio and start it again. The plain retries have had five minutes, so
// at this point the supervisor stops assuming the stack is healthy. This is an
// escalation, not a surrender: a retry is scheduled for the very next loop and
// the cycle continues for as long as the link is down.
void restart_radio() {
  Serial.println("WiFi still down after several minutes, restarting the radio");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  retry_in_flight = false;
  last_restart_ms = millis();
  // Straight back to trying, with the radio freshly up.
  next_retry_ms = millis();
}

}  // namespace

const char *wifi_current_ssid() { return joined_ssid; }

void wifi_restart_mdns() {
  MDNS.end();
  if (!MDNS.begin(mdns_hostname)) {
    Serial.println("mDNS responder failed to start");
  }
}

void wifi_begin(ReefState &state) {
  load_candidates();

  // candidates[0] is the saved network when there is one, so this is the same
  // order the supervisor retries in.
  for (uint8_t i = 0; i < candidate_count; ++i) {
    if (connect_to(candidates[i].ssid, candidates[i].password, state)) {
      // A retry should reach for the network we were actually on first, and only
      // then start alternating.
      next_candidate = i;
      return;
    }
    if (i == 0 && candidate_count > 1) {
      Serial.println("Saved network unavailable, falling back to the built-in one");
      state.add_event(EventKind::warn, "Saved Wi-Fi unavailable",
                      "Fell back to the built-in network");
    }
  }

  // Nothing came up. Leave it to the supervisor rather than blocking here any
  // longer -- the panel is more useful showing stale readings than showing
  // nothing while it keeps trying.
  Serial.println("WiFi down at boot; will keep retrying in the background");
}

void wifi_service(ReefState &state) {
  // A join takes the radio for itself and reports its own result; standing on
  // its toes here would only confuse both.
  if (state.join_state == JoinState::requested || state.join_state == JoinState::running) {
    return;
  }

  const uint32_t now = millis();
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    if (!link_up) {
      // Either our retry landed or the core's own reconnect did. Either way the
      // connection is new, so everything bound to the old one is rebuilt.
      on_link_up(WiFi.SSID().c_str(), state);
      state.add_event(EventKind::info, "Wi-Fi reconnected", joined_ssid);
      mark_saved_networks(state);
    }
    state.wifi_up = true;
    return;
  }

  if (link_up) {
    Serial.println("WiFi link lost");
    on_link_down(state);
    state.add_event(EventKind::danger, "Wi-Fi lost", "Reconnecting");
    down_since_ms = now;
    retry_backoff_ms = retry_backoff_min_ms;
    next_retry_ms = now + retry_backoff_min_ms;
    return;
  }

  state.wifi_up = false;
  if (down_since_ms == 0) {
    down_since_ms = now;
  }

  if (retry_in_flight) {
    if (now - retry_started_ms < retry_timeout_ms) {
      return;  // still waiting on it
    }
    Serial.printf("WiFi retry timed out, status: %d\n", WiFi.status());
    retry_in_flight = false;
    retry_backoff_ms = retry_backoff_ms * 2 > retry_backoff_max_ms ? retry_backoff_max_ms
                                                                  : retry_backoff_ms * 2;
    next_retry_ms = now + retry_backoff_ms;
    return;
  }

  // At most one restart per five minutes, so an AP that is genuinely absent does
  // not get the radio cycled on every pass.
  if (now - down_since_ms >= radio_restart_after_ms &&
      (last_restart_ms == 0 || now - last_restart_ms >= radio_restart_after_ms)) {
    restart_radio();
    return;
  }

  // A scan and a connect attempt fight over the radio, and the scan is the one
  // somebody is watching. It finishes in a couple of seconds; wait it out.
  if (state.scan_state == ScanState::running) {
    return;
  }

  // Nothing below this ever stops retrying. However long the network is away --
  // minutes or days -- the panel keeps reaching for it and picks it up on its own
  // when it comes back.
  if (static_cast<int32_t>(now - next_retry_ms) >= 0) {
    start_retry();
  }
}

void wifi_start_scan(ReefState &state) {
  if (state.scan_state == ScanState::running) {
    return;
  }
  WiFi.scanDelete();
  // Asynchronous: the radio does the work and the panel keeps drawing.
  const int16_t started = WiFi.scanNetworks(true);
  state.scan_state = started == WIFI_SCAN_RUNNING ? ScanState::running : ScanState::failed;
}

void wifi_poll_scan(ReefState &state) {
  if (state.scan_state != ScanState::running) {
    return;
  }

  const int16_t found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) {
    return;
  }
  if (found < 0) {
    state.scan_state = ScanState::failed;
    return;
  }

  uint8_t count = 0;
  for (int16_t i = 0; i < found && count < max_networks; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }

    // The same network often answers from several access points; keep the first
    // (strongest, since the scan is sorted) and drop the rest.
    bool duplicate = false;
    for (uint8_t seen = 0; seen < count; ++seen) {
      if (ssid == state.networks[seen].ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    Network &network = state.networks[count++];
    snprintf(network.ssid, sizeof(network.ssid), "%s", ssid.c_str());
    network.rssi = static_cast<int8_t>(WiFi.RSSI(i));
    network.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    network.saved = false;
  }

  state.network_count = count;
  mark_saved_networks(state);
  state.scan_state = ScanState::done;
  WiFi.scanDelete();
}

void wifi_run_join(ReefState &state) {
  state.join_state = JoinState::running;

  if (connect_to(state.join_ssid, state.join_password, state)) {
    save_credentials(state.join_ssid, state.join_password);
    // The try-list is now stale: the saved network has changed under it.
    load_candidates();
    state.join_state = JoinState::succeeded;
    state.add_event(EventKind::info, "Wi-Fi joined", state.join_ssid);
    mark_saved_networks(state);
  } else {
    state.join_state = JoinState::failed;
    state.add_event(EventKind::danger, "Wi-Fi join failed", state.join_ssid);
    // Get back onto something rather than sitting disconnected.
    wifi_begin(state);
  }

  // Never keep the passphrase in RAM longer than the attempt needs it.
  memset(state.join_password, 0, sizeof(state.join_password));
}
