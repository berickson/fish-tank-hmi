#pragma once

#include <Arduino.h>

// Everything the UI draws lives here. The Apex client writes it, the UI reads it;
// neither one reaches into the other.

constexpr uint8_t max_outlets = 12;
constexpr uint8_t spark_points = 16;
constexpr uint8_t max_events = 8;

// Apex output status[0] codes: "AON"/"AOF" are the program driving the outlet on
// or off, bare "ON"/"OFF" are a manual override that the Apex holds until cleared.
enum class OutletMode : uint8_t { off, automatic, on };

struct Outlet {
  char did[8];
  char name[20];
  OutletMode mode;
  bool live;  // what the outlet is actually doing right now
};

// A fixed ring of recent samples, newest last, used for the Home sparklines.
struct Trend {
  float points[spark_points];
  uint8_t count;

  void push(float value) {
    if (count < spark_points) {
      points[count++] = value;
      return;
    }
    memmove(points, points + 1, sizeof(float) * (spark_points - 1));
    points[spark_points - 1] = value;
  }

  void range(float &low, float &high) const {
    low = high = count ? points[0] : 0.0f;
    for (uint8_t i = 1; i < count; ++i) {
      low = min(low, points[i]);
      high = max(high, points[i]);
    }
  }
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
  uint32_t last_reply_ms = 0;   // millis() of the last successful poll
  bool ever_connected = false;  // true once we have seen one good poll

  // Seconds left in the Apex feed cycle at the moment of the last poll, plus when
  // that was, so the countdown can tick locally between polls.
  int feed_remaining_s = 0;
  uint32_t feed_sampled_ms = 0;

  // Apex wall clock, captured at the last poll, for timestamping events.
  uint32_t apex_epoch = 0;
  uint32_t apex_epoch_ms = 0;

  Event events[max_events];
  uint8_t event_count = 0;

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
