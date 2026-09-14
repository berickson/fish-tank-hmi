#pragma once

#include <Arduino.h>

// Everything the UI draws lives here. The Apex client writes it, the UI reads it;
// neither one reaches into the other.

constexpr uint8_t max_outlets = 12;
constexpr uint8_t max_events = 8;
constexpr uint8_t max_networks = 8;
// WPA2 allows 63; the field and the keyboard both stop there.
constexpr uint8_t max_password_len = 63;

// Apex output status[0] codes: "AON"/"AOF" are the program driving the outlet on
// or off, bare "ON"/"OFF" are a manual override that the Apex holds until cleared.
enum class OutletMode : uint8_t { off, automatic, on };

struct Outlet {
  char did[8];
  char name[20];
  OutletMode mode;
  bool live;  // what the outlet is actually doing right now
};

// The Home sparklines cover a fixed 24 hours, one column per horizontal pixel.
// The card leaves 130 px for the chart (480 screen, 10 px page padding a side,
// two 8 px gaps, then each card's own 1 px border and 8 px padding), and 128 is
// the largest column count that divides the day exactly: 128 * 675 s = 86400.
// An exact division matters because it is what lets a column be a fixed absolute
// slice of time rather than a fraction that has to be re-derived.
constexpr uint8_t trend_columns = 128;
constexpr uint32_t trend_bucket_s = 675;
// A column nothing has been recorded in. Readings are stored as hundredths, so
// no real reading can collide with it.
constexpr int16_t trend_empty = INT16_MIN;

// 24 hours of one probe as a min/max band, a column per pixel. Values are
// hundredths, which keeps a column to four bytes and matches what the sparkline
// has to hand LVGL anyway.
//
// Columns are anchored to absolute epoch multiples of `trend_bucket_s`, never to
// "now minus k columns". A closed column is therefore never recomputed: the
// window slides left by whole columns and is otherwise the same picture. Binning
// against a moving origin is what makes these charts wobble.
struct Trend {
  int16_t lo[trend_columns];
  int16_t hi[trend_columns];
  // Absolute column index (epoch / trend_bucket_s) drawn at the right edge.
  uint32_t newest;

  Trend() { clear(); }

  void clear();

  // Slide the window so `epoch` is the right edge, without recording anything.
  // Used before a backfill so history fills in from the left instead of the
  // chart scrolling a day's worth while it loads.
  void anchor(uint32_t epoch);

  // Fold one timestamped sample into its column. Samples older than the window
  // are dropped -- the Apex datalog hands us those routinely -- and a newer one
  // slides the window.
  void add(uint32_t epoch, float value);

  // Low and high across every recorded column. False when there is nothing yet.
  bool range(float &low, float &high) const;

 private:
  void slide_to(uint32_t column);
};

struct Reading {
  float value;
  bool valid;
  Trend trend;
};

enum class EventKind : uint8_t { info, warn, danger };

// Acknowledgeable history: things that happened and are now over. Live problems
// (an outlet out of auto, a dead Apex link) are derived from state instead, so
// they cannot be acknowledged away while they are still true.
struct Event {
  char title[32];
  char detail[40];
  char time[8];
  EventKind kind;
};

struct Network {
  char ssid[33];
  int8_t rssi;
  bool open;   // no passphrase needed
  bool saved;  // this is the network we are on or have credentials for
};

enum class ScanState : uint8_t { idle, running, done, failed };

// Joining blocks for several seconds, so it runs as a small state machine that
// lets the panel paint "Joining..." before the radio stalls everything.
enum class JoinState : uint8_t { none, requested, running, succeeded, failed };

struct ReefState {
  Reading temperature;
  Reading ph;
  Reading salinity;

  Outlet outlets[max_outlets];
  uint8_t outlet_count = 0;

  // Bumped on every successful poll. The UI redraws the sparklines only when
  // this moves, so the charts are not invalidated on every single frame.
  uint32_t revision = 0;

  bool wifi_up = false;
  bool apex_up = false;

  Network networks[max_networks];
  uint8_t network_count = 0;
  ScanState scan_state = ScanState::idle;

  JoinState join_state = JoinState::none;
  char join_ssid[33] = {};
  char join_password[max_password_len + 1] = {};

  // Panel rotated 180 degrees, so the USB cable can leave the other side.
  // Persisted in NVS; see settings.h.
  bool display_flipped = false;
  uint32_t last_reply_ms = 0;   // millis() of the last successful poll
  bool ever_connected = false;  // true once we have seen one good poll

  // Seconds left in the Apex feed cycle at the moment of the last poll, plus when
  // that was, so the countdown can tick locally between polls.
  int feed_remaining_s = 0;
  uint32_t feed_sampled_ms = 0;

  // Apex wall clock, captured at the last poll, for timestamping events and for
  // placing readings in the 24-hour trends. It is a true Unix epoch; the Apex's
  // configured UTC offset arrives alongside it and is what turns it into local
  // time for display.
  uint32_t apex_epoch = 0;
  uint32_t apex_epoch_ms = 0;
  int32_t tz_offset_s = 0;

  Event events[max_events];
  uint8_t event_count = 0;

  // Apex clock advanced to right now, or 0 before the first poll has told us
  // what time it is.
  uint32_t now_epoch() const {
    if (apex_epoch == 0) {
      return 0;
    }
    return apex_epoch + (millis() - apex_epoch_ms) / 1000;
  }

  int live_feed_seconds() const {
    if (feed_remaining_s <= 0) {
      return 0;
    }
    const int elapsed = static_cast<int>((millis() - feed_sampled_ms) / 1000);
    return max(0, feed_remaining_s - elapsed);
  }

  bool feeding() const { return live_feed_seconds() > 0; }

  uint8_t override_count() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < outlet_count; ++i) {
      if (outlets[i].mode != OutletMode::automatic) {
        ++count;
      }
    }
    return count;
  }

  // Count of things the Home banner and the CONTROL tab badge warn about.
  uint8_t warn_count() const { return override_count() + (apex_up ? 0 : 1); }

  Outlet *find_outlet(const char *did) {
    for (uint8_t i = 0; i < outlet_count; ++i) {
      if (strcmp(outlets[i].did, did) == 0) {
        return &outlets[i];
      }
    }
    return nullptr;
  }

  void add_event(EventKind kind, const char *title, const char *detail);
  void clear_events() { event_count = 0; }
  void format_now(char *out, size_t len) const;
};
