#include "apex.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "secrets.h"
#include "settings.h"
#include "wifi_manager.h"

namespace {

constexpr char apex_mdns_name[] = "apex";
// HTTP runs on the UI thread, so a dead Apex must fail fast or the panel freezes.
constexpr uint16_t http_timeout_ms = 2000;
// So does the mDNS query, and it blocks for its whole timeout when nothing
// answers. The library's own default is 2000 ms, which is a visible stutter on
// every poll while the Apex is away; a responder on the same LAN answers in
// milliseconds or not at all.
constexpr uint32_t mdns_query_timeout_ms = 600;
// Unreachable this long means the responder may be wedged or bound to an
// interface that no longer exists, which only a restart clears. This mirrors the
// radio restart in wifi_manager.cpp, one layer up. Polling never stops either
// way -- this only changes what each attempt is willing to try.
constexpr uint32_t mdns_restart_after_ms = 5UL * 60 * 1000;

IPAddress apex_ip;
// The address the Apex last actually answered on, remembered across boots. The
// controller sits on one address for months at a time, so this is the cheapest
// and most reliable way back to it when mDNS cannot answer.
IPAddress last_good_ip;
bool last_good_loaded = false;

uint32_t unreachable_since_ms = 0;
uint32_t last_mdns_restart_ms = 0;

void load_last_good() {
  if (last_good_loaded) {
    return;
  }
  last_good_loaded = true;
  const uint32_t stored = settings_load_apex_ip();
  if (stored != 0) {
    last_good_ip = IPAddress(stored);
    Serial.printf("Last known Apex address: %s\n", last_good_ip.toString().c_str());
  }
}

bool ensure_apex_ip() {
  if (apex_ip != INADDR_NONE) {
    return true;
  }

  load_last_good();

  const IPAddress resolved = MDNS.queryHost(apex_mdns_name, mdns_query_timeout_ms);
  if (resolved != INADDR_NONE) {
    apex_ip = resolved;
    Serial.printf("Resolved apex.local to %s\n", apex_ip.toString().c_str());
    return true;
  }

  Serial.println("mDNS lookup for apex.local failed");

  // mDNS being down does not mean the Apex is. Trying the address it answered on
  // last is what keeps the panel alive through a wedged responder -- and mDNS is
  // tried first, above, so an Apex that genuinely moved is still found.
  if (last_good_ip != INADDR_NONE) {
    apex_ip = last_good_ip;
    Serial.printf("Falling back to the last known Apex address %s\n",
                  apex_ip.toString().c_str());
    return true;
  }

  return false;
}

// One failed attempt. Keeps the clock running and escalates when plain retries
// have had long enough -- it never stops the caller from trying again.
void note_unreachable() {
  const uint32_t now = millis();

  // The clock measures "Apex away while the network is fine". With Wi-Fi down
  // there is nothing here to diagnose, and wifi_manager restarts the responder
  // on every reconnect anyway -- so hold the clock at zero rather than letting a
  // Wi-Fi outage bank five minutes and fire a pointless restart the moment the
  // link returns.
  if (WiFi.status() != WL_CONNECTED) {
    unreachable_since_ms = 0;
    return;
  }

  if (unreachable_since_ms == 0) {
    unreachable_since_ms = now;
  }

  if (now - unreachable_since_ms >= mdns_restart_after_ms &&
      (last_mdns_restart_ms == 0 || now - last_mdns_restart_ms >= mdns_restart_after_ms)) {
    Serial.println("Apex unreachable for several minutes, restarting mDNS");
    wifi_restart_mdns();
    last_mdns_restart_ms = now;
    // Re-resolve from scratch on the next attempt, fallback included.
    apex_ip = INADDR_NONE;
  }
}

// One good reply. Clears the escalation clock and banks the working address.
void note_reachable() {
  unreachable_since_ms = 0;
  if (apex_ip != INADDR_NONE && apex_ip != last_good_ip) {
    last_good_ip = apex_ip;
    last_good_loaded = true;
    settings_save_apex_ip(static_cast<uint32_t>(apex_ip));
    Serial.printf("Remembered Apex address %s\n", apex_ip.toString().c_str());
  }
}

OutletMode mode_from_status(const char *status) {
  if (status == nullptr) {
    return OutletMode::automatic;
  }
  // "AON"/"AOF" mean the outlet's own program is driving it; bare "ON"/"OFF" are
  // a manual override. Anything else is program-driven too, so treat it as auto
  // rather than raising a false out-of-auto alarm.
  if (status[0] == 'A') {
    return OutletMode::automatic;
  }
  if (strcmp(status, "ON") == 0) {
    return OutletMode::on;
  }
  if (strcmp(status, "OFF") == 0) {
    return OutletMode::off;
  }
  return OutletMode::automatic;
}

// Whether the outlet is actually energised right now, whatever put it there.
bool live_from_status(const char *status) {
  return status != nullptr && (strcmp(status, "AON") == 0 || strcmp(status, "ON") == 0);
}

const char *mode_keyword(OutletMode mode) {
  switch (mode) {
    case OutletMode::off:
      return "OFF";
    case OutletMode::on:
      return "ON";
    default:
      return "AUTO";
  }
}

void store_reading(Reading &reading, float value, uint32_t epoch) {
  reading.value = value;
  reading.valid = true;
  reading.trend.add(epoch, value);
}

}  // namespace

void apex_forget_address() { apex_ip = INADDR_NONE; }

void apex_retry_now() {
  // What the RETRY button means: assume nothing, including that the responder
  // that has been failing to resolve apex.local is healthy.
  apex_ip = INADDR_NONE;
  if (WiFi.status() == WL_CONNECTED) {
    wifi_restart_mdns();
    last_mdns_restart_ms = millis();
  }
}

