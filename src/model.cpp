#include "model.h"

namespace {

// Readings are carried as hundredths so a column's low and high fit two int16s.
// Everything this panel shows -- degrees F, pH, ppt -- is comfortably inside the
// range; the clamp is only there so a garbage probe reading cannot wrap.
int16_t scale_reading(float value) {
  const float scaled = roundf(value * 100.0f);
  if (scaled <= -32767.0f) {
    return -32767;
  }
  if (scaled >= 32767.0f) {
    return 32767;
  }
  return static_cast<int16_t>(scaled);
}

}  // namespace

void Trend::clear() {
  for (uint8_t i = 0; i < trend_columns; ++i) {
    lo[i] = hi[i] = trend_empty;
  }
  newest = 0;
}

void Trend::slide_to(uint32_t column) {
  const uint32_t shift = column - newest;
  if (shift >= trend_columns) {
    // Nothing currently on screen survives a gap this long.
    for (uint8_t i = 0; i < trend_columns; ++i) {
      lo[i] = hi[i] = trend_empty;
    }
    newest = column;
    return;
  }

  // Closed columns are carried across untouched -- the window slides, it is
  // never re-binned. That is what keeps the picture from wobbling.
  const uint8_t dropped = static_cast<uint8_t>(shift);
  const size_t kept = sizeof(int16_t) * (trend_columns - dropped);
  memmove(lo, lo + dropped, kept);
  memmove(hi, hi + dropped, kept);
  for (uint8_t i = trend_columns - dropped; i < trend_columns; ++i) {
    lo[i] = hi[i] = trend_empty;
  }
  newest = column;
}

void Trend::anchor(uint32_t epoch) {
  if (epoch == 0) {
    return;
  }
  const uint32_t column = epoch / trend_bucket_s;
  if (column > newest) {
    slide_to(column);
  }
}

void Trend::add(uint32_t epoch, float value) {
  if (epoch == 0) {
    return;  // no Apex clock yet, so there is nowhere to put this
  }

  const uint32_t column = epoch / trend_bucket_s;
  if (column > newest) {
    slide_to(column);
  }

  const uint32_t age = newest - column;
  if (age >= trend_columns) {
    return;  // off the left edge
  }

  const uint8_t index = static_cast<uint8_t>(trend_columns - 1 - age);
  const int16_t scaled = scale_reading(value);
  if (lo[index] == trend_empty) {
    lo[index] = hi[index] = scaled;
    return;
  }
  lo[index] = min(lo[index], scaled);
  hi[index] = max(hi[index], scaled);
}

bool Trend::range(float &low, float &high) const {
  int16_t lowest = 0;
  int16_t highest = 0;
  bool seen = false;

  for (uint8_t i = 0; i < trend_columns; ++i) {
    if (lo[i] == trend_empty) {
      continue;
    }
    if (!seen) {
      lowest = lo[i];
      highest = hi[i];
      seen = true;
      continue;
    }
    lowest = min(lowest, lo[i]);
    highest = max(highest, hi[i]);
  }

  if (!seen) {
    return false;
  }
  low = lowest / 100.0f;
  high = highest / 100.0f;
  return true;
}

void ReefState::format_now(char *out, size_t len) const {
  const uint32_t epoch = now_epoch();
  if (epoch == 0) {
    snprintf(out, len, "now");
    return;
  }

  // The Apex reports a true Unix epoch and its UTC offset separately, so local
  // time has to be assembled here.
  const uint32_t local = epoch + tz_offset_s;
  snprintf(out, len, "%02u:%02u", static_cast<unsigned>((local / 3600) % 24),
           static_cast<unsigned>((local / 60) % 60));
}

void ReefState::add_event(EventKind kind, const char *title, const char *detail) {
  // Collapse a repeat of the newest event rather than filling the ring with it.
  if (event_count > 0 && strcmp(events[0].title, title) == 0) {
    return;
  }

  if (event_count == max_events) {
    --event_count;
  }
  memmove(events + 1, events, sizeof(Event) * event_count);
  ++event_count;

  Event &event = events[0];
  event.kind = kind;
  snprintf(event.title, sizeof(event.title), "%s", title);
  snprintf(event.detail, sizeof(event.detail), "%s", detail);
  format_now(event.time, sizeof(event.time));
}
