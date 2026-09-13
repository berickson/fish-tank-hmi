#include "settings.h"

#include <Preferences.h>

namespace {
constexpr char prefs_namespace[] = "reefcfg";
constexpr char prefs_key_flipped[] = "flipped";
}  // namespace

void settings_load(ReefState &state) {
  Preferences prefs;
  if (!prefs.begin(prefs_namespace, true)) {
    return;  // nothing saved yet; the defaults in ReefState stand
  }
  state.display_flipped = prefs.getBool(prefs_key_flipped, false);
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
