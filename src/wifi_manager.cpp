#include "wifi_manager.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "apex.h"
#include "secrets.h"

namespace {

constexpr uint32_t wifi_connect_timeout_ms = 15000;
constexpr char prefs_namespace[] = "reefwifi";
constexpr char prefs_key_ssid[] = "ssid";
constexpr char prefs_key_password[] = "pass";

char joined_ssid[33] = {};

bool connect_to(const char *ssid, const char *password, ReefState &state) {
  Serial.printf("WiFi connecting to %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, password);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < wifi_connect_timeout_ms) {
    delay(250);
  }

  state.wifi_up = WiFi.status() == WL_CONNECTED;
  if (!state.wifi_up) {
    Serial.printf("WiFi connection to %s failed, status: %d\n", ssid, WiFi.status());
    joined_ssid[0] = '\0';
    return false;
  }

  snprintf(joined_ssid, sizeof(joined_ssid), "%s", ssid);
  Serial.printf("WiFi connected to %s, IP: %s\n", ssid, WiFi.localIP().toString().c_str());

  // mDNS is bound to the interface, so it has to come back up with the new
  // connection, and any cached Apex address belongs to the old network.
  MDNS.end();
  MDNS.begin("fish-tank-hmi");
  apex_forget_address();
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

void mark_saved_networks(ReefState &state) {
  for (uint8_t i = 0; i < state.network_count; ++i) {
    state.networks[i].saved = strcmp(state.networks[i].ssid, joined_ssid) == 0;
  }
}

}  // namespace

const char *wifi_current_ssid() { return joined_ssid; }

void wifi_begin(ReefState &state) {
  char ssid[33] = {};
  char password[max_password_len + 1] = {};

  if (load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
    if (connect_to(ssid, password, state)) {
      return;
    }
    // A saved network that will not come up should not strand the panel -- but
    // only fall back if the built-in network is actually a different one.
    if (strcmp(ssid, wifi_ssid) == 0) {
      return;
    }
    Serial.println("Saved network unavailable, falling back to the built-in one");
    state.add_event(EventKind::warn, "Saved Wi-Fi unavailable",
                    "Fell back to the built-in network");
  }

  connect_to(wifi_ssid, wifi_password, state);
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
