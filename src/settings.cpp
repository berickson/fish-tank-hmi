#include "settings.h"

#include <Preferences.h>

namespace {
constexpr char prefs_namespace[] = "reefcfg";
constexpr char prefs_key_flipped[] = "flipped";
constexpr char prefs_key_last_feed[] = "lastfeed";
constexpr char prefs_key_apex_ip[] = "apexip";
}  // namespace

void settings_load(ReefState &state) {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, true)) {
    return;  // nothing saved yet; the defaults in ReefState stand
  }
  state.display_flipped = prefs.getBool(prefs_key_flipped, false);
  // Carried over from before the power cut, so nothing has confirmed it yet
  // this boot.
  state.last_feed_epoch = prefs.getUInt(prefs_key_last_feed, 0);
  state.feed_confirmed = false;
  prefs.end();
}

void settings_save_display_flipped(bool flipped) {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, false)) {
    Serial.println("Could not open NVS to save display orientation");
    return;
  }
  prefs.putBool(prefs_key_flipped, flipped);
  prefs.end();
}

void settings_save_last_feed(uint32_t epoch) {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, false)) {
    Serial.println("Could not open NVS to save the feed time");
    return;
  }
  prefs.putUInt(prefs_key_last_feed, epoch);
  prefs.end();
}

void settings_save_apex_ip(uint32_t address) {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, false)) {
    Serial.println("Could not open NVS to save the Apex address");
    return;
  }
  prefs.putUInt(prefs_key_apex_ip, address);
  prefs.end();
}

uint32_t settings_load_apex_ip() {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, true)) {
    return 0;
  }
  const uint32_t address = prefs.getUInt(prefs_key_apex_ip, 0);
  prefs.end();
  return address;
}