bool apex_begin_request(HTTPClient &http, const String &path) {
  if (WiFi.status() != WL_CONNECTED || !ensure_apex_ip()) {
    return false;
  }

  http.begin(String("http://") + apex_ip.toString() + path);
  http.setAuthorization(apex_username, apex_password);
  http.setConnectTimeout(http_timeout_ms);
  http.setTimeout(http_timeout_ms);
  return true;
}

bool apex_poll(ReefState &state) {
  // state.wifi_up belongs to wifi_service(), which watches the link every loop
  // rather than only once per poll interval.
  HTTPClient http;
  if (!apex_begin_request(http, "/cgi-bin/status.json")) {
    note_unreachable();
    return false;
  }

  const int http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    Serial.printf("Apex request failed, code: %d\n", http_code);
    http.end();
    // Only a connection-level failure (a negative HTTPClient error) says the
    // address might be wrong. An Apex that answers with 401 or 500 is an Apex we
    // can reach, and throwing its address away over that is what turns one bad
    // reply into a hunt through mDNS.
    if (http_code < 0) {
      apex_forget_address();
    }
    note_unreachable();
    return false;
  }

  JsonDocument status_doc;
  const DeserializationError parse_error = deserializeJson(status_doc, http.getStream());
  http.end();

  if (parse_error) {
    Serial.printf("Apex JSON parse failed: %s\n", parse_error.c_str());
    note_unreachable();
    return false;
  }

  JsonObject istat = status_doc["istat"];
  if (istat.isNull()) {
    Serial.println("Apex response had no istat object");
    note_unreachable();
    return false;
  }

  state.apex_epoch = istat["date"].as<uint32_t>();
  state.apex_epoch_ms = millis();
  // Hours east of UTC, as a decimal ("-7.00"), so half-hour zones survive it.
  if (!istat["timezone"].isNull()) {
    state.tz_offset_s = lroundf(istat["timezone"].as<float>() * 3600.0f);
  }

  state.feed_remaining_s = istat["feed"]["active"].as<int>();
  state.feed_sampled_ms = millis();

  for (JsonObject probe : istat["inputs"].as<JsonArray>()) {
    const char *name = probe["name"];
    if (name == nullptr) {
      continue;
    }
    if (strcmp(name, "Tmp") == 0) {
      store_reading(state.temperature, probe["value"].as<float>(), state.apex_epoch);
    } else if (strcmp(name, "pH") == 0) {
      store_reading(state.ph, probe["value"].as<float>(), state.apex_epoch);
    } else if (strcmp(name, "Salt") == 0) {
      store_reading(state.salinity, probe["value"].as<float>(), state.apex_epoch);
    }
  }

  // The Apex lists variable-speed ports, alarms and 24 V link ports alongside the
  // real outlets; only `type == "outlet"` belongs on the Control screen.
  uint8_t count = 0;
  for (JsonObject output : istat["outputs"].as<JsonArray>()) {
    const char *type = output["type"];
    if (type == nullptr || strcmp(type, "outlet") != 0 || count >= max_outlets) {
      continue;
    }

    const char *did = output["did"];
    const char *name = output["name"];
    const char *status = output["status"][0];

    Outlet &outlet = state.outlets[count++];
    snprintf(outlet.did, sizeof(outlet.did), "%s", did != nullptr ? did : "");
    snprintf(outlet.name, sizeof(outlet.name), "%s", name != nullptr ? name : "?");
    outlet.mode = mode_from_status(status);
    outlet.live = live_from_status(status);
  }
  state.outlet_count = count;

  state.apex_up = true;
  state.ever_connected = true;
  state.last_reply_ms = millis();
  note_reachable();
  ++state.revision;
  return true;
}

bool apex_set_outlet(const char *did, OutletMode mode) {
  HTTPClient http;
  if (!apex_begin_request(http, String("/rest/status/outputs/") + did)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  const char *keyword = mode_keyword(mode);
  char body[96];
  snprintf(body, sizeof(body), "{\"did\":\"%s\",\"status\":[\"%s\",\"\",\"OK\",\"\"],\"to\":\"%s\"}",
           did, keyword, keyword);

  const int http_code = http.PUT(reinterpret_cast<uint8_t *>(body), strlen(body));
  const String reply = http_code == HTTP_CODE_OK ? http.getString() : String();
  http.end();

  if (http_code != HTTP_CODE_OK) {
    Serial.printf("Apex outlet %s -> %s failed, code: %d\n", did, keyword, http_code);
    return false;
  }

  // The Apex answers 200 even when it rejects the request; errorCode carries the
  // real verdict.
  if (reply.indexOf("\"errorCode\":0") < 0) {
    Serial.printf("Apex outlet %s -> %s refused: %s\n", did, keyword, reply.c_str());
    return false;
  }

  Serial.printf("Apex outlet %s -> %s\n", did, keyword);
  return true;
}

bool apex_set_feed_cycle(uint8_t cycle_index, bool active) {
  HTTPClient http;
  // The cycle index must be in the URL path (matches the Apex app's own request);
  // a body-only "name" field is silently accepted but never actually starts the cycle.
  if (!apex_begin_request(http, String("/rest/status/feed/") + cycle_index)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  char body[64];
  snprintf(body, sizeof(body), "{\"name\":%u,\"active\":%u}", cycle_index, active ? 1 : 0);

  const int http_code = http.PUT(reinterpret_cast<uint8_t *>(body), strlen(body));
  http.end();

  if (http_code != HTTP_CODE_OK) {
    Serial.printf("Apex feed request failed, code: %d\n", http_code);
    return false;
  }

  return true;
}
